/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "trace/trace_redact.h"

#include "trace/trace_ids.h"

#include <algorithm>
#include <cstring>

// Self-contained (no Qt / scheme.h) so it can be unit-tested on its own;
// trace.cpp static_asserts the ids against the generated scheme.

namespace Trace::details {
namespace {

constexpr auto kMaxDepth = 16;
constexpr auto kMaxRemembered = size_t(4096);

// SRP checks and email codes only appear in small requests; skipping big
// bodies (file parts) avoids masking random bytes that look like an id.
constexpr auto kMaxNestedScan = size_t(64 * 1024);

class Cursor {
public:
	Cursor(std::string &data, size_t from, size_t till)
	: _data(data)
	, _pos(from)
	, _end(std::min(till, data.size())) {
	}

	[[nodiscard]] size_t pos() const {
		return _pos;
	}
	[[nodiscard]] size_t end() const {
		return _end;
	}
	[[nodiscard]] bool u32(uint32_t &value) {
		if (_pos > _end || _end - _pos < 4) {
			return false;
		}
		std::memcpy(&value, _data.data() + _pos, 4);
		_pos += 4;
		return true;
	}
	[[nodiscard]] bool u64(uint64_t &value) {
		if (_pos > _end || _end - _pos < 8) {
			return false;
		}
		std::memcpy(&value, _data.data() + _pos, 8);
		_pos += 8;
		return true;
	}
	[[nodiscard]] bool skip(size_t bytes) {
		if (_pos > _end || _end - _pos < bytes) {
			return false;
		}
		_pos += bytes;
		return true;
	}
	// TL string / bytes. Returns content offset and length.
	[[nodiscard]] bool bytes(size_t &offset, size_t &length) {
		if (_pos >= _end) {
			return false;
		}
		const auto first = uint8_t(_data[_pos]);
		auto header = size_t(1);
		if (first < 254) {
			length = first;
		} else if (first == 254) {
			if (_end - _pos < 4) {
				return false;
			}
			length = size_t(uint8_t(_data[_pos + 1]))
				| (size_t(uint8_t(_data[_pos + 2])) << 8)
				| (size_t(uint8_t(_data[_pos + 3])) << 16);
			header = 4;
		} else {
			return false;
		}
		auto full = header + length;
		full = (full + 3) & ~size_t(3);
		if (_end - _pos < full) {
			return false;
		}
		offset = _pos + header;
		_pos += full;
		return true;
	}
	[[nodiscard]] bool maskBytes(char with) {
		auto offset = size_t();
		auto length = size_t();
		if (!bytes(offset, length)) {
			return false;
		}
		std::memset(_data.data() + offset, with, length);
		return true;
	}
	void put32(size_t at, uint32_t value) {
		if (at + 4 <= _data.size()) {
			std::memcpy(_data.data() + at, &value, 4);
		}
	}
	void zero(size_t from, size_t till) {
		if (from < till && till <= _data.size()) {
			std::memset(_data.data() + from, 0, till - from);
		}
	}

private:
	std::string &_data;
	size_t _pos = 0;
	size_t _end = 0;

};

// Overwrites [at + 4, till) with [marker][orig id][zeros], keeping the
// constructor id at `at`.
void RedactAfterId(std::string &data, size_t at, size_t till, uint32_t id) {
	auto cursor = Cursor(data, at, till);
	cursor.zero(at + 4, std::min(till, data.size()));
	if (at + 8 <= till) {
		cursor.put32(at + 4, kRedactedMarker);
	}
	if (at + 12 <= till) {
		cursor.put32(at + 8, id);
	}
}

// Generic in-place masking of nested credential constructors anywhere in
// an outgoing body (4-byte aligned scan): SRP checks and email codes.
void MaskNested(std::string &data, size_t from, size_t till, Summary &summary) {
	till = std::min(till, data.size());
	for (auto at = from; at + 4 <= till; at += 4) {
		auto id = uint32_t();
		std::memcpy(&id, data.data() + at, 4);
		if (id == ids::inputCheckPasswordSRP) {
			auto cursor = Cursor(data, at + 4, till);
			auto srpId = uint64_t();
			if (cursor.u64(srpId)
				&& cursor.maskBytes(0)
				&& cursor.maskBytes(0)) {
				summary.redacted.push_back("inputCheckPasswordSRP");
				at = cursor.pos() - 4;
			}
		} else if (id == ids::emailVerificationCode
			|| id == ids::emailVerificationGoogle
			|| id == ids::emailVerificationApple) {
			auto cursor = Cursor(data, at + 4, till);
			if (cursor.maskBytes('*')) {
				summary.redacted.push_back("emailVerification");
				at = cursor.pos() - 4;
			}
		}
	}
}

[[nodiscard]] bool SkipJsonValue(Cursor &cursor, int depth);

[[nodiscard]] bool SkipJsonObject(Cursor &cursor, int depth) {
	auto vectorId = uint32_t();
	auto count = uint32_t();
	if (!cursor.u32(vectorId) || vectorId != ids::vector || !cursor.u32(count)) {
		return false;
	}
	for (auto i = uint32_t(); i != count; ++i) {
		auto id = uint32_t();
		auto keyOffset = size_t();
		auto keyLength = size_t();
		if (!cursor.u32(id)
			|| id != ids::jsonObjectValue
			|| !cursor.bytes(keyOffset, keyLength)) {
			return false;
		}
		if (!SkipJsonValue(cursor, depth + 1)) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool SkipJsonValue(Cursor &cursor, int depth) {
	if (depth > kMaxDepth) {
		return false;
	}
	auto id = uint32_t();
	if (!cursor.u32(id)) {
		return false;
	}
	switch (id) {
	case ids::jsonNull: return true;
	case ids::jsonBool: return cursor.skip(4);
	case ids::jsonNumber: return cursor.skip(8);
	case ids::jsonString: {
		auto offset = size_t();
		auto length = size_t();
		return cursor.bytes(offset, length);
	}
	case ids::jsonArray: {
		auto vectorId = uint32_t();
		auto count = uint32_t();
		if (!cursor.u32(vectorId) || vectorId != ids::vector || !cursor.u32(count)) {
			return false;
		}
		for (auto i = uint32_t(); i != count; ++i) {
			if (!SkipJsonValue(cursor, depth + 1)) {
				return false;
			}
		}
		return true;
	}
	case ids::jsonObject: return SkipJsonObject(cursor, depth);
	}
	return false;
}

// initConnection params: zero the jsonNumber value of "tz_offset", which
// discloses the machine's UTC offset. Lengths are kept.
[[nodiscard]] bool MaskInitParams(
		std::string &data,
		Cursor &cursor,
		Summary &summary) {
	const auto start = cursor.pos();
	if (!SkipJsonValue(cursor, 0)) {
		return false;
	}
	const auto till = cursor.pos();
	// Walk again, this time looking for the key.
	auto walk = Cursor(data, start, till);
	auto id = uint32_t();
	auto vectorId = uint32_t();
	auto count = uint32_t();
	if (!walk.u32(id) || id != ids::jsonObject) {
		return true;
	}
	if (!walk.u32(vectorId) || !walk.u32(count)) {
		return true;
	}
	for (auto i = uint32_t(); i != count; ++i) {
		auto valueId = uint32_t();
		auto keyOffset = size_t();
		auto keyLength = size_t();
		if (!walk.u32(valueId) || !walk.bytes(keyOffset, keyLength)) {
			return true;
		}
		const auto key = std::string_view(data.data() + keyOffset, keyLength);
		const auto valueAt = walk.pos();
		if (!SkipJsonValue(walk, 1)) {
			return true;
		}
		if (key == "tz_offset") {
			auto valueType = uint32_t();
			std::memcpy(&valueType, data.data() + valueAt, 4);
			if (valueType == ids::jsonNumber) {
				walk.zero(valueAt + 4, valueAt + 12);
				summary.redacted.push_back("initConnection.params.tz_offset");
			}
		}
	}
	return true;
}

struct OutState {
	uint64_t msgId = 0;
	Summary *summary = nullptr;
	ResponseRedactions *redactions = nullptr;
	std::vector<uint32_t> *wrap = nullptr;
	uint32_t *method = nullptr;
};

void ProcessQuery(std::string &data, size_t at, size_t till, OutState &state, int depth);

void ProcessOutMessage(
		std::string &data,
		size_t at,
		size_t till,
		OutState &state,
		int depth) {
	if (depth > kMaxDepth) {
		state.summary->partial = true;
		return;
	}
	auto cursor = Cursor(data, at, till);
	auto id = uint32_t();
	if (!cursor.u32(id)) {
		return;
	}
	if (id == ids::msg_container) {
		auto count = uint32_t();
		if (!cursor.u32(count)) {
			state.summary->partial = true;
			return;
		}
		for (auto i = uint32_t(); i != count; ++i) {
			auto info = InnerInfo();
			if (!cursor.u64(info.msgId)
				|| !cursor.u32(info.seqNo)
				|| !cursor.u32(info.length)) {
				state.summary->partial = true;
				return;
			}
			const auto bodyAt = cursor.pos();
			if (!cursor.skip(info.length)) {
				state.summary->partial = true;
				return;
			}
			if (info.length >= 4) {
				std::memcpy(&info.type, data.data() + bodyAt, 4);
			}
			auto inner = OutState{
				.msgId = info.msgId,
				.summary = state.summary,
				.redactions = state.redactions,
				.wrap = nullptr,
				.method = &info.method,
			};
			ProcessQuery(data, bodyAt, bodyAt + info.length, inner, depth + 1);
			state.summary->inner.push_back(info);
		}
		return;
	}
	ProcessQuery(data, at, till, state, depth);
}

void ProcessQuery(
		std::string &data,
		size_t at,
		size_t till,
		OutState &state,
		[[maybe_unused]] int depth) {
	auto &summary = *state.summary;
	for (auto level = 0; level != kMaxDepth; ++level) {
		auto cursor = Cursor(data, at, till);
		auto id = uint32_t();
		if (!cursor.u32(id)) {
			return;
		}
		auto unwrapped = true;
		switch (id) {
		case ids::invokeWithLayer:
			unwrapped = cursor.skip(4);
			break;
		case ids::initConnection: {
			auto flags = uint32_t();
			unwrapped = cursor.u32(flags) && cursor.skip(4); // api_id
			for (auto i = 0; unwrapped && i != 6; ++i) {
				auto offset = size_t();
				auto length = size_t();
				unwrapped = cursor.bytes(offset, length);
			}
			if (unwrapped && (flags & 0x01)) { // proxy:flags.0?InputClientProxy
				auto proxyId = uint32_t();
				unwrapped = cursor.u32(proxyId)
					&& (proxyId == ids::inputClientProxy)
					&& cursor.maskBytes('*') // address
					&& cursor.skip(4); // port
				if (unwrapped) {
					summary.redacted.push_back("initConnection.proxy.address");
				}
			}
			if (unwrapped && (flags & 0x02)) { // params:flags.1?JSONValue
				unwrapped = MaskInitParams(data, cursor, summary);
			}
		} break;
		case ids::invokeAfterMsg:
			unwrapped = cursor.skip(8);
			break;
		case ids::invokeAfterMsgs: {
			auto vectorId = uint32_t();
			auto count = uint32_t();
			unwrapped = cursor.u32(vectorId)
				&& (vectorId == ids::vector)
				&& cursor.u32(count)
				&& (count < (1U << 20))
				&& cursor.skip(size_t(count) * 8);
		} break;
		case ids::invokeWithoutUpdates:
			break;
		case ids::invokeWithTakeout:
			unwrapped = cursor.skip(8);
			break;
		case ids::invokeWithMessagesRange: {
			auto rangeId = uint32_t();
			unwrapped = cursor.u32(rangeId) && cursor.skip(8);
		} break;
		case ids::invokeWithBusinessConnection: {
			auto offset = size_t();
			auto length = size_t();
			unwrapped = cursor.bytes(offset, length);
		} break;
		case ids::invokeWithGooglePlayIntegrity:
		case ids::invokeWithApnsSecret:
			unwrapped = cursor.maskBytes('*') && cursor.maskBytes('*');
			if (unwrapped) {
				summary.redacted.push_back(id == ids::invokeWithApnsSecret
					? "invokeWithApnsSecret"
					: "invokeWithGooglePlayIntegrity");
			}
			break;
		case ids::invokeWithReCaptcha:
			unwrapped = cursor.maskBytes('*');
			if (unwrapped) {
				summary.redacted.push_back("invokeWithReCaptcha");
			}
			break;
		default:
			unwrapped = false;
			break;
		}
		if (!unwrapped) {
			if (id == ids::invokeWithLayer
				|| id == ids::initConnection
				|| id == ids::invokeAfterMsg
				|| id == ids::invokeAfterMsgs
				|| id == ids::invokeWithTakeout
				|| id == ids::invokeWithMessagesRange
				|| id == ids::invokeWithBusinessConnection
				|| id == ids::invokeWithGooglePlayIntegrity
				|| id == ids::invokeWithApnsSecret
				|| id == ids::invokeWithReCaptcha) {
				// A wrapper we failed to parse: be safe, redact the rest.
				summary.partial = true;
				RedactAfterId(data, at, till, id);
				summary.redacted.push_back("unparsed_wrapper");
				return;
			}
			// Innermost query.
			if (state.method) {
				*state.method = id;
			}
			if (const auto name = SensitiveName(id)) {
				RedactAfterId(data, at, till, id);
				summary.redacted.push_back(name);
				state.redactions->remember(state.msgId, name);
				return;
			}
			if (id == ids::auth_sendCode) {
				// phone_number:string api_id:int api_hash:string
				auto c = Cursor(data, at + 4, till);
				auto offset = size_t();
				auto length = size_t();
				if (c.bytes(offset, length) && c.skip(4) && c.maskBytes('*')) {
					summary.redacted.push_back("auth.sendCode.api_hash");
				}
				summary.pii = true;
			} else if (id == ids::auth_exportLoginToken) {
				// api_id:int api_hash:string except_ids:Vector<long>
				auto c = Cursor(data, at + 4, till);
				if (c.skip(4) && c.maskBytes('*')) {
					summary.redacted.push_back("auth.exportLoginToken.api_hash");
				}
				state.redactions->remember(state.msgId, "auth.exportLoginToken");
			} else if (id == ids::auth_exportAuthorization) {
				// Request is just dc_id, the response is a credential.
				state.redactions->remember(state.msgId, "auth.exportAuthorization");
			}
			if (till - at <= kMaxNestedScan) {
				MaskNested(data, at + 4, till, summary);
			}
			return;
		}
		if (state.wrap) {
			state.wrap->push_back(id);
		}
		at = cursor.pos();
	}
	summary.partial = true;
}

[[nodiscard]] bool AlwaysSensitiveResult(uint32_t id) {
	switch (id) {
	case ids::auth_exportedAuthorization:
	case ids::auth_authorization:
	case ids::auth_loginTokenSuccess:
	case ids::auth_loginToken:
	case ids::account_tmpPassword:
	case ids::account_passwordSettings:
		return true;
	}
	return false;
}

void ProcessInMessage(
		std::string &data,
		size_t at,
		size_t till,
		uint32_t &type,
		uint64_t &reqMsgId,
		uint32_t &resultType,
		Summary &summary,
		ResponseRedactions &redactions,
		int depth) {
	auto cursor = Cursor(data, at, till);
	auto id = uint32_t();
	if (!cursor.u32(id)) {
		return;
	}
	type = id;
	if (id == ids::msg_container && depth == 0) {
		auto count = uint32_t();
		if (!cursor.u32(count)) {
			summary.partial = true;
			return;
		}
		for (auto i = uint32_t(); i != count; ++i) {
			auto info = InnerInfo();
			if (!cursor.u64(info.msgId)
				|| !cursor.u32(info.seqNo)
				|| !cursor.u32(info.length)) {
				summary.partial = true;
				return;
			}
			const auto bodyAt = cursor.pos();
			if (!cursor.skip(info.length)) {
				summary.partial = true;
				return;
			}
			ProcessInMessage(
				data,
				bodyAt,
				bodyAt + info.length,
				info.type,
				info.reqMsgId,
				info.resultType,
				summary,
				redactions,
				depth + 1);
			summary.inner.push_back(info);
		}
		return;
	}
	if (id != ids::rpc_result) {
		return;
	}
	if (!cursor.u64(reqMsgId)) {
		summary.partial = true;
		return;
	}
	const auto resultAt = cursor.pos();
	if (!cursor.u32(resultType)) {
		return;
	}
	const auto name = redactions.take(reqMsgId);
	if (resultType == ids::rpc_error) {
		return; // Errors carry no secrets and are useful as is.
	}
	if (name || AlwaysSensitiveResult(resultType)) {
		RedactAfterId(data, resultAt, till, resultType);
		summary.redacted.push_back(name
			? (std::string(name) + ".result")
			: std::string("result"));
	}
}

} // namespace

void ResponseRedactions::remember(uint64_t msgId, const char *name) {
	if (!msgId) {
		return;
	}
	if (_entries.size() >= kMaxRemembered) {
		_entries.erase(begin(_entries), begin(_entries) + kMaxRemembered / 2);
	}
	_entries.push_back({ msgId, name });
}

const char *ResponseRedactions::take(uint64_t msgId) {
	// Not erased: a response may be resent (e.g. after msg_resend_req),
	// and must stay redacted.
	for (auto i = _entries.rbegin(); i != _entries.rend(); ++i) {
		if (i->msgId == msgId) {
			return i->name;
		}
	}
	return nullptr;
}

const char *SensitiveName(uint32_t id) {
	switch (id) {
	case ids::auth_signIn: return "auth.signIn";
	case ids::auth_signUp: return "auth.signUp";
	case ids::auth_checkPassword: return "auth.checkPassword";
	case ids::auth_recoverPassword: return "auth.recoverPassword";
	case ids::auth_checkRecoveryPassword: return "auth.checkRecoveryPassword";
	case ids::auth_importAuthorization: return "auth.importAuthorization";
	case ids::auth_importBotAuthorization: return "auth.importBotAuthorization";
	case ids::auth_importLoginToken: return "auth.importLoginToken";
	case ids::auth_acceptLoginToken: return "auth.acceptLoginToken";
	case ids::auth_importWebTokenAuthorization: return "auth.importWebTokenAuthorization";
	case ids::account_getPasswordSettings: return "account.getPasswordSettings";
	case ids::account_updatePasswordSettings: return "account.updatePasswordSettings";
	case ids::account_confirmPasswordEmail: return "account.confirmPasswordEmail";
	case ids::account_getTmpPassword: return "account.getTmpPassword";
	case ids::account_saveSecureValue: return "account.saveSecureValue";
	case ids::account_changePhone: return "account.changePhone";
	case ids::account_confirmPhone: return "account.confirmPhone";
	case ids::account_verifyPhone: return "account.verifyPhone";
	case ids::account_verifyEmail: return "account.verifyEmail";
	case ids::account_deleteAccount: return "account.deleteAccount";
	case ids::payments_sendPaymentForm: return "payments.sendPaymentForm";
	case ids::messages_requestEncryption: return "messages.requestEncryption";
	case ids::messages_acceptEncryption: return "messages.acceptEncryption";
	}
	return nullptr;
}

void ProcessOutgoing(
		std::string &body,
		uint64_t msgId,
		Summary &summary,
		ResponseRedactions &redactions) {
	if (body.size() >= 4) {
		std::memcpy(&summary.type, body.data(), 4);
	}
	auto state = OutState{
		.msgId = msgId,
		.summary = &summary,
		.redactions = &redactions,
		.wrap = &summary.wrap,
		.method = &summary.method,
	};
	ProcessOutMessage(body, 0, body.size(), state, 0);
}

void ProcessIncoming(
		std::string &body,
		Summary &summary,
		ResponseRedactions &redactions) {
	ProcessInMessage(
		body,
		0,
		body.size(),
		summary.type,
		summary.reqMsgId,
		summary.resultType,
		summary,
		redactions,
		0);
}

} // namespace Trace::details
