/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "trace/trace_ui.h"

#include "trace/trace_lang_names.h"
#include "trace/trace_ui_core.h"
#include "trace/trace_writer.h"
#include "api/api_updates.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "calls/calls_call.h"
#include "calls/calls_instance.h"
#include "calls/group/calls_group_call.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_photo.h"
#include "data/data_story.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "history/history.h"
#include "history/history_item.h"
#include "info/info_controller.h"
#include "info/info_wrap_widget.h"
#include "lang/lang_instance.h"
#include "lang_auto_counts.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "media/view/media_view_open_common.h"
#include "settings/settings_common_session.h"
#include "ui/click_handler.h"
#include "ui/layers/box_content.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/widgets/rp_window.h"
#include "ui/widgets/scroll_area.h"
#include "window/main_window.h"
#include "window/section_widget.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_widgets.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QWidget>

#include <array>
#include <cmath>
#include <limits>
#include <typeinfo>
#include <unordered_map>

// What is recorded, and what never is (P14.8):
// - Input: button, named keys with modifiers, wheel direction and totals,
//   the receiver type and up to 3 parents, the click handler type (never
//   its url), menu items as lang key names. Printable keys only become a
//   bucketed count per field (none for password / code fields); typed
//   text, field contents, clipboard, tooltips, window titles, names and
//   phones are never read.
// - Ids of peers, documents, photos, stories and the account user are
//   "h:<hmac64>" with the per-run key in <run>/ui.key (raw with
//   TDESKTOP_TRACE_FLAGS=ui_raw_ids). Message and topic ids are in clear.
// - ui_verbose adds window-relative positions (8 px grid), widget sizes
//   and exact keystroke counts. noui turns all of this off.
// Markers: TDESKTOP_TRACE_MARKER_KEY (default Ctrl+Alt+Shift+M, "off"),
// a second press within 1 s asks for a label; lines appended to
// <run>/marker.in become markers too.

namespace Trace::Ui {
namespace {

using namespace UiCore;

constexpr auto kPathDepth = 4;
constexpr auto kPathWalk = 12;
constexpr auto kSectionRepeat = 0.05; // s
constexpr auto kMarkerDouble = 1.0; // s
constexpr auto kMediaWindowWait = 2.0; // s
constexpr auto kLangRebuildDelay = 60.0; // s
constexpr auto kPositionGrid = 8;
constexpr auto kMaxMarkerRead = qint64(64 * 1024);

class MarkerFilter final : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *watched, QEvent *e) override;

};

struct State {
	bool appAttached = false;
	bool quitting = false;
	bool locked = false;
	rpl::lifetime appLifetime;
	std::vector<base::weak_ptr<Window::Controller>> windows;
	std::unordered_map<const std::type_info*, std::string> typeCache;

	ScrollCoalescer scroll;
	TypeCoalescer typing;
	RepeatCoalescer repeat;
	std::unique_ptr<base::Timer> flushTimer;

	uint32_t pressA = 0;
	Qt::MouseButton pressButton = Qt::NoButton;
	double pressT = 0.;
	QPoint pressGlobal;

	QPointer<QWidget> lastSection;
	double lastSectionT = -1.;
	std::string lastSectionSig;

	bool mediaAwaiting = false;
	double mediaOpenT = 0.;
	int mediaW = -1;
	QPointer<QWidget> mediaWindow;

	Main::Session *activeSession = nullptr;
	base::flat_set<uint64> idleSessions;

	MarkerFilter *markerFilter = nullptr;
	QKeySequence markerKey;
	int markers = 0;
	double lastMarkerKeyT = -10.;
	QPointer<QWidget> markerBox;
	std::unique_ptr<QFileSystemWatcher> markerWatcher;
	QString markerPath;
	std::string markerCarry;

	QString langId;
	double langBuiltT = -1000.;
	QHash<QString, std::string> langNames;
};

State &S() {
	// Never destroyed: Qt objects inside are released on aboutToQuit.
	static const auto result = new State();
	return *result;
}

InputScope *CurrentScope = nullptr;

[[nodiscard]] double Now() {
	return Trace::details::Writer::Now();
}

void Emit(const char *kind, Record &record, UiStampMode mode) {
	UiRecord(kind, record.data(), mode);
}

[[nodiscard]] const std::string &TypeName(const std::type_info &info) {
	auto &cache = S().typeCache;
	const auto i = cache.find(&info);
	if (i != end(cache)) {
		return i->second;
	}
	return cache.emplace(
		&info,
		CleanTypeName(Demangle(info.name()))).first->second;
}

[[nodiscard]] const std::string &TypeName(not_null<const QObject*> object) {
	return TypeName(typeid(*object));
}

[[nodiscard]] bool Dropped(const std::string &name) {
	return (name == "QWidget")
		|| (name == "QObject")
		|| (name == "QWidgetWindow")
		|| (name == "Ui::RpWidget")
		|| (name == "Ui::RpWidgetWrap")
		|| (name == "Ui::RpWidgetBase");
}

[[nodiscard]] std::vector<std::string> Path(QObject *receiver) {
	auto result = std::vector<std::string>();
	auto object = receiver;
	for (auto i = 0; object && i != kPathWalk; ++i) {
		const auto &name = TypeName(object);
		if (!Dropped(name) && (result.empty() || result.back() != name)) {
			result.push_back(name);
			if (int(result.size()) == kPathDepth) {
				break;
			}
		}
		object = object->parent();
	}
	return result;
}

[[nodiscard]] bool Contains(QObject *object, std::string_view what) {
	for (auto i = 0; object && i != 64; ++i, object = object->parent()) {
		if (TypeName(object).find(what) != std::string::npos) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool InsideMarkerBox(QObject *receiver) {
	const auto box = S().markerBox.data();
	if (!box) {
		return false;
	}
	const auto root = box->parentWidget()
		? static_cast<QObject*>(box->parentWidget())
		: static_cast<QObject*>(box);
	for (auto object = receiver; object; object = object->parent()) {
		if (object == root) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] int WindowIndex(QWidget *widget) {
	if (!widget) {
		return -1;
	}
	const auto top = widget->window();
	const auto &windows = S().windows;
	for (auto i = 0, count = int(windows.size()); i != count; ++i) {
		if (const auto controller = windows[i].get()) {
			if (static_cast<QWidget*>(controller->widget().get()) == top) {
				return i;
			}
		}
	}
	return -1;
}

void AddWindow(Record &record, QWidget *widget) {
	if (const auto w = WindowIndex(widget); w >= 0) {
		record.num("w", w);
	}
}

void AddPosition(Record &record, QWidget *widget, QPoint local) {
	if (!On(kUiVerbose) || !widget) {
		return;
	}
	const auto position = widget->mapTo(widget->window(), local);
	const auto round = [](int value) {
		return ((value + kPositionGrid / 2) / kPositionGrid) * kPositionGrid;
	};
	record.num("x", round(position.x()));
	record.num("y", round(position.y()));
	record.key("size").raw("["
		+ std::to_string(widget->width())
		+ ","
		+ std::to_string(widget->height())
		+ "]");
}

[[nodiscard]] std::vector<std::string> Modifiers(Qt::KeyboardModifiers mods) {
	auto result = std::vector<std::string>();
	if (mods & Qt::ControlModifier) {
		result.push_back("ctrl");
	}
	if (mods & Qt::AltModifier) {
		result.push_back("alt");
	}
	if (mods & Qt::ShiftModifier) {
		result.push_back("shift");
	}
	if (mods & Qt::MetaModifier) {
		result.push_back("meta");
	}
	return result;
}

[[nodiscard]] bool IsModifierKey(int key) {
	switch (key) {
	case Qt::Key_Shift:
	case Qt::Key_Control:
	case Qt::Key_Meta:
	case Qt::Key_Alt:
	case Qt::Key_AltGr:
	case Qt::Key_CapsLock:
	case Qt::Key_NumLock:
	case Qt::Key_ScrollLock:
	case Qt::Key_Super_L:
	case Qt::Key_Super_R:
	case Qt::Key_Hyper_L:
	case Qt::Key_Hyper_R:
	case Qt::Key_Mode_switch:
	case Qt::Key_unknown:
	case 0:
		return true;
	}
	return false;
}

// Only reached for named keys or keys with Ctrl / Alt / Meta.
[[nodiscard]] std::string KeyName(int key) {
	switch (key) {
	case Qt::Key_Return:
	case Qt::Key_Enter: return "Enter";
	case Qt::Key_Escape: return "Escape";
	case Qt::Key_Tab: return "Tab";
	case Qt::Key_Backtab: return "Backtab";
	case Qt::Key_Backspace: return "Backspace";
	case Qt::Key_Delete: return "Delete";
	case Qt::Key_Insert: return "Insert";
	case Qt::Key_Home: return "Home";
	case Qt::Key_End: return "End";
	case Qt::Key_PageUp: return "PageUp";
	case Qt::Key_PageDown: return "PageDown";
	case Qt::Key_Left: return "Left";
	case Qt::Key_Right: return "Right";
	case Qt::Key_Up: return "Up";
	case Qt::Key_Down: return "Down";
	case Qt::Key_Space: return "Space";
	case Qt::Key_Menu: return "Menu";
	case Qt::Key_Print: return "Print";
	case Qt::Key_Pause: return "Pause";
	}
	if (key >= Qt::Key_F1 && key <= Qt::Key_F35) {
		return "F" + std::to_string(key - Qt::Key_F1 + 1);
	} else if ((key >= Qt::Key_A && key <= Qt::Key_Z)
		|| (key >= Qt::Key_0 && key <= Qt::Key_9)) {
		return std::string(1, char(key));
	} else if (key > 0x20 && key < 0x7F) {
		return std::string(1, char(key));
	}
	char buffer[16];
	std::snprintf(buffer, sizeof(buffer), "0x%x", unsigned(key));
	return buffer;
}

// Printable keys without Ctrl / Alt / Meta are typing (AltGr is Ctrl+Alt
// on Windows, so that counts as typing too).
[[nodiscard]] bool IsTyping(not_null<QKeyEvent*> e) {
	const auto text = e->text();
	if (text.isEmpty() || !text.at(0).isPrint()) {
		return false;
	}
	const auto mods = e->modifiers();
	const auto ctrl = (mods & Qt::ControlModifier) != 0;
	const auto alt = (mods & Qt::AltModifier) != 0;
	const auto meta = (mods & Qt::MetaModifier) != 0;
	return (!ctrl && !alt && !meta) || (ctrl && alt && !meta);
}

[[nodiscard]] bool IsMasked(QWidget *field) {
	if (const auto line = qobject_cast<QLineEdit*>(field)) {
		if (line->echoMode() != QLineEdit::Normal) {
			return true;
		}
	}
	for (auto widget = field; widget; widget = widget->parentWidget()) {
		if (dynamic_cast<::Ui::PasswordInput*>(widget)) {
			return true;
		}
		const auto &name = TypeName(widget);
		if (name.find("Intro::") != std::string::npos
			|| name.find("Passport::") != std::string::npos
			|| name.find("Passcode") != std::string::npos) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] bool IsSearch(QWidget *field) {
	return Contains(field, "Dialogs::Widget") || Contains(field, "SearchField");
}

[[nodiscard]] const char *ButtonName(Qt::MouseButton button) {
	switch (button) {
	case Qt::LeftButton: return "left";
	case Qt::RightButton: return "right";
	case Qt::MiddleButton: return "middle";
	case Qt::BackButton: return "back";
	case Qt::ForwardButton: return "forward";
	case Qt::NoButton: return "touch";
	default: return "other";
	}
}

[[nodiscard]] QPoint LocalPos(not_null<QMouseEvent*> e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return e->position().toPoint();
#else // Qt >= 6.0.0
	return e->pos();
#endif // Qt < 6.0.0
}

[[nodiscard]] QPoint GlobalPos(not_null<QMouseEvent*> e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	return e->globalPosition().toPoint();
#else // Qt >= 6.0.0
	return e->globalPos();
#endif // Qt < 6.0.0
}

[[nodiscard]] std::pair<int, Qt::KeyboardModifiers> FirstKey(
		const QKeySequence &sequence) {
	if (sequence.isEmpty()) {
		return { 0, Qt::NoModifier };
	}
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	const auto combination = sequence[0];
	return { int(combination.key()), combination.keyboardModifiers() };
#else // Qt >= 6.0.0
	const auto value = sequence[0];
	return {
		int(value & ~Qt::KeyboardModifierMask),
		Qt::KeyboardModifiers(value & Qt::KeyboardModifierMask),
	};
#endif // Qt < 6.0.0
}

[[nodiscard]] bool MatchesMarkerKey(not_null<QKeyEvent*> e) {
	const auto &key = S().markerKey;
	if (key.isEmpty() || IsModifierKey(e->key())) {
		return false;
	}
	const auto [wanted, wantedMods] = FirstKey(key);
	const auto mods = e->modifiers() & ~Qt::KeypadModifier;
	return (e->key() == wanted) && (mods == wantedMods);
}

// Flushing of coalesced spans.

void EmitScrollEnd(const ScrollEnd &end) {
	auto record = Record();
	record.num("a", end.a);
	record.num("n", end.n);
	record.num("dy", end.dy);
	if (end.dx) {
		record.num("dx", end.dx);
	}
	record.num("ms", end.ms);
	Emit("ui.scroll.end", record, UiStampMode::None);
}

void EmitTypeEnd(const TypeEnd &end) {
	auto record = Record();
	record.num("a", end.a);
	if (end.masked) {
		record.boolean("masked", true);
	} else {
		record.str("n", Bucket(end.n));
		if (On(kUiVerbose)) {
			record.num("count", end.n);
		}
	}
	if (end.search) {
		record.boolean("search", true);
	}
	record.num("ms", end.ms);
	Emit("ui.type.end", record, UiStampMode::None);
}

void EmitRepeatEnd(const RepeatEnd &end) {
	auto record = Record();
	record.num("a", end.a);
	record.num("repeat", end.repeat);
	record.num("ms", end.ms);
	Emit("ui.key.end", record, UiStampMode::None);
}

void FlushScroll() {
	if (const auto end = S().scroll.flush()) {
		EmitScrollEnd(*end);
	}
}

void FlushTyping() {
	if (const auto end = S().typing.flush()) {
		EmitTypeEnd(*end);
	}
}

void FlushRepeat() {
	if (const auto end = S().repeat.flush()) {
		EmitRepeatEnd(*end);
	}
}

void FlushAll() {
	FlushScroll();
	FlushTyping();
	FlushRepeat();
}

void ScheduleFlush();

void PollSpans() {
	auto &s = S();
	const auto now = Now();
	if (const auto end = s.scroll.poll(now)) {
		EmitScrollEnd(*end);
	}
	if (const auto end = s.typing.poll(now)) {
		EmitTypeEnd(*end);
	}
	if (const auto end = s.repeat.poll(now)) {
		EmitRepeatEnd(*end);
	}
	ScheduleFlush();
}

void ScheduleFlush() {
	auto &s = S();
	if (s.quitting) {
		return;
	}
	auto deadline = std::numeric_limits<double>::max();
	if (s.scroll.active()) {
		deadline = std::min(deadline, s.scroll.deadline());
	}
	if (s.typing.active()) {
		deadline = std::min(deadline, s.typing.deadline());
	}
	if (s.repeat.active()) {
		deadline = std::min(deadline, s.repeat.deadline());
	}
	if (deadline == std::numeric_limits<double>::max()) {
		if (s.flushTimer) {
			s.flushTimer->cancel();
		}
		return;
	}
	if (!s.flushTimer) {
		s.flushTimer = std::make_unique<base::Timer>(PollSpans);
	}
	const auto ms = std::max(
		crl::time(1),
		crl::time(std::ceil((deadline - Now()) * 1000.)) + 1);
	s.flushTimer->callOnce(ms);
}

// Lang keys of menu items.

[[nodiscard]] std::optional<std::string> LangKey(const QString &text) {
	auto &s = S();
	auto &lang = Lang::GetInstance();
	const auto now = Now();
	const auto id = lang.id();
	const auto build = [&] {
		s.langId = id;
		s.langBuiltT = now;
		s.langNames.clear();
		for (auto i = 0; i != details::kLangNamesCount; ++i) {
			const auto &entry = details::kLangNames[i];
			if (!entry.name) {
				continue;
			}
			for (auto shift = 0; shift != int(entry.size); ++shift) {
				const auto index = int(entry.index) + shift;
				if (index >= int(Lang::kKeysCount)) {
					break;
				}
				const auto value = lang.getValue(ushort(index));
				if (value.isEmpty() || s.langNames.contains(value)) {
					continue;
				}
				auto name = std::string(entry.name);
				if (entry.size > 1) {
					name += '#' + std::to_string(shift);
				}
				s.langNames.insert(value, std::move(name));
			}
		}
	};
	if (s.langId != id || s.langNames.isEmpty()) {
		build();
	}
	auto variants = std::array<QString, 2>{ text, QString(text).remove('&') };
	for (const auto &variant : variants) {
		const auto i = s.langNames.constFind(variant);
		if (i != s.langNames.cend()) {
			return *i;
		}
	}
	// Cloud lang pack updates keep the id, rebuild now and then on a miss.
	if (now - s.langBuiltT > kLangRebuildDelay) {
		build();
		for (const auto &variant : variants) {
			const auto i = s.langNames.constFind(variant);
			if (i != s.langNames.cend()) {
				return *i;
			}
		}
	}
	return std::nullopt;
}

// Semantic events.

[[nodiscard]] const char *PeerKind(not_null<PeerData*> peer) {
	if (peer->isSelf()) {
		return "self";
	} else if (peer->isUser()) {
		return peer->isBot() ? "bot" : "user";
	} else if (peer->isChat()) {
		return "group";
	} else if (peer->isForum()) {
		return "forum";
	} else if (peer->isMegagroup() || peer->isMonoforum()) {
		return "megagroup";
	}
	return "channel";
}

void AppState(const char *key, bool value) {
	auto record = Record();
	record.boolean(key, value);
	Emit("app.state", record, UiStampMode::Stamp);
}

void ChatEntry(int w, const Dialogs::RowDescriptor &row) {
	auto record = Record();
	record.num("w", w);
	if (const auto peer = row.key.peer()) {
		record.id("peer", peer->id.value);
		record.str("peer_kind", PeerKind(peer));
	} else {
		record.null("peer");
	}
	const auto topic = row.key.topic();
	record.num("topic", topic ? topic->rootId().bare : 0);
	record.boolean("sublist", row.key.sublist() != nullptr);
	record.num("msg", row.fullId.msg.bare);
	Emit("ui.chat", record, UiStampMode::StampOrApp);
}

void SearchInChat(int w, Dialogs::Key key) {
	auto record = Record();
	record.num("w", w);
	if (const auto peer = key ? key.peer() : nullptr) {
		record.id("peer", peer->id.value);
		record.str("peer_kind", PeerKind(peer));
	} else {
		record.null("peer");
	}
	Emit("ui.search.chat", record, UiStampMode::StampOrApp);
}

void Folder(int w, FilterId id) {
	auto record = Record();
	record.num("w", w);
	record.num("filter", id);
	Emit("ui.folder", record, UiStampMode::StampOrApp);
}

void BoxLayer(int w, bool shown) {
	auto record = Record();
	record.num("w", w);
	record.boolean("layer", true);
	record.boolean("shown", shown);
	Emit("ui.box", record, UiStampMode::Stamp);
}

void MediaOpen(int w, const Media::View::OpenRequest &request) {
	auto &s = S();
	auto record = Record();
	record.num("w", w);
	const auto document = request.document();
	const auto photo = request.photo();
	const auto story = request.story();
	const auto kind = request.cloudTheme()
		? "theme"
		: story
		? "story"
		: request.call()
		? "call"
		: document
		? "document"
		: photo
		? "photo"
		: request.peer()
		? "peer"
		: "other";
	record.str("kind", kind);
	if (document) {
		record.id("doc", document->id);
	}
	if (photo) {
		record.id("photo", photo->id);
	}
	if (story) {
		record.id("peer", story->peer()->id.value);
		record.id("story", uint64(story->id()));
	} else if (const auto peer = request.peer()) {
		record.id("peer", peer->id.value);
	}
	if (const auto item = request.item()) {
		auto inner = Record();
		inner.id("peer", item->history()->peer->id.value);
		inner.num("msg", item->id.bare);
		auto object = inner.take();
		object[0] = '{'; // ",\"peer\"..." -> "{\"peer\"..."
		record.key("item").raw(object).raw("}");
	}
	record.boolean("streaming", request.continueStreaming());
	Emit("ui.media.open", record, UiStampMode::StampOrApp);

	s.mediaW = w;
	s.mediaOpenT = Now();
	s.mediaAwaiting = !(s.mediaWindow && s.mediaWindow->isVisible());
}

void MediaClose() {
	auto &s = S();
	auto record = Record();
	if (s.mediaW >= 0) {
		record.num("w", s.mediaW);
	}
	s.mediaWindow = nullptr;
	s.mediaAwaiting = false;
	Emit("ui.media.close", record, UiStampMode::Stamp);
}

void CallState(const char *kind, PeerData *peer, bool active) {
	auto record = Record();
	record.str("kind", kind);
	record.boolean("active", active);
	if (peer) {
		record.id("peer", peer->id.value);
	}
	Emit("ui.call", record, UiStampMode::StampOrApp);
}

void SubscribeIdle(not_null<Main::Session*> session) {
	auto &s = S();
	if (!s.idleSessions.emplace(session->uniqueId()).second) {
		return;
	}
	const auto raw = session.get();
	session->updates().isIdleValue(
	) | rpl::on_next([=](bool idle) {
		if (S().activeSession == raw) {
			AppState("idle", idle);
		}
	}, session->lifetime());
}

void Account(Main::Session *session) {
	auto &s = S();
	s.activeSession = session;
	auto record = Record();
	if (session) {
		const auto account = &session->account();
		for (const auto &entry : Core::App().domain().accounts()) {
			if (entry.account.get() == account) {
				record.num("index", entry.index);
				break;
			}
		}
		record.id("user", session->userPeerId().value);
	} else {
		record.null("user");
	}
	Emit("ui.account", record, UiStampMode::Stamp);
	if (session) {
		SubscribeIdle(session);
	}
}

void AttachSession(int w, Window::SessionController *controller) {
	{
		auto record = Record();
		record.num("w", w);
		record.boolean("open", controller != nullptr);
		Emit("ui.session", record, UiStampMode::Stamp);
	}
	if (!controller) {
		return;
	}
	auto &lifetime = controller->lifetime();
	controller->activeChatEntryValue(
	) | rpl::on_next([=](const Dialogs::RowDescriptor &row) {
		ChatEntry(w, row);
	}, lifetime);
	controller->searchInChatValue(
	) | rpl::on_next([=](Dialogs::Key key) {
		SearchInChat(w, key);
	}, lifetime);
	controller->activeChatsFilter(
	) | rpl::on_next([=](FilterId id) {
		Folder(w, id);
	}, lifetime);
	controller->boxShownValue(
	) | rpl::on_next([=](bool shown) {
		BoxLayer(w, shown);
	}, lifetime);
}

int AttachWindow(not_null<Window::Controller*> controller) {
	auto &windows = S().windows;
	for (auto i = 0, count = int(windows.size()); i != count; ++i) {
		if (windows[i].get() == controller) {
			return i;
		}
	}
	const auto w = int(windows.size());
	windows.push_back(base::make_weak(controller.get()));
	controller->openInMediaViewRequests(
	) | rpl::on_next([=](const Media::View::OpenRequest &request) {
		MediaOpen(w, request);
	}, controller->lifetime());
	controller->sessionControllerValue(
	) | rpl::on_next([=](Window::SessionController *session) {
		AttachSession(w, session);
	}, controller->lifetime());
	return w;
}

// Markers.

void WriteMarker(const char *src, const std::string &label, bool user) {
	auto record = Record();
	record.num("n", ++S().markers);
	record.str("src", src);
	const auto clean = CleanLabel(label);
	if (!clean.empty()) {
		record.str("label", clean);
		if (user) {
			record.boolean("label_user", true);
		}
	}
	Emit("ui.marker", record, UiStampMode::None);
}

void OpenMarkerBox() {
	if (S().markerBox || !Core::IsAppLaunched()) {
		return;
	}
	const auto window = Core::App().activeWindow();
	if (!window) {
		return;
	}
	window->uiShow()->showBox(::Box([](not_null<::Ui::GenericBox*> box) {
		box->setObjectName(u"wtMarkerBox"_q);
		S().markerBox = box.get();
		box->setTitle(rpl::single(u"Trace marker"_q));
		const auto field = box->addRow(object_ptr<::Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Label"_q),
			QString()));
		field->setMaxLength(int(kMaxLabel));
		box->setFocusCallback([=] {
			field->setFocusFast();
		});
		const auto submit = [=] {
			const auto label = field->getLastText().toStdString();
			WriteMarker("key", label, true);
			box->closeBox();
		};
		field->submits(
		) | rpl::on_next([=](Qt::KeyboardModifiers) {
			submit();
		}, field->lifetime());
		box->addButton(rpl::single(u"Mark"_q), submit);
		box->addButton(rpl::single(u"Cancel"_q), [=] {
			box->closeBox();
		});
	}));
}

void MarkerKeyPressed() {
	auto &s = S();
	const auto now = Now();
	WriteMarker("key", std::string(), false);
	if (now - s.lastMarkerKeyT <= kMarkerDouble) {
		s.lastMarkerKeyT = -10.;
		crl::on_main(OpenMarkerBox);
	} else {
		s.lastMarkerKeyT = now;
	}
}

bool MarkerFilter::eventFilter(QObject *watched, QEvent *e) {
	if (e->type() == QEvent::KeyPress) {
		const auto key = static_cast<QKeyEvent*>(e);
		if (!key->isAutoRepeat() && MatchesMarkerKey(key)) {
			MarkerKeyPressed();
			return true;
		}
	}
	return QObject::eventFilter(watched, e);
}

void ReadMarkerFile() {
	auto &s = S();
	if (s.markerPath.isEmpty() || !s.markerWatcher) {
		return;
	}
	auto file = QFile(s.markerPath);
	if (file.open(QIODevice::ReadWrite)) {
		const auto data = file.read(kMaxMarkerRead);
		file.resize(0);
		file.close();
		const auto labels = TakeMarkerLines(
			s.markerCarry,
			std::string_view(data.constData(), size_t(data.size())));
		for (const auto &label : labels) {
			WriteMarker("file", label, false);
		}
	}
	if (QFile::exists(s.markerPath)
		&& !s.markerWatcher->files().contains(s.markerPath)) {
		s.markerWatcher->addPath(s.markerPath);
	}
}

void SetupMarkers() {
	auto &s = S();
	const auto key = qEnvironmentVariable("TDESKTOP_TRACE_MARKER_KEY");
	if (key.compare(u"off"_q, Qt::CaseInsensitive)) {
		s.markerKey = key.isEmpty()
			? QKeySequence(u"Ctrl+Alt+Shift+M"_q, QKeySequence::PortableText)
			: QKeySequence(key, QKeySequence::PortableText);
		if (!s.markerKey.isEmpty()) {
			s.markerFilter = new MarkerFilter(qApp);
			qApp->installEventFilter(s.markerFilter);
		}
	}
	const auto &directory = RunDirectory();
	if (directory.isEmpty()) {
		return;
	}
	s.markerPath = directory + u"/marker.in"_q;
#ifdef Q_OS_WIN
	const auto path = std::filesystem::path(s.markerPath.toStdWString());
#else // Q_OS_WIN
	const auto path = std::filesystem::path(
		QFile::encodeName(s.markerPath).toStdString());
#endif // Q_OS_WIN
	if (const auto file = Trace::details::OpenPrivateFile(path)) {
		std::fclose(file);
	}
	s.markerWatcher = std::make_unique<QFileSystemWatcher>();
	s.markerWatcher->addPath(s.markerPath);
	QObject::connect(
		s.markerWatcher.get(),
		&QFileSystemWatcher::fileChanged,
		ReadMarkerFile);
}

void Quit() {
	auto &s = S();
	if (s.quitting) {
		return;
	}
	FlushAll();
	s.quitting = true;
	s.appLifetime.destroy();
	s.flushTimer = nullptr;
	s.markerWatcher = nullptr;
	if (s.markerFilter) {
		qApp->removeEventFilter(s.markerFilter);
		delete base::take(s.markerFilter);
	}
}

void AttachApp() {
	auto &s = S();
	if (s.appAttached || !Core::IsAppLaunched()) {
		return;
	}
	s.appAttached = true;
	QObject::connect(qApp, &QCoreApplication::aboutToQuit, Quit);

	auto &app = Core::App();
	app.passcodeLockValue(
	) | rpl::on_next([](bool locked) {
		if (locked) {
			FlushAll();
		}
		S().locked = locked;
		AppState("locked", locked);
	}, s.appLifetime);
	app.appDeactivatedValue(
	) | rpl::on_next([](bool deactivated) {
		AppState("active", !deactivated);
	}, s.appLifetime);
	app.screenIsLockedValue(
	) | rpl::on_next([](bool locked) {
		AppState("screen_locked", locked);
	}, s.appLifetime);
	app.systemSleepEvents(
	) | rpl::on_next([] {
		AppState("sleep", true);
	}, s.appLifetime);
	app.domain().activeSessionValue(
	) | rpl::on_next([](Main::Session *session) {
		Account(session);
	}, s.appLifetime);
	app.calls().currentCallValue(
	) | rpl::on_next([](Calls::Call *call) {
		CallState("call", call ? call->user().get() : nullptr, call != nullptr);
	}, s.appLifetime);
	app.calls().currentGroupCallValue(
	) | rpl::on_next([](Calls::GroupCall *call) {
		CallState("group", call ? call->peer().get() : nullptr, call != nullptr);
	}, s.appLifetime);

	for (const auto widget : QApplication::topLevelWidgets()) {
		if (const auto main = dynamic_cast<Window::MainWindow*>(widget)) {
			AttachWindow(&main->controller());
		}
	}
	SetupMarkers();
}

// Show / Hide / window state.

void Section(not_null<QWidget*> widget, double now) {
	auto &s = S();
	auto record = Record();
	AddWindow(record, widget);
	record.str("type", TypeName(widget));
	auto signature = TypeName(widget);
	if (const auto wrap = dynamic_cast<Info::WrapWidget*>(widget.get())) {
		using Type = Info::Section::Type;
		const auto section = wrap->controller()->section();
		const auto info = [&]() -> const char* {
			switch (section.type()) {
			case Type::Profile: return "Profile";
			case Type::Media: return "Media";
			case Type::GlobalMedia: return "GlobalMedia";
			case Type::CommonGroups: return "CommonGroups";
			case Type::SimilarPeers: return "SimilarPeers";
			case Type::RequestsList: return "RequestsList";
			case Type::Community: return "Community";
			case Type::CommunityRequests: return "CommunityRequests";
			case Type::ReactionsList: return "ReactionsList";
			case Type::SavedSublists: return "SavedSublists";
			case Type::PeerGifts: return "PeerGifts";
			case Type::Members: return "Members";
			case Type::Settings: return "Settings";
			case Type::Downloads: return "Downloads";
			case Type::Stories: return "Stories";
			case Type::SavedMusic: return "SavedMusic";
			case Type::PollResults: return "PollResults";
			case Type::Statistics: return "Statistics";
			case Type::BotStarRef: return "BotStarRef";
			case Type::Boosts: return "Boosts";
			case Type::ChannelEarn: return "ChannelEarn";
			case Type::BotEarn: return "BotEarn";
			}
			return "Unknown";
		}();
		record.str("info", info);
		signature += '/';
		signature += info;
		if (section.type() == Type::Settings) {
			if (const auto &factory = section.settingsType()) {
				const auto settings = TemplateArgumentOrName(
					Demangle(typeid(*factory).name()));
				record.str("settings", settings);
				signature += '/';
				signature += settings;
			}
		}
		if (!wrap->property("wtUi").toBool()) {
			wrap->setProperty("wtUi", true);
			const auto weak = QPointer<QWidget>(wrap);
			wrap->contentChanged(
			) | rpl::on_next([=] {
				if (const auto strong = weak.data(); strong && !S().quitting) {
					Section(strong, Now());
				}
			}, wrap->lifetime());
		}
	}
	if (s.lastSection == widget.get()
		&& (s.lastSectionSig == signature)
		&& (now - s.lastSectionT) < kSectionRepeat) {
		return;
	}
	s.lastSection = widget.get();
	s.lastSectionT = now;
	s.lastSectionSig = std::move(signature);
	Emit("ui.section", record, UiStampMode::StampOrApp);
}

void BoxShown(not_null<QWidget*> widget, bool shown) {
	auto record = Record();
	AddWindow(record, widget);
	record.boolean("shown", shown);
	record.str("type", TypeName(widget));
	Emit("ui.box", record, UiStampMode::Stamp);
}

void WindowState(int w, const char *state) {
	auto record = Record();
	record.num("w", w);
	record.str("state", state);
	Emit("app.window", record, UiStampMode::Stamp);
}

void ViewEvent(QObject *receiver, QEvent *e, double now) {
	if (!receiver->isWidgetType() || InsideMarkerBox(receiver)) {
		return;
	}
	auto &s = S();
	const auto widget = static_cast<QWidget*>(receiver);
	const auto type = e->type();
	if (const auto main = dynamic_cast<Window::MainWindow*>(widget)) {
		if (type == QEvent::Show) {
			WindowState(AttachWindow(&main->controller()), "shown");
		} else if (type == QEvent::Hide) {
			if (const auto w = WindowIndex(widget); w >= 0) {
				WindowState(w, "hidden");
			}
		}
		return;
	}
	if (type == QEvent::Show) {
		if (widget->isWindow()
			&& s.mediaAwaiting
			&& (now - s.mediaOpenT) <= kMediaWindowWait
			&& dynamic_cast<::Ui::RpWindow*>(widget)) {
			s.mediaWindow = widget;
			s.mediaAwaiting = false;
		}
		if (dynamic_cast<Window::AbstractSectionWidget*>(widget)) {
			Section(widget, now);
		} else if (dynamic_cast<::Ui::BoxContent*>(widget)) {
			BoxShown(widget, true);
		}
	} else if (type == QEvent::Hide) {
		if (s.mediaWindow == widget) {
			MediaClose();
		} else if (dynamic_cast<::Ui::BoxContent*>(widget)) {
			BoxShown(widget, false);
		}
	}
}

void WindowEvent(QObject *receiver, QEvent *e) {
	if (!receiver->isWidgetType()) {
		return;
	}
	const auto widget = static_cast<QWidget*>(receiver);
	if (!dynamic_cast<Window::MainWindow*>(widget)) {
		return;
	}
	const auto w = WindowIndex(widget);
	if (w < 0) {
		return;
	}
	switch (e->type()) {
	case QEvent::WindowActivate: WindowState(w, "active"); break;
	case QEvent::WindowDeactivate: WindowState(w, "inactive"); break;
	case QEvent::WindowStateChange: {
		const auto state = widget->windowState();
		WindowState(w, (state & Qt::WindowMinimized)
			? "minimized"
			: (state & Qt::WindowFullScreen)
			? "fullscreen"
			: (state & Qt::WindowMaximized)
			? "maximized"
			: "normal");
	} break;
	default: break;
	}
}

// Input.

[[nodiscard]] uint32_t Press(
		not_null<QWidget*> widget,
		Qt::MouseButton button,
		QPoint local,
		QPoint global,
		bool doubleClick,
		double now) {
	auto &s = S();
	FlushAll();
	const auto a = NextAction();
	s.pressA = a;
	s.pressButton = button;
	s.pressT = now;
	s.pressGlobal = global;
	auto record = Record();
	record.num("a", a);
	AddWindow(record, widget);
	record.str("btn", ButtonName(button));
	if (doubleClick) {
		record.boolean("dbl", true);
	}
	record.list("path", Path(widget));
	AddPosition(record, widget, local);
	Emit("ui.press", record, UiStampMode::None);
	return a;
}

[[nodiscard]] uint32_t Release(
		not_null<QWidget*> widget,
		Qt::MouseButton button,
		QPoint local,
		QPoint global,
		double now) {
	auto &s = S();
	const auto matched = (s.pressA != 0) && (s.pressButton == button);
	const auto a = matched ? s.pressA : NextAction();
	s.pressA = 0;
	s.pressButton = Qt::NoButton;

	auto record = Record();
	record.num("a", a);
	AddWindow(record, widget);
	if (const auto item = dynamic_cast<::Ui::Menu::ItemBase*>(widget.get())) {
		const auto text = item->action()->text();
		if (const auto key = LangKey(text)) {
			record.str("item", *key);
		} else {
			record.null("item");
			record.num("len", text.size());
		}
		record.list("path", Path(widget));
		Emit("ui.menu", record, UiStampMode::None);
		return a;
	}
	record.str("btn", ButtonName(button));
	if (matched) {
		record.num("ms", std::llround((now - s.pressT) * 1000.));
		const auto moved = (global - s.pressGlobal).manhattanLength()
			> QApplication::startDragDistance();
		record.boolean("moved", moved);
	}
	record.list("path", Path(widget));
	// An activation: the same handler pressed and active, the rule of
	// ClickHandler::unpressed(). Its url is never read.
	const auto pressed = ClickHandler::getPressed();
	if (pressed && pressed == ClickHandler::getActive()) {
		record.str("handler", TypeName(typeid(*pressed)));
		record.num("entity", int(pressed->getTextEntity().type));
	}
	AddPosition(record, widget, local);
	Emit("ui.click", record, UiStampMode::None);
	return a;
}

[[nodiscard]] uint32_t Wheel(not_null<QWidget*> widget, not_null<QWheelEvent*> e) {
	auto &s = S();
	auto target = static_cast<const void*>(widget.get());
	auto object = static_cast<QObject*>(widget.get());
	for (auto i = 0; object && i != 8; ++i, object = object->parent()) {
		if (dynamic_cast<::Ui::ScrollArea*>(object)) {
			target = object;
			break;
		}
	}
	const auto angle = e->angleDelta();
	const auto dy = -int64_t(angle.y());
	const auto dx = -int64_t(angle.x());
	if (!s.scroll.active()) {
		FlushTyping();
		FlushRepeat();
	}
	const auto result = s.scroll.wheel(target, Now(), dy, dx);
	if (result.ended) {
		EmitScrollEnd(*result.ended);
	}
	if (result.begin) {
		auto record = Record();
		record.num("a", result.a);
		AddWindow(record, widget);
		const auto vertical = std::abs(dy) >= std::abs(dx);
		record.str("dir", vertical
			? (dy < 0 ? "up" : "down")
			: (dx < 0 ? "left" : "right"));
		record.list("path", Path(widget));
		if (On(kUiVerbose)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
			AddPosition(record, widget, e->position().toPoint());
#else // Qt >= 6.0.0
			AddPosition(record, widget, e->pos());
#endif // Qt < 6.0.0
		}
		Emit("ui.scroll", record, UiStampMode::None);
	}
	ScheduleFlush();
	return result.a;
}

[[nodiscard]] uint32_t Key(not_null<QWidget*> widget, not_null<QKeyEvent*> e) {
	auto &s = S();
	const auto key = e->key();
	if (IsModifierKey(key) || MatchesMarkerKey(e)) {
		return 0;
	}
	const auto now = Now();
	const auto erase = (key == Qt::Key_Backspace || key == Qt::Key_Delete);
	const auto typing = IsTyping(e);
	if (typing || (erase && s.typing.field() == widget.get())) {
		const auto same = (s.typing.field() == widget.get());
		const auto masked = same ? false : IsMasked(widget);
		const auto search = same ? false : IsSearch(widget);
		if (!s.typing.active()) {
			FlushScroll();
			FlushRepeat();
		}
		const auto result = s.typing.key(widget, now, masked, search, typing);
		if (result.ended) {
			EmitTypeEnd(*result.ended);
		}
		if (result.begin) {
			auto record = Record();
			record.num("a", result.a);
			AddWindow(record, widget);
			record.list("field", Path(widget));
			if (masked) {
				record.boolean("masked", true);
			}
			if (search) {
				record.boolean("search", true);
			}
			Emit("ui.type", record, UiStampMode::None);
		}
		ScheduleFlush();
		if (result.a) {
			return result.a;
		}
	}
	switch (key) {
	case Qt::Key_Return:
	case Qt::Key_Enter:
	case Qt::Key_Escape:
	case Qt::Key_Tab:
	case Qt::Key_Backtab:
		FlushTyping();
		break;
	}
	FlushScroll();
	const auto mods = e->modifiers() & ~Qt::KeypadModifier;
	const auto result = s.repeat.key(key, int(mods), e->isAutoRepeat(), now);
	if (result.ended) {
		EmitRepeatEnd(*result.ended);
	}
	if (result.emit) {
		auto record = Record();
		record.num("a", result.a);
		AddWindow(record, widget);
		record.str("key", KeyName(key));
		record.list("mods", Modifiers(mods));
		record.list("path", Path(widget));
		Emit("ui.key", record, UiStampMode::None);
	}
	ScheduleFlush();
	return result.a;
}

} // namespace

InputScope::InputScope(QObject *receiver, QEvent *e) {
	auto &s = S();
	if (!s.appAttached) {
		AttachApp();
	}
	if (s.quitting || !receiver) {
		return;
	}
	const auto type = e->type();
	const auto now = Now();
	switch (type) {
	case QEvent::Show:
	case QEvent::Hide:
		ViewEvent(receiver, e, now);
		return;
	case QEvent::WindowActivate:
	case QEvent::WindowDeactivate:
	case QEvent::WindowStateChange:
		WindowEvent(receiver, e);
		return;
	case QEvent::FocusIn:
		if (s.typing.active() && s.typing.field() != receiver) {
			FlushTyping();
		}
		return;
	case QEvent::ApplicationStateChange:
	case QEvent::NativeGesture:
		return;
	default:
		break;
	}
	// The passcode lock screen records only "app.state locked".
	if (s.locked || InsideMarkerBox(receiver)) {
		return;
	}
	if (type == QEvent::Shortcut) {
		// Sent to a QShortcut, not spontaneous. Shortcuts::RequestHandler
		// writes ui.shortcut with this action id; if nothing does, the
		// key sequence is written as ui.key when the dispatch ends.
		const auto shortcut = static_cast<QShortcutEvent*>(e);
		const auto [key, mods] = FirstKey(shortcut->key());
		FlushScroll();
		FlushRepeat();
		_a = NextAction();
		_shortcutEvent = true;
		_shortcutKey = key;
		_shortcutModifiers = int(mods);
	} else if (!receiver->isWidgetType() || !e->spontaneous()) {
		// QWindow deliveries wrap the widget ones, and events forwarded
		// by code (sendEvent) are inside the original's scope.
		return;
	} else {
		const auto widget = static_cast<QWidget*>(receiver);
		switch (type) {
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick: {
			const auto mouse = static_cast<QMouseEvent*>(e);
			_a = Press(
				widget,
				mouse->button(),
				LocalPos(mouse),
				GlobalPos(mouse),
				(type == QEvent::MouseButtonDblClick),
				now);
		} break;
		case QEvent::MouseButtonRelease: {
			const auto mouse = static_cast<QMouseEvent*>(e);
			_a = Release(
				widget,
				mouse->button(),
				LocalPos(mouse),
				GlobalPos(mouse),
				now);
		} break;
		case QEvent::TouchBegin:
			_a = Press(widget, Qt::NoButton, QPoint(), QPoint(), false, now);
			break;
		case QEvent::TouchEnd:
			_a = Release(widget, Qt::NoButton, QPoint(), QPoint(), now);
			break;
		case QEvent::Wheel:
			_a = Wheel(widget, static_cast<QWheelEvent*>(e));
			break;
		case QEvent::KeyPress:
			_a = Key(widget, static_cast<QKeyEvent*>(e));
			break;
		default:
			break;
		}
	}
	if (!_a) {
		return;
	}
	_entered = true;
	_previous = CurrentScope;
	CurrentScope = this;
	MainClock().enter(_a, now);
}

InputScope::~InputScope() {
	if (!_entered) {
		return;
	}
	if (_shortcutEvent && !_shortcutRecorded && !S().quitting) {
		auto record = Record();
		record.num("a", _a);
		record.str("key", KeyName(_shortcutKey));
		record.list(
			"mods",
			Modifiers(Qt::KeyboardModifiers(_shortcutModifiers)));
		record.str("via", "shortcut");
		Emit("ui.key", record, UiStampMode::None);
	}
	MainClock().leave(Now());
	CurrentScope = _previous;
}

void Shortcut(const std::vector<QString> &names) {
	if (names.empty() || S().quitting) {
		return;
	}
	auto record = Record();
	const auto scope = CurrentScope;
	const auto inside = (scope != nullptr) && !S().locked;
	const auto a = inside ? scope->_a : NextAction();
	if (inside) {
		scope->_shortcutRecorded = true;
	}
	record.num("a", a);
	record.str("cmd", names.front().toStdString());
	if (names.size() > 1) {
		auto list = std::vector<std::string>();
		list.reserve(names.size());
		for (const auto &name : names) {
			list.push_back(name.toStdString());
		}
		record.list("cmds", list);
	}
	if (inside) {
		Emit("ui.shortcut", record, UiStampMode::Stamp);
	} else {
		// From the tray, the macOS menu or code: requests sent by the
		// handler right after this are "deferred" to it.
		Emit("ui.shortcut", record, UiStampMode::None);
		MainClock().mark(a, Now());
	}
}

} // namespace Trace::Ui
