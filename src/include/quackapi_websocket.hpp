#pragma once

#include <stdint.h>

#include "duckdb/common/string.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb_httplib {
class Stream;
} // namespace duckdb_httplib

namespace duckdb {

class RandomEngine;

//! RFC 6455 §11.8 — the GUID concatenated with Sec-WebSocket-Key before SHA-1.
extern const char *const QUACKAPI_WS_GUID;

//! Largest single message quackapi will accept or assemble, inbound or
//! outbound. Same 8 MiB ceiling an HTTP body gets (QUACKAPI_PAYLOAD_MAX_LENGTH)
//! — a socket is not a licence to buffer more than a request may.
static constexpr uint64_t QUACKAPI_WS_MAX_MESSAGE_BYTES = 8ull * 1024ull * 1024ull;

//! Outbound payloads longer than this leave as FIN=0 + continuation frames.
//! Fixed, not an option: a peer must handle fragments either way (RFC 6455
//! §5.4), and a row that happens to be large is not a different protocol.
static constexpr uint64_t QUACKAPI_WS_FRAGMENT_BYTES = 64ull * 1024ull;

//! RFC 6455 §5.2 opcodes. Values are the wire nibble.
enum class QuackapiWsOpcode : uint8_t {
	CONTINUATION = 0x0,
	TEXT = 0x1,
	BINARY = 0x2,
	CLOSE = 0x8,
	PING = 0x9,
	PONG = 0xA,
};

//! RFC 6455 §7.4.1 close codes quackapi emits.
enum class QuackapiWsClose : uint16_t {
	NORMAL = 1000,
	GOING_AWAY = 1001,
	PROTOCOL_ERROR = 1002,
	UNSUPPORTED_DATA = 1003,
	INVALID_PAYLOAD = 1007,
	POLICY_VIOLATION = 1008,
	MESSAGE_TOO_BIG = 1009,
	INTERNAL_ERROR = 1011,
};

//! Lower-case wire name of an opcode ("text", "ping", …) for introspection rows.
string QuackapiWsOpcodeName(QuackapiWsOpcode opcode);

//! True when these bytes are valid UTF-8, i.e. they would have been legal in a
//! text frame (RFC 6455 §5.6). A binary frame carrying text is the only way
//! some clients can send one — the `radio` extension's transmit queue calls
//! ixwebsocket's sendBinary unconditionally — so a $message endpoint asks this
//! rather than refusing every binary frame on its opcode alone.
bool QuackapiWsPayloadIsText(const string &payload);

//! Sec-WebSocket-Accept from Sec-WebSocket-Key: base64(SHA-1(key + GUID)).
//! RFC 6455 §4.2.2 step 5.4. Throws when the key is not 16 base64-encoded bytes,
//! because a client that sent a malformed key gets a 400, never a made-up accept.
string QuackapiWsAcceptKey(const string &client_key);

//! One parsed HTTP request head, enough to decide and answer an Upgrade.
struct QuackapiWsHandshake {
	string method;
	//! Request target including any query string.
	string target;
	//! Path with the query string removed; no percent-decoding (route match does that).
	string path;
	string query;
	string version;
	case_insensitive_map_t<string> headers;
};

//! Parse a complete request head (through the terminating CRLFCRLF).
//! Returns false when the head is not a well-formed HTTP/1.x request line +
//! headers; the caller then leaves the bytes for httplib to reject its own way.
bool QuackapiWsParseHandshake(const string &head, QuackapiWsHandshake &out);

//! True when this head asks for an RFC 6455 upgrade (Upgrade: websocket).
//! Deliberately checks only the token, so a malformed WebSocket request is
//! still answered as one (with a 400) rather than falling through to a 404.
bool QuackapiWsIsUpgradeRequest(const QuackapiWsHandshake &head);

//! Why a handshake cannot be completed. NONE means it can.
enum class QuackapiWsHandshakeError : uint8_t {
	NONE = 0,
	//! Anything but GET (RFC 6455 §4.2.1 step 1) → 400.
	METHOD_NOT_GET,
	//! HTTP/1.0 or earlier → 400.
	VERSION_TOO_OLD,
	//! Connection header does not contain the Upgrade token → 400.
	MISSING_CONNECTION_UPGRADE,
	//! Sec-WebSocket-Key absent or not 16 base64 bytes → 400.
	BAD_KEY,
	//! Sec-WebSocket-Version is not 13 → 426 + Sec-WebSocket-Version: 13.
	UNSUPPORTED_VERSION,
};

//! Validate the upgrade request. Fills accept_out only when the result is NONE.
QuackapiWsHandshakeError QuackapiWsValidateHandshake(const QuackapiWsHandshake &head, string &accept_out);

//! Human-readable reason for a handshake error, used verbatim in the 400/426 body.
string QuackapiWsHandshakeErrorMessage(QuackapiWsHandshakeError error);

//! One decoded message (control or data) handed back by QuackapiWsConn::Read.
struct QuackapiWsMessage {
	QuackapiWsOpcode opcode = QuackapiWsOpcode::TEXT;
	string payload;
	//! Close frames only: the code from the first two payload bytes, 1005 when absent.
	uint16_t close_code = 0;
	//! Wire frames this message was assembled from: 1 unless it was fragmented.
	idx_t frame_count = 1;
};

//! What a read attempt produced.
enum class QuackapiWsReadResult : uint8_t {
	//! message is filled.
	MESSAGE = 0,
	//! Nothing arrived before the wait budget expired; the connection is still good.
	IDLE,
	//! Peer closed the TCP connection without a Close frame.
	PEER_GONE,
	//! The peer broke RFC 6455. close_code / error carry what to send and log.
	PROTOCOL_ERROR,
};

//! RFC 6455 framing over one httplib Stream. Both directions of the same
//! socket; `client_side` decides which way masking is mandatory (§5.3: a
//! client MUST mask, a server MUST NOT, and each MUST fail the connection on
//! the other's violation).
class QuackapiWsConn {
public:
	QuackapiWsConn(duckdb_httplib::Stream &stream, bool client_side);
	~QuackapiWsConn();

	//! Read one complete message, reassembling continuation frames. Control
	//! frames are returned as they arrive — the caller answers Ping and Close.
	//! wait_budget_ms: negative blocks on the stream's own read timeout, 0 polls
	//! without waiting, positive waits that many milliseconds for the first byte.
	QuackapiWsReadResult Read(QuackapiWsMessage &message, int64_t wait_budget_ms);

	//! True when bytes are already buffered or the socket is readable right now.
	bool HasPendingInput();

	//! Send one message, fragmenting above QUACKAPI_WS_FRAGMENT_BYTES.
	bool Send(QuackapiWsOpcode opcode, const string &payload);
	//! Send a Close frame carrying code + reason. Never throws; a dead socket
	//! is expected here and is reported by the return value.
	bool SendClose(QuackapiWsClose code, const string &reason);
	//! Send one fragment of a message: the opening fragment carries the data
	//! opcode, every later one CONTINUATION, and only the last has fin set.
	bool SendFragment(QuackapiWsOpcode opcode, bool fin, const string &payload);
	//! Write raw bytes with no framing at all (protocol debugging).
	bool SendRaw(const string &bytes);

	//! Close code and reason chosen for the last PROTOCOL_ERROR read.
	QuackapiWsClose ErrorCode() const {
		return error_code;
	}
	const string &ErrorReason() const {
		return error_reason;
	}
	//! True once a Close frame has been written on this connection.
	bool CloseSent() const {
		return close_sent;
	}

private:
	//! Read exactly `size` bytes. Returns false on EOF, error, or timeout.
	bool ReadExact(char *out, size_t size);
	bool WriteAll(const char *data, size_t size);
	bool SendFrame(QuackapiWsOpcode opcode, bool fin, const char *payload, size_t size);
	void Fail(QuackapiWsClose code, const string &reason);

	duckdb_httplib::Stream &stream;
	bool client_side;
	//! Opcode of the message being reassembled; CONTINUATION means none is open.
	QuackapiWsOpcode fragment_opcode = QuackapiWsOpcode::CONTINUATION;
	bool fragment_open = false;
	string fragment_payload;
	//! Frames accumulated into the message currently being reassembled.
	idx_t fragment_frames = 0;
	QuackapiWsClose error_code = QuackapiWsClose::PROTOCOL_ERROR;
	string error_reason;
	bool close_sent = false;
	//! Source of the per-frame masking key. Null on the server side, which must
	//! never mask (RFC 6455 §5.1).
	unique_ptr<RandomEngine> masking_rng;
};

//! Register quackapi_ws_accept() and quackapi_ws_connect().
void RegisterQuackapiWebsocketFunctions(ExtensionLoader &loader);

} // namespace duckdb
