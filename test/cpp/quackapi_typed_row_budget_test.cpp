#include "quackapi_util.hpp"

#include "duckdb.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/write_stream.hpp"

#include <iostream>

using namespace duckdb;

namespace {

class ByteCounter : public WriteStream {
public:
	idx_t bytes = 0;
	void WriteData(const_data_ptr_t, idx_t size) override {
		bytes += size;
	}
};

idx_t SerializedSize(const Value &value) {
	ByteCounter counter;
	BinarySerializer::Serialize(value, counter);
	return counter.bytes;
}

idx_t ResponseSize(const QuackapiTypedResult &result) {
	child_list_t<LogicalType> fields;
	fields.emplace_back("columns", result.columns.type());
	fields.emplace_back("rows", result.rows.type());
	auto response_type = LogicalType::STRUCT(std::move(fields));
	return SerializedSize(Value::STRUCT(response_type, vector<Value> {result.columns, result.rows}));
}

struct InputRows {
	vector<string> names;
	vector<LogicalType> types;
	vector<vector<Value>> rows;
};

InputRows ReadRows(Connection &connection, idx_t count) {
	auto result = connection.Query(
	    "SELECT i::BIGINT AS id, "
	    "CASE WHEN i % 5 = 0 THEN NULL ELSE CAST('blob-' || i::VARCHAR AS BLOB) END AS payload, "
	    "CASE WHEN i % 3 = 0 THEN NULL ELSE ((i % 100000)::DECIMAL(18,2) / 100)::DECIMAL(18,2) END AS amount, "
	    "[i::INTEGER, NULL::INTEGER, (i + 1)::INTEGER] AS items "
	    "FROM range(" +
	    to_string(count) + ") AS t(i)");
	if (result->HasError()) {
		throw InvalidInputException("typed budget fixture query failed: %s", result->GetError());
	}
	InputRows input;
	input.names = QuackapiNames(result->GetNames());
	input.types = result->GetTypes();
	while (auto chunk = result->Fetch()) {
		for (idx_t row = 0; row < chunk->size(); row++) {
			vector<Value> values;
			values.reserve(chunk->ColumnCount());
			for (idx_t col = 0; col < chunk->ColumnCount(); col++) {
				values.emplace_back(chunk->GetValue(col, row));
			}
			input.rows.emplace_back(std::move(values));
		}
	}
	return input;
}

QuackapiTypedResult Collect(const InputRows &input, idx_t cap) {
	QuackapiTypedRowCollector collector(input.names, input.types, cap);
	for (const auto &row : input.rows) {
		collector.AppendRow(row);
	}
	return collector.Finish();
}

bool RejectsAt(const InputRows &input, idx_t cap) {
	try {
		(void)Collect(input, cap);
	} catch (const QuackapiTypedRowsTooLarge &) {
		return true;
	}
	return false;
}

} // namespace

int main() {
	try {
		DuckDB database(nullptr);
		Connection connection(database);
		const idx_t counts[] = {0, 1, 127, 128, 16383, 16384};
		for (auto count : counts) {
			auto input = ReadRows(connection, count);
			auto exact = Collect(input, NumericLimits<idx_t>::Maximum());
			auto exact_bytes = ResponseSize(exact);
			auto accepted = Collect(input, exact_bytes);
			if (ResponseSize(accepted) != exact_bytes) {
				std::cerr << "exact-cap typed response size changed for " << count << " rows\n";
				return 1;
			}
			if (exact_bytes == 0 || !RejectsAt(input, exact_bytes - 1)) {
				std::cerr << "one-byte-under cap was accepted for " << count << " rows\n";
				return 1;
			}
		}
	} catch (const std::exception &ex) {
		std::cerr << ex.what() << '\n';
		return 1;
	}
	return 0;
}
