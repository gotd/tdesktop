/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "trace/trace_ui_core.h"

#include "trace/trace_writer.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#endif // __GNUC__ || __clang__

namespace Trace::UiCore {
namespace {

constexpr auto kMaxCache = size_t(1) << 16;
constexpr auto kMaxCarry = size_t(4096);

std::atomic<uint32_t> ActionCounter = 0;

[[nodiscard]] int64_t Ms(double seconds) {
	const auto result = std::llround(seconds * 1000.);
	return (result < 0) ? 0 : result;
}

void EraseAll(std::string &value, std::string_view what) {
	for (auto i = value.find(what); i != std::string::npos; i = value.find(what, i)) {
		value.erase(i, what.size());
	}
}

[[nodiscard]] bool IsSpace(char ch) {
	return (ch == ' ');
}

} // namespace

uint32_t NextAction() {
	return ++ActionCounter;
}

const char *Bucket(int count) {
	if (count <= 0) {
		return "0";
	} else if (count == 1) {
		return "1";
	} else if (count <= 5) {
		return "2-5";
	} else if (count <= 20) {
		return "6-20";
	} else if (count <= 80) {
		return "21-80";
	}
	return "81+";
}

std::string Demangle(const char *name) {
	if (!name) {
		return std::string();
	}
#if defined(__GNUC__) || defined(__clang__)
	auto status = 0;
	const auto demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
	if (demangled) {
		auto result = std::string(demangled);
		std::free(demangled);
		if (status == 0) {
			return result;
		}
	}
#endif // __GNUC__ || __clang__
	return std::string(name);
}

std::string CleanTypeName(std::string_view demangled) {
	auto value = std::string(demangled);
	EraseAll(value, "(anonymous namespace)::");
	EraseAll(value, "`anonymous namespace'::");
	EraseAll(value, "class ");
	EraseAll(value, "struct ");
	auto result = std::string();
	result.reserve(value.size());
	auto depth = 0;
	for (const auto ch : value) {
		if (ch == '<') {
			++depth;
		} else if (ch == '>') {
			if (depth > 0) {
				--depth;
			}
		} else if (!depth) {
			result += ch;
		}
	}
	while (!result.empty() && IsSpace(result.back())) {
		result.pop_back();
	}
	return result;
}

std::string TemplateArgumentOrName(std::string_view demangled) {
	const auto open = demangled.find('<');
	if (open == std::string_view::npos) {
		return CleanTypeName(demangled);
	}
	auto depth = 0;
	for (auto i = open + 1; i < demangled.size(); ++i) {
		const auto ch = demangled[i];
		if (ch == '<') {
			++depth;
		} else if (ch == '>' || ch == ',') {
			if (!depth) {
				const auto argument = demangled.substr(open + 1, i - open - 1);
				auto result = CleanTypeName(argument);
				return result.empty() ? CleanTypeName(demangled) : result;
			} else if (ch == '>') {
				--depth;
			}
		}
	}
	return CleanTypeName(demangled);
}

std::string HashId(
		const unsigned char *key,
		size_t keySize,
		uint64_t value) {
	char decimal[24];
	const auto length = std::snprintf(
		decimal,
		sizeof(decimal),
		"%llu",
		static_cast<unsigned long long>(value));
	unsigned char digest[EVP_MAX_MD_SIZE];
	auto digestSize = 0U;
	const auto ok = HMAC(
		EVP_sha256(),
		key,
		int(keySize),
		reinterpret_cast<const unsigned char*>(decimal),
		size_t(length),
		digest,
		&digestSize);
	if (!ok || digestSize < 8) {
		return "h:";
	}
	return "h:" + details::Hex(digest, 8);
}

void IdFormatter::setKey(const unsigned char *key, size_t size) {
	_key.assign(key, key + size);
	_cache.clear();
}

void IdFormatter::setRaw(bool raw) {
	_raw = raw;
}

bool IdFormatter::raw() const {
	return _raw;
}

std::string IdFormatter::format(uint64_t value) {
	if (_raw) {
		return std::to_string(value);
	}
	const auto i = _cache.find(value);
	if (i != end(_cache)) {
		return i->second;
	}
	if (_cache.size() >= kMaxCache) {
		_cache.clear();
	}
	auto result = HashId(_key.data(), _key.size(), value);
	_cache.emplace(value, result);
	return result;
}

IdFormatter &Ids() {
	static auto result = IdFormatter();
	return result;
}

void GenerateKey(unsigned char *key, size_t size) {
	if (RAND_bytes(key, int(size)) == 1) {
		return;
	}
	auto device = std::random_device();
	for (auto i = size_t(); i != size; ++i) {
		key[i] = static_cast<unsigned char>(device());
	}
}

bool WriteKeyFile(
		const std::filesystem::path &directory,
		const unsigned char *key,
		size_t size,
		std::string &error) {
	const auto path = directory / "ui.key";
	const auto file = details::OpenPrivateFile(path);
	if (!file) {
		error = "could not create ui.key";
		return false;
	}
	const auto text = details::Hex(key, size) + '\n';
	const auto written = std::fwrite(text.data(), 1, text.size(), file);
	const auto closed = (std::fclose(file) == 0);
	if (written != text.size() || !closed) {
		error = "could not write ui.key";
		return false;
	}
	return true;
}

void StampClock::setOptions(StampOptions options) {
	_options = options;
}

const StampOptions &StampClock::options() const {
	return _options;
}

void StampClock::enter(uint32_t a, double now) {
	_open.push_back({ a, now });
}

void StampClock::leave(double now) {
	if (_open.empty()) {
		return;
	}
	_lastA = _open.back().a;
	_lastEnd = now;
	_open.pop_back();
}

uint32_t StampClock::current() const {
	return _open.empty() ? 0 : _open.back().a;
}

bool StampClock::inScope() const {
	return !_open.empty();
}

void StampClock::mark(uint32_t a, double now) {
	_lastA = a;
	_lastEnd = now;
}

std::optional<Stamp> StampClock::compute(double now) const {
	if (!_open.empty()) {
		const auto &open = _open.back();
		const auto age = Ms(now - open.start);
		if (double(age) > _options.nestedMs) {
			// A nested event loop (modal dialog) inside the handler.
			return Stamp{ open.a, "after", age };
		}
		return Stamp{ open.a, "sync", -1 };
	} else if (!_lastA) {
		return std::nullopt;
	}
	const auto dt = Ms(now - _lastEnd);
	if (double(dt) <= _options.deferredMs) {
		return Stamp{ _lastA, "deferred", dt };
	} else if (double(dt) <= _options.afterMs) {
		return Stamp{ _lastA, "after", dt };
	}
	return std::nullopt;
}

void AppendStamp(std::string &to, const Stamp &stamp) {
	to += ",\"ui\":{\"a\":";
	to += std::to_string(stamp.a);
	to += ",\"rel\":\"";
	to += stamp.rel;
	to += '"';
	if (stamp.dt >= 0) {
		to += ",\"dt\":";
		to += std::to_string(stamp.dt);
	}
	to += '}';
}

bool StampClock::append(std::string &to, double now) const {
	if (const auto stamp = compute(now)) {
		AppendStamp(to, *stamp);
		return true;
	}
	return false;
}

void StampClock::reset() {
	_open.clear();
	_lastA = 0;
	_lastEnd = 0.;
}

StampClock &MainClock() {
	static auto result = StampClock();
	return result;
}

ScrollCoalescer::ScrollCoalescer(double quietMs) : _quiet(quietMs) {
}

ScrollCoalescer::Result ScrollCoalescer::wheel(
		const void *target,
		double now,
		int64_t dy,
		int64_t dx) {
	auto result = Result();
	if (_active
		&& (target != _target || (now - _last) * 1000. > _quiet)) {
		result.ended = finish();
	}
	if (!_active) {
		_active = true;
		_a = NextAction();
		_target = target;
		_start = now;
		_n = 0;
		_dy = _dx = 0;
		result.begin = true;
	}
	++_n;
	_dy += dy;
	_dx += dx;
	_last = now;
	result.a = _a;
	return result;
}

std::optional<ScrollEnd> ScrollCoalescer::poll(double now) {
	if (_active && (now - _last) * 1000. >= _quiet) {
		return finish();
	}
	return std::nullopt;
}

std::optional<ScrollEnd> ScrollCoalescer::flush() {
	if (_active) {
		return finish();
	}
	return std::nullopt;
}

bool ScrollCoalescer::active() const {
	return _active;
}

double ScrollCoalescer::deadline() const {
	return _last + _quiet / 1000.;
}

ScrollEnd ScrollCoalescer::finish() {
	_active = false;
	_target = nullptr;
	return { _a, _n, _dy, _dx, Ms(_last - _start) };
}

TypeCoalescer::TypeCoalescer(double quietMs) : _quiet(quietMs) {
}

TypeCoalescer::Result TypeCoalescer::key(
		const void *field,
		double now,
		bool masked,
		bool search,
		bool startsSpan) {
	auto result = Result();
	if (_active
		&& (field != _field || (now - _last) * 1000. > _quiet)) {
		result.ended = finish();
	}
	if (!_active) {
		if (!startsSpan) {
			return result;
		}
		_active = true;
		_a = NextAction();
		_field = field;
		_start = now;
		_n = 0;
		_masked = masked;
		_search = search;
		result.begin = true;
	}
	++_n;
	_last = now;
	result.a = _a;
	return result;
}

std::optional<TypeEnd> TypeCoalescer::poll(double now) {
	if (_active && (now - _last) * 1000. >= _quiet) {
		return finish();
	}
	return std::nullopt;
}

std::optional<TypeEnd> TypeCoalescer::flush() {
	if (_active) {
		return finish();
	}
	return std::nullopt;
}

bool TypeCoalescer::active() const {
	return _active;
}

const void *TypeCoalescer::field() const {
	return _active ? _field : nullptr;
}

double TypeCoalescer::deadline() const {
	return _last + _quiet / 1000.;
}

TypeEnd TypeCoalescer::finish() {
	_active = false;
	_field = nullptr;
	return { _a, _n, _masked, _search, Ms(_last - _start) };
}

RepeatCoalescer::RepeatCoalescer(double quietMs) : _quiet(quietMs) {
}

RepeatCoalescer::Result RepeatCoalescer::key(
		int key,
		int modifiers,
		bool autoRepeat,
		double now) {
	auto result = Result();
	if (_active
		&& autoRepeat
		&& key == _key
		&& modifiers == _modifiers
		&& (now - _last) * 1000. <= _quiet) {
		++_repeat;
		_last = now;
		result.a = _a;
		return result;
	}
	result.ended = finish();
	_active = true;
	_a = NextAction();
	_key = key;
	_modifiers = modifiers;
	_start = _last = now;
	_repeat = 0;
	result.emit = true;
	result.a = _a;
	return result;
}

std::optional<RepeatEnd> RepeatCoalescer::poll(double now) {
	if (_active && (now - _last) * 1000. >= _quiet) {
		return finish();
	}
	return std::nullopt;
}

std::optional<RepeatEnd> RepeatCoalescer::flush() {
	return finish();
}

double RepeatCoalescer::deadline() const {
	return _last + _quiet / 1000.;
}

bool RepeatCoalescer::active() const {
	return _active;
}

std::optional<RepeatEnd> RepeatCoalescer::finish() {
	if (!_active) {
		return std::nullopt;
	}
	_active = false;
	if (!_repeat) {
		return std::nullopt;
	}
	return RepeatEnd{ _a, _repeat, Ms(_last - _start) };
}

std::string CleanLabel(std::string_view label) {
	auto result = std::string();
	result.reserve(std::min(label.size(), kMaxLabel * 4));
	auto points = size_t();
	auto i = size_t();
	while (i < label.size() && points < kMaxLabel) {
		const auto lead = static_cast<unsigned char>(label[i]);
		auto length = size_t(1);
		if (lead >= 0xF0 && lead <= 0xF4) {
			length = 4;
		} else if (lead >= 0xE0) {
			length = (lead <= 0xEF) ? 3 : 0;
		} else if (lead >= 0xC2) {
			length = 2;
		} else if (lead >= 0x80) {
			length = 0;
		}
		auto valid = (length > 0) && (i + length <= label.size());
		for (auto j = size_t(1); valid && j < length; ++j) {
			const auto next = static_cast<unsigned char>(label[i + j]);
			valid = ((next & 0xC0) == 0x80);
		}
		if (!valid) {
			result += '?';
			++i;
		} else if (length == 1 && (lead < 0x20 || lead == 0x7F)) {
			result += ' ';
			++i;
		} else {
			result.append(label.data() + i, length);
			i += length;
		}
		++points;
	}
	const auto first = result.find_first_not_of(' ');
	if (first == std::string::npos) {
		return std::string();
	}
	const auto last = result.find_last_not_of(' ');
	return result.substr(first, last - first + 1);
}

std::vector<std::string> TakeMarkerLines(
		std::string &carry,
		std::string_view chunk) {
	carry.append(chunk.data(), chunk.size());
	auto result = std::vector<std::string>();
	auto from = size_t();
	while (true) {
		const auto till = carry.find('\n', from);
		if (till == std::string::npos) {
			break;
		}
		auto line = std::string_view(carry).substr(from, till - from);
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		auto label = CleanLabel(line);
		if (!label.empty()) {
			result.push_back(std::move(label));
		}
		from = till + 1;
	}
	carry.erase(0, from);
	if (carry.size() > kMaxCarry) {
		carry.resize(kMaxCarry);
	}
	return result;
}

Record::Record() {
	_data.reserve(192);
}

Record &Record::key(std::string_view name) {
	_data += ",\"";
	_data += name;
	_data += "\":";
	return *this;
}

Record &Record::num(std::string_view name, int64_t value) {
	key(name);
	_data += std::to_string(value);
	return *this;
}

Record &Record::str(std::string_view name, std::string_view value) {
	key(name);
	details::AppendJsonString(_data, value);
	return *this;
}

Record &Record::boolean(std::string_view name, bool value) {
	key(name);
	_data += value ? "true" : "false";
	return *this;
}

Record &Record::null(std::string_view name) {
	key(name);
	_data += "null";
	return *this;
}

Record &Record::id(std::string_view name, uint64_t value) {
	return str(name, Ids().format(value));
}

Record &Record::list(
		std::string_view name,
		const std::vector<std::string> &values) {
	key(name);
	_data += '[';
	for (auto i = size_t(); i != values.size(); ++i) {
		if (i) {
			_data += ',';
		}
		details::AppendJsonString(_data, values[i]);
	}
	_data += ']';
	return *this;
}

Record &Record::raw(std::string_view value) {
	_data += value;
	return *this;
}

const std::string &Record::data() const {
	return _data;
}

std::string Record::take() {
	return std::move(_data);
}

} // namespace Trace::UiCore
