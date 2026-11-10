/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
// Widget tree, input injection and capture for the control server.
//
// Input goes through QWindowSystemInterface, the same entry point the
// platform plugin uses for real input, so enter/leave, hover state, the
// implicit mouse grab, popups, double clicks and QShortcut matching all
// behave as with a real mouse and keyboard.
#include "control/control_internal.h"

#include "ui/rp_widget.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QRegularExpression>
#include <QtGui/QAccessible>
#include <QtGui/QBackingStore>
#include <QtGui/QGuiApplication>
#include <QtGui/QInputMethodEvent>
#include <QtGui/QKeySequence>
#include <QtGui/QPainter>
#include <QtGui/QWindow>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QWidget>

#include <qpa/qplatformbackingstore.h>
#include <qpa/qwindowsysteminterface.h>

#include <cxxabi.h>
#include <cstdlib>
#include <map>
#include <optional>
#include <typeinfo>
#include <vector>

namespace Control {
namespace {

constexpr auto kMaxNodes = 50000;
constexpr auto kMaxItems = 5000;

struct MouseState {
	QPointer<QWindow> under;
	Qt::MouseButtons buttons;
};
MouseState Mouse;

[[nodiscard]] ulong Timestamp() {
	static auto timer = [] {
		auto result = QElapsedTimer();
		result.start();
		return result;
	}();
	static auto last = ulong(0);
	last = std::max(last + 1, ulong(timer.elapsed()) + 1000);
	return last;
}

[[nodiscard]] std::vector<QPointer<QWidget>> &Registry() {
	static auto result = std::vector<QPointer<QWidget>>();
	return result;
}

[[nodiscard]] QJsonArray RectJson(const QRect &rect) {
	return QJsonArray{ rect.x(), rect.y(), rect.width(), rect.height() };
}

[[nodiscard]] QRect WindowRect(QWidget *widget) {
	const auto window = widget->window();
	const auto topLeft = (widget == window)
		? QPoint()
		: widget->mapTo(window, QPoint());
	return QRect(topLeft, widget->size());
}

[[nodiscard]] std::vector<QWidget*> ChildWidgets(QWidget *widget) {
	auto result = std::vector<QWidget*>();
	for (const auto child : widget->children()) {
		if (const auto w = qobject_cast<QWidget*>(child)) {
			result.push_back(w);
		}
	}
	return result;
}

[[nodiscard]] Ui::RpWidget *Rp(QWidget *widget) {
	return dynamic_cast<Ui::RpWidget*>(widget);
}

[[nodiscard]] QString Role(QWidget *widget) {
	if (const auto rp = Rp(widget)) {
		const auto role = rp->accessibilityRole();
		switch (role) {
		case QAccessible::Button: return u"button"_q;
		case QAccessible::CheckBox: return u"checkbox"_q;
		case QAccessible::RadioButton: return u"radio"_q;
		case QAccessible::Link: return u"link"_q;
		case QAccessible::StaticText: return u"text"_q;
		case QAccessible::EditableText: return u"edit"_q;
		case QAccessible::List: return u"list"_q;
		case QAccessible::ListItem: return u"listitem"_q;
		case QAccessible::MenuItem: return u"menuitem"_q;
		case QAccessible::PopupMenu: return u"menu"_q;
		case QAccessible::Dialog: return u"dialog"_q;
		case QAccessible::ScrollBar: return u"scrollbar"_q;
		case QAccessible::Slider: return u"slider"_q;
		case QAccessible::PageTab: return u"tab"_q;
		case QAccessible::Switch: return u"switch"_q;
		case QAccessible::NoRole: return QString();
		default: return QString::number(int(role));
		}
	}
	return QString();
}

[[nodiscard]] QString WidgetValue(QWidget *widget) {
	if (const auto rp = Rp(widget)) {
		return rp->accessibilityValue();
	} else if (const auto edit = qobject_cast<QLineEdit*>(widget)) {
		return edit->text();
	}
	return QString();
}

struct TreeOptions {
	bool text = false;
	bool items = false;
	bool all = false;
	bool flat = false;
	int depth = 1000;
	int itemsMax = 200;
};

struct TreeState {
	TreeOptions options;
	int nodes = 0;
	bool truncated = false;
	QJsonArray flat;
};

[[nodiscard]] QJsonArray Items(QWidget *widget, int max) {
	auto result = QJsonArray();
	const auto rp = Rp(widget);
	if (!rp) {
		return result;
	}
	const auto count = std::min(rp->accessibilityChildCount(), kMaxItems);
	const auto visible = widget->visibleRegion().boundingRect();
	const auto origin = WindowRect(widget).topLeft();
	for (auto i = 0; i != count && result.size() < max; ++i) {
		const auto local = rp->accessibilityChildRect(i);
		if (local.isEmpty() || !local.intersects(visible)) {
			continue;
		}
		result.push_back(QJsonObject{
			{ u"i"_q, i },
			{ u"name"_q, rp->accessibilityChildName(i) },
			{ u"rect"_q, RectJson(local.translated(origin)) },
		});
	}
	return result;
}

QJsonObject Node(QWidget *widget, const QString &path, int depth, TreeState &state) {
	++state.nodes;
	auto result = QJsonObject{
		{ u"path"_q, path },
		{ u"type"_q, TypeName(widget) },
		{ u"rect"_q, RectJson(WindowRect(widget)) },
	};
	if (!widget->objectName().isEmpty()) {
		result.insert(u"name"_q, widget->objectName());
	}
	if (!widget->isVisible()) {
		result.insert(u"visible"_q, false);
	}
	if (!widget->isEnabled()) {
		result.insert(u"enabled"_q, false);
	}
	if (widget->hasFocus()) {
		result.insert(u"focus"_q, true);
	}
	if (state.options.text) {
		const auto role = Role(widget);
		if (!role.isEmpty()) {
			result.insert(u"role"_q, role);
		}
		const auto text = WidgetText(widget);
		if (!text.isEmpty()) {
			result.insert(u"text"_q, text);
		}
		const auto value = WidgetValue(widget);
		if (!value.isEmpty() && value != text) {
			result.insert(u"value"_q, value);
		}
	}
	if (state.options.items && widget->isVisible()) {
		const auto items = Items(widget, state.options.itemsMax);
		if (!items.isEmpty()) {
			result.insert(u"items"_q, items);
		}
	}
	if (state.options.flat) {
		state.flat.push_back(result);
	}
	auto children = QJsonArray();
	if (depth < state.options.depth) {
		auto index = 0;
		for (const auto child : ChildWidgets(widget)) {
			const auto childPath = path + '/' + QString::number(index++);
			if (child->isWindow()
				|| (!state.options.all && !child->isVisible())) {
				continue;
			} else if (state.nodes >= kMaxNodes) {
				state.truncated = true;
				break;
			}
			auto node = Node(child, childPath, depth + 1, state);
			if (!state.options.flat) {
				children.push_back(std::move(node));
			}
		}
	}
	if (!children.isEmpty()) {
		result.insert(u"children"_q, children);
	}
	return result;
}

struct FindQuery {
	QString type;
	QString name;
	QString text;
	QString role;
	bool exact = false;
	bool visible = true;
};

[[nodiscard]] FindQuery ParseQuery(const QJsonObject &args) {
	auto result = FindQuery();
	result.type = args.value(u"type"_q).toString();
	result.name = args.value(u"name"_q).toString();
	result.text = args.value(u"text"_q).toString();
	result.role = args.value(u"role"_q).toString();
	result.exact = args.value(u"exact"_q).toBool(false);
	result.visible = args.value(u"visible"_q).toBool(true);
	return result;
}

[[nodiscard]] bool Matches(QWidget *widget, const FindQuery &query) {
	if (query.visible && !widget->isVisible()) {
		return false;
	}
	if (!query.type.isEmpty()) {
		const auto type = TypeName(widget);
		if (query.exact ? (type != query.type) : !type.contains(query.type)) {
			return false;
		}
	}
	if (!query.name.isEmpty() && widget->objectName() != query.name) {
		return false;
	}
	if (!query.role.isEmpty() && Role(widget) != query.role) {
		return false;
	}
	if (!query.text.isEmpty()) {
		const auto text = WidgetText(widget);
		if (query.exact
			? (text != query.text)
			: !text.contains(query.text, Qt::CaseInsensitive)) {
			return false;
		}
	}
	return true;
}

void FindIn(
		QWidget *widget,
		const QString &path,
		const FindQuery &query,
		int limit,
		QJsonArray &result,
		std::vector<QWidget*> *widgets) {
	if (result.size() >= limit) {
		return;
	} else if (query.visible && !widget->isVisible()) {
		return;
	}
	if (Matches(widget, query)) {
		auto entry = QJsonObject{
			{ u"path"_q, path },
			{ u"type"_q, TypeName(widget) },
			{ u"rect"_q, RectJson(WindowRect(widget)) },
		};
		const auto text = WidgetText(widget);
		if (!text.isEmpty()) {
			entry.insert(u"text"_q, text);
		}
		result.push_back(entry);
		if (widgets) {
			widgets->push_back(widget);
		}
	}
	auto index = 0;
	for (const auto child : ChildWidgets(widget)) {
		const auto childPath = path + '/' + QString::number(index++);
		if (!child->isWindow()) {
			FindIn(child, childPath, query, limit, result, widgets);
		}
	}
}

[[nodiscard]] std::vector<QWidget*> SearchRoots(
		const QJsonObject &args,
		QString *error) {
	auto result = std::vector<QWidget*>();
	if (args.contains(u"within"_q)) {
		if (const auto root = ResolvePath(
				args.value(u"within"_q).toString(),
				error)) {
			result.push_back(root);
		}
		return result;
	}
	if (args.contains(u"window"_q)) {
		if (const auto window = ResolveWindow(args, error)) {
			result.push_back(window);
		}
		return result;
	}
	Windows(); // Registers new windows.
	for (const auto &window : Registry()) {
		if (window && window->isVisible()) {
			result.push_back(window);
		}
	}
	return result;
}

[[nodiscard]] QWindow *Handle(QWidget *window) {
	return window ? window->windowHandle() : nullptr;
}

void Flush() {
	QWindowSystemInterface::flushWindowSystemEvents();
}

void EnsureUnder(QWindow *handle, QPointF local, QPointF global) {
	if (Mouse.under == handle) {
		return;
	}
	if (Mouse.under) {
		QWindowSystemInterface::handleLeaveEvent(Mouse.under);
	}
	QWindowSystemInterface::handleEnterEvent(handle, local, global);
	Mouse.under = handle;
	Flush();
}

bool MouseEvent(
		const Target &target,
		QEvent::Type type,
		Qt::MouseButton button,
		Qt::KeyboardModifiers mods) {
	const auto handle = Handle(target.window);
	if (!handle) {
		return false;
	}
	const auto local = QPointF(target.point);
	const auto global = QPointF(handle->mapToGlobal(target.point));
	EnsureUnder(handle, local, global);
	if (type == QEvent::MouseButtonPress) {
		Mouse.buttons |= button;
	} else if (type == QEvent::MouseButtonRelease) {
		Mouse.buttons &= ~button;
	}
	QWindowSystemInterface::handleMouseEvent(
		handle,
		Timestamp(),
		local,
		global,
		Mouse.buttons,
		(type == QEvent::MouseMove) ? Qt::NoButton : button,
		type,
		mods);
	Flush();
	return true;
}

[[nodiscard]] QPoint CenterOf(QWidget *widget) {
	const auto visible = widget->visibleRegion().boundingRect();
	const auto local = visible.isEmpty()
		? widget->rect().center()
		: visible.center();
	const auto window = widget->window();
	return (widget == window) ? local : widget->mapTo(window, local);
}

[[nodiscard]] QImage CaptureOne(QWidget *window, Source source, Source *used) {
	if (source == Source::Auto) {
		source = CountGlWidgets(window) ? Source::Grab : Source::BackingStore;
	}
	if (source == Source::BackingStore) {
		const auto store = window->backingStore();
		const auto platform = store ? store->handle() : nullptr;
		auto image = platform ? platform->toImage() : QImage();
		if (!image.isNull()) {
			// Detach from the backing store before it paints again.
			image = image.copy();
			image.setDevicePixelRatio(window->devicePixelRatioF());
			if (used) {
				*used = Source::BackingStore;
			}
			return image;
		}
	}
	if (used) {
		*used = Source::Grab;
	}
	return window->grab().toImage();
}

} // namespace

QString TypeName(const QObject *object) {
	if (!object) {
		return QString();
	}
	static auto cache = std::map<const std::type_info*, QString>();
	const auto &info = typeid(*object);
	const auto i = cache.find(&info);
	if (i != cache.end()) {
		return i->second;
	}
	auto status = 0;
	const auto demangled = abi::__cxa_demangle(
		info.name(),
		nullptr,
		nullptr,
		&status);
	auto result = (status == 0 && demangled)
		? QString::fromUtf8(demangled)
		: QString::fromUtf8(info.name());
	std::free(demangled);
	cache.emplace(&info, result);
	return result;
}

int WindowId(QWidget *window) {
	if (!window) {
		return -1;
	}
	auto &registry = Registry();
	for (auto i = 0, count = int(registry.size()); i != count; ++i) {
		if (registry[i] == window) {
			return i;
		}
	}
	registry.push_back(window);
	return int(registry.size()) - 1;
}

QJsonArray Windows() {
	const auto primary = PrimaryWindow();
	if (primary) {
		WindowId(primary);
	}
	for (const auto widget : QApplication::topLevelWidgets()) {
		if (widget->isVisible() && widget->windowType() != Qt::Desktop) {
			WindowId(widget);
		}
	}
	auto result = QJsonArray();
	const auto &registry = Registry();
	for (auto i = 0, count = int(registry.size()); i != count; ++i) {
		const auto window = registry[i].data();
		if (!window || !window->isVisible()) {
			continue;
		}
		auto entry = QJsonObject{
			{ u"id"_q, i },
			{ u"type"_q, TypeName(window) },
			{ u"geometry"_q, RectJson(window->geometry()) },
			{ u"dpr"_q, window->devicePixelRatioF() },
			{ u"active"_q, window->isActiveWindow() },
		};
		if (window == primary) {
			entry.insert(u"primary"_q, true);
		}
		if (!window->windowTitle().isEmpty()) {
			entry.insert(u"title"_q, window->windowTitle());
		}
		if (const auto gl = CountGlWidgets(window)) {
			entry.insert(u"gl_widgets"_q, gl);
		}
		result.push_back(entry);
	}
	return result;
}

QWidget *ResolveWindow(const QJsonObject &args, QString *error) {
	const auto value = args.value(u"window"_q);
	if (!value.isUndefined() && !value.isNull()) {
		Windows();
		const auto id = value.toInt(-1);
		const auto &registry = Registry();
		if (id < 0 || id >= int(registry.size()) || !registry[id]) {
			*error = u"no window %1"_q.arg(value.toVariant().toString());
			return nullptr;
		}
		return registry[id];
	}
	if (const auto popup = QApplication::activePopupWidget()) {
		if (args.value(u"popup"_q).toBool(false)) {
			WindowId(popup);
			return popup;
		}
	}
	if (const auto primary = PrimaryWindow()) {
		if (primary->isVisible()) {
			WindowId(primary);
			return primary;
		}
	}
	if (const auto active = QApplication::activeWindow()) {
		WindowId(active);
		return active;
	}
	for (const auto widget : QApplication::topLevelWidgets()) {
		if (widget->isVisible() && widget->windowHandle()) {
			WindowId(widget);
			return widget;
		}
	}
	*error = u"no visible window"_q;
	return nullptr;
}

QWidget *ResolvePath(const QString &path, QString *error) {
	const auto parts = path.split('/', Qt::SkipEmptyParts);
	if (parts.isEmpty()) {
		*error = u"empty path"_q;
		return nullptr;
	}
	auto ok = false;
	const auto id = parts[0].toInt(&ok);
	Windows();
	const auto &registry = Registry();
	if (!ok || id < 0 || id >= int(registry.size()) || !registry[id]) {
		*error = u"no window in path: "_q + path;
		return nullptr;
	}
	auto widget = registry[id].data();
	for (auto i = 1; i < parts.size(); ++i) {
		const auto index = parts[i].toInt(&ok);
		const auto children = ChildWidgets(widget);
		if (!ok || index < 0 || index >= int(children.size())) {
			*error = u"no widget at path: "_q + parts.mid(0, i + 1).join('/');
			return nullptr;
		}
		widget = children[index];
	}
	return widget;
}

QString PathOf(QWidget *widget) {
	auto parts = QStringList();
	while (widget && !widget->isWindow()) {
		const auto parent = widget->parentWidget();
		const auto children = ChildWidgets(parent);
		const auto i = std::find(children.begin(), children.end(), widget);
		parts.push_front(QString::number(int(i - children.begin())));
		widget = parent;
	}
	parts.push_front(QString::number(WindowId(widget)));
	return parts.join('/');
}

QString WidgetText(QWidget *widget) {
	if (const auto rp = Rp(widget)) {
		const auto name = rp->accessibilityName();
		if (!name.isEmpty()) {
			return name;
		}
	}
	if (const auto label = qobject_cast<QLabel*>(widget)) {
		return label->text();
	} else if (const auto button = qobject_cast<QAbstractButton*>(widget)) {
		return button->text();
	}
	return QString();
}

QJsonObject Tree(const QJsonObject &args, QString *error) {
	auto state = TreeState();
	state.options.text = args.value(u"text"_q).toBool(false);
	state.options.items = args.value(u"items"_q).toBool(false);
	state.options.all = args.value(u"all"_q).toBool(false);
	state.options.flat = args.value(u"flat"_q).toBool(false);
	state.options.depth = args.value(u"depth"_q).toInt(1000);
	state.options.itemsMax = args.value(u"items_max"_q).toInt(200);

	auto roots = QJsonArray();
	if (args.contains(u"path"_q)) {
		const auto path = args.value(u"path"_q).toString();
		const auto widget = ResolvePath(path, error);
		if (!widget) {
			return {};
		}
		roots.push_back(Node(widget, PathOf(widget), 0, state));
	} else {
		auto searchError = QString();
		const auto windows = SearchRoots(args, &searchError);
		if (!searchError.isEmpty()) {
			*error = searchError;
			return {};
		}
		for (const auto window : windows) {
			auto node = Node(
				window,
				QString::number(WindowId(window)),
				0,
				state);
			node.insert(u"window"_q, WindowId(window));
			node.insert(u"geometry"_q, RectJson(window->geometry()));
			roots.push_back(std::move(node));
		}
	}
	auto result = QJsonObject{
		{ u"nodes"_q, state.nodes },
	};
	if (state.truncated) {
		result.insert(u"truncated"_q, true);
	}
	if (state.options.flat) {
		result.insert(u"widgets"_q, state.flat);
	} else {
		result.insert(u"windows"_q, roots);
	}
	return result;
}

QJsonArray Find(const QJsonObject &args, int limit) {
	auto error = QString();
	const auto query = ParseQuery(args);
	auto result = QJsonArray();
	for (const auto root : SearchRoots(args, &error)) {
		FindIn(root, PathOf(root), query, limit, result, nullptr);
	}
	return result;
}

bool ResolveTarget(const QJsonObject &args, Target *target, QString *error) {
	auto widget = (QWidget*)nullptr;
	if (args.contains(u"path"_q)) {
		widget = ResolvePath(args.value(u"path"_q).toString(), error);
		if (!widget) {
			return false;
		}
	} else if (args.contains(u"find"_q)) {
		const auto find = args.value(u"find"_q).toObject();
		const auto nth = find.value(u"nth"_q).toInt(0);
		const auto query = ParseQuery(find);
		auto found = QJsonArray();
		auto widgets = std::vector<QWidget*>();
		for (const auto root : SearchRoots(find, error)) {
			FindIn(root, PathOf(root), query, nth + 1, found, &widgets);
		}
		if (int(widgets.size()) <= nth) {
			*error = u"no widget matches find"_q;
			return false;
		}
		widget = widgets[nth];
	}
	if (widget) {
		target->widget = widget;
		target->window = widget->window();
		if (args.contains(u"item"_q)) {
			const auto rp = Rp(widget);
			const auto item = args.value(u"item"_q).toInt(-1);
			if (!rp || item < 0 || item >= rp->accessibilityChildCount()) {
				*error = u"no item %1"_q.arg(item);
				return false;
			}
			const auto local = rp->accessibilityChildRect(item);
			if (local.isEmpty()) {
				*error = u"item %1 has no geometry"_q.arg(item);
				return false;
			}
			target->point = WindowRect(widget).topLeft() + local.center();
		} else {
			target->point = CenterOf(widget);
		}
		// "dx"/"dy" offsets are in pixels from the computed point,
		// except for scroll, where they are deltas.
		if (args.value(u"cmd"_q).toString() != u"scroll"_q) {
			target->point += QPoint(
				args.value(u"dx"_q).toInt(0),
				args.value(u"dy"_q).toInt(0));
		}
		return true;
	}
	if (!args.contains(u"x"_q) || !args.contains(u"y"_q)) {
		*error = u"need x and y, path or find"_q;
		return false;
	}
	target->window = ResolveWindow(args, error);
	if (!target->window) {
		return false;
	}
	target->point = QPoint(
		args.value(u"x"_q).toInt(),
		args.value(u"y"_q).toInt());
	return true;
}

void Activate(QWidget *window) {
	const auto handle = Handle(window);
	if (!handle) {
		return;
	}
	window->raise();
	window->activateWindow();
	QWindowSystemInterface::handleFocusWindowChanged(
		handle,
		Qt::ActiveWindowFocusReason);
	Flush();
}

bool Move(const Target &target, Qt::KeyboardModifiers mods) {
	return MouseEvent(target, QEvent::MouseMove, Qt::NoButton, mods);
}

bool Press(
		const Target &target,
		Qt::MouseButton button,
		Qt::KeyboardModifiers mods,
		bool down) {
	return MouseEvent(
		target,
		down ? QEvent::MouseButtonPress : QEvent::MouseButtonRelease,
		button,
		mods);
}

bool Click(
		const Target &target,
		Qt::MouseButton button,
		Qt::KeyboardModifiers mods,
		int count) {
	// Hover first: tdesktop picks the click handler under the cursor on
	// mouse move, before the press.
	if (!Move(target, mods)) {
		return false;
	}
	for (auto i = 0; i != count; ++i) {
		if (!target.window
			|| !Press(target, button, mods, true)
			|| !target.window
			|| !Press(target, button, mods, false)) {
			return false;
		}
	}
	return true;
}

bool Wheel(
		const Target &target,
		QPoint pixelDelta,
		QPoint angleDelta,
		Qt::KeyboardModifiers mods) {
	const auto handle = Handle(target.window);
	if (!handle) {
		return false;
	}
	const auto local = QPointF(target.point);
	const auto global = QPointF(handle->mapToGlobal(target.point));
	EnsureUnder(handle, local, global);
	QWindowSystemInterface::handleWheelEvent(
		handle,
		Timestamp(),
		local,
		global,
		pixelDelta,
		angleDelta,
		mods);
	Flush();
	return true;
}

Qt::KeyboardModifiers ParseModifiers(const QJsonValue &value) {
	auto list = QStringList();
	if (value.isArray()) {
		for (const auto &item : value.toArray()) {
			list.push_back(item.toString());
		}
	} else {
		list = value.toString().split(
			QRegularExpression(u"[+, ]"_q),
			Qt::SkipEmptyParts);
	}
	auto result = Qt::KeyboardModifiers();
	for (const auto &item : list) {
		const auto name = item.toLower();
		if (name == u"ctrl"_q || name == u"control"_q) {
			result |= Qt::ControlModifier;
		} else if (name == u"shift"_q) {
			result |= Qt::ShiftModifier;
		} else if (name == u"alt"_q) {
			result |= Qt::AltModifier;
		} else if (name == u"meta"_q || name == u"super"_q) {
			result |= Qt::MetaModifier;
		}
	}
	return result;
}

[[nodiscard]] QString KeyText(int key, Qt::KeyboardModifiers mods) {
	if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
		return QString();
	}
	switch (key) {
	case Qt::Key_Return:
	case Qt::Key_Enter: return u"\r"_q;
	case Qt::Key_Tab: return u"\t"_q;
	case Qt::Key_Backspace: return QString(QChar(0x08));
	case Qt::Key_Escape: return QString(QChar(0x1b));
	case Qt::Key_Delete: return QString(QChar(0x7f));
	case Qt::Key_Space: return u" "_q;
	}
	if (key >= 0x20 && key < 0x7f) {
		const auto ch = QChar(key);
		return (mods & Qt::ShiftModifier)
			? QString(ch.toUpper())
			: QString(ch.toLower());
	}
	return QString();
}

bool SendKey(
		QWidget *window,
		int key,
		Qt::KeyboardModifiers mods,
		const QString &text) {
	const auto handle = Handle(window);
	if (!handle) {
		return false;
	}
	if (QGuiApplication::focusWindow() != handle) {
		Activate(window);
	}
	QWindowSystemInterface::handleKeyEvent(
		handle,
		Timestamp(),
		QEvent::KeyPress,
		key,
		mods,
		text);
	Flush();
	QWindowSystemInterface::handleKeyEvent(
		handle,
		Timestamp(),
		QEvent::KeyRelease,
		key,
		mods,
		text);
	Flush();
	return true;
}

bool Key(QWidget *window, const QString &sequence, QString *error) {
	const auto keys = QKeySequence::fromString(
		sequence,
		QKeySequence::PortableText);
	if (keys.isEmpty()) {
		*error = u"bad key: "_q + sequence;
		return false;
	}
	for (auto i = 0; i != keys.count(); ++i) {
		const auto combination = keys[i];
		const auto key = int(combination.key());
		const auto mods = combination.keyboardModifiers();
		if (key == Qt::Key_unknown) {
			*error = u"bad key: "_q + sequence;
			return false;
		}
		if (!SendKey(window, key, mods, KeyText(key, mods))) {
			*error = u"window has no native handle"_q;
			return false;
		}
	}
	return true;
}

bool TypeText(
		QWidget *window,
		const QString &text,
		bool asKeys,
		QString *error) {
	if (!asKeys) {
		const auto handle = Handle(window);
		if (handle && QGuiApplication::focusWindow() != handle) {
			Activate(window);
		}
		const auto focus = QGuiApplication::focusObject();
		if (!focus) {
			*error = u"no focused widget"_q;
			return false;
		}
		auto event = QInputMethodEvent();
		event.setCommitString(text);
		QCoreApplication::sendEvent(focus, &event);
		return true;
	}
	for (const auto ch : text) {
		const auto ascii = (ch.unicode() < 0x80) && ch.isLetterOrNumber();
		const auto key = ascii ? int(ch.toUpper().unicode()) : 0;
		const auto mods = (ascii && ch.isUpper())
			? Qt::ShiftModifier
			: Qt::NoModifier;
		if (!SendKey(window, key, mods, QString(ch))) {
			*error = u"window has no native handle"_q;
			return false;
		}
	}
	return true;
}

Source ParseSource(const QString &value, bool *ok) {
	*ok = true;
	if (value == u"grab"_q) {
		return Source::Grab;
	} else if (value == u"backingstore"_q || value == u"store"_q) {
		return Source::BackingStore;
	} else if (value == u"auto"_q || value.isEmpty()) {
		return Source::Auto;
	}
	*ok = false;
	return Source::Auto;
}

QString SourceName(Source source) {
	switch (source) {
	case Source::Grab: return u"grab"_q;
	case Source::BackingStore: return u"backingstore"_q;
	case Source::Auto: return u"auto"_q;
	}
	return QString();
}

int CountGlWidgets(QWidget *window) {
	auto result = 0;
	for (const auto child : window->findChildren<QWidget*>()) {
		if (child->isVisible()
			&& (child->inherits("QOpenGLWidget")
				|| child->inherits("QQuickWidget")
				|| child->inherits("QRhiWidget"))) {
			++result;
		}
	}
	return result;
}

QImage Capture(QWidget *window, Source source, bool composite, Source *used) {
	auto result = CaptureOne(window, source, used);
	if (!composite || result.isNull()) {
		return result;
	}
	// Popups, menus and tooltips are separate top-level windows.
	const auto geometry = window->geometry();
	auto painter = std::optional<QPainter>();
	for (const auto widget : QApplication::topLevelWidgets()) {
		if (widget == window
			|| !widget->isVisible()
			|| widget->windowType() == Qt::Desktop
			|| !widget->geometry().intersects(geometry)) {
			continue;
		}
		const auto type = widget->windowType();
		if (type != Qt::Popup
			&& type != Qt::ToolTip
			&& type != Qt::Tool
			&& type != Qt::SplashScreen
			&& !(widget->windowFlags() & Qt::WindowStaysOnTopHint)) {
			continue;
		}
		const auto image = CaptureOne(widget, source, nullptr);
		if (image.isNull()) {
			continue;
		}
		if (!painter) {
			if (result.format() != QImage::Format_ARGB32_Premultiplied
				&& result.format() != QImage::Format_RGB32) {
				result = result.convertToFormat(
					QImage::Format_ARGB32_Premultiplied);
			}
			painter.emplace(&result);
		}
		painter->drawImage(
			QPointF(widget->geometry().topLeft() - geometry.topLeft()),
			image);
	}
	return result;
}

} // namespace Control
