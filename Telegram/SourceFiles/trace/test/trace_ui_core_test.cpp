/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
// Host-side test of the Qt-free UI tracing parts (trace_ui_core) and of
// their records going through the writer. Build and run with
// Telegram/SourceFiles/trace/test/run_host_tests.sh (ASan + UBSan).

#include "trace/trace_ui_core.h"
#include "trace/trace_writer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace Trace::UiCore;

namespace {

int Failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK(" #cond ") failed\n"; \
		++Failures; \
	} \
} while (false)

#define CHECK_EQ(a, b) do { \
	const auto &va = (a); \
	const auto &vb = (b); \
	if (!(va == vb)) { \
		std::cerr << __FILE__ << ":" << __LINE__ << ": " #a " == " #b \
			<< " failed: '" << va << "' vs '" << vb << "'\n"; \
		++Failures; \
	} \
} while (false)

struct Node {
	virtual ~Node() = default;
};
namespace Ui {
template <typename T>
struct SlideWrap : Node {
};
} // namespace Ui
namespace {
struct Hidden : Node {
};
} // namespace

void TestBuckets() {
	CHECK_EQ(std::string(Bucket(0)), "0");
	CHECK_EQ(std::string(Bucket(1)), "1");
	CHECK_EQ(std::string(Bucket(2)), "2-5");
	CHECK_EQ(std::string(Bucket(5)), "2-5");
	CHECK_EQ(std::string(Bucket(6)), "6-20");
	CHECK_EQ(std::string(Bucket(20)), "6-20");
	CHECK_EQ(std::string(Bucket(21)), "21-80");
	CHECK_EQ(std::string(Bucket(80)), "21-80");
	CHECK_EQ(std::string(Bucket(81)), "81+");
	CHECK_EQ(std::string(Bucket(100000)), "81+");
}

void TestTypeNames() {
	const Node &wrap = Ui::SlideWrap<int>();
	const Node &hidden = Hidden();
	CHECK_EQ(CleanTypeName(Demangle(typeid(wrap).name())), "Ui::SlideWrap");
	CHECK_EQ(
		CleanTypeName(Demangle(typeid(hidden).name())),
		"Hidden");
	CHECK_EQ(CleanTypeName("class Ui::IconButton"), "Ui::IconButton");
	CHECK_EQ(
		CleanTypeName("Ui::SlideWrap<Ui::PaddingWrap<Ui::RpWidget> >"),
		"Ui::SlideWrap");
	CHECK_EQ(
		TemplateArgumentOrName(
			"Settings::SectionFactory<Settings::PrivacySecurity>"),
		"Settings::PrivacySecurity");
	CHECK_EQ(
		TemplateArgumentOrName("A::F<B::C<int>, D>"),
		"B::C");
	CHECK_EQ(TemplateArgumentOrName("Plain::Name"), "Plain::Name");
}

void TestIds() {
	unsigned char key[kIdKeySize];
	for (auto i = 0; i != int(kIdKeySize); ++i) {
		key[i] = static_cast<unsigned char>(i);
	}
	// Python: hmac.new(bytes(range(32)), str(v).encode(), sha256)
	CHECK_EQ(HashId(key, sizeof(key), 0), "h:3a8b171143bc3fe5");
	CHECK_EQ(HashId(key, sizeof(key), 123456789), "h:a2fb4a2e5d7a21b1");
	CHECK_EQ(
		HashId(key, sizeof(key), (uint64_t(2) << 48) | 1234567890),
		"h:ded9f4c600ec87d6");
	CHECK_EQ(
		HashId(key, sizeof(key), UINT64_MAX),
		"h:350af12a8b912a3a");

	auto formatter = IdFormatter();
	formatter.setKey(key, sizeof(key));
	CHECK_EQ(formatter.format(123456789), "h:a2fb4a2e5d7a21b1");
	CHECK_EQ(formatter.format(123456789), "h:a2fb4a2e5d7a21b1"); // cached
	formatter.setRaw(true);
	CHECK_EQ(formatter.format(123456789), "123456789");

	unsigned char a[kIdKeySize] = {};
	unsigned char b[kIdKeySize] = {};
	GenerateKey(a, sizeof(a));
	GenerateKey(b, sizeof(b));
	CHECK(std::memcmp(a, b, sizeof(a)) != 0);
}

void TestStamp() {
	auto clock = StampClock();
	CHECK(!clock.compute(1.0));
	clock.enter(7, 1.0);
	auto stamp = clock.compute(1.1);
	CHECK(stamp && stamp->a == 7 && std::string(stamp->rel) == "sync");
	CHECK(stamp && stamp->dt == -1);
	// Nested loop: an inner scope wins, then the outer one is back.
	clock.enter(8, 1.2);
	stamp = clock.compute(1.25);
	CHECK(stamp && stamp->a == 8);
	clock.leave(1.3);
	stamp = clock.compute(1.35);
	CHECK(stamp && stamp->a == 7 && std::string(stamp->rel) == "sync");
	// The outer scope is older than 500 ms: downgraded to "after".
	stamp = clock.compute(1.6);
	CHECK(stamp && stamp->a == 7 && std::string(stamp->rel) == "after");
	CHECK(stamp && stamp->dt == 600);
	clock.leave(2.0);
	stamp = clock.compute(2.1);
	CHECK(stamp && stamp->a == 7 && std::string(stamp->rel) == "deferred");
	CHECK(stamp && stamp->dt == 100);
	stamp = clock.compute(2.15);
	CHECK(stamp && std::string(stamp->rel) == "deferred" && stamp->dt == 150);
	stamp = clock.compute(2.151);
	CHECK(stamp && std::string(stamp->rel) == "after" && stamp->dt == 151);
	stamp = clock.compute(5.0);
	CHECK(stamp && std::string(stamp->rel) == "after" && stamp->dt == 3000);
	CHECK(!clock.compute(5.002));

	auto out = std::string();
	CHECK(!clock.append(out, 10.));
	CHECK(out.empty());
	clock.mark(9, 10.);
	CHECK(clock.append(out, 10.));
	CHECK_EQ(out, ",\"ui\":{\"a\":9,\"rel\":\"deferred\",\"dt\":0}");
	out.clear();
	clock.enter(10, 11.);
	CHECK(clock.append(out, 11.));
	CHECK_EQ(out, ",\"ui\":{\"a\":10,\"rel\":\"sync\"}");
	clock.leave(11.);

	auto options = StampOptions();
	options.afterMs = 10000.;
	clock.setOptions(options);
	stamp = clock.compute(20.);
	CHECK(stamp && stamp->a == 10 && std::string(stamp->rel) == "after");
	CHECK(!clock.compute(21.01));
}

void TestScroll() {
	auto scroll = ScrollCoalescer(400.);
	int areaA = 0, areaB = 0;
	auto r = scroll.wheel(&areaA, 1.0, -120, 0);
	CHECK(r.begin && r.a && !r.ended);
	const auto first = r.a;
	for (auto i = 1; i != 14; ++i) {
		r = scroll.wheel(&areaA, 1.0 + i * 0.09, -120, 0);
		CHECK(!r.begin && r.a == first && !r.ended);
	}
	CHECK(!scroll.poll(1.0 + 13 * 0.09 + 0.3));
	auto end = scroll.poll(1.0 + 13 * 0.09 + 0.401);
	CHECK(end && end->a == first && end->n == 14);
	CHECK(end && end->dy == -1680 && end->dx == 0 && end->ms == 1170);
	CHECK(!scroll.active());
	// Another area ends the gesture at once.
	r = scroll.wheel(&areaA, 5.0, 120, 0);
	const auto second = r.a;
	CHECK(r.begin && second != first);
	r = scroll.wheel(&areaB, 5.1, 0, 120);
	CHECK(r.begin && r.ended && r.ended->a == second && r.ended->n == 1);
	CHECK(r.ended && r.ended->dy == 120);
	end = scroll.flush();
	CHECK(end && end->dx == 120 && end->ms == 0);
	CHECK(!scroll.flush());
	// A gap longer than the quiet time starts a new gesture.
	r = scroll.wheel(&areaA, 9.0, 1, 0);
	const auto third = r.a;
	r = scroll.wheel(&areaA, 9.5, 1, 0);
	CHECK(r.begin && r.a != third && r.ended && r.ended->a == third);
}

void TestTyping() {
	auto typing = TypeCoalescer(1500.);
	int field = 0, other = 0;
	auto r = typing.key(&field, 1.0, false, false, false);
	CHECK(!r.begin && !r.a); // Backspace outside a span.
	r = typing.key(&field, 1.0, false, true);
	CHECK(r.begin && r.a);
	const auto span = r.a;
	for (auto i = 1; i != 6; ++i) {
		r = typing.key(&field, 1.0 + i * 0.2, true, false);
		CHECK(!r.begin && r.a == span);
	}
	r = typing.key(&field, 2.2, false, false, false); // Backspace counts.
	CHECK(r.a == span);
	CHECK(typing.field() == &field);
	CHECK(!typing.poll(3.6));
	auto end = typing.poll(3.7);
	CHECK(end && end->a == span && end->n == 7 && end->ms == 1200);
	CHECK(end && !end->masked && end->search); // From the span start.
	r = typing.key(&field, 4.0, true, false);
	const auto masked = r.a;
	CHECK(r.begin);
	r = typing.key(&other, 4.1, false, false);
	CHECK(r.begin && r.ended && r.ended->a == masked && r.ended->masked);
	CHECK(r.ended && r.ended->n == 1);
	end = typing.flush();
	CHECK(end && end->n == 1);
	CHECK(!typing.active() && !typing.field());
}

void TestRepeat() {
	auto repeat = RepeatCoalescer(400.);
	auto r = repeat.key(0x01000013, 0, false, 1.0); // Up
	CHECK(r.emit && r.a && !r.ended);
	const auto up = r.a;
	for (auto i = 1; i != 10; ++i) {
		r = repeat.key(0x01000013, 0, true, 1.0 + i * 0.03);
		CHECK(!r.emit && r.a == up);
	}
	r = repeat.key(0x01000015, 0, false, 1.5); // Down
	CHECK(r.emit && r.a != up && r.ended);
	CHECK(r.ended && r.ended->a == up && r.ended->repeat == 9);
	CHECK(r.ended && r.ended->ms == 270);
	// No repeats, no ui.key.end.
	CHECK(!repeat.poll(2.0));
	CHECK(!repeat.active());
	r = repeat.key(0x01000015, 0, false, 3.0);
	const auto down = r.a;
	r = repeat.key(0x01000015, 0, false, 3.1); // Not auto-repeat: new key.
	CHECK(r.emit && r.a != down && !r.ended);
	r = repeat.key(0x01000015, 0, true, 3.2);
	CHECK(!r.emit);
	auto end = repeat.poll(3.61);
	CHECK(end && end->repeat == 1);
}

void TestMarkers() {
	auto carry = std::string();
	auto lines = TakeMarkerLines(carry, "first\nsecond\r\n\n  \npart");
	CHECK_EQ(lines.size(), size_t(2));
	if (lines.size() == 2) {
		CHECK_EQ(lines[0], "first");
		CHECK_EQ(lines[1], "second");
	}
	CHECK_EQ(carry, "part");
	lines = TakeMarkerLines(carry, "ial\tline\n");
	CHECK_EQ(lines.size(), size_t(1));
	if (!lines.empty()) {
		CHECK_EQ(lines[0], "partial line");
	}
	CHECK(carry.empty());

	CHECK_EQ(CleanLabel("  a\x01" "b  "), "a b");
	CHECK_EQ(CleanLabel("\xff" "x"), "?x");
	CHECK_EQ(CleanLabel("\xd0\x9f\xd1\x80"), "\xd0\x9f\xd1\x80"); // UTF-8
	CHECK_EQ(CleanLabel("\xd0"), "?"); // Truncated sequence.
	auto longLabel = std::string();
	for (auto i = 0; i != 300; ++i) {
		longLabel += "\xe2\x82\xac"; // 3-byte code point
	}
	const auto capped = CleanLabel(longLabel);
	CHECK_EQ(capped.size(), size_t(kMaxLabel * 3));

	carry.clear();
	lines = TakeMarkerLines(carry, std::string(10000, 'x'));
	CHECK(lines.empty());
	CHECK_EQ(carry.size(), size_t(4096));
}

void TestRecords(const std::string &directory) {
	unsigned char key[kIdKeySize];
	for (auto i = 0; i != int(kIdKeySize); ++i) {
		key[i] = static_cast<unsigned char>(i);
	}
	Ids().setRaw(false);
	Ids().setKey(key, sizeof(key));

	auto error = std::string();
	CHECK(WriteKeyFile(directory, key, sizeof(key), error));
	{
		auto in = std::ifstream(directory + "/ui.key");
		auto text = std::string();
		std::getline(in, text);
		CHECK_EQ(text, "000102030405060708090a0b0c0d0e0f"
			"101112131415161718191a1b1c1d1e1f");
		struct stat st = {};
		CHECK(::stat((directory + "/ui.key").c_str(), &st) == 0);
		CHECK_EQ(int(st.st_mode & 0777), 0600);
	}

	auto record = Record();
	record.num("a", 41).num("w", 0).str("btn", "left");
	record.list("path", { "Dialogs::InnerWidget", "Ui::ScrollArea" });
	CHECK_EQ(record.data(), ",\"a\":41,\"w\":0,\"btn\":\"left\","
		"\"path\":[\"Dialogs::InnerWidget\",\"Ui::ScrollArea\"]");

	auto chat = Record();
	chat.num("w", 0).id("peer", 123456789).str("peer_kind", "user");
	chat.num("topic", 0).boolean("sublist", false).num("msg", 0);
	chat.null("none").str("label", "quote \" and \\ and \n");
	auto stamp = std::string();
	AppendStamp(stamp, Stamp{ 41, "deferred", 0 });
	chat.raw(stamp);
	CHECK_EQ(chat.data(), ",\"w\":0,\"peer\":\"h:a2fb4a2e5d7a21b1\","
		"\"peer_kind\":\"user\",\"topic\":0,\"sublist\":false,\"msg\":0,"
		"\"none\":null,\"label\":\"quote \\\" and \\\\ and \\n\","
		"\"ui\":{\"a\":41,\"rel\":\"deferred\",\"dt\":0}");

	// Through the writer: one JSON object per line with seq / t / th.
	auto writer = Trace::details::Writer();
	auto options = Trace::details::WriterOptions();
	options.directory = directory + "/run";
	options.header = ",\"v\":1,\"ui\":{\"v\":1,\"ids\":\"hmac64\",\"raw_ids\":false}";
	CHECK(writer.start(std::move(options), error));
	writer.push("main", ",\"ev\":\"ui.press\"" + record.data(), nullptr);
	writer.push("main", ",\"ev\":\"ui.chat\"" + chat.data(), nullptr);
	writer.stop();
	auto in = std::ifstream(directory + "/run/events-0000.jsonl");
	auto lines = std::vector<std::string>();
	for (auto line = std::string(); std::getline(in, line);) {
		lines.push_back(line);
	}
	CHECK_EQ(lines.size(), size_t(3));
	if (lines.size() == 3) {
		CHECK(lines[0].find("\"ev\":\"hdr\"") != std::string::npos);
		CHECK(lines[0].find("\"ui\":{\"v\":1,\"ids\":\"hmac64\"") != std::string::npos);
		CHECK(lines[1].find("\"th\":\"main\",\"ev\":\"ui.press\",\"a\":41") != std::string::npos);
		CHECK(lines[2].find("\"peer\":\"h:a2fb4a2e5d7a21b1\"") != std::string::npos);
		CHECK(lines[2].back() == '}');
	}
	std::ofstream(directory + "/lines.jsonl") << lines[1] << "\n" << lines[2] << "\n";
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::cerr << "usage: " << argv[0] << " <temp dir>\n";
		return 2;
	}
	const auto directory = std::string(argv[1]);
	TestBuckets();
	TestTypeNames();
	TestIds();
	TestStamp();
	TestScroll();
	TestTyping();
	TestRepeat();
	TestMarkers();
	TestRecords(directory);
	CHECK(NextAction() > 0);
	if (Failures) {
		std::cerr << Failures << " check(s) failed\n";
		return 1;
	}
	std::cout << "trace_ui_core: all checks passed\n";
	return 0;
}
