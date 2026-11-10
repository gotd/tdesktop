/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "trace/trace_writer.h"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else // _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif // _WIN32

namespace Trace::details {
namespace {

constexpr auto kBlobMagic = "TDWT";
constexpr auto kBlobVersion = uint16_t(1);
constexpr auto kBlobFileHeader = 6; // magic + u16 version
constexpr auto kBlobRecordHeader = 16; // u64 seq, u32 len, u32 crc32c
constexpr auto kQueueEventsMax = size_t(1) << 20;

const auto kStart = std::chrono::steady_clock::now();

std::FILE *OpenPrivate(const std::filesystem::path &path) {
#ifdef _WIN32
	return _wfopen(path.c_str(), L"wb");
#else // _WIN32
	const auto fd = ::open(
		path.c_str(),
		O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		S_IRUSR | S_IWUSR);
	if (fd < 0) {
		return nullptr;
	}
	const auto result = ::fdopen(fd, "wb");
	if (!result) {
		::close(fd);
	}
	return result;
#endif // _WIN32
}

std::string SegmentName(const char *prefix, int index, const char *suffix) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%s-%04d%s", prefix, index, suffix);
	return buffer;
}

void AppendNumber(std::string &to, uint64_t value) {
	char buffer[32];
	const auto size = std::snprintf(
		buffer,
		sizeof(buffer),
		"%llu",
		static_cast<unsigned long long>(value));
	to.append(buffer, size);
}

std::array<uint32_t, 256> MakeCrcTable() {
	auto result = std::array<uint32_t, 256>();
	for (auto i = uint32_t(); i != 256; ++i) {
		auto value = i;
		for (auto j = 0; j != 8; ++j) {
			value = (value & 1) ? ((value >> 1) ^ 0x82F63B78U) : (value >> 1);
		}
		result[i] = value;
	}
	return result;
}

} // namespace

uint32_t Crc32c(const void *data, size_t size) {
	static const auto table = MakeCrcTable();
	auto crc = ~uint32_t(0);
	const auto bytes = static_cast<const uint8_t*>(data);
	for (auto i = size_t(); i != size; ++i) {
		crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
	}
	return ~crc;
}

std::string Base64(const void *data, size_t size) {
	static constexpr char kAlphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const auto bytes = static_cast<const uint8_t*>(data);
	auto result = std::string();
	result.reserve(((size + 2) / 3) * 4);
	auto i = size_t();
	for (; i + 2 < size; i += 3) {
		const auto v = (uint32_t(bytes[i]) << 16)
			| (uint32_t(bytes[i + 1]) << 8)
			| uint32_t(bytes[i + 2]);
		result.push_back(kAlphabet[(v >> 18) & 63]);
		result.push_back(kAlphabet[(v >> 12) & 63]);
		result.push_back(kAlphabet[(v >> 6) & 63]);
		result.push_back(kAlphabet[v & 63]);
	}
	if (i < size) {
		auto v = uint32_t(bytes[i]) << 16;
		if (i + 1 < size) {
			v |= uint32_t(bytes[i + 1]) << 8;
		}
		result.push_back(kAlphabet[(v >> 18) & 63]);
		result.push_back(kAlphabet[(v >> 12) & 63]);
		result.push_back((i + 1 < size) ? kAlphabet[(v >> 6) & 63] : '=');
		result.push_back('=');
	}
	return result;
}

std::string Hex(const void *data, size_t size) {
	static constexpr char kDigits[] = "0123456789abcdef";
	const auto bytes = static_cast<const uint8_t*>(data);
	auto result = std::string();
	result.reserve(size * 2);
	for (auto i = size_t(); i != size; ++i) {
		result.push_back(kDigits[bytes[i] >> 4]);
		result.push_back(kDigits[bytes[i] & 0x0F]);
	}
	return result;
}

void AppendFixed(std::string &to, double value, int precision) {
	char buffer[64];
	const auto result = std::to_chars(
		buffer,
		buffer + sizeof(buffer),
		value,
		std::chars_format::fixed,
		precision);
	if (result.ec != std::errc()) {
		to += '0';
		return;
	}
	to.append(buffer, result.ptr);
}

void AppendJsonString(std::string &to, std::string_view value) {
	static constexpr char kDigits[] = "0123456789abcdef";
	to.push_back('"');
	for (const auto ch : value) {
		const auto c = static_cast<unsigned char>(ch);
		switch (c) {
		case '"': to.append("\\\""); break;
		case '\\': to.append("\\\\"); break;
		case '\n': to.append("\\n"); break;
		case '\r': to.append("\\r"); break;
		case '\t': to.append("\\t"); break;
		default:
			if (c < 0x20) {
				to.append("\\u00");
				to.push_back(kDigits[c >> 4]);
				to.push_back(kDigits[c & 0x0F]);
			} else {
				to.push_back(char(c));
			}
		}
	}
	to.push_back('"');
}

Writer::Writer() = default;

Writer::~Writer() {
	stop();
}

double Writer::Now() {
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now() - kStart).count();
	return double(ns) / 1e9;
}

void Writer::push(std::string thread, std::string fields, std::string *blob) {
	const auto size = fields.size()
		+ thread.size()
		+ (blob ? blob->size() : 0)
		+ 64;
	auto lock = std::unique_lock(_mutex);
	if (_stopping || _discarded) {
		return;
	} else if (_queuedBytes + size > _options.queueBytes
		|| _queue.size() >= kQueueEventsMax) {
		++_dropped;
		return;
	}
	auto &event = _queue.emplace_back();
	event.seq = ++_seq;
	event.t = Now();
	event.thread = std::move(thread);
	event.fields = std::move(fields);
	if (blob) {
		event.blob = std::move(*blob);
		event.hasBlob = true;
	}
	_queuedBytes += size;
	const auto started = _started;
	lock.unlock();
	if (started) {
		_cv.notify_one();
	}
}

bool Writer::start(WriterOptions options, std::string &error) {
	auto ec = std::error_code();
	std::filesystem::create_directories(options.directory, ec);
	if (ec) {
		error = "could not create directory: " + ec.message();
		return false;
	}
	std::filesystem::permissions(
		options.directory,
		std::filesystem::perms::owner_all,
		std::filesystem::perm_options::replace,
		ec);
	{
		auto lock = std::unique_lock(_mutex);
		const auto queueBytes = _options.queueBytes;
		_options = std::move(options);
		if (!_options.queueBytes) {
			_options.queueBytes = queueBytes;
		}
	}
	if (!openSegment(0)) {
		error = "could not create trace files";
		return false;
	}
	{
		auto lock = std::unique_lock(_mutex);
		_started = true;
	}
	_thread = std::thread([this] { run(); });
	return true;
}

void Writer::discard() {
	auto lock = std::unique_lock(_mutex);
	_discarded = true;
	_queue.clear();
	_queuedBytes = 0;
}

void Writer::stop() {
	{
		auto lock = std::unique_lock(_mutex);
		if (_stopping) {
			return;
		}
		_stopping = true;
	}
	_cv.notify_one();
	if (_thread.joinable()) {
		_thread.join();
	}
	closeSegment();
}

void Writer::run() {
	auto batch = std::deque<QueuedEvent>();
	while (true) {
		auto dropped = uint64_t();
		auto stopping = false;
		{
			auto lock = std::unique_lock(_mutex);
			_cv.wait_for(lock, std::chrono::milliseconds(250), [&] {
				return _stopping || !_queue.empty();
			});
			std::swap(batch, _queue);
			_queuedBytes = 0;
			dropped = std::exchange(_dropped, 0);
			stopping = _stopping;
		}
		if (dropped) {
			writeDropped(dropped);
		}
		for (auto &event : batch) {
			writeOne(event);
		}
		batch.clear();
		if (_events) {
			std::fflush(_events);
		}
		if (_blobs) {
			std::fflush(_blobs);
		}
		if (stopping) {
			auto lock = std::unique_lock(_mutex);
			if (_queue.empty()) {
				break;
			}
		}
	}
}

void Writer::writeHeader() {
	auto line = std::string("{\"seq\":0,\"ev\":\"hdr\",\"v\":1,\"seg\":");
	AppendNumber(line, uint64_t(_segment));
	line += _options.header;
	line += "}\n";
	std::fwrite(line.data(), 1, line.size(), _events);
	_eventsBytes += line.size();
}

void Writer::writeDropped(uint64_t count) {
	auto event = QueuedEvent();
	{
		auto lock = std::unique_lock(_mutex);
		event.seq = ++_seq;
	}
	event.t = Now();
	event.thread = "trace";
	event.fields = ",\"ev\":\"dropped\",\"count\":";
	AppendNumber(event.fields, count);
	writeOne(event);
}

bool Writer::openSegment(int index) {
	const auto events = _options.directory
		/ SegmentName("events", index, ".jsonl");
	const auto blobs = _options.directory
		/ SegmentName("blobs", index, ".bin");
	_events = OpenPrivate(events);
	_blobs = OpenPrivate(blobs);
	if (!_events || !_blobs) {
		if (_events) {
			std::fclose(std::exchange(_events, nullptr));
		}
		if (_blobs) {
			std::fclose(std::exchange(_blobs, nullptr));
		}
		return false;
	}
	_segment = index;
	_eventsBytes = _blobsBytes = 0;
	std::fwrite(kBlobMagic, 1, 4, _blobs);
	const uint8_t version[2] = {
		uint8_t(kBlobVersion & 0xFF),
		uint8_t(kBlobVersion >> 8),
	};
	std::fwrite(version, 1, 2, _blobs);
	_blobsBytes = kBlobFileHeader;
	writeHeader();
	return true;
}

void Writer::closeSegment() {
	if (!_events && !_blobs) {
		return;
	}
	if (_events) {
		std::fclose(std::exchange(_events, nullptr));
	}
	if (_blobs) {
		std::fclose(std::exchange(_blobs, nullptr));
	}
	if (_segment >= 0) {
		_closed.push_back({ _segment, _eventsBytes + _blobsBytes });
	}
}

void Writer::enforceTotal() {
	auto total = _eventsBytes + _blobsBytes;
	for (const auto &segment : _closed) {
		total += segment.bytes;
	}
	// Segment 0 holds the handshake and the first sync, never delete it.
	while (total > _options.totalBytes) {
		const auto i = std::find_if(
			begin(_closed),
			end(_closed),
			[](const Segment &segment) { return segment.index != 0; });
		if (i == end(_closed)) {
			break;
		}
		auto ec = std::error_code();
		std::filesystem::remove(
			_options.directory / SegmentName("events", i->index, ".jsonl"),
			ec);
		std::filesystem::remove(
			_options.directory / SegmentName("blobs", i->index, ".bin"),
			ec);
		total -= i->bytes;
		_closed.erase(i);
	}
}

void Writer::writeOne(QueuedEvent &event) {
	if (!_events) {
		return;
	}
	if (_eventsBytes > _options.segmentEventsBytes
		|| _blobsBytes > _options.segmentBlobsBytes) {
		const auto next = _segment + 1;
		closeSegment();
		if (!openSegment(next)) {
			return;
		}
		enforceTotal();
	}
	auto line = std::string();
	line.reserve(event.fields.size() + 160);
	line += "{\"seq\":";
	AppendNumber(line, event.seq);
	line += ",\"t\":";
	AppendFixed(line, event.t, 6);
	line += ",\"th\":";
	AppendJsonString(line, event.thread);
	line += event.fields;
	if (event.hasBlob) {
		const auto size = event.blob.size();
		const auto crc = Crc32c(event.blob.data(), size);
		uint8_t header[kBlobRecordHeader];
		std::memcpy(header, &event.seq, 8); // little-endian hosts
		const auto len32 = uint32_t(size);
		std::memcpy(header + 8, &len32, 4);
		std::memcpy(header + 12, &crc, 4);
		const auto offset = _blobsBytes + kBlobRecordHeader;
		std::fwrite(header, 1, kBlobRecordHeader, _blobs);
		if (size) {
			std::fwrite(event.blob.data(), 1, size, _blobs);
		}
		_blobsBytes += kBlobRecordHeader + size;

		unsigned char sha[SHA256_DIGEST_LENGTH];
		SHA256(
			reinterpret_cast<const unsigned char*>(event.blob.data()),
			size,
			sha);
		line += ",\"blob\":{\"seg\":";
		AppendNumber(line, uint64_t(_segment));
		line += ",\"off\":";
		AppendNumber(line, offset);
		line += ",\"len\":";
		AppendNumber(line, size);
		line += ",\"crc32c\":";
		AppendNumber(line, crc);
		line += ",\"sha256\":\"";
		line += Hex(sha, sizeof(sha));
		line += "\"}";
		if (_options.inlineMax && size <= _options.inlineMax) {
			line += ",\"b64\":\"";
			line += Base64(event.blob.data(), size);
			line += '"';
		}
	}
	line += "}\n";
	std::fwrite(line.data(), 1, line.size(), _events);
	_eventsBytes += line.size();
}

} // namespace Trace::details
