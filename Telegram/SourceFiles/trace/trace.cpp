/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "trace/trace.h"

#include "trace/trace_ids.h"
#include "trace/trace_redact.h"
#include "trace/trace_ui_core.h"
#include "trace/trace_writer.h"
#include "core/version.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtproto_auth_key.h"
#include "mtproto/mtproto_response.h"
#include "scheme.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <unordered_map>

// Runtime switches (read once at startup):
//   TDESKTOP_TRACE=0|off       disable (default: on in builds configured
//                              with TDESKTOP_TRACE_DEFAULT=ON)
//   TDESKTOP_TRACE=1|on        enable, default directory
//   TDESKTOP_TRACE=<dir>       enable, write runs under <dir>
//   TDESKTOP_TRACE_FLAGS=storage_bodies,inline,noui,ui_raw_ids,ui_verbose
//   TDESKTOP_TRACE_UI_WINDOW=<ms>  "after" stamp window (default 3000)
//   TDESKTOP_TRACE_MAX=<bytes>[K|M|G]  total size cap (default 2G)
//   command line: -notrace / -trace
// Default directory: <working dir>/trace/<UTC timestamp>-<pid>/.

#ifndef TDESKTOP_TRACE_DEFAULT_ON
#define TDESKTOP_TRACE_DEFAULT_ON 0
#endif // TDESKTOP_TRACE_DEFAULT_ON

namespace Trace {
namespace {

using namespace details;

#define TRACE_CHECK_ID(name, value) \
	static_assert(ids::name == uint32_t(mtpc_##name), "Trace id mismatch.");
TRACE_IDS(TRACE_CHECK_ID)
#undef TRACE_CHECK_ID

constexpr auto kDcShift = 10000;
constexpr auto kMaxPendingRequests = size_t(1) << 16;

struct Config {
	std::string directory;
	uint64_t totalBytes = 2ULL * 1024 * 1024 * 1024;
	bool inlineBodies = false;
	double uiAfterMs = 3000.;
};

Config &Cfg() {
	static auto result = Config();
	return result;
}

Writer &W() {
	// Never destroyed: events may come from static destructors at exit.
	static const auto result = new Writer();
	return *result;
}

[[nodiscard]] std::string Lower(const char *value) {
	auto result = std::string(value ? value : "");
	for (auto &ch : result) {
		if (ch >= 'A' && ch <= 'Z') {
			ch = char(ch - 'A' + 'a');
		}
	}
	return result;
}

[[nodiscard]] uint64_t ParseSize(const char *value, uint64_t fallback) {
	if (!value || !*value) {
		return fallback;
	}
	char *end = nullptr;
	const auto number = std::strtoull(value, &end, 10);
	if (!number) {
		return fallback;
	}
	switch (end ? *end : 0) {
	case 'k': case 'K': return number << 10;
	case 'm': case 'M': return number << 20;
	case 'g': case 'G': return number << 30;
	}
	return number;
}

uint32_t InitialFlags() {
	auto result = TDESKTOP_TRACE_DEFAULT_ON ? uint32_t(kOn) : uint32_t(0);
	result |= kUi;
	auto &config = Cfg();
	const auto env = std::getenv("TDESKTOP_TRACE");
	if (env && *env) {
		const auto value = Lower(env);
		if (value == "0" || value == "off" || value == "false" || value == "no") {
			result = 0;
		} else {
			result |= kOn;
			if (value != "1" && value != "on" && value != "true" && value != "yes") {
				config.directory = env;
			}
		}
	}
	const auto flags = Lower(std::getenv("TDESKTOP_TRACE_FLAGS"));
	auto from = size_t();
	while (from <= flags.size()) {
		const auto till = std::min(flags.find(',', from), flags.size());
		const auto flag = flags.substr(from, till - from);
		if (flag == "storage_bodies") {
			result |= kStorageBodies;
		} else if (flag == "inline") {
			config.inlineBodies = true;
		} else if (flag == "noui") {
			result &= ~uint32_t(kUi);
		} else if (flag == "ui_raw_ids") {
			result |= kUiRawIds;
		} else if (flag == "ui_verbose") {
			result |= kUiVerbose;
		}
		from = till + 1;
	}
	config.totalBytes = ParseSize(
		std::getenv("TDESKTOP_TRACE_MAX"),
		config.totalBytes);
	if (const auto window = std::getenv("TDESKTOP_TRACE_UI_WINDOW")) {
		const auto ms = std::strtoull(window, nullptr, 10);
		if (ms > 0 && ms <= 600000) {
			config.uiAfterMs = double(ms);
		}
	}
	return result;
}

const auto kMainThread = std::this_thread::get_id();

[[nodiscard]] const std::string &ThreadName() {
	static auto counter = std::atomic<int>(0);
	thread_local const auto result = [] {
		if (std::this_thread::get_id() == kMainThread) {
			return std::string("main");
		}
		return "t" + std::to_string(++counter);
	}();
	return result;
}

struct Cause {
	bool set = false;
	int32_t req = 0;
	uint64_t outer = 0;
	uint32_t type = 0;
};
thread_local auto CurrentCause = Cause();
thread_local auto LastReason = static_cast<const char*>(nullptr);
thread_local auto LastReasonCode = int64_t(0);
thread_local auto CurrentFileName = QString();
thread_local auto InsideWriteEncrypted = false;

QString WorkingDir;
QString RunDir;

std::mutex RedactionsMutex;
ResponseRedactions Redactions;

struct Pending {
	double t = 0.;
	uint32_t type = 0;
};
std::mutex PendingMutex;
std::unordered_map<int32_t, Pending> PendingRequests;

class Fields final {
public:
	explicit Fields(const char *event) : _stampable(Stampable(event)) {
		_data.reserve(256);
		_data += ",\"ev\":\"";
		_data += event;
		_data += '"';
	}

	Fields &key(const char *name) {
		_data += ",\"";
		_data += name;
		_data += "\":";
		return *this;
	}
	Fields &num(const char *name, int64_t value) {
		key(name);
		_data += std::to_string(value);
		return *this;
	}
	Fields &id(const char *name, uint64_t value) {
		key(name);
		_data += '"';
		_data += std::to_string(value);
		_data += '"';
		return *this;
	}
	Fields &sid(const char *name, int64_t value) {
		key(name);
		_data += '"';
		_data += std::to_string(value);
		_data += '"';
		return *this;
	}
	Fields &str(const char *name, std::string_view value) {
		key(name);
		AppendJsonString(_data, value);
		return *this;
	}
	Fields &str(const char *name, const QString &value) {
		const auto utf8 = value.toUtf8();
		return str(name, std::string_view(utf8.constData(), utf8.size()));
	}
	Fields &boolean(const char *name, bool value) {
		key(name);
		_data += value ? "true" : "false";
		return *this;
	}
	Fields &dbl(const char *name, double value) {
		key(name);
		details::AppendFixed(_data, value, 3);
		return *this;
	}
	Fields &type(const char *name, uint32_t value) {
		char buffer[16];
		std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
		return str(name, std::string_view(buffer));
	}
	Fields &raw(std::string_view value) {
		_data += value;
		return *this;
	}
	Fields &kv(const KV &value) {
		switch (value.kind) {
		case KV::Kind::Int: return num(value.key, value.i);
		case KV::Kind::Id: return id(value.key, value.u);
		case KV::Kind::SignedId: return sid(value.key, value.i);
		case KV::Kind::Str:
			return str(value.key, std::string_view(value.s ? value.s : ""));
		case KV::Kind::QStr: return str(value.key, *value.q);
		case KV::Kind::Bool: return boolean(value.key, value.b);
		case KV::Kind::Double: return dbl(value.key, value.d);
		}
		return *this;
	}
	Fields &dc(int32_t shiftedDcId) {
		num("sdc", shiftedDcId);
		const auto absolute = (shiftedDcId < 0) ? -shiftedDcId : shiftedDcId;
		num("dc", absolute % kDcShift);
		num("shift", absolute / kDcShift);
		return *this;
	}
	Fields &authKey(const AuthKeyPtr &key) {
		if (!key) {
			return id("key", 0);
		}
		const auto keyId = key->keyId();
		id("key", keyId);
		str("key_hex", Hex(&keyId, sizeof(keyId))); // little-endian hosts
		const auto kind = (key->type() == MTP::AuthKey::Type::Temporary
			|| key->expiresAt() != 0)
			? "temp"
			: (key->type() == MTP::AuthKey::Type::Local)
			? "local"
			: "perm";
		return str("key_kind", std::string_view(kind));
	}

	[[nodiscard]] std::string take() {
		return std::move(_data);
	}
	[[nodiscard]] bool stampable() const {
		return _stampable;
	}
	// Main thread, no cause: the UI action stamp (P14).
	void stamp() {
		if (On(kOn | kUi)
			&& !CurrentCause.set
			&& std::this_thread::get_id() == kMainThread) {
			UiCore::MainClock().append(_data, Writer::Now());
		}
	}

private:
	// Requests and storage get the UI stamp (ui.* records have their own).
	[[nodiscard]] static bool Stampable(const char *event) {
		return !std::strcmp(event, "req.new")
			|| !std::strcmp(event, "req.caller")
			|| !std::strcmp(event, "req.cancel")
			|| !std::strncmp(event, "st.", 3);
	}

	std::string _data;
	bool _stampable = false;

};

void Emit(Fields &fields, std::string *blob = nullptr) {
	if (fields.stampable()) {
		fields.stamp();
	}
	if (CurrentCause.set) {
		fields.key("cause");
		fields.raw("{\"req\":");
		fields.raw(std::to_string(CurrentCause.req));
		fields.raw(",\"outer\":\"");
		fields.raw(std::to_string(CurrentCause.outer));
		char buffer[48];
		std::snprintf(buffer, sizeof(buffer), "\",\"type\":\"0x%08x\"}", CurrentCause.type);
		fields.raw(buffer);
	}
	W().push(ThreadName(), fields.take(), blob);
}

[[nodiscard]] std::string TypeHex(uint32_t value) {
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "\"0x%08x\"", value);
	return buffer;
}

void AppendSummary(Fields &fields, const Summary &summary, Dir dir) {
	fields.type("type", summary.type);
	if (dir == Dir::Out && summary.method && summary.method != summary.type) {
		fields.type("method", summary.method);
	}
	if (!summary.wrap.empty()) {
		fields.key("wrap").raw("[");
		for (auto i = size_t(); i != summary.wrap.size(); ++i) {
			fields.raw(i ? "," : "").raw(TypeHex(summary.wrap[i]));
		}
		fields.raw("]");
	}
	if (summary.reqMsgId) {
		fields.id("req_msg_id", summary.reqMsgId);
		fields.type("result", summary.resultType);
	}
	if (!summary.inner.empty()) {
		fields.key("inner").raw("[");
		auto first = true;
		for (const auto &inner : summary.inner) {
			fields.raw(first ? "{" : ",{");
			first = false;
			fields.raw("\"msg_id\":\"").raw(std::to_string(inner.msgId));
			fields.raw("\",\"seqno\":").raw(std::to_string(inner.seqNo));
			fields.raw(",\"len\":").raw(std::to_string(inner.length));
			fields.raw(",\"type\":").raw(TypeHex(inner.type));
			if (inner.method && inner.method != inner.type) {
				fields.raw(",\"method\":").raw(TypeHex(inner.method));
			}
			if (inner.reqMsgId) {
				fields.raw(",\"req_msg_id\":\"");
				fields.raw(std::to_string(inner.reqMsgId));
				fields.raw("\",\"result\":").raw(TypeHex(inner.resultType));
			}
			fields.raw("}");
		}
		fields.raw("]");
	}
	if (!summary.redacted.empty()) {
		fields.key("redacted").raw("[");
		for (auto i = size_t(); i != summary.redacted.size(); ++i) {
			auto quoted = std::string();
			AppendJsonString(quoted, summary.redacted[i]);
			fields.raw(i ? "," : "").raw(quoted);
		}
		fields.raw("]");
	}
	if (summary.pii) {
		fields.boolean("pii", true);
	}
	if (summary.partial) {
		fields.boolean("partial", true);
	}
}

[[nodiscard]] QString RelativeFile(const QString &file) {
	return (!WorkingDir.isEmpty() && file.startsWith(WorkingDir))
		? file.mid(WorkingDir.size())
		: file;
}

[[nodiscard]] uint32_t ReadBE32(const std::string &data, size_t at) {
	return (uint32_t(uint8_t(data[at])) << 24)
		| (uint32_t(uint8_t(data[at + 1])) << 16)
		| (uint32_t(uint8_t(data[at + 2])) << 8)
		| uint32_t(uint8_t(data[at + 3]));
}

// dbiMtpAuthorization: QByteArray(wide tag u64, user u64, main dc i32,
// 2 x [count i32, count x (dc i32, key 256 bytes)]), all big-endian.
// Zeroes the keys, keeps the ids. On any doubt zeroes everything.
void RedactMtpAuthorization(std::string &data, std::vector<std::string> &redacted) {
	constexpr auto kKeySize = size_t(256);
	auto ok = (data.size() >= 8);
	auto at = size_t(8) + 8 + 8 + 4;
	for (auto list = 0; ok && list != 2; ++list) {
		if (at + 4 > data.size()) {
			ok = false;
			break;
		}
		const auto count = ReadBE32(data, at);
		at += 4;
		if (count > 1024) {
			ok = false;
			break;
		}
		for (auto i = uint32_t(); i != count; ++i) {
			if (at + 4 + kKeySize > data.size()) {
				ok = false;
				break;
			}
			std::memset(data.data() + at + 4, 0, kKeySize);
			at += 4 + kKeySize;
		}
	}
	if (!ok) {
		std::memset(data.data() + 4, 0, data.size() > 4 ? data.size() - 4 : 0);
	}
	redacted.push_back("mtp_authorization.keys");
}

void RedactStorage(
		std::string &data,
		const QString &file,
		const char *function,
		std::vector<std::string> &redacted) {
	const auto name = file.mid(file.lastIndexOf('/') + 1);
	const auto localKey = (function && std::strstr(function, "WrapLocalKey"))
		|| name.startsWith(u"key_"_q)
		|| name.startsWith(u"wallet_keyring_"_q);
	if (localKey) {
		std::memset(data.data(), 0, data.size());
		redacted.push_back("key_material");
	} else if (data.size() >= 4 && ReadBE32(data, 0) == 0x4B) {
		RedactMtpAuthorization(data, redacted);
	}
}

[[nodiscard]] const char *StorageEvent(StorageOp op) {
	switch (op) {
	case StorageOp::Write: return "st.write";
	case StorageOp::WriteRaw: return "st.write.raw";
	case StorageOp::Encrypt: return "st.encrypt";
	case StorageOp::Commit: return "st.commit";
	case StorageOp::Remove: return "st.remove";
	case StorageOp::Read: return "st.read";
	case StorageOp::Decrypt: return "st.decrypt";
	case StorageOp::DecryptFail: return "st.decrypt";
	}
	return "st.unknown";
}

[[nodiscard]] std::string RandomUuid() {
	auto device = std::random_device();
	uint8_t bytes[16];
	for (auto i = 0; i != 16; i += 4) {
		const auto value = uint32_t(device());
		std::memcpy(bytes + i, &value, 4);
	}
	bytes[6] = (bytes[6] & 0x0F) | 0x40;
	bytes[8] = (bytes[8] & 0x3F) | 0x80;
	const auto hex = Hex(bytes, 16);
	return hex.substr(0, 8) + '-' + hex.substr(8, 4) + '-'
		+ hex.substr(12, 4) + '-' + hex.substr(16, 4) + '-'
		+ hex.substr(20);
}

void AppendCaller(Fields &fields, const std::source_location &location) {
	auto file = std::string_view(location.file_name());
	if (const auto i = file.rfind("SourceFiles/"); i != std::string_view::npos) {
		file = file.substr(i + 12);
	}
	fields.str("file", file);
	fields.num("line", location.line());
	fields.str("function", std::string_view(location.function_name()));
	if (const auto slash = file.find('/'); slash != std::string_view::npos) {
		fields.str("subsystem", file.substr(0, slash));
	}
}

} // namespace

std::atomic<uint32_t> Flags = InitialFlags();

void Start(const QString &workingDir, const QList<QString> &arguments) {
	static auto started = false;
	if (started) {
		return;
	}
	started = true;

	if (arguments.contains(u"-notrace"_q)) {
		Flags = 0;
	} else if (arguments.contains(u"-trace"_q)) {
		Flags |= kOn;
	}
	if (!On()) {
		W().discard();
		LOG(("Trace: off."));
		return;
	}
	WorkingDir = workingDir;

	const auto &config = Cfg();
	auto base = config.directory.empty()
		? (workingDir + u"trace"_q)
		: QFile::decodeName(config.directory.c_str());
	while (base.endsWith('/')) {
		base.chop(1);
	}
	const auto now = QDateTime::currentDateTimeUtc();
	const auto pid = QCoreApplication::applicationPid();
	const auto directory = base
		+ '/'
		+ now.toString(u"yyyyMMdd'T'HHmmss'Z'"_q)
		+ '-'
		+ QString::number(pid);

	auto header = Fields("hdr");
	header.str("run", RandomUuid());
	header.str("app", std::string_view(AppVersionStr));
	header.num("layer", MTP::details::kCurrentLayer);
#ifdef TDESKTOP_TRACE_API_SHA256
	header.str("api_sha256", std::string_view(TDESKTOP_TRACE_API_SHA256));
#endif // TDESKTOP_TRACE_API_SHA256
#ifdef TDESKTOP_TRACE_MTPROTO_SHA256
	header.str("mtproto_sha256", std::string_view(TDESKTOP_TRACE_MTPROTO_SHA256));
#endif // TDESKTOP_TRACE_MTPROTO_SHA256
	header.str("wall_utc", now.toString(Qt::ISODateWithMs));
	header.dbl("mono_t", Writer::Now());
	header.num("pid", pid);
	header.key("flags").raw(On(kStorageBodies)
		? "[\"storage_bodies\"]"
		: "[]");
	if (On(kUi)) {
		header.key("ui").raw(On(kUiRawIds)
			? "{\"v\":1,\"ids\":\"raw\",\"raw_ids\":true"
			: "{\"v\":1,\"ids\":\"hmac64\",\"raw_ids\":false");
		header.raw(On(kUiVerbose) ? ",\"verbose\":true}" : "}");
	}
	// Drop the leading ,"ev":"hdr": the writer writes its own.
	auto headerFields = header.take();
	headerFields.erase(0, std::strlen(",\"ev\":\"hdr\""));

	auto options = WriterOptions();
#ifdef Q_OS_WIN
	options.directory = std::filesystem::path(directory.toStdWString());
#else // Q_OS_WIN
	options.directory = std::filesystem::path(
		QFile::encodeName(directory).toStdString());
#endif // Q_OS_WIN
	options.header = std::move(headerFields);
	options.totalBytes = config.totalBytes;
	options.inlineMax = config.inlineBodies ? 1024 : 0;

	const auto runPath = options.directory;
	auto error = std::string();
	if (!W().start(std::move(options), error)) {
		Flags = 0;
		W().discard();
		LOG(("Trace Error: could not write to %1: %2"
			).arg(directory, QString::fromStdString(error)));
		return;
	}
	std::atexit([] { W().stop(); });
	RunDir = directory;
	if (On(kUi)) {
		auto clock = UiCore::StampOptions();
		clock.afterMs = config.uiAfterMs;
		UiCore::MainClock().setOptions(clock);
		UiCore::Ids().setRaw(On(kUiRawIds));
		if (!On(kUiRawIds)) {
			unsigned char key[UiCore::kIdKeySize];
			UiCore::GenerateKey(key, sizeof(key));
			UiCore::Ids().setKey(key, sizeof(key));
			if (!UiCore::WriteKeyFile(runPath, key, sizeof(key), error)) {
				LOG(("Trace Error: %1, UI ids can not be joined offline."
					).arg(QString::fromStdString(error)));
			}
			std::memset(key, 0, sizeof(key));
		}
	}
	LOG(("Trace: writing wire and storage trace to %1 "
		"(disable with TDESKTOP_TRACE=0 or -notrace)").arg(directory));
}

void Message(
		Dir dir,
		int32_t shiftedDcId,
		int protocolDcId,
		const AuthKeyPtr &key,
		const QString &connectionTag,
		const int32_t *hdr,
		const int32_t *end) {
	if (!hdr || end < hdr + 8) {
		return;
	}
	auto salt = uint64_t();
	auto session = uint64_t();
	auto msgId = uint64_t();
	std::memcpy(&salt, hdr + 0, 8);
	std::memcpy(&session, hdr + 2, 8);
	std::memcpy(&msgId, hdr + 4, 8);
	const auto seqNo = uint32_t(hdr[6]);
	const auto length = uint32_t(hdr[7]);
	const auto available = size_t(end - (hdr + 8)) * sizeof(int32_t);
	const auto size = std::min(size_t(length), available);

	auto body = std::string(reinterpret_cast<const char*>(hdr + 8), size);
	auto summary = Summary();
	{
		auto lock = std::unique_lock(RedactionsMutex);
		if (dir == Dir::Out) {
			ProcessOutgoing(body, msgId, summary, Redactions);
		} else {
			ProcessIncoming(body, summary, Redactions);
		}
	}

	auto fields = Fields("msg");
	fields.str("dir", std::string_view(dir == Dir::Out ? "out" : "in"));
	fields.dc(shiftedDcId);
	fields.num("pdc", protocolDcId);
	fields.str("conn", connectionTag);
	fields.authKey(key);
	fields.sid("session", int64_t(session));
	fields.id("salt", salt);
	fields.id("msg_id", msgId);
	fields.num("seqno", seqNo);
	fields.num("len", length);
	if (size != length) {
		fields.boolean("truncated", true);
	}
	AppendSummary(fields, summary, dir);
	Emit(fields, &body);
}

void Ev(const char *kind, int32_t shiftedDcId, std::initializer_list<KV> kv) {
	auto fields = Fields(kind);
	fields.dc(shiftedDcId);
	for (const auto &value : kv) {
		fields.kv(value);
	}
	Emit(fields);
}

void Reason(int32_t shiftedDcId, const char *reason, int64_t code) {
	LastReason = reason;
	LastReasonCode = code;
	auto fields = Fields("conn.reason");
	fields.dc(shiftedDcId);
	fields.str("reason", std::string_view(reason));
	if (code) {
		fields.num("code", code);
	}
	Emit(fields);
}

void Restart(int32_t shiftedDcId, int64_t retryTimeoutMs) {
	auto fields = Fields("conn.restart");
	fields.dc(shiftedDcId);
	fields.str("reason", std::string_view(LastReason ? LastReason : "unknown"));
	if (LastReasonCode) {
		fields.num("code", LastReasonCode);
	}
	fields.num("retry_ms", retryTimeoutMs);
	LastReason = nullptr;
	LastReasonCode = 0;
	Emit(fields);
}

void KeySet(int32_t shiftedDcId, const AuthKeyPtr &key) {
	auto fields = Fields("key.set");
	fields.dc(shiftedDcId);
	fields.authKey(key);
	if (key) {
		fields.num("key_dc", key->dcId());
		fields.num("expires_at", key->expiresAt());
	}
	Emit(fields);
}

void RequestNew(
		int32_t requestId,
		const MTP::details::SerializedRequest &request,
		int32_t shiftedDcId,
		int32_t signedDcId,
		int64_t canWait,
		int32_t afterRequestId,
		bool needsLayer) {
	constexpr auto kBody = MTP::details::SerializedRequest::kMessageBodyPosition;
	const auto size = request ? request->size() : 0;
	const auto type = (size > kBody) ? uint32_t((*request)[kBody]) : 0U;
	const auto now = Writer::Now();
	{
		auto lock = std::unique_lock(PendingMutex);
		if (PendingRequests.size() >= kMaxPendingRequests) {
			PendingRequests.clear();
		}
		PendingRequests[requestId] = { now, type };
	}
	auto fields = Fields("req.new");
	fields.num("req", requestId);
	fields.type("type", type);
	fields.dc(shiftedDcId);
	fields.boolean("to_main", signedDcId < 0);
	fields.num("size", (size > kBody) ? (size - kBody) * 4 : 0);
	if (canWait) {
		fields.num("can_wait", canWait);
	}
	if (afterRequestId) {
		fields.num("after", afterRequestId);
	}
	fields.boolean("needs_layer", needsLayer);
	Emit(fields);
}

void RequestMsg(
		int32_t shiftedDcId,
		int32_t requestId,
		uint64_t was,
		uint64_t now,
		uint32_t seqNo) {
	auto fields = Fields("req.msg");
	fields.dc(shiftedDcId);
	fields.num("req", requestId);
	fields.id("was", was);
	fields.id("msg_id", now);
	fields.num("seqno", seqNo);
	Emit(fields);
}

void RequestResend(
		int32_t shiftedDcId,
		int32_t requestId,
		uint64_t msgId,
		const char *reason) {
	auto fields = Fields("req.resend");
	fields.dc(shiftedDcId);
	fields.num("req", requestId);
	fields.id("msg_id", msgId);
	fields.str("reason", std::string_view(reason));
	Emit(fields);
}

void RequestDone(const MTP::Response &response) {
	const auto &reply = response.reply;
	const auto type = reply.isEmpty() ? 0U : uint32_t(reply[0]);
	auto pending = std::optional<Pending>();
	{
		auto lock = std::unique_lock(PendingMutex);
		const auto i = PendingRequests.find(response.requestId);
		if (i != end(PendingRequests)) {
			pending = i->second;
			PendingRequests.erase(i);
		}
	}
	const auto failed = (type == mtpc_rpc_error);
	auto fields = Fields(failed ? "req.fail" : "req.done");
	fields.num("req", response.requestId);
	if (pending) {
		fields.type("method", pending->type);
		fields.dbl("lat_ms", (Writer::Now() - pending->t) * 1000.);
	}
	fields.type("type", type);
	fields.id("outer", response.outerMsgId);
	fields.num("len", int64_t(reply.size()) * 4);
	if (failed && reply.size() >= 3) {
		fields.num("code", reply[1]);
		// error_message:string
		const auto bytes = reinterpret_cast<const uint8_t*>(reply.constData() + 2);
		const auto available = size_t(reply.size() - 2) * 4;
		auto length = size_t(bytes[0]);
		auto header = size_t(1);
		if (length == 254 && available >= 4) {
			length = size_t(bytes[1]) | (size_t(bytes[2]) << 8) | (size_t(bytes[3]) << 16);
			header = 4;
		}
		if (header + length <= available) {
			fields.str("error", std::string_view(
				reinterpret_cast<const char*>(bytes + header),
				length));
		}
	}
	Emit(fields);
}

void RequestCancel(int32_t requestId, uint64_t msgId) {
	{
		auto lock = std::unique_lock(PendingMutex);
		PendingRequests.erase(requestId);
	}
	auto fields = Fields("req.cancel");
	fields.num("req", requestId);
	if (msgId) {
		fields.id("msg_id", msgId);
	}
	Emit(fields);
}

void RequestCaller(int32_t requestId, const std::source_location &location) {
	auto fields = Fields("req.caller");
	fields.num("req", requestId);
	AppendCaller(fields, location);
	Emit(fields);
}

CauseScope::CauseScope(const MTP::Response &response)
: _active(true)
, _req(CurrentCause.req)
, _outer(CurrentCause.outer)
, _type(CurrentCause.type)
, _wasSet(CurrentCause.set) {
	CurrentCause.set = true;
	CurrentCause.req = response.requestId;
	CurrentCause.outer = response.outerMsgId;
	CurrentCause.type = response.reply.isEmpty()
		? 0U
		: uint32_t(response.reply[0]);
}

CauseScope::~CauseScope() {
	if (_active) {
		CurrentCause.set = _wasSet;
		CurrentCause.req = _req;
		CurrentCause.outer = _outer;
		CurrentCause.type = _type;
	}
}

void Storage(
		StorageOp op,
		const QString &file,
		const QByteArray *plain,
		int64_t size,
		const std::source_location *location) {
	if ((op == StorageOp::Encrypt || op == StorageOp::WriteRaw)
		&& InsideWriteEncrypted) {
		return;
	}
	const auto relative = RelativeFile(file);
	auto fields = Fields(StorageEvent(op));
	fields.str("file", relative);
	if (op == StorageOp::DecryptFail) {
		fields.boolean("ok", false);
		fields.str("why", std::string_view("bad_key"));
	}
	// Plaintexts of encrypted parts start with a 4-byte length.
	const auto skip = (op == StorageOp::Write
		|| op == StorageOp::Encrypt
		|| op == StorageOp::Decrypt) ? 4 : 0;
	fields.num("len", std::max(size - skip, int64_t(0)));
	if (location) {
		fields.str("caller", std::string_view(location->function_name()));
	}
	auto blob = std::string();
	auto withBlob = false;
	if (plain && On(kStorageBodies) && plain->size() >= skip) {
		blob.assign(plain->constData() + skip, plain->size() - skip);
		auto redacted = std::vector<std::string>();
		RedactStorage(
			blob,
			relative,
			location ? location->function_name() : nullptr,
			redacted);
		if (!redacted.empty()) {
			fields.key("redacted").raw("[");
			for (auto i = size_t(); i != redacted.size(); ++i) {
				auto quoted = std::string();
				AppendJsonString(quoted, redacted[i]);
				fields.raw(i ? "," : "").raw(quoted);
			}
			fields.raw("]");
		}
		withBlob = true;
	}
	Emit(fields, withBlob ? &blob : nullptr);
}

void StorageRead(
		const QString &file,
		bool ok,
		int32_t version,
		int64_t size,
		const std::source_location &location) {
	auto fields = Fields("st.read");
	fields.str("file", RelativeFile(file));
	fields.boolean("ok", ok);
	if (ok) {
		fields.num("version", version);
		fields.num("len", size);
	}
	fields.str("caller", std::string_view(location.function_name()));
	Emit(fields);
}

void SetCurrentFile(const QString &file) {
	CurrentFileName = file;
}

const QString &CurrentFile() {
	return CurrentFileName;
}

WriteEncryptedScope::WriteEncryptedScope()
: _was(InsideWriteEncrypted) {
	InsideWriteEncrypted = true;
}

WriteEncryptedScope::~WriteEncryptedScope() {
	InsideWriteEncrypted = _was;
}

bool InWriteEncrypted() {
	return InsideWriteEncrypted;
}

void UiRecord(const char *kind, const std::string &fields, UiStampMode mode) {
	auto record = Fields(kind);
	const auto now = Writer::Now();
	auto &clock = UiCore::MainClock();
	auto stamp = (mode != UiStampMode::None && !CurrentCause.set)
		? clock.compute(now)
		: std::nullopt;
	if (mode == UiStampMode::StampOrApp && !stamp) {
		const auto a = UiCore::NextAction();
		record.num("a", a);
		record.str("src", std::string_view("app"));
		if (!CurrentCause.set) {
			clock.mark(a, now);
		}
	}
	record.raw(fields);
	if (stamp) {
		auto appended = std::string();
		UiCore::AppendStamp(appended, *stamp);
		record.raw(appended);
	}
	Emit(record);
}

const QString &RunDirectory() {
	return RunDir;
}

} // namespace Trace
