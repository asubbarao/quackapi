// Integration regression: a mutating POST that returns an error is attempted
// once, while GET keeps DuckDB's existing retry behavior.
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/config.hpp"

#include "httplib.hpp"

#include <atomic>
#include <cassert>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct RequestCounts {
	std::atomic<int> posts {0};
	std::atomic<int> gets {0};
};

class CountingClient final : public duckdb::HTTPClient {
public:
	CountingClient(const duckdb::string &origin, RequestCounts &counts)
	    : duckdb::HTTPClient(origin), client(PlainOrigin(origin)), counts(counts) {
		client.set_connection_timeout(5);
		client.set_read_timeout(5);
		client.set_write_timeout(5);
		client.set_keep_alive(true);
	}

	void Initialize(duckdb::HTTPParams &) override {
	}

	duckdb::unique_ptr<duckdb::HTTPResponse> Get(duckdb::GetRequestInfo &info) override {
		counts.gets.fetch_add(1);
		return Do(client.Get(info.path));
	}

	duckdb::unique_ptr<duckdb::HTTPResponse> Put(duckdb::PutRequestInfo &) override {
		throw duckdb::NotImplementedException("test client does not implement PUT");
	}

	duckdb::unique_ptr<duckdb::HTTPResponse> Head(duckdb::HeadRequestInfo &) override {
		throw duckdb::NotImplementedException("test client does not implement HEAD");
	}

	duckdb::unique_ptr<duckdb::HTTPResponse> Delete(duckdb::DeleteRequestInfo &) override {
		throw duckdb::NotImplementedException("test client does not implement DELETE");
	}

	duckdb::unique_ptr<duckdb::HTTPResponse> Post(duckdb::PostRequestInfo &info) override {
		counts.posts.fetch_add(1);
		std::string body(reinterpret_cast<const char *>(info.buffer_in), info.buffer_in_len);
		return Do(client.Post(info.path, body, "application/json"));
	}

private:
	static duckdb::string PlainOrigin(const duckdb::string &origin) {
		if (origin.compare(0, 8, "https://") == 0) {
			return "http://" + origin.substr(8);
		}
		return origin;
	}

	duckdb::unique_ptr<duckdb::HTTPResponse> Do(const duckdb_httplib::Result &result) {
		auto response = duckdb::make_uniq<duckdb::HTTPResponse>(
		    duckdb::HTTPUtil::ToStatusCode(result ? result->status : 0));
		if (result.error() != duckdb_httplib::Error::Success) {
			response->request_error = duckdb_httplib::to_string(result.error());
			return response;
		}
		response->body = result->body;
		response->reason = result->reason;
		for (const auto &entry : result->headers) {
			response->headers.Insert(entry.first, entry.second);
		}
		return response;
	}

	duckdb_httplib::Client client;
	RequestCounts &counts;
};

class CountingHTTPUtil final : public duckdb::HTTPUtil {
public:
	explicit CountingHTTPUtil(RequestCounts &counts) : counts(counts) {
	}

	duckdb::string GetName() const override {
		return "Test-Curl";
	}

	duckdb::unique_ptr<duckdb::HTTPParams> InitializeParameters(duckdb::DatabaseInstance &,
	                                                            const duckdb::string &) override {
		return duckdb::make_uniq<duckdb::HTTPParams>(*this);
	}

	duckdb::unique_ptr<duckdb::HTTPClient> InitializeClient(duckdb::HTTPParams &, const duckdb::string &origin) override {
		return duckdb::make_uniq<CountingClient>(origin, counts);
	}

private:
	RequestCounts &counts;
};

std::string SqlQuote(const std::string &value) {
	std::string out = "'";
	for (auto ch : value) {
		out += ch;
		if (ch == '\'') {
			out += '\'';
		}
	}
	out += "'";
	return out;
}

void Check(duckdb::QueryResult &result) {
	if (result.HasError()) {
		throw std::runtime_error(result.GetError());
	}
}

int Status(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query(sql);
	Check(*result);
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() != 1) {
		throw std::runtime_error("expected one result row");
	}
	return chunk->GetValue(0, 0).GetValue<int32_t>();
}

} // namespace

int main(int argc, char **argv) {
	const std::string extension = argc > 1 ? argv[1] : "build/release/extension/quackapi/quackapi.duckdb_extension";

	RequestCounts counts;
	std::atomic<int> endpoint_posts {0};
	std::atomic<int> endpoint_gets {0};
	duckdb_httplib::Server server;
	server.Post("/mutate-then-error", [&endpoint_posts](const duckdb_httplib::Request &,
	                                                     duckdb_httplib::Response &response) {
		// This represents a mutation that happened before the upstream reported
		// failure. Connection: close also makes the uncertainty explicit to the
		// caller; it must not cause a replay.
		endpoint_posts.fetch_add(1);
		response.status = 500;
		response.set_header("Connection", "close");
		response.set_content("mutation committed before failure", "text/plain");
	});
	server.Get("/read-then-error", [&endpoint_gets](const duckdb_httplib::Request &,
	                                                 duckdb_httplib::Response &response) {
		endpoint_gets.fetch_add(1);
		response.status = 500;
		response.set_header("Connection", "close");
		response.set_content("temporary read failure", "text/plain");
	});

	const auto port = server.bind_to_any_port("127.0.0.1");
	if (port <= 0) {
		throw std::runtime_error("could not bind regression server");
	}
	std::thread server_thread([&server]() { server.listen_after_bind(); });

	try {
		duckdb::DuckDB database(nullptr);
		database.instance->config.SetHTTPUtil(duckdb::make_shared_ptr<CountingHTTPUtil>(counts));
		duckdb::Connection connection(database);
		auto load = connection.Query("LOAD " + SqlQuote(extension));
		Check(*load);

		const std::string origin = "https://127.0.0.1:" + std::to_string(port);
		const auto post_status = Status(
		    connection, "SELECT (quackapi_post(" + SqlQuote(origin + "/mutate-then-error") + ", '{}')).status");
		assert(post_status == 500);
		assert(counts.posts.load() == 1);
		assert(endpoint_posts.load() == 1);

		const auto get_status = Status(
		    connection, "SELECT (quackapi_fetch(" + SqlQuote(origin + "/read-then-error") + ")).status");
		assert(get_status == 500);
		assert(counts.gets.load() == static_cast<int>(duckdb::HTTPParams::DEFAULT_RETRIES + 1));
		assert(endpoint_gets.load() == static_cast<int>(duckdb::HTTPParams::DEFAULT_RETRIES + 1));
	} catch (...) {
		server.stop();
		server_thread.join();
		throw;
	}

	server.stop();
	server_thread.join();
	return 0;
}
