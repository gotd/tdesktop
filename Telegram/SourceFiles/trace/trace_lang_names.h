/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

// Lang key names by codegen slot, generated at build time from
// gen/lang_auto.indices by Telegram/cmake/trace_lang_names.cmake. A key
// with plural forms takes `size` consecutive slots from `index`.

namespace Trace::Ui::details {

struct LangName {
	unsigned short index = 0;
	unsigned short size = 0;
	const char *name = nullptr;
};

extern const LangName kLangNames[];
extern const int kLangNamesCount;

} // namespace Trace::Ui::details
