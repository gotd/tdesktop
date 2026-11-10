/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Trace::details {

// Marker written over the body of a redacted constructor:
// [orig constructor id][kRedactedMarker][orig id][zeros...].
inline constexpr uint32_t kRedactedMarker = 0x7EDAC7EDU;

struct InnerInfo {
	uint64_t msgId = 0;
	uint32_t seqNo = 0;
	uint32_t length = 0;
	uint32_t type = 0;
	uint32_t method = 0; // Outgoing: innermost constructor below wrappers.
	uint64_t reqMsgId = 0; // Incoming rpc_result.
	uint32_t resultType = 0; // Incoming rpc_result.
};

struct Summary {
	uint32_t type = 0;
	uint32_t method = 0;
	uint64_t reqMsgId = 0;
	uint32_t resultType = 0;
	std::vector<uint32_t> wrap;
	std::vector<InnerInfo> inner;
	std::vector<std::string> redacted;
	bool pii = false;
	bool partial = false;
};

// Remembers outgoing msg_ids whose responses must be redacted.
class ResponseRedactions {
public:
	void remember(uint64_t msgId, const char *name);
	[[nodiscard]] const char *take(uint64_t msgId);

private:
	struct Entry {
		uint64_t msgId = 0;
		const char *name = nullptr;
	};
	std::vector<Entry> _entries;

};

// Walks (and redacts in place) one encrypted message body.
// Lengths are always preserved.
void ProcessOutgoing(
	std::string &body,
	uint64_t msgId,
	Summary &summary,
	ResponseRedactions &redactions);
void ProcessIncoming(
	std::string &body,
	Summary &summary,
	ResponseRedactions &redactions);

[[nodiscard]] const char *SensitiveName(uint32_t id);

} // namespace Trace::details
