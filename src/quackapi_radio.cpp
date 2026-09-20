#include "quackapi_radio.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace duckdb {

namespace {

//! Messages a topic keeps for `quackapi_topic_messages()`. The oldest is
//! evicted past this, and a cursor still pointing below the oldest is
//! fast-forwarded — the skip is counted, never hidden.
constexpr idx_t QUACKAPI_TOPIC_DEFAULT_RETAIN = 1024;

//! How long `quackapi_topic_poll` waits for the first message before returning
//! no rows. The socket session re-polls immediately, so this is the sleep, not
//! the delivery latency: a publish wakes the waiter at once.
constexpr int64_t QUACKAPI_TOPIC_DEFAULT_WAIT_MS = 250;

//! A cursor nothing has polled for this long is forgotten, so a server does not
//! accumulate one entry per socket that ever connected. A returning cursor
//! resumes at the topic's tail, exactly as a new one does.
constexpr int64_t QUACKAPI_TOPIC_CURSOR_TTL_MS = 300000;

//! Longest a single poll may block. Well under the cursor TTL on purpose: a
//! waiter holds a `Topic &` across the wait, and only its own live cursor is
//! what stops the idle sweep from erasing that node underneath it.
constexpr int64_t QUACKAPI_TOPIC_MAX_WAIT_MS = 60000;

int64_t NowMillis() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

int64_t NowMicros() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

struct TopicMessage {
	uint64_t message_id = 0;
	string payload;
	int64_t published_us = 0;
};

//! One subscriber's position in a topic. Keyed by whatever string the SQL
//! passes — for a socket session that is `$request_id`, which quackapi mints
//! per handshake and echoes in the `X-Request-ID` response header.
struct TopicCursor {
	uint64_t next_message_id = 0;
	uint64_t delivered = 0;
	//! Messages the ring dropped before this cursor reached them.
	uint64_t skipped = 0;
	int64_t last_seen_ms = 0;
};

struct Topic {
	std::deque<TopicMessage> retained;
	idx_t retain = QUACKAPI_TOPIC_DEFAULT_RETAIN;
	//! Id the next publish will hand out. Also the tail a new cursor starts at.
	uint64_t next_message_id = 1;
	uint64_t evicted = 0;
	std::map<string, TopicCursor> cursors;
};

//! One broadcast condition variable for every topic: a publish wakes all
//! waiters and each re-checks its own topic. At socket scale (half of
//! worker_threads) the spurious wakeups cost less than a map of condvars.
struct TopicBroker {
	std::mutex lock;
	std::condition_variable arrived;
	std::map<string, Topic> topics;

	static TopicBroker &Get() {
		static TopicBroker broker;
		return broker;
	}
};

//! Forget cursors nothing has polled inside the TTL, and topics left with
//! neither cursors nor messages. `keep` is the topic the caller holds a
//! reference to. Callers hold the broker lock.
void SweepIdle(TopicBroker &broker, const string &keep, int64_t now_ms) {
	auto topic_it = broker.topics.begin();
	while (topic_it != broker.topics.end()) {
		auto &topic = topic_it->second;
		auto cursor_it = topic.cursors.begin();
		while (cursor_it != topic.cursors.end()) {
			if (now_ms - cursor_it->second.last_seen_ms > QUACKAPI_TOPIC_CURSOR_TTL_MS) {
				cursor_it = topic.cursors.erase(cursor_it);
			} else {
				++cursor_it;
			}
		}
		// A waiter always holds a cursor, so a topic with none has no waiter
		// whose `Topic &` this could invalidate.
		if (topic_it->first != keep && topic.cursors.empty() && topic.retained.empty()) {
			topic_it = broker.topics.erase(topic_it);
		} else {
			++topic_it;
		}
	}
}

Value TimestampValue(int64_t micros) {
	return Value::TIMESTAMP(timestamp_t(micros));
}

//===--------------------------------------------------------------------===//
// quackapi_topic_publish(topic, payload [, retain := 1024])
//===--------------------------------------------------------------------===//

struct TopicPublishBindData : public TableFunctionData {
	string topic;
	string payload;
	idx_t retain = QUACKAPI_TOPIC_DEFAULT_RETAIN;
};

struct TopicPublishGlobalState : public GlobalTableFunctionState {
	uint64_t message_id = 0;
	int64_t published_us = 0;
	vector<Value> subscribers;
	bool emitted = false;
};

unique_ptr<FunctionData> TopicPublishBind(ClientContext &, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<TopicPublishBindData>();
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("quackapi_topic_publish(topic, payload): neither argument may be NULL");
	}
	bind_data->topic = input.inputs[0].GetValue<string>();
	bind_data->payload = input.inputs[1].GetValue<string>();
	if (bind_data->topic.empty()) {
		throw BinderException("quackapi_topic_publish: topic may not be the empty string");
	}
	auto retain_param = input.named_parameters.find("retain");
	if (retain_param != input.named_parameters.end()) {
		auto retain = retain_param->second.GetValue<int64_t>();
		if (retain < 1) {
			throw BinderException("quackapi_topic_publish: retain must be at least 1, got %lld", (long long)retain);
		}
		bind_data->retain = static_cast<idx_t>(retain);
	}

	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("message_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("topic");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("published_at");
	return_types.emplace_back(LogicalType::LIST(LogicalType::VARCHAR));
	names.emplace_back("subscribers");
	return std::move(bind_data);
}

//! The publish itself lives in init, not bind: a prepared route or socket
//! handler binds once and executes many times, and every execution must send
//! exactly one message.
unique_ptr<GlobalTableFunctionState> TopicPublishInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TopicPublishBindData>();
	auto state = make_uniq<TopicPublishGlobalState>();
	auto &broker = TopicBroker::Get();
	auto now_ms = NowMillis();
	{
		std::lock_guard<std::mutex> guard(broker.lock);
		SweepIdle(broker, bind_data.topic, now_ms);
		auto &topic = broker.topics[bind_data.topic];
		topic.retain = bind_data.retain;

		TopicMessage message;
		message.message_id = topic.next_message_id++;
		message.payload = bind_data.payload;
		message.published_us = NowMicros();
		state->message_id = message.message_id;
		state->published_us = message.published_us;
		topic.retained.push_back(message);
		while (topic.retained.size() > topic.retain) {
			topic.retained.pop_front();
			topic.evicted++;
		}
		for (auto &cursor : topic.cursors) {
			state->subscribers.emplace_back(Value(cursor.first));
		}
	}
	broker.arrived.notify_all();
	return std::move(state);
}

void TopicPublishExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<TopicPublishBindData>();
	auto &state = data_p.global_state->Cast<TopicPublishGlobalState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	state.emitted = true;
	output.SetValue(0, 0, Value::UBIGINT(state.message_id));
	output.SetValue(1, 0, Value(bind_data.topic));
	output.SetValue(2, 0, TimestampValue(state.published_us));
	output.SetValue(3, 0, Value::LIST(LogicalType::VARCHAR, state.subscribers));
	output.SetCardinality(1);
}

//===--------------------------------------------------------------------===//
// quackapi_topic_poll(topic, cursor [, wait_ms := 250])
//===--------------------------------------------------------------------===//

struct TopicPollBindData : public TableFunctionData {
	string topic;
	string cursor;
	int64_t wait_ms = QUACKAPI_TOPIC_DEFAULT_WAIT_MS;
};

struct TopicMessageListState : public GlobalTableFunctionState {
	vector<TopicMessage> messages;
	idx_t offset = 0;
};

unique_ptr<FunctionData> TopicPollBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<TopicPollBindData>();
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("quackapi_topic_poll(topic, cursor): neither argument may be NULL");
	}
	bind_data->topic = input.inputs[0].GetValue<string>();
	bind_data->cursor = input.inputs[1].GetValue<string>();
	if (bind_data->topic.empty() || bind_data->cursor.empty()) {
		throw BinderException("quackapi_topic_poll: topic and cursor may not be the empty string");
	}
	auto wait_param = input.named_parameters.find("wait_ms");
	if (wait_param != input.named_parameters.end()) {
		bind_data->wait_ms = wait_param->second.GetValue<int64_t>();
		if (bind_data->wait_ms < 0 || bind_data->wait_ms > QUACKAPI_TOPIC_MAX_WAIT_MS) {
			throw BinderException("quackapi_topic_poll: wait_ms must be between 0 and %lld, got %lld",
			                      (long long)QUACKAPI_TOPIC_MAX_WAIT_MS, (long long)bind_data->wait_ms);
		}
	}

	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("message_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("topic");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("payload");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("published_at");
	return std::move(bind_data);
}

//! Registering the cursor and draining it happen in init, so each execution of
//! a prepared socket handler is one poll. The first sight of a cursor puts it
//! at the topic's tail: a subscriber gets what is published after it arrives,
//! and the backlog is `quackapi_topic_messages()`, which no cursor touches.
unique_ptr<GlobalTableFunctionState> TopicPollInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TopicPollBindData>();
	auto state = make_uniq<TopicMessageListState>();
	auto &broker = TopicBroker::Get();
	auto now_ms = NowMillis();

	std::unique_lock<std::mutex> guard(broker.lock);
	SweepIdle(broker, bind_data.topic, now_ms);
	auto &topic = broker.topics[bind_data.topic];
	auto cursor_entry = topic.cursors.find(bind_data.cursor);
	if (cursor_entry == topic.cursors.end()) {
		TopicCursor fresh;
		fresh.next_message_id = topic.next_message_id;
		cursor_entry = topic.cursors.insert(std::make_pair(bind_data.cursor, fresh)).first;
	}
	auto &cursor = cursor_entry->second;
	cursor.last_seen_ms = now_ms;

	if (bind_data.wait_ms > 0 && cursor.next_message_id >= topic.next_message_id) {
		broker.arrived.wait_for(guard, std::chrono::milliseconds(bind_data.wait_ms),
		                        [&topic, &cursor] { return cursor.next_message_id < topic.next_message_id; });
	}

	// Anything the ring dropped before this cursor reached it is gone. Count
	// the gap rather than pretending the messages were delivered.
	if (!topic.retained.empty() && cursor.next_message_id < topic.retained.front().message_id) {
		cursor.skipped += topic.retained.front().message_id - cursor.next_message_id;
		cursor.next_message_id = topic.retained.front().message_id;
	}
	for (auto &message : topic.retained) {
		if (message.message_id >= cursor.next_message_id) {
			state->messages.push_back(message);
		}
	}
	cursor.next_message_id = topic.next_message_id;
	cursor.delivered += state->messages.size();
	return std::move(state);
}

void TopicPollExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<TopicPollBindData>();
	auto &state = data_p.global_state->Cast<TopicMessageListState>();
	idx_t row = 0;
	while (state.offset < state.messages.size() && row < STANDARD_VECTOR_SIZE) {
		auto &message = state.messages[state.offset];
		output.SetValue(0, row, Value::UBIGINT(message.message_id));
		output.SetValue(1, row, Value(bind_data.topic));
		output.SetValue(2, row, Value(message.payload));
		output.SetValue(3, row, TimestampValue(message.published_us));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

//===--------------------------------------------------------------------===//
// quackapi_topic_messages(topic)
//===--------------------------------------------------------------------===//

struct TopicMessagesBindData : public TableFunctionData {
	string topic;
};

unique_ptr<FunctionData> TopicMessagesBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<TopicMessagesBindData>();
	if (input.inputs[0].IsNull()) {
		throw BinderException("quackapi_topic_messages(topic): topic may not be NULL");
	}
	bind_data->topic = input.inputs[0].GetValue<string>();

	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("message_id");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("topic");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("payload");
	return_types.emplace_back(LogicalType::TIMESTAMP);
	names.emplace_back("published_at");
	return std::move(bind_data);
}

unique_ptr<GlobalTableFunctionState> TopicMessagesInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TopicMessagesBindData>();
	auto state = make_uniq<TopicMessageListState>();
	auto &broker = TopicBroker::Get();
	std::lock_guard<std::mutex> guard(broker.lock);
	auto topic_entry = broker.topics.find(bind_data.topic);
	if (topic_entry != broker.topics.end()) {
		for (auto &message : topic_entry->second.retained) {
			state->messages.push_back(message);
		}
	}
	return std::move(state);
}

void TopicMessagesExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<TopicMessagesBindData>();
	auto &state = data_p.global_state->Cast<TopicMessageListState>();
	idx_t row = 0;
	while (state.offset < state.messages.size() && row < STANDARD_VECTOR_SIZE) {
		auto &message = state.messages[state.offset];
		output.SetValue(0, row, Value::UBIGINT(message.message_id));
		output.SetValue(1, row, Value(bind_data.topic));
		output.SetValue(2, row, Value(message.payload));
		output.SetValue(3, row, TimestampValue(message.published_us));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

//===--------------------------------------------------------------------===//
// quackapi_topics()
//===--------------------------------------------------------------------===//

struct TopicsRow {
	string topic;
	uint64_t retained = 0;
	uint64_t retain = 0;
	uint64_t next_message_id = 0;
	uint64_t evicted = 0;
	vector<Value> cursors;
};

struct TopicsGlobalState : public GlobalTableFunctionState {
	vector<TopicsRow> rows;
	idx_t offset = 0;
};

LogicalType CursorStructType() {
	child_list_t<LogicalType> children;
	children.emplace_back("cursor", LogicalType::VARCHAR);
	children.emplace_back("next_message_id", LogicalType::UBIGINT);
	children.emplace_back("delivered", LogicalType::UBIGINT);
	children.emplace_back("skipped", LogicalType::UBIGINT);
	return LogicalType::STRUCT(children);
}

unique_ptr<FunctionData> TopicsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                    vector<string> &names) {
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("topic");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("retained");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("retain");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("next_message_id");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("evicted");
	return_types.emplace_back(LogicalType::LIST(CursorStructType()));
	names.emplace_back("cursors");
	return make_uniq<TableFunctionData>();
}

unique_ptr<GlobalTableFunctionState> TopicsInit(ClientContext &, TableFunctionInitInput &) {
	auto state = make_uniq<TopicsGlobalState>();
	auto &broker = TopicBroker::Get();
	std::lock_guard<std::mutex> guard(broker.lock);
	for (auto &entry : broker.topics) {
		TopicsRow row;
		row.topic = entry.first;
		row.retained = entry.second.retained.size();
		row.retain = entry.second.retain;
		row.next_message_id = entry.second.next_message_id;
		row.evicted = entry.second.evicted;
		for (auto &cursor : entry.second.cursors) {
			child_list_t<Value> fields;
			fields.emplace_back("cursor", Value(cursor.first));
			fields.emplace_back("next_message_id", Value::UBIGINT(cursor.second.next_message_id));
			fields.emplace_back("delivered", Value::UBIGINT(cursor.second.delivered));
			fields.emplace_back("skipped", Value::UBIGINT(cursor.second.skipped));
			row.cursors.emplace_back(Value::STRUCT(fields));
		}
		state->rows.push_back(row);
	}
	return std::move(state);
}

void TopicsExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<TopicsGlobalState>();
	idx_t row = 0;
	while (state.offset < state.rows.size() && row < STANDARD_VECTOR_SIZE) {
		auto &source = state.rows[state.offset];
		output.SetValue(0, row, Value(source.topic));
		output.SetValue(1, row, Value::UBIGINT(source.retained));
		output.SetValue(2, row, Value::UBIGINT(source.retain));
		output.SetValue(3, row, Value::UBIGINT(source.next_message_id));
		output.SetValue(4, row, Value::UBIGINT(source.evicted));
		output.SetValue(5, row, Value::LIST(CursorStructType(), source.cursors));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

} // namespace

void RegisterQuackapiRadioFunctions(ExtensionLoader &loader) {
	TableFunction publish("quackapi_topic_publish", {LogicalType::VARCHAR, LogicalType::VARCHAR}, TopicPublishExec,
	                      TopicPublishBind, TopicPublishInit);
	publish.named_parameters["retain"] = LogicalType::BIGINT;
	loader.RegisterFunction(publish);

	TableFunction poll("quackapi_topic_poll", {LogicalType::VARCHAR, LogicalType::VARCHAR}, TopicPollExec,
	                   TopicPollBind, TopicPollInit);
	poll.named_parameters["wait_ms"] = LogicalType::BIGINT;
	loader.RegisterFunction(poll);

	loader.RegisterFunction(TableFunction("quackapi_topic_messages", {LogicalType::VARCHAR}, TopicMessagesExec,
	                                      TopicMessagesBind, TopicMessagesInit));

	loader.RegisterFunction(TableFunction("quackapi_topics", {}, TopicsExec, TopicsBind, TopicsInit));
}

} // namespace duckdb
