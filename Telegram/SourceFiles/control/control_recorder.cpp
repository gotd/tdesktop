/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
// Frame recording for the control server.
//
// A precise QTimer on the main thread captures the window at the requested
// fps; worker threads encode the frames as JPEG into <dir>/frames/. Each
// captured frame gets a line in <dir>/frames.jsonl with its trace clock
// time, so frames can be lined up with the wire trace. The queue is
// bounded: when the encoders fall behind, frames are dropped and counted.
#include "control/control_internal.h"

#include <QtCore/QBuffer>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtGui/QImageWriter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Control {
namespace {

constexpr auto kGlCheckMs = 1000;

struct Job {
	int index = 0;
	QImage image;
};

class Recorder final {
public:
	Recorder(RecordOptions options) : _options(std::move(options)) {
	}

	~Recorder() {
		finish();
	}

	bool start(QString *error) {
		if (!EnsureDir(_options.dir + u"/frames"_q, error)) {
			return false;
		}
		_jsonl.setFileName(_options.dir + u"/frames.jsonl"_q);
		if (!WritePrivateFile(_jsonl.fileName(), QByteArray(), error)
			|| !_jsonl.open(QIODevice::WriteOnly | QIODevice::Append)) {
			*error = u"could not create frames.jsonl"_q;
			return false;
		}
		for (auto i = 0; i != _options.threads; ++i) {
			_threads.emplace_back([this] { work(); });
		}
		_elapsed.start();
		_startT = TraceNow();
		_timer.setTimerType(Qt::PreciseTimer);
		QObject::connect(&_timer, &QTimer::timeout, [this] { tick(); });
		_timer.start(std::max(1, int(std::lround(1000. / _options.fps))));
		writeLine(QJsonObject{
			{ u"start"_q, true },
			{ u"t"_q, _startT },
			{ u"fps"_q, _options.fps },
			{ u"quality"_q, _options.quality },
			{ u"scale"_q, _options.scale },
			{ u"source"_q, SourceName(_options.source) },
			{ u"window"_q, WindowId(_options.window) },
		});
		tick();
		return true;
	}

	[[nodiscard]] QJsonObject status() const {
		return QJsonObject{
			{ u"dir"_q, _options.dir },
			{ u"fps_target"_q, _options.fps },
			{ u"frames"_q, _captured },
			{ u"dropped"_q, _dropped },
			{ u"elapsed_s"_q, _elapsed.isValid()
				? (_elapsed.nsecsElapsed() / 1e9)
				: 0. },
		};
	}

	void marker(const QString &label, double t, int n) {
		writeLine(QJsonObject{
			{ u"marker"_q, label },
			{ u"n"_q, n },
			{ u"t"_q, t },
			{ u"ms"_q, _elapsed.nsecsElapsed() / 1e6 },
		});
	}

	QJsonObject stop() {
		_timer.stop();
		const auto duration = _elapsed.nsecsElapsed() / 1e9;
		finish();
		const auto frames = _captured;
		writeLine(QJsonObject{
			{ u"stop"_q, true },
			{ u"t"_q, TraceNow() },
			{ u"frames"_q, frames },
			{ u"dropped"_q, _dropped },
		});
		_jsonl.close();
		const auto encoded = _encoded.load();
		auto result = QJsonObject{
			{ u"dir"_q, _options.dir },
			{ u"frames"_q, frames },
			{ u"encoded"_q, encoded },
			{ u"dropped"_q, _dropped },
			{ u"failed"_q, _failed.load() },
			{ u"duration_s"_q, duration },
			{ u"fps_target"_q, _options.fps },
			{ u"fps"_q, duration > 0. ? (frames / duration) : 0. },
			{ u"capture_ms_avg"_q, frames ? (_captureMs / frames) : 0. },
			{ u"capture_ms_max"_q, _captureMsMax },
			{ u"encode_ms_avg"_q, encoded
				? (_encodeUs.load() / 1000. / encoded)
				: 0. },
			{ u"bytes"_q, double(_bytes.load()) },
			{ u"width"_q, _width },
			{ u"height"_q, _height },
			{ u"source"_q, SourceName(_used) },
			{ u"t_start"_q, _startT },
		};
		return result;
	}

private:
	void tick() {
		const auto window = _options.window.data();
		const auto ms = _elapsed.nsecsElapsed() / 1e6;
		if (!window || ms / 1000. > _options.maxSeconds) {
			_timer.stop();
			return;
		}
		if (!window->isVisible()) {
			return;
		}
		if (_options.source == Source::Auto
			&& (!_glChecked.isValid() || _glChecked.elapsed() > kGlCheckMs)) {
			_glChecked.start();
			_auto = CountGlWidgets(window) ? Source::Grab : Source::BackingStore;
		}
		const auto t = TraceNow();
		auto timer = QElapsedTimer();
		timer.start();
		auto used = Source::Auto;
		auto image = Capture(
			window,
			(_options.source == Source::Auto) ? _auto : _options.source,
			_options.composite,
			&used);
		const auto captureMs = timer.nsecsElapsed() / 1e6;
		if (image.isNull()) {
			return;
		}
		_used = used;
		_captureMs += captureMs;
		_captureMsMax = std::max(_captureMsMax, captureMs);
		const auto geometry = window->geometry();
		{
			auto lock = std::unique_lock(_mutex);
			if (int(_queue.size()) >= _options.queue) {
				lock.unlock();
				++_dropped;
				writeLine(QJsonObject{
					{ u"drop"_q, _dropped },
					{ u"t"_q, t },
					{ u"ms"_q, ms },
				});
				return;
			}
		}
		const auto index = _captured++;
		const auto width = (_options.scale < 1.)
			? int(std::lround(image.width() * _options.scale))
			: image.width();
		const auto height = (_options.scale < 1.)
			? int(std::lround(image.height() * _options.scale))
			: image.height();
		_width = width;
		_height = height;
		writeLine(QJsonObject{
			{ u"i"_q, index },
			{ u"t"_q, t },
			{ u"ms"_q, ms },
			{ u"file"_q, FrameName(index) },
			{ u"geometry"_q, QJsonArray{
				geometry.x(),
				geometry.y(),
				geometry.width(),
				geometry.height() } },
			{ u"dpr"_q, image.devicePixelRatio() },
			{ u"w"_q, width },
			{ u"h"_q, height },
			{ u"cap_ms"_q, captureMs },
		});
		{
			auto lock = std::unique_lock(_mutex);
			_queue.push_back({ index, std::move(image) });
		}
		_cv.notify_one();
	}

	[[nodiscard]] static QString FrameName(int index) {
		return u"frames/%1.jpg"_q.arg(index, 6, 10, QChar('0'));
	}

	void work() {
		while (true) {
			auto job = Job();
			{
				auto lock = std::unique_lock(_mutex);
				_cv.wait(lock, [&] { return _stopping || !_queue.empty(); });
				if (_queue.empty()) {
					return;
				}
				job = std::move(_queue.front());
				_queue.pop_front();
			}
			auto timer = QElapsedTimer();
			timer.start();
			auto image = std::move(job.image);
			if (_options.scale < 1.) {
				image = image.scaled(
					image.size() * _options.scale,
					Qt::IgnoreAspectRatio,
					Qt::SmoothTransformation);
			}
			image = image.convertToFormat(QImage::Format_RGB888);
			auto bytes = QByteArray();
			{
				auto buffer = QBuffer(&bytes);
				buffer.open(QIODevice::WriteOnly);
				auto writer = QImageWriter(&buffer, "JPG");
				writer.setQuality(_options.quality);
				if (!writer.write(image)) {
					++_failed;
					continue;
				}
			}
			const auto path = _options.dir + '/' + FrameName(job.index);
			if (!WritePrivateFile(path, bytes, nullptr)) {
				++_failed;
				continue;
			}
			_bytes += bytes.size();
			_encodeUs += timer.nsecsElapsed() / 1000;
			++_encoded;
		}
	}

	void finish() {
		{
			auto lock = std::unique_lock(_mutex);
			_stopping = true;
		}
		_cv.notify_all();
		for (auto &thread : _threads) {
			if (thread.joinable()) {
				thread.join();
			}
		}
		_threads.clear();
	}

	void writeLine(const QJsonObject &object) {
		if (_jsonl.isOpen()) {
			_jsonl.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
			_jsonl.write("\n", 1);
			_jsonl.flush();
		}
	}

	RecordOptions _options;
	QTimer _timer;
	QElapsedTimer _elapsed;
	QElapsedTimer _glChecked;
	Source _auto = Source::BackingStore;
	Source _used = Source::Auto;
	double _startT = 0.;
	QFile _jsonl;

	int _captured = 0;
	int _dropped = 0;
	double _captureMs = 0.;
	double _captureMsMax = 0.;
	int _width = 0;
	int _height = 0;

	std::mutex _mutex;
	std::condition_variable _cv;
	std::deque<Job> _queue;
	bool _stopping = false;
	std::vector<std::thread> _threads;
	std::atomic<int> _encoded = 0;
	std::atomic<int> _failed = 0;
	std::atomic<int64_t> _bytes = 0;
	std::atomic<int64_t> _encodeUs = 0;

};

std::unique_ptr<Recorder> Active;

} // namespace

bool RecordStart(RecordOptions options, QString *error) {
	if (Active) {
		*error = u"already recording"_q;
		return false;
	}
	auto recorder = std::make_unique<Recorder>(std::move(options));
	if (!recorder->start(error)) {
		return false;
	}
	Active = std::move(recorder);
	return true;
}

bool RecordActive() {
	return Active != nullptr;
}

QJsonObject RecordStatus() {
	return Active ? Active->status() : QJsonObject{
		{ u"recording"_q, false },
	};
}

void RecordStop(Reply done) {
	if (!Active) {
		done(Fail(u"not recording"_q));
		return;
	}
	// Joining the encoders blocks the main thread for at most a queue of
	// frames, a few hundred milliseconds.
	const auto recorder = std::move(Active);
	done(recorder->stop());
}

void RecordMarker(const QString &label, double t, int n) {
	if (Active) {
		Active->marker(label, t, n);
	}
}

} // namespace Control
