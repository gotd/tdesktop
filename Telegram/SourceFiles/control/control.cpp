/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
// Local control server for the gotd/tdesktop fork (docs/control.md).
//
// Opt-in only: it starts when TDESKTOP_CONTROL=<unix socket path> is set
// or "-control <path>" is passed, and does nothing otherwise. It listens
// on a unix socket (0600, in a 0700 directory) and serves newline
// delimited JSON requests on the main thread, so an agent can drive the
// client (input, navigation), read its widget tree and capture frames.
// Nothing here talks to the network.
//
// There is no hook in upstream code: Q_COREAPP_STARTUP_FUNCTION runs
// Startup() when the QApplication is constructed.
#include "control/control_internal.h"

#include "core/launcher.h"
#include "logs.h"
#include "settings.h"

#ifdef TDESKTOP_WIRE_TRACE
#include "trace/trace.h"
#include "trace/trace_writer.h"
#endif // TDESKTOP_WIRE_TRACE

#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>
#include <QtWidgets/QApplication>

#include <chrono>
#include <deque>
#include <map>
#include <memory>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif // !_WIN32

#ifdef TDESKTOP_CONTROL_OFFSCREEN
#include <QtCore/QtPlugin>
// The static Qt build has only the xcb and wayland platforms imported.
// "offscreen" makes QT_QPA_PLATFORM=offscreen work for headless runs.
Q_IMPORT_PLUGIN(QOffscreenIntegrationPlugin)
#endif // TDESKTOP_CONTROL_OFFSCREEN

namespace Control {
namespace {

constexpr auto kMaxLine = 1024 * 1024;
constexpr auto kPollMs = 20;

const auto kStart = std::chrono::steady_clock::now();

using Handler = void(*)(const QJsonObject &args, Reply reply);

[[nodiscard]] const std::map<QString, Handler> &Handlers();

class PaintWatcher final : public QObject {
public:
	PaintWatcher() {
		_timer.start();
	}

	[[nodiscard]] double ago() const {
		return (_timer.nsecsElapsed() - _last) / 1e6;
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		switch (event->type()) {
		case QEvent::Paint:
		case QEvent::UpdateRequest:
		case QEvent::UpdateLater:
			_last = _timer.nsecsElapsed();
			break;
		default:
			break;
		}
		return false;
	}

private:
	QElapsedTimer _timer;
	qint64 _last = 0;

};

PaintWatcher *Watcher = nullptr;

class Connection final : public QObject {
public:
	explicit Connection(QLocalSocket *socket) : _socket(socket) {
		_socket->setParent(this);
		connect(_socket, &QLocalSocket::readyRead, this, [this] {
			read();
		});
		connect(_socket, &QLocalSocket::disconnected, this, [this] {
			deleteLater();
		});
	}

private:
	void read() {
		_buffer += _socket->readAll();
		while (true) {
			const auto end = _buffer.indexOf('\n');
			if (end < 0) {
				if (_buffer.size() > kMaxLine) {
					send(Fail(u"line too long"_q));
					_buffer.clear();
				}
				break;
			}
			auto line = _buffer.left(end).trimmed();
			_buffer.remove(0, end + 1);
			if (!line.isEmpty()) {
				_queue.push_back(std::move(line));
			}
		}
		next();
	}

	void next() {
		if (_busy || _queue.empty()) {
			return;
		}
		_busy = true;
		const auto line = std::move(_queue.front());
		_queue.pop_front();

		auto error = QJsonParseError();
		const auto document = QJsonDocument::fromJson(line, &error);
		if (error.error != QJsonParseError::NoError || !document.isObject()) {
			finish({}, Fail(u"bad json: "_q + error.errorString()));
			return;
		}
		const auto args = document.object();
		const auto id = args.value(u"id"_q);
		const auto cmd = args.value(u"cmd"_q).toString();
		const auto i = Handlers().find(cmd);
		if (i == Handlers().end()) {
			finish(id, Fail(cmd.isEmpty()
				? u"missing cmd"_q
				: (u"unknown cmd: "_q + cmd)));
			return;
		}
		const auto weak = QPointer<Connection>(this);
		const auto replied = std::make_shared<bool>(false);
		i->second(args, [=](QJsonObject result) {
			if (*replied) {
				return;
			}
			*replied = true;
			if (const auto strong = weak.data()) {
				strong->finish(id, std::move(result));
			}
		});
	}

	void finish(const QJsonValue &id, QJsonObject result) {
		if (!id.isUndefined() && !id.isNull()) {
			result.insert(u"id"_q, id);
		}
		send(result);
		_busy = false;
		// Don't recurse: the next request runs from the event loop.
		QTimer::singleShot(0, this, [this] { next(); });
	}

	void send(const QJsonObject &result) {
		auto data = QJsonDocument(result).toJson(QJsonDocument::Compact);
		data += '\n';
		_socket->write(data);
		_socket->flush();
	}

	QLocalSocket *_socket = nullptr;
	QByteArray _buffer;
	std::deque<QByteArray> _queue;
	bool _busy = false;

};

class Server final : public QObject {
public:
	bool listen(const QString &path, QString *error) {
		if (!EnsureParentDir(path, error)) {
			return false;
		}
#ifndef _WIN32
		const auto native = QFile::encodeName(path);
		struct stat st;
		if (::lstat(native.constData(), &st) == 0) {
			if (!S_ISSOCK(st.st_mode)) {
				*error = u"exists and is not a socket: "_q + path;
				return false;
			}
			QLocalServer::removeServer(path);
		}
#endif // !_WIN32
		_server.setSocketOptions(QLocalServer::UserAccessOption);
		if (!_server.listen(path)) {
			*error = _server.errorString();
			return false;
		}
#ifndef _WIN32
		::chmod(native.constData(), S_IRUSR | S_IWUSR);
#endif // !_WIN32
		connect(&_server, &QLocalServer::newConnection, this, [this] {
			while (const auto socket = _server.nextPendingConnection()) {
				new Connection(socket); // Owned by itself, deleted on close.
			}
		});
		_path = path;
		return true;
	}

	~Server() {
		_server.close();
		if (!_path.isEmpty()) {
			QLocalServer::removeServer(_path);
		}
	}

private:
	QLocalServer _server;
	QString _path;

};

Server *Instance = nullptr;
int MarkerCounter = 0;

[[nodiscard]] QString RequestedPath() {
	auto result = qEnvironmentVariable("TDESKTOP_CONTROL");
	const auto &arguments = Core::Launcher::Instance().arguments();
	const auto i = arguments.indexOf(u"-control"_q);
	if (i >= 0 && i + 1 < arguments.size()) {
		result = arguments[i + 1];
	}
	return result;
}

// The launcher treats unknown arguments as links / files to open, so
// "-control <path>" would end up in the share box. Take them out.
void DropFromStartUrls(const QString &path) {
	const auto &arguments = Core::Launcher::Instance().arguments();
	const auto i = arguments.indexOf(u"-control"_q);
	if (i < 0) {
		return;
	}
	const auto flag = QUrl::fromUserInput(u"-control"_q);
	const auto absolute = QFileInfo(path).absoluteFilePath();
	auto &urls = cRefStartUrls();
	urls.erase(std::remove_if(urls.begin(), urls.end(), [&](const QUrl &url) {
		if (url == flag) {
			return true;
		} else if (!url.isLocalFile()) {
			return false;
		}
		const auto local = url.toLocalFile();
		return (local == path)
			|| (QFileInfo(local).absoluteFilePath() == absolute)
			|| (QFileInfo(local).fileName() == QFileInfo(path).fileName());
	}), urls.end());
}

void StartServer(const QString &path) {
	if (Instance) {
		return;
	}
	auto error = QString();
	auto server = std::make_unique<Server>();
	const auto absolute = AbsolutePath(path);
	if (!server->listen(absolute, &error)) {
		LOG(("Control: could not listen on '%1': %2").arg(absolute, error));
		return;
	}
	Instance = server.release();
	Watcher = new PaintWatcher();
	qApp->installEventFilter(Watcher);
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] {
		if (RecordActive()) {
			RecordStop([](QJsonObject) {});
		}
		delete std::exchange(Instance, nullptr);
	});
	LOG(("Control: listening on '%1' (platform: %2).").arg(
		absolute,
		QGuiApplication::platformName()));
}

void Startup() {
	const auto path = RequestedPath();
	if (path.isEmpty()) {
		return;
	}
	DropFromStartUrls(path);
	// The QApplication is still being constructed here.
	QTimer::singleShot(0, [=] { StartServer(path); });
}

// Commands.

void CmdPing(const QJsonObject &args, Reply reply) {
	reply(Ok({
		{ u"pong"_q, true },
		{ u"t"_q, TraceNow() },
		{ u"pid"_q, QCoreApplication::applicationPid() },
	}));
}

void CmdHelp(const QJsonObject &args, Reply reply) {
	auto list = QJsonArray();
	for (const auto &[name, handler] : Handlers()) {
		list.push_back(name);
	}
	reply(Ok({ { u"commands"_q, list } }));
}

void CmdState(const QJsonObject &args, Reply reply) {
	reply(State(args));
}

void CmdWindows(const QJsonObject &args, Reply reply) {
	reply(Ok({ { u"windows"_q, Windows() } }));
}

void CmdTree(const QJsonObject &args, Reply reply) {
	auto error = QString();
	const auto result = Tree(args, &error);
	reply(error.isEmpty() ? Ok(result) : Fail(error));
}

void CmdFind(const QJsonObject &args, Reply reply) {
	const auto limit = args.value(u"limit"_q).toInt(50);
	const auto found = Find(args, limit);
	reply(Ok({ { u"count"_q, found.size() }, { u"widgets"_q, found } }));
}

[[nodiscard]] QJsonObject TargetJson(const Target &target) {
	auto result = QJsonObject{
		{ u"window"_q, WindowId(target.window) },
		{ u"x"_q, target.point.x() },
		{ u"y"_q, target.point.y() },
	};
	if (target.widget) {
		result.insert(u"path"_q, PathOf(target.widget));
		result.insert(u"type"_q, TypeName(target.widget));
	}
	return result;
}

[[nodiscard]] Qt::MouseButton ParseButton(const QString &value) {
	if (value == u"right"_q) {
		return Qt::RightButton;
	} else if (value == u"middle"_q) {
		return Qt::MiddleButton;
	}
	return Qt::LeftButton;
}

void CmdClick(const QJsonObject &args, Reply reply) {
	auto target = Target();
	auto error = QString();
	if (!ResolveTarget(args, &target, &error)) {
		reply(Fail(error));
		return;
	}
	const auto button = ParseButton(args.value(u"button"_q).toString());
	const auto mods = ParseModifiers(args.value(u"mods"_q));
	const auto count = std::clamp(args.value(u"count"_q).toInt(1), 1, 3);
	if (!Click(target, button, mods, count)) {
		reply(Fail(u"window has no native handle"_q));
		return;
	}
	reply(Ok(TargetJson(target)));
}

void CmdPress(const QJsonObject &args, Reply reply) {
	auto target = Target();
	auto error = QString();
	if (!ResolveTarget(args, &target, &error)) {
		reply(Fail(error));
		return;
	}
	const auto button = ParseButton(args.value(u"button"_q).toString());
	const auto mods = ParseModifiers(args.value(u"mods"_q));
	const auto down = (args.value(u"cmd"_q).toString() == u"press"_q);
	if (!Press(target, button, mods, down)) {
		reply(Fail(u"window has no native handle"_q));
		return;
	}
	reply(Ok(TargetJson(target)));
}

void CmdMove(const QJsonObject &args, Reply reply) {
	auto target = Target();
	auto error = QString();
	if (!ResolveTarget(args, &target, &error)) {
		reply(Fail(error));
		return;
	}
	if (!Move(target, ParseModifiers(args.value(u"mods"_q)))) {
		reply(Fail(u"window has no native handle"_q));
		return;
	}
	reply(Ok(TargetJson(target)));
}

void CmdScroll(const QJsonObject &args, Reply reply) {
	auto target = Target();
	auto error = QString();
	if (!ResolveTarget(args, &target, &error)) {
		reply(Fail(error));
		return;
	}
	// "dy"/"dx": pixels (a touchpad-like event with both deltas), or
	// "notches": wheel clicks of 120 angle units, no pixel delta.
	const auto notches = args.value(u"notches"_q).toDouble(0.);
	const auto dx = args.value(u"dx"_q).toInt(0);
	const auto dy = args.value(u"dy"_q).toInt(0);
	auto pixel = QPoint(dx, dy);
	auto angle = QPoint(dx, dy);
	if (notches != 0.) {
		pixel = QPoint();
		angle = QPoint(0, int(std::lround(notches * 120)));
	}
	if (pixel.isNull() && angle.isNull()) {
		reply(Fail(u"need dy, dx or notches"_q));
		return;
	}
	const auto steps = std::clamp(args.value(u"steps"_q).toInt(1), 1, 1000);
	const auto interval = std::clamp(
		args.value(u"interval_ms"_q).toInt(16),
		0,
		10000);
	const auto mods = ParseModifiers(args.value(u"mods"_q));
	if (!Move(target, mods)) {
		reply(Fail(u"window has no native handle"_q));
		return;
	}
	auto result = TargetJson(target);
	result.insert(u"steps"_q, steps);
	if (steps == 1) {
		Wheel(target, pixel, angle, mods);
		reply(Ok(result));
		return;
	}
	// Spread the deltas over the steps, the way a real wheel does.
	const auto left = std::make_shared<int>(steps);
	const auto timer = new QTimer(qApp);
	timer->setTimerType(Qt::PreciseTimer);
	const auto tick = [=] {
		if (!target.window || !*left) {
			timer->deleteLater();
			reply(target.window
				? Ok(result)
				: Fail(u"window closed while scrolling"_q));
			return;
		}
		--*left;
		Wheel(target, pixel, angle, mods);
	};
	QObject::connect(timer, &QTimer::timeout, tick);
	tick();
	timer->start(interval);
}

void CmdKey(const QJsonObject &args, Reply reply) {
	auto error = QString();
	const auto window = ResolveWindow(args, &error);
	if (!window) {
		reply(Fail(error));
		return;
	}
	const auto keys = args.value(u"key"_q).toString();
	if (!Key(window, keys, &error)) {
		reply(Fail(error));
		return;
	}
	reply(Ok({ { u"window"_q, WindowId(window) } }));
}

void CmdType(const QJsonObject &args, Reply reply) {
	auto error = QString();
	const auto window = ResolveWindow(args, &error);
	if (!window) {
		reply(Fail(error));
		return;
	}
	const auto text = args.value(u"text"_q).toString();
	const auto keys = (args.value(u"mode"_q).toString() == u"keys"_q);
	if (!TypeText(window, text, keys, &error)) {
		reply(Fail(error));
		return;
	}
	reply(Ok({
		{ u"window"_q, WindowId(window) },
		{ u"length"_q, text.size() },
	}));
}

void CmdActivate(const QJsonObject &args, Reply reply) {
	auto error = QString();
	const auto window = ResolveWindow(args, &error);
	if (!window) {
		reply(Fail(error));
		return;
	}
	Activate(window);
	reply(Ok({ { u"window"_q, WindowId(window) } }));
}

void CmdResize(const QJsonObject &args, Reply reply) {
	auto error = QString();
	const auto window = ResolveWindow(args, &error);
	if (!window) {
		reply(Fail(error));
		return;
	}
	const auto w = args.value(u"w"_q).toInt(window->width());
	const auto h = args.value(u"h"_q).toInt(window->height());
	if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
		reply(Fail(u"bad size"_q));
		return;
	}
	if (args.contains(u"x"_q) || args.contains(u"y"_q)) {
		window->move(
			args.value(u"x"_q).toInt(window->x()),
			args.value(u"y"_q).toInt(window->y()));
	}
	window->resize(w, h);
	const auto geometry = window->geometry();
	reply(Ok({
		{ u"window"_q, WindowId(window) },
		{ u"geometry"_q, QJsonArray{
			geometry.x(),
			geometry.y(),
			geometry.width(),
			geometry.height() } },
	}));
}

void CmdWait(const QJsonObject &args, Reply reply) {
	const auto what = args.value(u"for"_q).toString();
	const auto timeout = args.value(u"timeout_ms"_q).toInt(10000);
	if (what.isEmpty()) {
		const auto ms = std::clamp(args.value(u"ms"_q).toInt(0), 0, 3600000);
		QTimer::singleShot(ms, qApp, [=] {
			reply(Ok({ { u"waited_ms"_q, ms } }));
		});
		return;
	}
	auto check = std::function<bool()>();
	if (what == u"idle"_q) {
		// No widget painted for quiet_ms: animations and layout settled.
		const auto quiet = args.value(u"quiet_ms"_q).toInt(300);
		check = [=] { return LastPaintAgo() >= quiet; };
	} else if (what == u"authorized"_q) {
		check = [] { return Authorized(); };
	} else if (what == u"window"_q) {
		check = [] {
			const auto window = PrimaryWindow();
			return window && window->isVisible();
		};
	} else if (what == u"widget"_q || what == u"gone"_q) {
		const auto gone = (what == u"gone"_q);
		check = [=] { return Find(args, 1).isEmpty() == gone; };
	} else {
		reply(Fail(u"unknown wait target: "_q + what));
		return;
	}
	auto started = QElapsedTimer();
	started.start();
	if (check()) {
		reply(Ok({ { u"waited_ms"_q, 0 } }));
		return;
	}
	const auto timer = new QTimer(qApp);
	QObject::connect(timer, &QTimer::timeout, [=] {
		const auto elapsed = started.elapsed();
		if (check()) {
			timer->deleteLater();
			reply(Ok({ { u"waited_ms"_q, double(elapsed) } }));
		} else if (elapsed >= timeout) {
			timer->deleteLater();
			reply(Fail(u"timeout waiting for "_q + what));
		}
	});
	timer->start(kPollMs);
}

void CmdScreenshot(const QJsonObject &args, Reply reply) {
	auto error = QString();
	const auto window = ResolveWindow(args, &error);
	if (!window) {
		reply(Fail(error));
		return;
	}
	auto ok = true;
	const auto source = ParseSource(
		args.value(u"source"_q).toString(u"grab"_q),
		&ok);
	if (!ok) {
		reply(Fail(u"bad source"_q));
		return;
	}
	auto path = args.value(u"path"_q).toString();
	if (path.isEmpty()) {
		static auto counter = 0;
		path = OutputDir() + u"shots/shot-%1.png"_q.arg(
			++counter,
			4,
			10,
			QChar('0'));
	}
	path = AbsolutePath(path);
	auto timer = QElapsedTimer();
	timer.start();
	auto used = source;
	const auto image = Capture(
		window,
		source,
		args.value(u"composite"_q).toBool(true),
		&used);
	if (image.isNull()) {
		reply(Fail(u"capture failed"_q));
		return;
	}
	const auto captureMs = timer.nsecsElapsed() / 1e6;
	auto bytes = QByteArray();
	{
		auto buffer = QBuffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		const auto format = path.endsWith(u".jpg"_q, Qt::CaseInsensitive)
			|| path.endsWith(u".jpeg"_q, Qt::CaseInsensitive);
		image.save(
			&buffer,
			format ? "JPG" : "PNG",
			format ? args.value(u"quality"_q).toInt(90) : -1);
	}
	if (!EnsureParentDir(path, &error)
		|| !WritePrivateFile(path, bytes, &error)) {
		reply(Fail(error));
		return;
	}
	reply(Ok({
		{ u"path"_q, path },
		{ u"window"_q, WindowId(window) },
		{ u"width"_q, image.width() },
		{ u"height"_q, image.height() },
		{ u"dpr"_q, image.devicePixelRatio() },
		{ u"source"_q, SourceName(used) },
		{ u"bytes"_q, double(bytes.size()) },
		{ u"capture_ms"_q, captureMs },
		{ u"t"_q, TraceNow() },
	}));
}

void CmdRecordStart(const QJsonObject &args, Reply reply) {
	auto error = QString();
	auto options = RecordOptions();
	options.window = ResolveWindow(args, &error);
	if (!options.window) {
		reply(Fail(error));
		return;
	}
	auto ok = true;
	options.source = ParseSource(
		args.value(u"source"_q).toString(u"auto"_q),
		&ok);
	if (!ok) {
		reply(Fail(u"bad source"_q));
		return;
	}
	options.fps = std::clamp(args.value(u"fps"_q).toInt(30), 1, 60);
	options.quality = std::clamp(args.value(u"quality"_q).toInt(80), 1, 100);
	options.scale = std::clamp(args.value(u"scale"_q).toDouble(1.), 0.1, 1.);
	options.composite = args.value(u"composite"_q).toBool(true);
	options.queue = std::clamp(args.value(u"queue"_q).toInt(16), 1, 256);
	options.threads = std::clamp(args.value(u"threads"_q).toInt(4), 1, 8);
	options.maxSeconds = std::clamp(
		args.value(u"max_s"_q).toDouble(600.),
		0.1,
		3600.);
	auto dir = args.value(u"dir"_q).toString();
	if (dir.isEmpty()) {
		dir = OutputDir() + u"rec-%1"_q.arg(
			QDateTime::currentMSecsSinceEpoch());
	}
	options.dir = AbsolutePath(dir);
	if (!RecordStart(options, &error)) {
		reply(Fail(error));
		return;
	}
	auto result = RecordStatus();
	result.insert(u"window"_q, WindowId(options.window));
	reply(Ok(result));
}

void CmdRecordStop(const QJsonObject &args, Reply reply) {
	if (!RecordActive()) {
		reply(Fail(u"not recording"_q));
		return;
	}
	RecordStop([=](QJsonObject stats) {
		reply(Ok(stats));
	});
}

void CmdRecordStatus(const QJsonObject &args, Reply reply) {
	reply(Ok(RecordStatus()));
}

void CmdMarker(const QJsonObject &args, Reply reply) {
	const auto label = args.value(u"label"_q).toString().left(200);
	const auto n = ++MarkerCounter;
	const auto t = TraceNow();
	auto traced = false;
#ifdef TDESKTOP_WIRE_TRACE
	if (Trace::On()) {
		Trace::Ev("ui.marker", 0, {
			{ "n", n },
			{ "src", "control" },
			{ "label", label },
		});
		traced = true;
	}
#endif // TDESKTOP_WIRE_TRACE
	const auto recorded = RecordActive();
	if (recorded) {
		RecordMarker(label, t, n);
	}
	reply(Ok({
		{ u"n"_q, n },
		{ u"t"_q, t },
		{ u"trace"_q, traced },
		{ u"frames"_q, recorded },
	}));
}

void CmdOpenPeer(const QJsonObject &args, Reply reply) {
	reply(OpenPeer(args));
}

void CmdBack(const QJsonObject &args, Reply reply) {
	reply(Back(args));
}

void CmdShortcut(const QJsonObject &args, Reply reply) {
	reply(Shortcut(args));
}

void CmdChats(const QJsonObject &args, Reply reply) {
	reply(Chats(args));
}

void CmdQuit(const QJsonObject &args, Reply reply) {
	reply(Quit(args));
}

const std::map<QString, Handler> &Handlers() {
	static const auto result = std::map<QString, Handler>{
		{ u"ping"_q, CmdPing },
		{ u"help"_q, CmdHelp },
		{ u"state"_q, CmdState },
		{ u"windows"_q, CmdWindows },
		{ u"tree"_q, CmdTree },
		{ u"find"_q, CmdFind },
		{ u"click"_q, CmdClick },
		{ u"press"_q, CmdPress },
		{ u"release"_q, CmdPress },
		{ u"move"_q, CmdMove },
		{ u"scroll"_q, CmdScroll },
		{ u"key"_q, CmdKey },
		{ u"type"_q, CmdType },
		{ u"activate"_q, CmdActivate },
		{ u"resize"_q, CmdResize },
		{ u"wait"_q, CmdWait },
		{ u"screenshot"_q, CmdScreenshot },
		{ u"record_start"_q, CmdRecordStart },
		{ u"record_stop"_q, CmdRecordStop },
		{ u"record_status"_q, CmdRecordStatus },
		{ u"marker"_q, CmdMarker },
		{ u"open_peer"_q, CmdOpenPeer },
		{ u"back"_q, CmdBack },
		{ u"shortcut"_q, CmdShortcut },
		{ u"chats"_q, CmdChats },
		{ u"quit"_q, CmdQuit },
	};
	return result;
}

} // namespace

QJsonObject Ok(QJsonObject fields) {
	fields.insert(u"ok"_q, true);
	return fields;
}

QJsonObject Fail(const QString &error) {
	return QJsonObject{
		{ u"ok"_q, false },
		{ u"error"_q, error },
	};
}

double TraceNow() {
#ifdef TDESKTOP_WIRE_TRACE
	return Trace::details::Writer::Now();
#else // TDESKTOP_WIRE_TRACE
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now() - kStart).count();
	return double(ns) / 1e9;
#endif // TDESKTOP_WIRE_TRACE
}

double LastPaintAgo() {
	return Watcher ? Watcher->ago() : 0.;
}

QString AbsolutePath(const QString &path) {
	return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QString OutputDir() {
	const auto result = AbsolutePath(cWorkingDir() + u"control"_q) + '/';
	auto error = QString();
	EnsureDir(result, &error);
	return result;
}

bool EnsureDir(const QString &path, QString *error) {
	const auto absolute = AbsolutePath(path);
	if (QFileInfo(absolute).isDir()) {
		return true;
	}
	auto parent = QFileInfo(absolute).absolutePath();
	if (parent != absolute && !EnsureDir(parent, error)) {
		return false;
	}
#ifndef _WIN32
	if (::mkdir(QFile::encodeName(absolute).constData(), S_IRWXU) != 0
		&& errno != EEXIST) {
		*error = u"could not create directory: "_q + absolute;
		return false;
	}
	return true;
#else // !_WIN32
	if (!QDir().mkpath(absolute)) {
		*error = u"could not create directory: "_q + absolute;
		return false;
	}
	return true;
#endif // !_WIN32
}

bool EnsureParentDir(const QString &path, QString *error) {
	return EnsureDir(QFileInfo(AbsolutePath(path)).absolutePath(), error);
}

bool WritePrivateFile(
		const QString &path,
		const QByteArray &data,
		QString *error) {
#ifndef _WIN32
	const auto fd = ::open(
		QFile::encodeName(path).constData(),
		O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		S_IRUSR | S_IWUSR);
	if (fd < 0) {
		if (error) {
			*error = u"could not create file: "_q + path;
		}
		return false;
	}
	auto written = qint64(0);
	while (written < data.size()) {
		const auto chunk = ::write(
			fd,
			data.constData() + written,
			size_t(data.size() - written));
		if (chunk <= 0) {
			::close(fd);
			if (error) {
				*error = u"could not write file: "_q + path;
			}
			return false;
		}
		written += chunk;
	}
	::close(fd);
	return true;
#else // !_WIN32
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| file.write(data) != data.size()) {
		if (error) {
			*error = u"could not write file: "_q + path;
		}
		return false;
	}
	return true;
#endif // !_WIN32
}

} // namespace Control

namespace {

void ControlStartup() {
	Control::Startup();
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(ControlStartup)
