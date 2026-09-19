#include "quackapi_websocket.hpp"

#include <string.h>

#include "duckdb/common/exception.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include "httplib.hpp"
#include "mbedtls_wrapper.hpp"
#include "mbedtls/base64.h"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

const char *const QUACKAPI_WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

namespace {

//! Outbound client socket budgets. A WebSocket client is long-lived by nature,
//! so the read budget is the caller's idle_timeout_ms, not this.
constexpr time_t QUACKAPI_WS_CLIENT_CONNECT_TIMEOUT_SEC = 5;
constexpr time_t QUACKAPI_WS_CLIENT_IO_TIMEOUT_SEC = 30;

//! Standard base64 (not base64url — RFC 6455 uses the padded alphabet).
string Base64Encode(const string &raw) {
	size_t olen = 0;
	mbedtls_base64_encode(nullptr, 0, &olen, reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
	string encoded;
	encoded.resize(olen);
	if (olen > 0 && mbedtls_base64_encode(reinterpret_cast<unsigned char *>(&encoded[0]), encoded.size(), &olen,
	                                      reinterpret_cast<const unsigned char *>(raw.data()), raw.size()) != 0) {
		throw InternalException("quackapi: websocket base64 encode failed");
	}
	encoded.resize(olen);
	return encoded;
}

//! Decode standard base64. Returns false on any malformed input.
bool Base64Decode(const string &encoded, string &out) {
	size_t olen = 0;
	auto probe = mbedtls_base64_decode(nullptr, 0, &olen, reinterpret_cast<const unsigned char *>(encoded.data()),
	                                   encoded.size());
	// MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL is the expected "here is the size" answer.
	if (probe != 0 && probe != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
		return false;
	}
	out.resize(olen);
	if (olen == 0) {
		return true;
	}
	if (mbedtls_base64_decode(reinterpret_cast<unsigned char *>(&out[0]), out.size(), &olen,
	                          reinterpret_cast<const unsigned char *>(encoded.data()), encoded.size()) != 0) {
		return false;
	}
	out.resize(olen);
	return true;
}

//! True when `list` carries `token` as one comma-separated element, ignoring case.
//! Connection: keep-alive, Upgrade is the common browser spelling.
bool HeaderListHasToken(const string &list, const string &token) {
	auto parts = StringUtil::Split(list, ',');
	for (idx_t i = 0; i < parts.size(); i++) {
		auto part = parts[i];
		StringUtil::Trim(part);
		if (StringUtil::CIEquals(part, token)) {
			return true;
		}
	}
	return false;
}

void AppendBigEndian16(string &out, uint16_t value) {
	out += static_cast<char>((value >> 8) & 0xFF);
	out += static_cast<char>(value & 0xFF);
}

void AppendBigEndian64(string &out, uint64_t value) {
	for (int shift = 56; shift >= 0; shift -= 8) {
		out += static_cast<char>((value >> shift) & 0xFF);
	}
}

} // namespace

string QuackapiWsOpcodeName(QuackapiWsOpcode opcode) {
	switch (opcode) {
	case QuackapiWsOpcode::CONTINUATION:
		return "continuation";
	case QuackapiWsOpcode::TEXT:
		return "text";
	case QuackapiWsOpcode::BINARY:
		return "binary";
	case QuackapiWsOpcode::CLOSE:
		return "close";
	case QuackapiWsOpcode::PING:
		return "ping";
	case QuackapiWsOpcode::PONG:
		return "pong";
	default:
		return "unknown";
	}
}

string QuackapiWsAcceptKey(const string &client_key) {
	string decoded;
	if (!Base64Decode(client_key, decoded) || decoded.size() != 16) {
		throw InvalidInputException(
		    "quackapi: Sec-WebSocket-Key must be 16 base64-encoded bytes (RFC 6455 §4.1), got \"%s\"", client_key);
	}
	duckdb_mbedtls::MbedTlsWrapper::SHA1State sha1;
	sha1.AddString(client_key + QUACKAPI_WS_GUID);
	return Base64Encode(sha1.Finalize());
}

bool QuackapiWsParseHandshake(const string &head, QuackapiWsHandshake &out) {
	auto line_end = head.find("\r\n");
	if (line_end == string::npos) {
		return false;
	}
	auto request_line = head.substr(0, line_end);
	auto sp1 = request_line.find(' ');
	if (sp1 == string::npos) {
		return false;
	}
	auto sp2 = request_line.find(' ', sp1 + 1);
	if (sp2 == string::npos) {
		return false;
	}
	out.method = request_line.substr(0, sp1);
	out.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
	out.version = request_line.substr(sp2 + 1);
	if (!StringUtil::StartsWith(out.version, "HTTP/")) {
		return false;
	}
	// Same derivation httplib performs in Server::parse_request_line, so a
	// socket path and a route path are the same string for the same request.
	auto fragment = out.target.find('#');
	if (fragment != string::npos) {
		out.target.erase(fragment);
	}
	auto question = out.target.find('?');
	if (question == string::npos) {
		out.path = duckdb_httplib::decode_path_component(out.target);
		out.query.clear();
	} else {
		out.path = duckdb_httplib::decode_path_component(out.target.substr(0, question));
		out.query = out.target.substr(question + 1);
	}
	if (out.path.empty() || out.path[0] != '/') {
		return false;
	}

	idx_t pos = line_end + 2;
	while (pos < head.size()) {
		auto next = head.find("\r\n", pos);
		if (next == string::npos) {
			break;
		}
		if (next == pos) {
			// Empty line — end of the header block.
			return true;
		}
		auto line = head.substr(pos, next - pos);
		auto colon = line.find(':');
		if (colon != string::npos) {
			auto key = line.substr(0, colon);
			auto val = line.substr(colon + 1);
			StringUtil::Trim(key);
			StringUtil::Trim(val);
			// First value wins, matching CollectHeaders in the HTTP path.
			if (out.headers.find(key) == out.headers.end()) {
				out.headers[key] = val;
			}
		}
		pos = next + 2;
	}
	return true;
}

bool QuackapiWsIsUpgradeRequest(const QuackapiWsHandshake &head) {
	auto it = head.headers.find("Upgrade");
	if (it == head.headers.end()) {
		return false;
	}
	return HeaderListHasToken(it->second, "websocket");
}

QuackapiWsHandshakeError QuackapiWsValidateHandshake(const QuackapiWsHandshake &head, string &accept_out) {
	if (!StringUtil::CIEquals(head.method, "GET")) {
		return QuackapiWsHandshakeError::METHOD_NOT_GET;
	}
	if (head.version != "HTTP/1.1") {
		return QuackapiWsHandshakeError::VERSION_TOO_OLD;
	}
	auto connection = head.headers.find("Connection");
	if (connection == head.headers.end() || !HeaderListHasToken(connection->second, "Upgrade")) {
		return QuackapiWsHandshakeError::MISSING_CONNECTION_UPGRADE;
	}
	auto version = head.headers.find("Sec-WebSocket-Version");
	if (version == head.headers.end() || version->second != "13") {
		return QuackapiWsHandshakeError::UNSUPPORTED_VERSION;
	}
	auto key = head.headers.find("Sec-WebSocket-Key");
	if (key == head.headers.end()) {
		return QuackapiWsHandshakeError::BAD_KEY;
	}
	string decoded;
	if (!Base64Decode(key->second, decoded) || decoded.size() != 16) {
		return QuackapiWsHandshakeError::BAD_KEY;
	}
	accept_out = QuackapiWsAcceptKey(key->second);
	return QuackapiWsHandshakeError::NONE;
}

string QuackapiWsHandshakeErrorMessage(QuackapiWsHandshakeError error) {
	switch (error) {
	case QuackapiWsHandshakeError::METHOD_NOT_GET:
		return "WebSocket upgrade requires GET";
	case QuackapiWsHandshakeError::VERSION_TOO_OLD:
		return "WebSocket upgrade requires HTTP/1.1";
	case QuackapiWsHandshakeError::MISSING_CONNECTION_UPGRADE:
		return "WebSocket upgrade requires Connection: Upgrade";
	case QuackapiWsHandshakeError::BAD_KEY:
		return "Sec-WebSocket-Key must be 16 base64-encoded bytes";
	case QuackapiWsHandshakeError::UNSUPPORTED_VERSION:
		return "Only Sec-WebSocket-Version 13 is supported";
	default:
		return "WebSocket upgrade accepted";
	}
}

//===--------------------------------------------------------------------===//
// QuackapiWsConn — RFC 6455 framing over one httplib Stream
//===--------------------------------------------------------------------===//

QuackapiWsConn::QuackapiWsConn(duckdb_httplib::Stream &stream_p, bool client_side_p)
    : stream(stream_p), client_side(client_side_p) {
	if (client_side) {
		masking_rng = make_uniq<RandomEngine>();
	}
}

QuackapiWsConn::~QuackapiWsConn() = default;

bool QuackapiWsConn::ReadExact(char *out, size_t size) {
	size_t filled = 0;
	while (filled < size) {
		auto n = stream.read(out + filled, size - filled);
		if (n <= 0) {
			return false;
		}
		filled += static_cast<size_t>(n);
	}
	return true;
}

bool QuackapiWsConn::WriteAll(const char *data, size_t size) {
	size_t sent = 0;
	while (sent < size) {
		auto n = stream.write(data + sent, size - sent);
		if (n <= 0) {
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	return true;
}

bool QuackapiWsConn::HasPendingInput() {
	if (stream.is_readable()) {
		return true;
	}
	// select with a zero budget: "is there a byte right now", never a wait.
	return duckdb_httplib::detail::select_read(stream.socket(), 0, 0) > 0;
}

void QuackapiWsConn::Fail(QuackapiWsClose code, const string &reason) {
	error_code = code;
	error_reason = reason;
}

bool QuackapiWsConn::SendFrame(QuackapiWsOpcode opcode, bool fin, const char *payload, size_t size) {
	string frame;
	frame += static_cast<char>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode));
	const uint8_t mask_bit = client_side ? 0x80 : 0x00;
	if (size <= 125) {
		frame += static_cast<char>(mask_bit | static_cast<uint8_t>(size));
	} else if (size <= 0xFFFF) {
		frame += static_cast<char>(mask_bit | 126);
		AppendBigEndian16(frame, static_cast<uint16_t>(size));
	} else {
		frame += static_cast<char>(mask_bit | 127);
		AppendBigEndian64(frame, static_cast<uint64_t>(size));
	}
	if (!client_side) {
		frame.append(payload, size);
		return WriteAll(frame.data(), frame.size());
	}
	// RFC 6455 §5.3: every client frame carries a fresh, unpredictable 32-bit
	// masking key, all drawn from one engine for the life of the connection.
	uint32_t key_word = masking_rng->NextRandomInteger();
	unsigned char key[4];
	key[0] = static_cast<unsigned char>((key_word >> 24) & 0xFF);
	key[1] = static_cast<unsigned char>((key_word >> 16) & 0xFF);
	key[2] = static_cast<unsigned char>((key_word >> 8) & 0xFF);
	key[3] = static_cast<unsigned char>(key_word & 0xFF);
	frame.append(reinterpret_cast<const char *>(key), 4);
	auto body_offset = frame.size();
	frame.append(payload, size);
	for (size_t i = 0; i < size; i++) {
		frame[body_offset + i] = static_cast<char>(frame[body_offset + i] ^ static_cast<char>(key[i % 4]));
	}
	return WriteAll(frame.data(), frame.size());
}

bool QuackapiWsConn::Send(QuackapiWsOpcode opcode, const string &payload) {
	const bool is_control = (static_cast<uint8_t>(opcode) & 0x08) != 0;
	if (is_control) {
		// RFC 6455 §5.5: control frames are never fragmented and cap at 125 bytes.
		if (payload.size() > 125) {
			throw InternalException("quackapi: websocket control frame payload exceeds 125 bytes");
		}
		return SendFrame(opcode, true, payload.data(), payload.size());
	}
	if (payload.size() <= QUACKAPI_WS_FRAGMENT_BYTES) {
		return SendFrame(opcode, true, payload.data(), payload.size());
	}
	size_t offset = 0;
	bool first = true;
	while (offset < payload.size()) {
		auto chunk = payload.size() - offset;
		if (chunk > QUACKAPI_WS_FRAGMENT_BYTES) {
			chunk = static_cast<size_t>(QUACKAPI_WS_FRAGMENT_BYTES);
		}
		const bool last = (offset + chunk) >= payload.size();
		auto frame_opcode = first ? opcode : QuackapiWsOpcode::CONTINUATION;
		if (!SendFrame(frame_opcode, last, payload.data() + offset, chunk)) {
			return false;
		}
		offset += chunk;
		first = false;
	}
	return true;
}

bool QuackapiWsConn::SendClose(QuackapiWsClose code, const string &reason) {
	if (close_sent) {
		return true;
	}
	close_sent = true;
	string payload;
	AppendBigEndian16(payload, static_cast<uint16_t>(code));
	// 125 control-frame bytes minus the 2-byte code.
	payload += reason.size() > 123 ? reason.substr(0, 123) : reason;
	return SendFrame(QuackapiWsOpcode::CLOSE, true, payload.data(), payload.size());
}

bool QuackapiWsConn::SendFragment(QuackapiWsOpcode opcode, bool fin, const string &payload) {
	if ((static_cast<uint8_t>(opcode) & 0x08) != 0) {
		throw InternalException("quackapi: a control frame cannot be a fragment");
	}
	return SendFrame(opcode, fin, payload.data(), payload.size());
}

bool QuackapiWsConn::SendRaw(const string &bytes) {
	return WriteAll(bytes.data(), bytes.size());
}

QuackapiWsReadResult QuackapiWsConn::Read(QuackapiWsMessage &message, int64_t wait_budget_ms) {
	while (true) {
		if (!stream.is_readable()) {
			bool ready;
			if (wait_budget_ms < 0) {
				// Block on the stream's own read timeout.
				ready = stream.wait_readable();
			} else {
				// 0 polls without waiting; anything else is that many milliseconds.
				ready = duckdb_httplib::detail::select_read(stream.socket(), wait_budget_ms / 1000,
				                                           (wait_budget_ms % 1000) * 1000) > 0;
			}
			if (!ready) {
				return QuackapiWsReadResult::IDLE;
			}
		}

		unsigned char header[2];
		if (!ReadExact(reinterpret_cast<char *>(header), 2)) {
			return QuackapiWsReadResult::PEER_GONE;
		}
		const bool fin = (header[0] & 0x80) != 0;
		const uint8_t rsv = header[0] & 0x70;
		const uint8_t raw_opcode = header[0] & 0x0F;
		const bool masked = (header[1] & 0x80) != 0;
		uint64_t payload_len = header[1] & 0x7F;

		if (rsv != 0) {
			// No extension was negotiated, so RSV1-3 must be zero (§5.2).
			Fail(QuackapiWsClose::PROTOCOL_ERROR, "reserved frame bits set without a negotiated extension");
			return QuackapiWsReadResult::PROTOCOL_ERROR;
		}
		if (raw_opcode != 0x0 && raw_opcode != 0x1 && raw_opcode != 0x2 && raw_opcode != 0x8 && raw_opcode != 0x9 &&
		    raw_opcode != 0xA) {
			Fail(QuackapiWsClose::PROTOCOL_ERROR, "unknown opcode " + std::to_string(raw_opcode));
			return QuackapiWsReadResult::PROTOCOL_ERROR;
		}
		auto opcode = static_cast<QuackapiWsOpcode>(raw_opcode);
		const bool is_control = (raw_opcode & 0x08) != 0;

		if (masked == client_side) {
			// §5.3: a client MUST mask and a server MUST NOT; either violation
			// is a connection failure, never something to tolerate.
			Fail(QuackapiWsClose::PROTOCOL_ERROR,
			     client_side ? "server sent a masked frame" : "client sent an unmasked frame");
			return QuackapiWsReadResult::PROTOCOL_ERROR;
		}
		if (is_control && !fin) {
			Fail(QuackapiWsClose::PROTOCOL_ERROR, "control frame was fragmented");
			return QuackapiWsReadResult::PROTOCOL_ERROR;
		}
		if (is_control && payload_len > 125) {
			Fail(QuackapiWsClose::PROTOCOL_ERROR, "control frame payload exceeds 125 bytes");
			return QuackapiWsReadResult::PROTOCOL_ERROR;
		}

		if (payload_len == 126) {
			unsigned char extended[2];
			if (!ReadExact(reinterpret_cast<char *>(extended), 2)) {
				return QuackapiWsReadResult::PEER_GONE;
			}
			payload_len = (static_cast<uint64_t>(extended[0]) << 8) | static_cast<uint64_t>(extended[1]);
			if (payload_len <= 125) {
				Fail(QuackapiWsClose::PROTOCOL_ERROR, "length was not minimally encoded");
				return QuackapiWsReadResult::PROTOCOL_ERROR;
			}
		} else if (payload_len == 127) {
			unsigned char extended[8];
			if (!ReadExact(reinterpret_cast<char *>(extended), 8)) {
				return QuackapiWsReadResult::PEER_GONE;
			}
			payload_len = 0;
			for (int i = 0; i < 8; i++) {
				payload_len = (payload_len << 8) | static_cast<uint64_t>(extended[i]);
			}
			if ((payload_len >> 63) != 0) {
				Fail(QuackapiWsClose::PROTOCOL_ERROR, "most significant length bit must be zero");
				return QuackapiWsReadResult::PROTOCOL_ERROR;
			}
			if (payload_len <= 0xFFFF) {
				Fail(QuackapiWsClose::PROTOCOL_ERROR, "length was not minimally encoded");
				return QuackapiWsReadResult::PROTOCOL_ERROR;
			}
		}

		// Checked against the declared length, before a single byte is reserved.
		if (payload_len > QUACKAPI_WS_MAX_MESSAGE_BYTES ||
		    fragment_payload.size() + payload_len > QUACKAPI_WS_MAX_MESSAGE_BYTES) {
			Fail(QuackapiWsClose::MESSAGE_TOO_BIG,
			     "message exceeds " + std::to_string(QUACKAPI_WS_MAX_MESSAGE_BYTES) + " bytes");
			return QuackapiWsReadResult::PROTOCOL_ERROR;
		}

		unsigned char mask_key[4] = {0, 0, 0, 0};
		if (masked && !ReadExact(reinterpret_cast<char *>(mask_key), 4)) {
			return QuackapiWsReadResult::PEER_GONE;
		}

		string payload;
		if (payload_len > 0) {
			payload.resize(static_cast<size_t>(payload_len));
			if (!ReadExact(&payload[0], payload.size())) {
				return QuackapiWsReadResult::PEER_GONE;
			}
			if (masked) {
				for (size_t i = 0; i < payload.size(); i++) {
					payload[i] = static_cast<char>(payload[i] ^ static_cast<char>(mask_key[i % 4]));
				}
			}
		}

		if (is_control) {
			message.opcode = opcode;
			message.payload = payload;
			message.close_code = 0;
			message.frame_count = 1;
			if (opcode == QuackapiWsOpcode::CLOSE) {
				if (payload.size() == 1) {
					Fail(QuackapiWsClose::PROTOCOL_ERROR, "close payload of one byte carries no code");
					return QuackapiWsReadResult::PROTOCOL_ERROR;
				}
				// 1005 is the RFC's "no status code was present" sentinel.
				message.close_code = 1005;
				if (payload.size() >= 2) {
					message.close_code = static_cast<uint16_t>((static_cast<unsigned char>(payload[0]) << 8) |
					                                           static_cast<unsigned char>(payload[1]));
					message.payload = payload.substr(2);
					if (!Utf8Proc::IsValid(message.payload.data(), message.payload.size())) {
						Fail(QuackapiWsClose::INVALID_PAYLOAD, "close reason is not valid UTF-8");
						return QuackapiWsReadResult::PROTOCOL_ERROR;
					}
				}
			}
			return QuackapiWsReadResult::MESSAGE;
		}

		if (opcode == QuackapiWsOpcode::CONTINUATION) {
			if (!fragment_open) {
				Fail(QuackapiWsClose::PROTOCOL_ERROR, "continuation frame with no message open");
				return QuackapiWsReadResult::PROTOCOL_ERROR;
			}
			fragment_payload += payload;
			fragment_frames++;
		} else {
			if (fragment_open) {
				Fail(QuackapiWsClose::PROTOCOL_ERROR, "new data frame while a fragmented message was open");
				return QuackapiWsReadResult::PROTOCOL_ERROR;
			}
			fragment_open = true;
			fragment_opcode = opcode;
			fragment_payload = payload;
			fragment_frames = 1;
		}

		if (!fin) {
			// Mid-message: block on the stream deadline rather than re-charging
			// the caller's idle budget, which would cut a message in half.
			wait_budget_ms = -1;
			continue;
		}

		message.opcode = fragment_opcode;
		message.payload = fragment_payload;
		message.close_code = 0;
		message.frame_count = fragment_frames;
		fragment_open = false;
		fragment_opcode = QuackapiWsOpcode::CONTINUATION;
		fragment_payload.clear();
		fragment_frames = 0;
		if (message.opcode == QuackapiWsOpcode::TEXT &&
		    !Utf8Proc::IsValid(message.payload.data(), message.payload.size())) {
			// §5.6: a text message is UTF-8 by definition, and a VARCHAR bind
			// downstream depends on it.
			Fail(QuackapiWsClose::INVALID_PAYLOAD, "text message is not valid UTF-8");
			return QuackapiWsReadResult::PROTOCOL_ERROR;
		}
		return QuackapiWsReadResult::MESSAGE;
	}
}

//===--------------------------------------------------------------------===//
// quackapi_ws_accept(key) — the handshake derivation, on its own
//===--------------------------------------------------------------------===//

namespace {

void WsAcceptScalar(DataChunk &args, ExpressionState &, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t key) {
		return StringVector::AddString(result, QuackapiWsAcceptKey(key.GetString()));
	});
}

//===--------------------------------------------------------------------===//
// quackapi_ws_connect(url, …) — the client half, so a socket can be tested
//===--------------------------------------------------------------------===//

//! ws://host[:port]/path — the only scheme this listener speaks.
void SplitWsUrl(const string &url, string &host, int &port, string &path) {
	string rest;
	if (StringUtil::StartsWith(StringUtil::Lower(url), "ws://")) {
		rest = url.substr(5);
	} else if (StringUtil::StartsWith(StringUtil::Lower(url), "wss://")) {
		throw InvalidInputException(
		    "quackapi_ws_connect: wss:// is not supported — quackapi_serve listens on plain TCP, so terminate TLS in "
		    "front of it and connect with ws://");
	} else {
		throw InvalidInputException("quackapi_ws_connect: url must start with ws:// (got \"%s\")", url);
	}
	auto slash = rest.find('/');
	string authority;
	if (slash == string::npos) {
		authority = rest;
		path = "/";
	} else {
		authority = rest.substr(0, slash);
		path = rest.substr(slash);
	}
	port = 80;
	if (!authority.empty() && authority[0] == '[') {
		auto bracket = authority.rfind(']');
		if (bracket == string::npos) {
			throw InvalidInputException("quackapi_ws_connect: unterminated IPv6 host in \"%s\"", url);
		}
		host = authority.substr(1, bracket - 1);
		if (bracket + 1 < authority.size() && authority[bracket + 1] == ':') {
			port = std::stoi(authority.substr(bracket + 2));
		}
	} else {
		auto colon = authority.rfind(':');
		if (colon == string::npos) {
			host = authority;
		} else {
			host = authority.substr(0, colon);
			port = std::stoi(authority.substr(colon + 1));
		}
	}
	if (host.empty()) {
		throw InvalidInputException("quackapi_ws_connect: could not parse a host from \"%s\"", url);
	}
}

struct WsConnectBindData : public TableFunctionData {
	string url;
	vector<string> send;
	string raw;
	int64_t fragment_size = 0;
	bool ping = false;
	int64_t max_frames = 0;
	int64_t idle_timeout_ms = 1000;
};

struct WsConnectFrame {
	string opcode;
	string payload;
	//! Close frames only; NULL everywhere else.
	bool has_code = false;
	uint16_t code = 0;
	//! Wire frames this message arrived in — 1 unless the peer fragmented it.
	idx_t frames = 1;
};

struct WsConnectGlobalState : public GlobalTableFunctionState {
	vector<WsConnectFrame> frames;
	idx_t offset = 0;
};

//! Read the response head one byte at a time so no frame byte is consumed with it.
bool ReadHandshakeResponse(socket_t sock, string &head) {
	head.clear();
	char c = 0;
	while (head.size() < 8192) {
		auto n = duckdb_httplib::detail::read_socket(sock, &c, 1, CPPHTTPLIB_RECV_FLAGS);
		if (n <= 0) {
			return false;
		}
		head += c;
		if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) {
			return true;
		}
	}
	return false;
}

//! First line of an HTTP response, for the error a failed upgrade throws.
string StatusLineOf(const string &head) {
	auto line_end = head.find("\r\n");
	return line_end == string::npos ? head : head.substr(0, line_end);
}

//! Value of one response header, case-insensitively. Empty when absent.
string ResponseHeader(const string &head, const string &name) {
	idx_t pos = 0;
	auto line_end = head.find("\r\n");
	if (line_end == string::npos) {
		return string();
	}
	pos = line_end + 2;
	while (pos < head.size()) {
		auto next = head.find("\r\n", pos);
		if (next == string::npos || next == pos) {
			break;
		}
		auto line = head.substr(pos, next - pos);
		auto colon = line.find(':');
		if (colon != string::npos) {
			auto key = line.substr(0, colon);
			StringUtil::Trim(key);
			if (StringUtil::CIEquals(key, name)) {
				auto val = line.substr(colon + 1);
				StringUtil::Trim(val);
				return val;
			}
		}
		pos = next + 2;
	}
	return string();
}

void RecordFrame(vector<WsConnectFrame> &frames, const QuackapiWsMessage &message) {
	WsConnectFrame frame;
	frame.opcode = QuackapiWsOpcodeName(message.opcode);
	frame.payload = message.payload;
	frame.frames = message.frame_count;
	if (message.opcode == QuackapiWsOpcode::CLOSE) {
		frame.has_code = true;
		frame.code = message.close_code;
	}
	frames.push_back(frame);
}

unique_ptr<FunctionData> WsConnectBind(ClientContext &, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<WsConnectBindData>();
	bind_data->url = input.inputs[0].GetValue<string>();

	auto send_entry = input.named_parameters.find("send");
	if (send_entry != input.named_parameters.end() && !send_entry->second.IsNull()) {
		for (auto &child : ListValue::GetChildren(send_entry->second)) {
			if (child.IsNull()) {
				throw InvalidInputException("quackapi_ws_connect: send list must not contain NULL");
			}
			bind_data->send.push_back(child.GetValue<string>());
		}
	}
	auto raw_entry = input.named_parameters.find("raw");
	if (raw_entry != input.named_parameters.end() && !raw_entry->second.IsNull()) {
		bind_data->raw = StringValue::Get(raw_entry->second.DefaultCastAs(LogicalType::BLOB));
	}
	auto fragment_entry = input.named_parameters.find("fragment_size");
	if (fragment_entry != input.named_parameters.end() && !fragment_entry->second.IsNull()) {
		bind_data->fragment_size = fragment_entry->second.GetValue<int64_t>();
		if (bind_data->fragment_size < 0) {
			throw InvalidInputException("quackapi_ws_connect: fragment_size must be >= 0");
		}
	}
	auto ping_entry = input.named_parameters.find("ping");
	if (ping_entry != input.named_parameters.end() && !ping_entry->second.IsNull()) {
		bind_data->ping = ping_entry->second.GetValue<bool>();
	}
	auto max_entry = input.named_parameters.find("max_frames");
	if (max_entry != input.named_parameters.end() && !max_entry->second.IsNull()) {
		bind_data->max_frames = max_entry->second.GetValue<int64_t>();
		if (bind_data->max_frames < 0) {
			throw InvalidInputException("quackapi_ws_connect: max_frames must be >= 0");
		}
	}
	auto idle_entry = input.named_parameters.find("idle_timeout_ms");
	if (idle_entry != input.named_parameters.end() && !idle_entry->second.IsNull()) {
		bind_data->idle_timeout_ms = idle_entry->second.GetValue<int64_t>();
		if (bind_data->idle_timeout_ms <= 0) {
			throw InvalidInputException("quackapi_ws_connect: idle_timeout_ms must be > 0");
		}
	}

	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("seq");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("opcode");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("payload");
	return_types.emplace_back(LogicalType::INTEGER);
	names.emplace_back("code");
	return_types.emplace_back(LogicalType::BIGINT);
	names.emplace_back("frames");
	return std::move(bind_data);
}

//! Socket holder so every throw below still closes the fd.
struct WsClientSocket {
	explicit WsClientSocket(socket_t sock_p) : sock(sock_p) {
	}
	~WsClientSocket() {
		if (sock != INVALID_SOCKET) {
			duckdb_httplib::detail::shutdown_socket(sock);
			duckdb_httplib::detail::close_socket(sock);
		}
	}
	socket_t sock;
};

unique_ptr<GlobalTableFunctionState> WsConnectInit(ClientContext &, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<WsConnectBindData>();
	auto state = make_uniq<WsConnectGlobalState>();

	string host;
	string path;
	int port = 80;
	SplitWsUrl(bind_data.url, host, port, path);

	duckdb_httplib::Error connect_error = duckdb_httplib::Error::Success;
	auto raw_sock = duckdb_httplib::detail::create_client_socket(
	    host, "", port, AF_UNSPEC, /*tcp_nodelay=*/true, /*ipv6_v6only=*/false, duckdb_httplib::default_socket_options,
	    QUACKAPI_WS_CLIENT_CONNECT_TIMEOUT_SEC, 0, QUACKAPI_WS_CLIENT_IO_TIMEOUT_SEC, 0,
	    QUACKAPI_WS_CLIENT_IO_TIMEOUT_SEC, 0, /*intf=*/"", connect_error);
	if (raw_sock == INVALID_SOCKET) {
		throw IOException("quackapi_ws_connect: could not connect to %s:%d (%s)", host, port,
		                  duckdb_httplib::to_string(connect_error));
	}
	WsClientSocket holder(raw_sock);

	// RFC 6455 §4.1: the key is 16 freshly generated random bytes, base64'd.
	string nonce;
	nonce.resize(16);
	{
		RandomEngine engine;
		engine.RandomData(reinterpret_cast<data_ptr_t>(&nonce[0]), nonce.size());
	}
	auto key = Base64Encode(nonce);

	string request = "GET " + path + " HTTP/1.1\r\n";
	request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
	request += "Upgrade: websocket\r\n";
	request += "Connection: Upgrade\r\n";
	request += "Sec-WebSocket-Key: " + key + "\r\n";
	request += "Sec-WebSocket-Version: 13\r\n\r\n";
	{
		size_t sent = 0;
		while (sent < request.size()) {
			auto n = duckdb_httplib::detail::send_socket(holder.sock, request.data() + sent, request.size() - sent,
			                                             CPPHTTPLIB_SEND_FLAGS);
			if (n <= 0) {
				throw IOException("quackapi_ws_connect: handshake write to %s:%d failed", host, port);
			}
			sent += static_cast<size_t>(n);
		}
	}

	string head;
	if (!ReadHandshakeResponse(holder.sock, head)) {
		throw IOException("quackapi_ws_connect: %s:%d closed before answering the handshake", host, port);
	}
	auto status_line = StatusLineOf(head);
	if (status_line.find(" 101") == string::npos) {
		throw IOException("quackapi_ws_connect: server did not upgrade: %s", status_line);
	}
	auto accept = ResponseHeader(head, "Sec-WebSocket-Accept");
	auto expected = QuackapiWsAcceptKey(key);
	if (accept != expected) {
		throw IOException("quackapi_ws_connect: Sec-WebSocket-Accept was \"%s\", expected \"%s\"", accept, expected);
	}

	duckdb_httplib::detail::SocketStream stream(holder.sock, QUACKAPI_WS_CLIENT_IO_TIMEOUT_SEC, 0,
	                                            QUACKAPI_WS_CLIENT_IO_TIMEOUT_SEC, 0);
	QuackapiWsConn conn(stream, /*client_side=*/true);

	for (idx_t i = 0; i < bind_data.send.size(); i++) {
		auto &payload = bind_data.send[i];
		bool sent = true;
		if (bind_data.fragment_size > 0 && payload.size() > static_cast<size_t>(bind_data.fragment_size)) {
			size_t offset = 0;
			bool first = true;
			while (offset < payload.size() && sent) {
				auto chunk = payload.size() - offset;
				if (chunk > static_cast<size_t>(bind_data.fragment_size)) {
					chunk = static_cast<size_t>(bind_data.fragment_size);
				}
				const bool last = (offset + chunk) >= payload.size();
				// The opening fragment carries TEXT; every later one CONTINUATION (§5.4).
				sent = conn.SendFragment(first ? QuackapiWsOpcode::TEXT : QuackapiWsOpcode::CONTINUATION, last,
				                         payload.substr(offset, chunk));
				offset += chunk;
				first = false;
			}
		} else {
			sent = conn.Send(QuackapiWsOpcode::TEXT, payload);
		}
		if (!sent) {
			throw IOException("quackapi_ws_connect: send failed on message %lld", static_cast<long long>(i));
		}
	}
	if (!bind_data.raw.empty()) {
		conn.SendRaw(bind_data.raw);
	}
	if (bind_data.ping) {
		conn.Send(QuackapiWsOpcode::PING, "quackapi");
	}

	bool saw_close = false;
	while (!saw_close) {
		QuackapiWsMessage message;
		auto result = conn.Read(message, bind_data.idle_timeout_ms);
		if (result == QuackapiWsReadResult::IDLE || result == QuackapiWsReadResult::PEER_GONE) {
			break;
		}
		if (result == QuackapiWsReadResult::PROTOCOL_ERROR) {
			conn.SendClose(conn.ErrorCode(), conn.ErrorReason());
			throw IOException("quackapi_ws_connect: server violated RFC 6455: %s", conn.ErrorReason());
		}
		RecordFrame(state->frames, message);
		switch (message.opcode) {
		case QuackapiWsOpcode::CLOSE:
			saw_close = true;
			conn.SendClose(QuackapiWsClose::NORMAL, "");
			break;
		case QuackapiWsOpcode::PING:
			conn.Send(QuackapiWsOpcode::PONG, message.payload);
			break;
		case QuackapiWsOpcode::PONG:
			break;
		default:
			break;
		}
		if (bind_data.max_frames > 0 && static_cast<int64_t>(state->frames.size()) >= bind_data.max_frames) {
			break;
		}
	}
	if (!saw_close) {
		// Finish the close handshake rather than dropping the TCP connection.
		conn.SendClose(QuackapiWsClose::NORMAL, "");
		QuackapiWsMessage message;
		if (conn.Read(message, bind_data.idle_timeout_ms) == QuackapiWsReadResult::MESSAGE &&
		    message.opcode == QuackapiWsOpcode::CLOSE) {
			RecordFrame(state->frames, message);
		}
	}

	return std::move(state);
}

void WsConnectExec(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<WsConnectGlobalState>();
	idx_t row = 0;
	while (state.offset < state.frames.size() && row < STANDARD_VECTOR_SIZE) {
		auto &frame = state.frames[state.offset];
		output.SetValue(0, row, Value::BIGINT(static_cast<int64_t>(state.offset) + 1));
		output.SetValue(1, row, Value(frame.opcode));
		output.SetValue(2, row, Value(frame.payload));
		output.SetValue(3, row, frame.has_code ? Value::INTEGER(static_cast<int32_t>(frame.code))
		                                       : Value(LogicalType::INTEGER));
		output.SetValue(4, row, Value::BIGINT(static_cast<int64_t>(frame.frames)));
		row++;
		state.offset++;
	}
	output.SetCardinality(row);
}

} // namespace

void RegisterQuackapiWebsocketFunctions(ExtensionLoader &loader) {
	ScalarFunction accept("quackapi_ws_accept", {LogicalType::VARCHAR}, LogicalType::VARCHAR, WsAcceptScalar);
	loader.RegisterFunction(accept);

	TableFunction connect("quackapi_ws_connect", {LogicalType::VARCHAR}, WsConnectExec, WsConnectBind, WsConnectInit);
	connect.named_parameters["send"] = LogicalType::LIST(LogicalType::VARCHAR);
	connect.named_parameters["raw"] = LogicalType::BLOB;
	connect.named_parameters["fragment_size"] = LogicalType::BIGINT;
	connect.named_parameters["ping"] = LogicalType::BOOLEAN;
	connect.named_parameters["max_frames"] = LogicalType::BIGINT;
	connect.named_parameters["idle_timeout_ms"] = LogicalType::BIGINT;
	loader.RegisterFunction(connect);
}

} // namespace duckdb
