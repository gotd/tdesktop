/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Internal interface of the gotd/tdesktop control server (control.cpp).
// Everything here runs on the main thread, except the frame encoders in
// control_recorder.cpp.

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <functional>

class QWidget;
class QWindow;

namespace Control {

using Reply = std::function<void(QJsonObject)>;

[[nodiscard]] QJsonObject Ok(QJsonObject fields = {});
[[nodiscard]] QJsonObject Fail(const QString &error);

// Seconds on the wire trace clock ("t" in events-NNNN.jsonl) when the
// trace module is compiled in, else seconds since process start.
[[nodiscard]] double TraceNow();

// <working dir>/control/, created with mode 0700.
[[nodiscard]] QString OutputDir();
// Creates the directories of |path| (each new one 0700). False on error.
bool EnsureParentDir(const QString &path, QString *error);
bool EnsureDir(const QString &path, QString *error);
// Writes |data| into a new 0600 file.
bool WritePrivateFile(
	const QString &path,
	const QByteArray &data,
	QString *error);
[[nodiscard]] QString AbsolutePath(const QString &path);

// Widgets (control_widgets.cpp).
[[nodiscard]] QString TypeName(const QObject *object);
[[nodiscard]] QJsonArray Windows();
[[nodiscard]] int WindowId(QWidget *window);
// "window" argument (id) or the active / primary window.
[[nodiscard]] QWidget *ResolveWindow(
	const QJsonObject &args,
	QString *error);
// "path" argument: "<window id>/<child index>/...".
[[nodiscard]] QWidget *ResolvePath(const QString &path, QString *error);
[[nodiscard]] QString PathOf(QWidget *widget);
// Finds the target of an input command: "path" (+ "item"), "find" (an
// object with find arguments), or "x"/"y" in "window" coordinates.
// Returns the top-level widget and the point in its coordinates.
struct Target {
	QPointer<QWidget> window;
	QPoint point;
	QPointer<QWidget> widget; // When resolved by path or find.
};
[[nodiscard]] bool ResolveTarget(
	const QJsonObject &args,
	Target *target,
	QString *error);
[[nodiscard]] QJsonObject Tree(const QJsonObject &args, QString *error);
[[nodiscard]] QJsonArray Find(const QJsonObject &args, int limit);
[[nodiscard]] QString WidgetText(QWidget *widget);

// Input (control_widgets.cpp).
void Activate(QWidget *window);
bool Move(const Target &target, Qt::KeyboardModifiers mods);
bool Click(
	const Target &target,
	Qt::MouseButton button,
	Qt::KeyboardModifiers mods,
	int count);
bool Press(
	const Target &target,
	Qt::MouseButton button,
	Qt::KeyboardModifiers mods,
	bool down);
bool Wheel(
	const Target &target,
	QPoint pixelDelta,
	QPoint angleDelta,
	Qt::KeyboardModifiers mods);
bool Key(
	QWidget *window,
	const QString &sequence,
	QString *error);
bool TypeText(
	QWidget *window,
	const QString &text,
	bool asKeys,
	QString *error);
[[nodiscard]] Qt::KeyboardModifiers ParseModifiers(const QJsonValue &value);

// Capture (control_widgets.cpp).
enum class Source {
	Auto,
	Grab,
	BackingStore,
};
[[nodiscard]] Source ParseSource(const QString &value, bool *ok);
[[nodiscard]] QString SourceName(Source source);
// Renders |window| (with the visible popups over it when |composite|).
[[nodiscard]] QImage Capture(
	QWidget *window,
	Source source,
	bool composite,
	Source *used);
[[nodiscard]] int CountGlWidgets(QWidget *window);

// Paint activity, for "wait idle" (control.cpp).
[[nodiscard]] double LastPaintAgo();

// Recording (control_recorder.cpp).
struct RecordOptions {
	QString dir;
	QPointer<QWidget> window;
	int fps = 30;
	int quality = 80;
	double scale = 1.;
	bool composite = true;
	Source source = Source::Auto;
	int queue = 16;
	int threads = 4;
	double maxSeconds = 600.;
};
bool RecordStart(RecordOptions options, QString *error);
[[nodiscard]] bool RecordActive();
[[nodiscard]] QJsonObject RecordStatus();
// Stops the timer, drains the encoders and calls |done| with stats.
void RecordStop(Reply done);
void RecordMarker(const QString &label, double t, int n);

// Application semantics (control_app.cpp).
[[nodiscard]] QJsonObject State(const QJsonObject &args);
[[nodiscard]] QJsonObject OpenPeer(const QJsonObject &args);
[[nodiscard]] QJsonObject Back(const QJsonObject &args);
[[nodiscard]] QJsonObject Shortcut(const QJsonObject &args);
[[nodiscard]] QJsonObject Chats(const QJsonObject &args);
[[nodiscard]] QJsonObject Quit(const QJsonObject &args);
[[nodiscard]] bool Authorized();
[[nodiscard]] QWidget *PrimaryWindow();

} // namespace Control
