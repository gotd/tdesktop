/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Qt-free parts of UI tracing (P14): action ids, the "ui" stamp, id
// hashing, coalescing of wheel / typing / key repeat, marker file lines
// and record building. Built into td_mtproto and unit-testable on its
// own; the Qt glue is trace/trace_ui.cpp (Telegram target).
//
// Everything here is main-thread only, except the action counter
// (atomic).

namespace Trace::UiCore {

// Per-run action counter, ids start at 1.
[[nodiscard]] uint32_t NextAction();

// Bucketed keystroke count: "1", "2-5", "6-20", "21-80", "81+" ("0" for 0).
[[nodiscard]] const char *Bucket(int count);

// Demangles a typeid().name() (GCC / Clang Itanium, MSVC "class X").
[[nodiscard]] std::string Demangle(const char *name);
// Strips template arguments, "(anonymous namespace)::" and
// "class " / "struct " prefixes: "Ui::SlideWrap<Ui::RpWidget>" ->
// "Ui::SlideWrap".
[[nodiscard]] std::string CleanTypeName(std::string_view demangled);
// "Settings::SectionFactory<Settings::PrivacySecurity>" ->
// "Settings::PrivacySecurity"; other names are cleaned as above.
[[nodiscard]] std::string TemplateArgumentOrName(std::string_view demangled);

// Ids: HMAC-SHA256 over the decimal value with a 32-byte per-run key,
// truncated to 64 bits, written as "h:<16 hex>". With raw ids the
// decimal value itself.
inline constexpr auto kIdKeySize = size_t(32);
[[nodiscard]] std::string HashId(
	const unsigned char *key,
	size_t keySize,
	uint64_t value);

class IdFormatter final {
public:
	void setKey(const unsigned char *key, size_t size);
	void setRaw(bool raw);
	[[nodiscard]] bool raw() const;
	[[nodiscard]] std::string format(uint64_t value);

private:
	std::vector<unsigned char> _key;
	std::unordered_map<uint64_t, std::string> _cache;
	bool _raw = false;

};
[[nodiscard]] IdFormatter &Ids();

// 32 random bytes from OpenSSL (std::random_device as a fallback).
void GenerateKey(unsigned char *key, size_t size);
// Writes <dir>/ui.key: 64 lowercase hex chars and '\n', mode 0600.
[[nodiscard]] bool WriteKeyFile(
	const std::filesystem::path &directory,
	const unsigned char *key,
	size_t size,
	std::string &error);

// The "ui":{a,rel,dt} stamp.
struct StampOptions {
	double deferredMs = 150.;
	double afterMs = 3000.;
	double nestedMs = 500.; // Older sync scopes are written as "after".
};

struct Stamp {
	uint32_t a = 0;
	const char *rel = nullptr; // "sync", "deferred", "after"
	int64_t dt = -1; // ms, -1 for sync
};

class StampClock final {
public:
	void setOptions(StampOptions options);
	[[nodiscard]] const StampOptions &options() const;

	// Input scopes nest (nested event loops): the inner one wins until it
	// leaves.
	void enter(uint32_t a, double now);
	void leave(double now);
	// The action of the innermost open scope, 0 outside of scopes.
	[[nodiscard]] uint32_t current() const;
	[[nodiscard]] bool inScope() const;

	// A synthetic or out-of-scope action: later events get it as
	// "deferred" / "after".
	void mark(uint32_t a, double now);

	[[nodiscard]] std::optional<Stamp> compute(double now) const;
	// Appends ,"ui":{...} if there is a stamp.
	bool append(std::string &to, double now) const;

	void reset();

private:
	struct Open {
		uint32_t a = 0;
		double start = 0.;
	};
	StampOptions _options;
	std::vector<Open> _open;
	uint32_t _lastA = 0;
	double _lastEnd = 0.;

};
[[nodiscard]] StampClock &MainClock();

void AppendStamp(std::string &to, const Stamp &stamp);

// Wheel / touch scroll: one ui.scroll at the start of a gesture, one
// ui.scroll.end after quietMs of quiet, on another scroll target or on
// other input.
struct ScrollEnd {
	uint32_t a = 0;
	int n = 0;
	int64_t dy = 0;
	int64_t dx = 0;
	int64_t ms = 0;
};

class ScrollCoalescer final {
public:
	explicit ScrollCoalescer(double quietMs = 400.);

	struct Result {
		bool begin = false;
		uint32_t a = 0;
		std::optional<ScrollEnd> ended; // Of the previous gesture.
	};
	Result wheel(const void *target, double now, int64_t dy, int64_t dx);
	[[nodiscard]] std::optional<ScrollEnd> poll(double now);
	[[nodiscard]] std::optional<ScrollEnd> flush();
	[[nodiscard]] bool active() const;
	[[nodiscard]] double deadline() const; // When poll() would end it.

private:
	[[nodiscard]] ScrollEnd finish();

	double _quiet = 400.;
	bool _active = false;
	uint32_t _a = 0;
	const void *_target = nullptr;
	double _start = 0.;
	double _last = 0.;
	int _n = 0;
	int64_t _dy = 0;
	int64_t _dx = 0;

};

// Typing: ui.type at the first printable key in a field, ui.type.end
// quietMs after the last one, on another field, focus change, Enter or
// other input. Only the count is kept.
struct TypeEnd {
	uint32_t a = 0;
	int n = 0;
	bool masked = false;
	bool search = false;
	int64_t ms = 0;
};

class TypeCoalescer final {
public:
	explicit TypeCoalescer(double quietMs = 1500.);

	struct Result {
		bool begin = false;
		uint32_t a = 0;
		std::optional<TypeEnd> ended; // Of the previous span.
	};
	// startsSpan: false for Backspace / Delete, which are only counted
	// inside a span of the same field (else the result has a == 0).
	Result key(
		const void *field,
		double now,
		bool masked,
		bool search,
		bool startsSpan = true);
	[[nodiscard]] std::optional<TypeEnd> poll(double now);
	[[nodiscard]] std::optional<TypeEnd> flush();
	[[nodiscard]] bool active() const;
	[[nodiscard]] const void *field() const;
	[[nodiscard]] double deadline() const;

private:
	[[nodiscard]] TypeEnd finish();

	double _quiet = 1500.;
	bool _active = false;
	uint32_t _a = 0;
	const void *_field = nullptr;
	double _start = 0.;
	double _last = 0.;
	int _n = 0;
	bool _masked = false;
	bool _search = false;

};

// Auto-repeat of a named key: the first press is a ui.key, repeats only
// count; when the run ends with repeats a ui.key.end {a,repeat,ms}.
struct RepeatEnd {
	uint32_t a = 0;
	int repeat = 0;
	int64_t ms = 0;
};

class RepeatCoalescer final {
public:
	explicit RepeatCoalescer(double quietMs = 400.);

	struct Result {
		bool emit = false; // Write a ui.key for this press.
		uint32_t a = 0;
		std::optional<RepeatEnd> ended;
	};
	Result key(int key, int modifiers, bool autoRepeat, double now);
	[[nodiscard]] std::optional<RepeatEnd> poll(double now);
	[[nodiscard]] std::optional<RepeatEnd> flush();
	[[nodiscard]] double deadline() const;
	[[nodiscard]] bool active() const;

private:
	[[nodiscard]] std::optional<RepeatEnd> finish();

	double _quiet = 400.;
	bool _active = false;
	uint32_t _a = 0;
	int _key = 0;
	int _modifiers = 0;
	double _start = 0.;
	double _last = 0.;
	int _repeat = 0;

};

// Marker labels: at most 200 code points, control characters replaced by
// spaces, trimmed.
inline constexpr auto kMaxLabel = size_t(200);
[[nodiscard]] std::string CleanLabel(std::string_view label);
// Splits newly read bytes of marker.in into labels. An unterminated last
// line stays in carry for the next read (carry is capped at 4 KiB).
[[nodiscard]] std::vector<std::string> TakeMarkerLines(
	std::string &carry,
	std::string_view chunk);

// ",\"k\":v" JSON fields of one record.
class Record final {
public:
	Record();

	Record &key(std::string_view name);
	Record &num(std::string_view name, int64_t value);
	Record &str(std::string_view name, std::string_view value);
	Record &boolean(std::string_view name, bool value);
	Record &null(std::string_view name);
	Record &id(std::string_view name, uint64_t value); // Ids().format()
	Record &list(std::string_view name, const std::vector<std::string> &values);
	Record &raw(std::string_view value);

	[[nodiscard]] const std::string &data() const;
	[[nodiscard]] std::string take();

private:
	std::string _data;

};

} // namespace Trace::UiCore
