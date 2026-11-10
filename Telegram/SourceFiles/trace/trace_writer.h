/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Qt-free trace writer: a bounded queue drained by one thread into
// events-NNNN.jsonl + blobs-NNNN.bin segments. Unit-testable on its own.

namespace Trace::details {

struct WriterOptions {
	std::filesystem::path directory;
	std::string header; // JSON fields of the "hdr" record, each ",k:v".
	uint64_t segmentEventsBytes = 64ULL * 1024 * 1024;
	uint64_t segmentBlobsBytes = 256ULL * 1024 * 1024;
	uint64_t totalBytes = 2ULL * 1024 * 1024 * 1024;
	size_t inlineMax = 0; // Bodies up to this size also go inline as b64.
	uint64_t queueBytes = 128ULL * 1024 * 1024;
};

struct QueuedEvent {
	uint64_t seq = 0;
	double t = 0.;
	std::string thread;
	std::string fields; // ",\"ev\":...,..." without braces.
	std::string blob;
	bool hasBlob = false;
};

class Writer final {
public:
	Writer();
	~Writer();

	// Events can be pushed before start(), they wait in the queue.
	void push(std::string thread, std::string fields, std::string *blob);
	[[nodiscard]] bool start(WriterOptions options, std::string &error);
	void stop();
	void discard();

	[[nodiscard]] static double Now();

private:
	struct Segment {
		int index = 0;
		uint64_t bytes = 0;
	};

	void run();
	void writeOne(QueuedEvent &event);
	[[nodiscard]] bool openSegment(int index);
	void closeSegment();
	void enforceTotal();
	void writeHeader();
	void writeDropped(uint64_t count);

	std::mutex _mutex;
	std::condition_variable _cv;
	std::deque<QueuedEvent> _queue;
	uint64_t _queuedBytes = 0;
	uint64_t _seq = 0;
	uint64_t _dropped = 0;
	bool _stopping = false;
	bool _started = false;
	bool _discarded = false;
	std::thread _thread;

	// Writer thread only.
	WriterOptions _options;
	std::FILE *_events = nullptr;
	std::FILE *_blobs = nullptr;
	int _segment = -1;
	uint64_t _eventsBytes = 0;
	uint64_t _blobsBytes = 0;
	std::vector<Segment> _closed;

};

[[nodiscard]] uint32_t Crc32c(const void *data, size_t size);
[[nodiscard]] std::string Base64(const void *data, size_t size);
[[nodiscard]] std::string Hex(const void *data, size_t size);
void AppendJsonString(std::string &to, std::string_view value);

// Appends value in fixed notation with a '.' decimal point, whatever the
// process locale is (printf's %f follows LC_NUMERIC, which Qt sets).
void AppendFixed(std::string &to, double value, int precision);

} // namespace Trace::details
