/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>

// Constructor ids used by the tracer, kept here so the redactor builds
// without the generated scheme; trace.cpp checks them against scheme.h.
#define TRACE_IDS(X) \
	X(vector, 0x1cb5c415) \
	X(rpc_result, 0xf35c6d01) \
	X(msg_container, 0x73f1f8dc) \
	X(gzip_packed, 0x3072cfa1) \
	X(inputCheckPasswordSRP, 0xd27ff082) \
	X(emailVerificationCode, 0x922e55a9) \
	X(emailVerificationGoogle, 0xdb909ec2) \
	X(emailVerificationApple, 0x96d074fd) \
	X(jsonObjectValue, 0xc0de1bd9) \
	X(jsonNull, 0x3f6d7b68) \
	X(jsonBool, 0xc7345e6a) \
	X(jsonNumber, 0x2be0dfa4) \
	X(jsonString, 0xb71e767a) \
	X(jsonArray, 0xf7444763) \
	X(jsonObject, 0x99c1d49d) \
	X(inputClientProxy, 0x75588b3f) \
	X(invokeWithLayer, 0xda9b0d0d) \
	X(initConnection, 0xc1cd5ea9) \
	X(invokeAfterMsg, 0xcb9f372d) \
	X(invokeAfterMsgs, 0x3dc4b4f0) \
	X(invokeWithoutUpdates, 0xbf9459b7) \
	X(invokeWithTakeout, 0xaca9fd2e) \
	X(invokeWithMessagesRange, 0x365275f2) \
	X(invokeWithBusinessConnection, 0xdd289f8e) \
	X(invokeWithGooglePlayIntegrity, 0x1df92984) \
	X(invokeWithApnsSecret, 0xdae54f8) \
	X(invokeWithReCaptcha, 0xadbb0f94) \
	X(auth_sendCode, 0xa677244f) \
	X(auth_exportLoginToken, 0xb7e085fe) \
	X(auth_exportAuthorization, 0xe5bfffcd) \
	X(auth_exportedAuthorization, 0xb434e2b8) \
	X(auth_authorization, 0x2ea2c0d4) \
	X(auth_loginTokenSuccess, 0x390d5c5e) \
	X(auth_loginToken, 0x629f1980) \
	X(account_tmpPassword, 0xdb64fd34) \
	X(account_passwordSettings, 0x9a5c33e5) \
	X(rpc_error, 0x2144ca19) \
	X(auth_signIn, 0x8d52a951) \
	X(auth_signUp, 0xaac7b717) \
	X(auth_checkPassword, 0xd18b4d16) \
	X(auth_recoverPassword, 0x37096c70) \
	X(auth_checkRecoveryPassword, 0xd36bf79) \
	X(auth_importAuthorization, 0xa57a7dad) \
	X(auth_importBotAuthorization, 0x67a3ff2c) \
	X(auth_importLoginToken, 0x95ac5ce4) \
	X(auth_acceptLoginToken, 0xe894ad4d) \
	X(auth_importWebTokenAuthorization, 0x2db873a9) \
	X(account_getPasswordSettings, 0x9cd4eaf9) \
	X(account_updatePasswordSettings, 0xa59b102f) \
	X(account_confirmPasswordEmail, 0x8fdf1920) \
	X(account_getTmpPassword, 0x449e0b51) \
	X(account_saveSecureValue, 0x899fe31d) \
	X(account_changePhone, 0x70c32edb) \
	X(account_confirmPhone, 0x5f2178c3) \
	X(account_verifyPhone, 0x4dd3a7f6) \
	X(account_verifyEmail, 0x32da4cf) \
	X(account_deleteAccount, 0xa2c0cf74) \
	X(payments_sendPaymentForm, 0x2d03522f) \
	X(messages_requestEncryption, 0xf64daf43) \
	X(messages_acceptEncryption, 0x3dbc0415)

namespace Trace::details::ids {

#define TRACE_ID_CONSTANT(name, value) \
	inline constexpr uint32_t name = uint32_t(value);
TRACE_IDS(TRACE_ID_CONSTANT)
#undef TRACE_ID_CONSTANT

} // namespace Trace::details::ids
