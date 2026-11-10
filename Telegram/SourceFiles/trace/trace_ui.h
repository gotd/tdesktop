/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// UI and user-action tracing (P14) for the gotd/tdesktop fork.
//
// Input events seen by Core::Sandbox::notify become ui.press / ui.click /
// ui.key / ui.scroll / ui.type / ui.menu records with an action id "a",
// and every request or storage event emitted on the main thread during
// or shortly after the action carries "ui":{a,rel,dt}. View state (open
// chat, section, box, media viewer, account, lock, idle, calls) comes from
// rpl producers subscribed lazily, with no hooks. Typed text, field
// contents, URLs, names and phones are never recorded; ids are per-run
// HMACs (key in <run>/ui.key). See trace/trace_ui.cpp.
//
// Hooks: MTP_WT_UI_SCOPE in Sandbox::notify, MTP_WT_UI(Shortcut(...)) in
// Shortcuts::RequestHandler (macros in trace/trace.h).

#include "trace/trace.h"

#include <QtCore/QEvent>
#include <QtCore/QString>

#include <vector>

class QObject;

namespace Trace::Ui {

[[nodiscard]] inline bool Observed(QEvent::Type type) {
	switch (type) {
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonRelease:
	case QEvent::MouseButtonDblClick:
	case QEvent::Wheel:
	case QEvent::KeyPress:
	case QEvent::Shortcut:
	case QEvent::TouchBegin:
	case QEvent::TouchEnd:
	case QEvent::NativeGesture:
	case QEvent::FocusIn:
	case QEvent::Show:
	case QEvent::Hide:
	case QEvent::WindowActivate:
	case QEvent::WindowDeactivate:
	case QEvent::WindowStateChange:
	case QEvent::ApplicationStateChange:
		return true;
	default:
		return false;
	}
}

// Wraps the synchronous dispatch of one observed event (main thread).
class InputScope final {
public:
	InputScope(QObject *receiver, QEvent *e);
	~InputScope();

	InputScope(const InputScope &) = delete;
	InputScope &operator=(const InputScope &) = delete;

private:
	friend void Shortcut(const std::vector<QString> &names);

	InputScope *_previous = nullptr;
	uint32_t _a = 0;
	bool _entered = false;
	bool _shortcutEvent = false;
	bool _shortcutRecorded = false;
	int _shortcutKey = 0;
	int _shortcutModifiers = 0;

};

// ui.shortcut with the command names (Shortcuts::RequestHandler).
void Shortcut(const std::vector<QString> &names);

template <typename Commands, typename Names>
void Shortcut(const Commands &commands, const Names &names) {
	if (commands.empty()) {
		return;
	}
	auto list = std::vector<QString>();
	list.reserve(commands.size());
	for (const auto &command : commands) {
		const auto i = names.find(command);
		list.push_back((i != names.end())
			? i->second
			: (QString(QChar(u'#')) + QString::number(int(command))));
	}
	Shortcut(list);
}

} // namespace Trace::Ui
