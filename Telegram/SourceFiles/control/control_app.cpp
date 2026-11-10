/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
// Application-level commands of the control server: state, navigation,
// shortcuts. Only stable, public entry points of Core / Window / Data.
#include "control/control_internal.h"

#include "core/application.h"
#include "core/shortcuts.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "intro/intro_step.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "ui/layers/box_content.h"
#include "window/section_widget.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"

#ifdef TDESKTOP_WIRE_TRACE
#include "trace/trace.h"
#endif // TDESKTOP_WIRE_TRACE

#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>

namespace Control {
namespace {

// Same names as Shortcuts' CommandByName (core/shortcuts.cpp), which is
// file-local there. Only commands with a stable meaning are listed.
const auto kShortcuts = std::map<QString, Shortcuts::Command>{
	{ u"close_telegram"_q, Shortcuts::Command::Close },
	{ u"lock_telegram"_q, Shortcuts::Command::Lock },
	{ u"minimize_telegram"_q, Shortcuts::Command::Minimize },
	{ u"search"_q, Shortcuts::Command::Search },
	{ u"previous_chat"_q, Shortcuts::Command::ChatPrevious },
	{ u"next_chat"_q, Shortcuts::Command::ChatNext },
	{ u"first_chat"_q, Shortcuts::Command::ChatFirst },
	{ u"last_chat"_q, Shortcuts::Command::ChatLast },
	{ u"self_chat"_q, Shortcuts::Command::ChatSelf },
	{ u"pinned_chat1"_q, Shortcuts::Command::ChatPinned1 },
	{ u"pinned_chat2"_q, Shortcuts::Command::ChatPinned2 },
	{ u"pinned_chat3"_q, Shortcuts::Command::ChatPinned3 },
	{ u"pinned_chat4"_q, Shortcuts::Command::ChatPinned4 },
	{ u"pinned_chat5"_q, Shortcuts::Command::ChatPinned5 },
	{ u"previous_folder"_q, Shortcuts::Command::FolderPrevious },
	{ u"next_folder"_q, Shortcuts::Command::FolderNext },
	{ u"all_chats"_q, Shortcuts::Command::ShowAllChats },
	{ u"folder1"_q, Shortcuts::Command::ShowFolder1 },
	{ u"folder2"_q, Shortcuts::Command::ShowFolder2 },
	{ u"folder3"_q, Shortcuts::Command::ShowFolder3 },
	{ u"last_folder"_q, Shortcuts::Command::ShowFolderLast },
	{ u"account1"_q, Shortcuts::Command::ShowAccount1 },
	{ u"account2"_q, Shortcuts::Command::ShowAccount2 },
	{ u"account3"_q, Shortcuts::Command::ShowAccount3 },
	{ u"show_archive"_q, Shortcuts::Command::ShowArchive },
	{ u"show_contacts"_q, Shortcuts::Command::ShowContacts },
	{ u"show_scheduled"_q, Shortcuts::Command::ShowScheduled },
	{ u"read_chat"_q, Shortcuts::Command::ReadChat },
	{ u"archive_chat"_q, Shortcuts::Command::ArchiveChat },
	{ u"show_chat_menu"_q, Shortcuts::Command::ShowChatMenu },
	{ u"show_chat_preview"_q, Shortcuts::Command::ShowChatPreview },
	{ u"media_play"_q, Shortcuts::Command::MediaPlay },
	{ u"media_pause"_q, Shortcuts::Command::MediaPause },
	{ u"media_playpause"_q, Shortcuts::Command::MediaPlayPause },
	{ u"media_stop"_q, Shortcuts::Command::MediaStop },
	{ u"media_previous"_q, Shortcuts::Command::MediaPrevious },
	{ u"media_next"_q, Shortcuts::Command::MediaNext },
};

[[nodiscard]] QString PeerString(PeerId id) {
	if (peerIsUser(id)) {
		return u"user:%1"_q.arg(peerToUser(id).bare);
	} else if (peerIsChat(id)) {
		return u"chat:%1"_q.arg(peerToChat(id).bare);
	} else if (peerIsChannel(id)) {
		return u"channel:%1"_q.arg(peerToChannel(id).bare);
	}
	return QString::number(id.value);
}

// "self", "user:ID", "chat:ID", "channel:ID", or a Bot API style id:
// positive is a user, -100... a channel, other negative a chat.
[[nodiscard]] PeerId ParsePeer(
		const QJsonValue &value,
		Main::Session *session) {
	auto text = value.isString()
		? value.toString()
		: QString::number(qint64(value.toDouble()));
	if (text == u"self"_q || text == u"me"_q) {
		return session ? session->userPeerId() : PeerId();
	}
	const auto colon = text.indexOf(':');
	auto ok = false;
	if (colon > 0) {
		const auto kind = text.left(colon);
		const auto bare = text.mid(colon + 1).toULongLong(&ok);
		if (!ok || !bare) {
			return PeerId();
		} else if (kind == u"user"_q) {
			return peerFromUser(UserId(bare));
		} else if (kind == u"chat"_q) {
			return peerFromChat(ChatId(bare));
		} else if (kind == u"channel"_q) {
			return peerFromChannel(ChannelId(bare));
		}
		return PeerId();
	}
	const auto number = text.toLongLong(&ok);
	if (!ok || !number) {
		return PeerId();
	} else if (number > 0) {
		return peerFromUser(UserId(uint64(number)));
	}
	const auto positive = uint64(-number);
	constexpr auto kChannelShift = uint64(1000000000000ULL);
	if (positive > kChannelShift) {
		return peerFromChannel(ChannelId(positive - kChannelShift));
	}
	return peerFromChat(ChatId(positive));
}

[[nodiscard]] Window::Controller *ActiveWindow() {
	if (!Core::IsAppLaunched()) {
		return nullptr;
	}
	if (const auto window = Core::App().activePrimaryWindow()) {
		return window;
	}
	return Core::App().activeWindow();
}

[[nodiscard]] Window::SessionController *ActiveController() {
	const auto window = ActiveWindow();
	return window ? window->sessionController() : nullptr;
}

[[nodiscard]] QString PeerType(not_null<PeerData*> peer) {
	if (peer->isUser()) {
		return peer->asUser()->isBot() ? u"bot"_q : u"user"_q;
	} else if (peer->isChat()) {
		return u"group"_q;
	} else if (peer->isMegagroup()) {
		return u"supergroup"_q;
	} else if (peer->isChannel()) {
		return u"channel"_q;
	}
	return QString();
}

[[nodiscard]] QJsonObject PeerJson(not_null<PeerData*> peer) {
	return QJsonObject{
		{ u"peer"_q, PeerString(peer->id) },
		{ u"name"_q, peer->name() },
		{ u"kind"_q, PeerType(peer) },
	};
}

struct Visible {
	QStringList sections;
	QStringList boxes;
	QString introStep;
};

void CollectVisible(QWidget *widget, Visible &result) {
	if (!widget->isVisible()) {
		return;
	}
	if (dynamic_cast<Window::AbstractSectionWidget*>(widget)) {
		result.sections.push_back(TypeName(widget));
	} else if (dynamic_cast<Ui::BoxContent*>(widget)) {
		result.boxes.push_back(TypeName(widget));
	} else if (dynamic_cast<Intro::details::Step*>(widget)) {
		result.introStep = TypeName(widget);
	}
	for (const auto child : widget->children()) {
		if (const auto w = qobject_cast<QWidget*>(child)) {
			if (!w->isWindow()) {
				CollectVisible(w, result);
			}
		}
	}
}

[[nodiscard]] QString AppStateName(Qt::ApplicationState state) {
	switch (state) {
	case Qt::ApplicationActive: return u"active"_q;
	case Qt::ApplicationInactive: return u"inactive"_q;
	case Qt::ApplicationHidden: return u"hidden"_q;
	case Qt::ApplicationSuspended: return u"suspended"_q;
	}
	return QString();
}

} // namespace

QWidget *PrimaryWindow() {
	const auto window = ActiveWindow();
	return window ? static_cast<QWidget*>(window->widget().get()) : nullptr;
}

bool Authorized() {
	if (!Core::IsAppLaunched() || !Core::App().domain().started()) {
		return false;
	}
	return Core::App().domain().active().sessionExists();
}

QJsonObject State(const QJsonObject &args) {
	auto result = QJsonObject{
		{ u"t"_q, TraceNow() },
		{ u"platform"_q, QGuiApplication::platformName() },
		{ u"app_state"_q, AppStateName(QGuiApplication::applicationState()) },
		{ u"launched"_q, Core::IsAppLaunched() },
		{ u"paint_ago_ms"_q, LastPaintAgo() },
		{ u"recording"_q, RecordActive() },
	};
#ifdef TDESKTOP_WIRE_TRACE
	result.insert(u"trace"_q, Trace::On());
#endif // TDESKTOP_WIRE_TRACE
	if (!Core::IsAppLaunched()) {
		return Ok(result);
	}
	auto &app = Core::App();
	result.insert(u"locked"_q, app.passcodeLocked());
	result.insert(
		u"idle_ms"_q,
		double(crl::now() - app.lastNonIdleTime()));

	auto &domain = app.domain();
	if (domain.started()) {
		const auto &active = domain.active();
		auto index = -1;
		for (const auto &[i, account] : domain.accounts()) {
			if (account.get() == &active) {
				index = i;
			}
		}
		result.insert(u"accounts"_q, int(domain.accounts().size()));
		result.insert(u"account_index"_q, index);
		result.insert(u"authorized"_q, active.sessionExists());
		if (active.sessionExists()) {
			const auto &session = active.session();
			result.insert(u"user"_q, PeerString(session.userPeerId()));
			result.insert(u"user_name"_q, session.user()->name());
		}
	}

	const auto window = PrimaryWindow();
	if (window) {
		const auto geometry = window->geometry();
		result.insert(u"window"_q, QJsonObject{
			{ u"id"_q, WindowId(window) },
			{ u"geometry"_q, QJsonArray{
				geometry.x(),
				geometry.y(),
				geometry.width(),
				geometry.height() } },
			{ u"active"_q, window->isActiveWindow() },
			{ u"visible"_q, window->isVisible() },
			{ u"minimized"_q, window->isMinimized() },
			{ u"dpr"_q, window->devicePixelRatioF() },
			{ u"gl_widgets"_q, CountGlWidgets(window) },
		});
		auto visible = Visible();
		CollectVisible(window, visible);
		result.insert(u"sections"_q, QJsonArray::fromStringList(visible.sections));
		result.insert(u"boxes"_q, QJsonArray::fromStringList(visible.boxes));
		if (!visible.introStep.isEmpty()) {
			result.insert(u"intro_step"_q, visible.introStep);
		}
	}
	if (const auto popup = QApplication::activePopupWidget()) {
		result.insert(u"popup"_q, TypeName(popup));
	}
	if (const auto controller = ActiveController()) {
		result.insert(u"layer"_q, controller->isLayerShown());
		const auto key = controller->activeChatCurrent();
		if (const auto peer = key.peer()) {
			result.insert(u"chat"_q, PeerJson(peer));
		}
	}
	return Ok(result);
}

QJsonObject OpenPeer(const QJsonObject &args) {
	const auto controller = ActiveController();
	if (!controller) {
		return Fail(u"no session window"_q);
	}
	const auto session = &controller->session();
	const auto id = ParsePeer(args.value(u"peer"_q), session);
	if (!id) {
		return Fail(u"bad peer, use self, user:ID, chat:ID, channel:ID"_q);
	}
	const auto peer = session->data().peerLoaded(id);
	if (!peer) {
		return Fail(u"peer not loaded: "_q + PeerString(id));
	}
	const auto msg = args.value(u"msg"_q).toInteger(0);
	controller->showPeerHistory(
		id,
		Window::SectionShow::Way::ClearStack,
		msg ? MsgId(msg) : ShowAtUnreadMsgId);
	return Ok(PeerJson(peer));
}

QJsonObject Back(const QJsonObject &args) {
	const auto controller = ActiveController();
	if (!controller) {
		return Fail(u"no session window"_q);
	}
	controller->showBackFromStack();
	return Ok();
}

QJsonObject Shortcut(const QJsonObject &args) {
	const auto name = args.value(u"name"_q).toString();
	const auto i = kShortcuts.find(name);
	if (i == kShortcuts.end()) {
		auto names = QJsonArray();
		for (const auto &[known, command] : kShortcuts) {
			names.push_back(known);
		}
		auto result = Fail(u"unknown shortcut: "_q + name);
		result.insert(u"known"_q, names);
		return result;
	}
	const auto handled = Shortcuts::Launch(i->second);
	return Ok({ { u"handled"_q, handled } });
}

QJsonObject Chats(const QJsonObject &args) {
	const auto controller = ActiveController();
	if (!controller) {
		return Fail(u"no session window"_q);
	}
	const auto limit = std::clamp(args.value(u"limit"_q).toInt(50), 1, 10000);
	auto list = QJsonArray();
	const auto indexed = controller->session().data().chatsList()->indexed();
	for (const auto &row : *indexed) {
		if (list.size() >= limit) {
			break;
		}
		if (const auto peer = row->key().peer()) {
			list.push_back(PeerJson(peer));
		}
	}
	return Ok({ { u"chats"_q, list } });
}

QJsonObject Quit(const QJsonObject &args) {
	QTimer::singleShot(0, qApp, [] { Core::Quit(); });
	return Ok();
}

} // namespace Control
