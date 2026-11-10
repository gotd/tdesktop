/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Wire and storage tracing for the gotd/tdesktop fork.
//
// Records the plaintext of every encrypted MTProto message (both
// directions), request lifecycle, connection / key / session lifecycle
// and local storage I/O into <dir>/events-NNNN.jsonl + blobs-NNNN.bin.
// Observes only: nothing here changes what is sent, when, or in which
// order. Key material is never written. See trace/trace.cpp.
//
// Hook sites in upstream files are single MTP_WT(...) lines that cost one
// relaxed atomic load and a branch when tracing is off at runtime and
// compile to nothing without TDESKTOP_WIRE_TRACE.

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <source_location>
#include <type_traits>

class QString;
class QByteArray;
template <typename T>
class QList;

namespace MTP {
class AuthKey;
struct Response;
namespace details {
class SerializedRequest;
} // namespace details
} // namespace MTP

namespace Trace {

enum Flag : uint32_t {
	kOn = 1U << 0,
	kStorageBodies = 1U << 6,
	kInline = 1U << 8,
};

extern std::atomic<uint32_t> Flags;

[[nodiscard]] inline bool On(uint32_t flags = kOn) {
	return (Flags.load(std::memory_order_relaxed) & flags) == flags;
}

// Called once from Core::Launcher after the working dir is known.
// Handles "-notrace" / "-trace" and logs where the trace goes.
void Start(const QString &workingDir, const QList<QString> &arguments);

// 64-bit identifier, written as a decimal string.
struct Id {
	uint64_t value = 0;
};
// Signed 64-bit identifier (session ids, like teled), decimal string.
struct SignedId {
	int64_t value = 0;
};

struct KV {
	enum class Kind : unsigned char {
		Int,
		Id,
		SignedId,
		Str,
		QStr,
		Bool,
		Double,
	};
	template <
		typename T,
		typename = std::enable_if_t<
			std::is_integral_v<T> && !std::is_same_v<T, bool>>>
	KV(const char *key, T value) : key(key) {
		if constexpr (std::is_unsigned_v<T> && sizeof(T) >= 8) {
			if (value > uint64_t(INT64_MAX)) {
				kind = Kind::Id;
				u = value;
				return;
			}
		}
		kind = Kind::Int;
		i = int64_t(value);
	}
	KV(const char *key, Id value) : key(key), kind(Kind::Id), u(value.value) {
	}
	KV(const char *key, SignedId value)
	: key(key), kind(Kind::SignedId), i(value.value) {
	}
	KV(const char *key, const char *value)
	: key(key), kind(Kind::Str), s(value) {
	}
	KV(const char *key, const QString &value)
	: key(key), kind(Kind::QStr), q(&value) {
	}
	KV(const char *key, bool value) : key(key), kind(Kind::Bool), b(value) {
	}
	KV(const char *key, double value)
	: key(key), kind(Kind::Double), d(value) {
	}

	const char *key = nullptr;
	Kind kind = Kind::Int;
	union {
		int64_t i;
		uint64_t u;
		double d;
		bool b;
		const char *s;
		const QString *q;
	};
};

enum class Dir : unsigned char {
	Out,
	In,
};

using AuthKeyPtr = std::shared_ptr<MTP::AuthKey>;

// Encrypted-layer plaintext. hdr points at the salt (8 ints of header:
// salt, session, msg_id, seq_no, length), end is the end of the body.
void Message(
	Dir dir,
	int32_t shiftedDcId,
	int protocolDcId,
	const AuthKeyPtr &key,
	const QString &connectionTag,
	const int32_t *hdr,
	const int32_t *end);

// Generic event.
void Ev(const char *kind, int32_t shiftedDcId, std::initializer_list<KV> kv = {});

// Records why the connection is about to restart; the next Restart() on
// this thread reports it.
void Reason(int32_t shiftedDcId, const char *reason, int64_t code = 0);
void Restart(int32_t shiftedDcId, int64_t retryTimeoutMs);
void KeySet(int32_t shiftedDcId, const AuthKeyPtr &key);

void RequestNew(
	int32_t requestId,
	const MTP::details::SerializedRequest &request,
	int32_t shiftedDcId,
	int32_t signedDcId,
	int64_t canWait,
	int32_t afterRequestId,
	bool needsLayer);
void RequestMsg(
	int32_t shiftedDcId,
	int32_t requestId,
	uint64_t was,
	uint64_t now,
	uint32_t seqNo);
void RequestResend(
	int32_t shiftedDcId,
	int32_t requestId,
	uint64_t msgId,
	const char *reason);
void RequestDone(const MTP::Response &response);
void RequestCancel(int32_t requestId, uint64_t msgId);
void RequestCaller(int32_t requestId, const std::source_location &location);

// Thread-local causal context: events emitted synchronously while a
// response / update is being handled carry "cause".
class CauseScope final {
public:
	explicit CauseScope(const MTP::Response &response);
	~CauseScope();

	CauseScope(const CauseScope &) = delete;
	CauseScope &operator=(const CauseScope &) = delete;

private:
	bool _active = false;
	int32_t _req = 0;
	uint64_t _outer = 0;
	uint32_t _type = 0;
	bool _wasSet = false;

};

enum class StorageOp : unsigned char {
	Write, // st.write: plaintext before local encryption
	WriteRaw, // st.write.raw: unencrypted part of a file
	Encrypt, // st.encrypt: PrepareEncrypted outside of a file write
	Commit, // st.commit: WriteManager writes a file to disk
	Remove, // st.remove
	Read, // st.read: file read (ok / missing)
	Decrypt, // st.decrypt: plaintext after local decryption
	DecryptFail, // st.decrypt with "ok":false
};

void Storage(
	StorageOp op,
	const QString &file,
	const QByteArray *plain,
	int64_t size,
	const std::source_location *location);
void StorageRead(
	const QString &file,
	bool ok,
	int32_t version,
	int64_t size,
	const std::source_location &location);

// Which file a following DecryptLocal() belongs to (same thread).
void SetCurrentFile(const QString &file);
[[nodiscard]] const QString &CurrentFile();

// Suppresses st.encrypt / st.write.raw while inside writeEncrypted().
class WriteEncryptedScope final {
public:
	WriteEncryptedScope();
	~WriteEncryptedScope();

	WriteEncryptedScope(const WriteEncryptedScope &) = delete;
	WriteEncryptedScope &operator=(const WriteEncryptedScope &) = delete;

private:
	bool _was = false;

};
[[nodiscard]] bool InWriteEncrypted();

} // namespace Trace

#ifdef TDESKTOP_WIRE_TRACE
#define MTP_WT(call) do { \
	if (::Trace::On()) { \
		::Trace::call; \
	} \
} while (false)
#define MTP_WT_SCOPE(name, response) \
	auto name = std::optional<::Trace::CauseScope>(); \
	if (::Trace::On()) { \
		name.emplace(response); \
	}
// Default trailing parameter capturing the caller, for declarations.
#define MTP_WT_LOC , std::source_location wtLoc = std::source_location::current()
// The same parameter, for out-of-line definitions.
#define MTP_WT_LOC_DEF , std::source_location wtLoc
// Passes the captured location on.
#define MTP_WT_PASS , wtLoc
#else // TDESKTOP_WIRE_TRACE
#define MTP_WT(call) do {} while (false)
#define MTP_WT_SCOPE(name, response)
#define MTP_WT_LOC
#define MTP_WT_LOC_DEF
#define MTP_WT_PASS
#endif // TDESKTOP_WIRE_TRACE
