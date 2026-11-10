/*
This file is part of the gotd/tdesktop fork for teled
(https://github.com/gotd/teled), a testing MTProto server.

Runtime configuration of the built-in DC endpoints and RSA public keys.
See docs/teled.md for the user-facing description.
*/
#include "mtproto/teled/teled_config.h"

// Generated at configure time by Telegram/cmake/teled.cmake.
#ifndef TDESKTOP_TELED_BUILD_DEFAULTS
#error "Build defaults are missing, include Telegram/cmake/teled.cmake."
#endif // TDESKTOP_TELED_BUILD_DEFAULTS
#include "teled_build_defaults.h"

#ifdef TDESKTOP_TELED_STANDALONE
#include <QtCore/QDebug>
#define LOG(msg) (qWarning().noquote() << QString msg)
#else // TDESKTOP_TELED_STANDALONE
#include "logs.h"
#endif // TDESKTOP_TELED_STANDALONE

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QHostInfo>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>

namespace MTP::teled {
namespace {

constexpr auto kEnvServer = "TDESKTOP_TELED_SERVER";
constexpr auto kEnvRsaKey = "TDESKTOP_TELED_RSA_KEY";
constexpr auto kEnvPin = "TDESKTOP_TELED_PIN";
constexpr auto kEnvConfig = "TDESKTOP_TELED_CONFIG";
constexpr auto kConfigName = "teled.json";

struct BIODeleter {
	void operator()(BIO *value) const {
		BIO_free(value);
	}
};
using BIOPtr = std::unique_ptr<BIO, BIODeleter>;

struct RSADeleter {
	void operator()(RSA *value) const {
		RSA_free(value);
	}
};
using RSAPtr = std::unique_ptr<RSA, RSADeleter>;

void Report(QStringList *list, const QString &message) {
	if (list) {
		list->push_back(message);
	}
}

BIOPtr ReadBIO(const QByteArray &data) {
	return BIOPtr(BIO_new_mem_buf(data.constData(), int(data.size())));
}

RSAPtr ReadPublic(const QByteArray &pem) {
	const auto bio = ReadBIO(pem);
	if (!bio) {
		return nullptr;
	}
	return RSAPtr(pem.contains("BEGIN RSA PUBLIC KEY")
		? PEM_read_bio_RSAPublicKey(bio.get(), nullptr, nullptr, nullptr)
		: PEM_read_bio_RSA_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
}

// PKCS#1 "RSA PUBLIC KEY" PEM of the public part of a private key.
QByteArray PublicFromPrivate(const QByteArray &pem) {
	const auto in = ReadBIO(pem);
	if (!in) {
		return {};
	}
	const auto rsa = RSAPtr(
		PEM_read_bio_RSAPrivateKey(in.get(), nullptr, nullptr, nullptr));
	if (!rsa) {
		return {};
	}
	const auto out = BIOPtr(BIO_new(BIO_s_mem()));
	if (!out || !PEM_write_bio_RSAPublicKey(out.get(), rsa.get())) {
		return {};
	}
	char *data = nullptr;
	const auto size = BIO_get_mem_data(out.get(), &data);
	return (size > 0 && data) ? QByteArray(data, int(size)) : QByteArray();
}

// TL "bytes" serialization, as in MTP_bytes(...).write().
void AppendTLBytes(QByteArray &to, const QByteArray &value) {
	const auto size = value.size();
	if (size < 254) {
		to.append(char(size));
	} else {
		to.append(char(254));
		to.append(char(size & 0xFF));
		to.append(char((size >> 8) & 0xFF));
		to.append(char((size >> 16) & 0xFF));
	}
	to.append(value);
	while (to.size() % 4) {
		to.append(char(0));
	}
}

QByteArray BigNumBytes(const BIGNUM *number) {
	auto result = QByteArray(BN_num_bytes(number), char(0));
	BN_bn2bin(number, reinterpret_cast<unsigned char*>(result.data()));
	return result;
}

// Same value as MTP::details::RSAPublicKey::fingerprint(): lower 64 bits
// of SHA1(TL bytes n + TL bytes e), little endian.
quint64 Fingerprint(const RSA *rsa) {
	const BIGNUM *n = nullptr;
	const BIGNUM *e = nullptr;
	RSA_get0_key(rsa, &n, &e, nullptr);
	auto buffer = QByteArray();
	AppendTLBytes(buffer, BigNumBytes(n));
	AppendTLBytes(buffer, BigNumBytes(e));
	unsigned char hash[SHA_DIGEST_LENGTH] = { 0 };
	SHA1(
		reinterpret_cast<const unsigned char*>(buffer.constData()),
		size_t(buffer.size()),
		hash);
	auto result = quint64(0);
	for (auto i = 0; i != 8; ++i) {
		result |= quint64(hash[12 + i]) << (8 * i);
	}
	return result;
}

QString FingerprintText(quint64 value) {
	return QStringLiteral("%1 (%2)").arg(
		QString::number(value, 16).rightJustified(16, QChar(u'0'))
	).arg(qint64(value));
}

std::optional<bool> ParseBool(const QString &value) {
	const auto v = value.trimmed().toLower();
	if (v == QStringLiteral("1")
		|| v == QStringLiteral("true")
		|| v == QStringLiteral("yes")
		|| v == QStringLiteral("on")) {
		return true;
	} else if (v == QStringLiteral("0")
		|| v == QStringLiteral("false")
		|| v == QStringLiteral("no")
		|| v == QStringLiteral("off")) {
		return false;
	}
	return std::nullopt;
}

std::optional<Endpoint> ParseOne(QString item, QString *error) {
	auto dcId = kDefaultDcId;
	static const auto DcRegExp = QRegularExpression(QStringLiteral("^(\\d+)@(.+)$"));
	if (const auto match = DcRegExp.match(item); match.hasMatch()) {
		dcId = match.captured(1).toInt();
		item = match.captured(2);
	}
	auto host = QString();
	auto portText = QString();
	if (item.startsWith(QChar(u'['))) {
		const auto close = item.indexOf(QChar(u']'));
		if (close < 0) {
			*error = QStringLiteral("missing ']'");
			return std::nullopt;
		}
		host = item.mid(1, close - 1);
		const auto rest = item.mid(close + 1);
		if (!rest.isEmpty()) {
			if (!rest.startsWith(QChar(u':'))) {
				*error = QStringLiteral("expected ':' after ']'");
				return std::nullopt;
			}
			portText = rest.mid(1);
		}
	} else if (item.count(QChar(u':')) == 1) {
		const auto colon = item.indexOf(QChar(u':'));
		host = item.left(colon);
		portText = item.mid(colon + 1);
	} else {
		// No port, or a bare IPv6 address.
		host = item;
	}
	auto port = kDefaultPort;
	if (!portText.isEmpty()) {
		auto ok = false;
		port = portText.toInt(&ok);
		if (!ok || port <= 0 || port > 65535) {
			*error = QStringLiteral("bad port '%1'").arg(portText);
			return std::nullopt;
		}
	}
	if (dcId <= 0 || dcId >= 10000) {
		*error = QStringLiteral("bad dc id %1").arg(dcId);
		return std::nullopt;
	}
	if (host.isEmpty()) {
		*error = QStringLiteral("empty host");
		return std::nullopt;
	}
	auto address = QHostAddress();
	if (!address.setAddress(host)) {
		// Blocking lookup, hostnames are only expected for local setups
		// such as docker compose service names.
		const auto info = QHostInfo::fromName(host);
		const auto addresses = info.addresses();
		if (addresses.isEmpty()) {
			*error = QStringLiteral("could not resolve '%1': %2")
				.arg(host, info.errorString());
			return std::nullopt;
		}
		address = addresses.front();
	}
	const auto ipv6 = (address.protocol() == QAbstractSocket::IPv6Protocol);
	return Endpoint{
		.dcId = dcId,
		.ip = address.toString().toStdString(),
		.port = port,
		.ipv6 = ipv6,
	};
}

QByteArray ReadFile(const QString &path, QStringList *log) {
	auto file = QFile(path);
	if (!file.open(QIODevice::ReadOnly)) {
		Report(log, QStringLiteral("Error: could not read '%1': %2")
			.arg(path, file.errorString()));
		return {};
	}
	return file.readAll();
}

QStringList JsonStrings(const QJsonValue &value) {
	auto result = QStringList();
	if (value.isString()) {
		result.push_back(value.toString());
	} else if (value.isArray()) {
		for (const auto &item : value.toArray()) {
			if (item.isString()) {
				result.push_back(item.toString());
			}
		}
	}
	return result;
}

struct JsonConfig {
	QString path;
	QStringList servers;
	QStringList keyFiles; // Absolute.
	QStringList keys; // Inline PEM.
	std::optional<bool> pin;
};

std::optional<JsonConfig> ReadJson(const Sources &sources, QStringList *log) {
	auto path = sources.configPath;
	if (path.isEmpty()) {
		const auto candidates = QStringList{
			QDir::current().absoluteFilePath(QString::fromLatin1(kConfigName)),
			sources.exeDir.isEmpty()
				? QString()
				: QDir(sources.exeDir).absoluteFilePath(
					QString::fromLatin1(kConfigName)),
		};
		for (const auto &candidate : candidates) {
			if (!candidate.isEmpty() && QFileInfo::exists(candidate)) {
				path = candidate;
				break;
			}
		}
		if (path.isEmpty()) {
			return std::nullopt;
		}
	}
	const auto data = ReadFile(path, log);
	if (data.isEmpty()) {
		return std::nullopt;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(data, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		Report(log, QStringLiteral("Error: bad JSON in '%1': %2")
			.arg(path, error.errorString()));
		return std::nullopt;
	}
	const auto object = document.object();
	const auto dir = QFileInfo(path).absoluteDir();
	auto result = JsonConfig();
	result.path = path;
	result.servers = JsonStrings(object.value(QStringLiteral("servers")))
		+ JsonStrings(object.value(QStringLiteral("server")));
	for (const auto &file : JsonStrings(object.value(QStringLiteral("rsa_key_file")))) {
		result.keyFiles.push_back(dir.absoluteFilePath(file));
	}
	result.keys = JsonStrings(object.value(QStringLiteral("rsa_key")));
	if (const auto pin = object.value(QStringLiteral("pin")); pin.isBool()) {
		result.pin = pin.toBool();
	}
	return result;
}

} // namespace

std::vector<Endpoint> ParseServers(const QString &value, QStringList *log) {
	auto result = std::vector<Endpoint>();
	static const auto Separators = QRegularExpression(QStringLiteral("[,;\\s]+"));
	for (const auto &item : value.split(Separators, Qt::SkipEmptyParts)) {
		auto error = QString();
		if (const auto parsed = ParseOne(item, &error)) {
			const auto duplicate = std::find_if(
				result.begin(),
				result.end(),
				[&](const Endpoint &e) {
					return e.dcId == parsed->dcId
						&& e.ip == parsed->ip
						&& e.port == parsed->port;
				});
			if (duplicate == result.end()) {
				result.push_back(*parsed);
			}
		} else {
			Report(log, QStringLiteral("Error: bad server '%1': %2").arg(item, error));
		}
	}
	return result;
}

std::vector<QByteArray> ParsePublicKeys(
		const QByteArray &pem,
		QStringList *log) {
	auto result = std::vector<QByteArray>();
	constexpr auto kBegin = "-----BEGIN ";
	constexpr auto kDashes = "-----";
	auto from = qsizetype(0);
	while (true) {
		const auto begin = pem.indexOf(kBegin, from);
		if (begin < 0) {
			break;
		}
		const auto labelStart = begin + qsizetype(strlen(kBegin));
		const auto labelEnd = pem.indexOf(kDashes, labelStart);
		if (labelEnd < 0) {
			Report(log, QStringLiteral("Error: truncated PEM header"));
			break;
		}
		const auto label = pem.mid(labelStart, labelEnd - labelStart);
		const auto endMarker = QByteArray(
			QByteArray("-----END ") + label + "-----");
		const auto end = pem.indexOf(endMarker, labelEnd);
		if (end < 0) {
			Report(log, QStringLiteral("Error: missing '%1'")
				.arg(QString::fromLatin1(endMarker)));
			break;
		}
		from = end + endMarker.size();
		const auto block = QByteArray(pem.mid(begin, from - begin) + '\n');
		auto publicPem = QByteArray();
		if (label == "RSA PUBLIC KEY" || label == "PUBLIC KEY") {
			publicPem = block;
		} else if (label == "RSA PRIVATE KEY") {
			publicPem = PublicFromPrivate(block);
			if (publicPem.isEmpty()) {
				Report(
					log,
					QStringLiteral("Error: could not read RSA private key"));
				continue;
			}
		} else {
			Report(log, QStringLiteral("Error: unsupported PEM block '%1', "
				"expected 'RSA PUBLIC KEY', 'PUBLIC KEY' or 'RSA PRIVATE KEY'"
			).arg(QString::fromLatin1(label)));
			continue;
		}
		const auto rsa = ReadPublic(publicPem);
		if (!rsa) {
			Report(log, QStringLiteral("Error: could not read RSA key "
				"from '%1' block"
			).arg(QString::fromLatin1(label)));
			continue;
		}
		if (std::find(result.begin(), result.end(), publicPem)
			== result.end()) {
			result.push_back(publicPem);
			Report(log, QStringLiteral("RSA key fingerprint %1")
				.arg(FingerprintText(Fingerprint(rsa.get()))));
		}
	}
	return result;
}

Config Load(const Sources &sources, QStringList *log) {
	auto result = Config();
	auto sourcesList = QStringList();
	const auto json = ReadJson(sources, log);
	if (json) {
		Report(log, QStringLiteral("Using config '%1'").arg(json->path));
	}

	// Endpoints.
	const auto tryServers = [&](const QString &value, const QString &source) {
		if (!result.endpoints.empty() || value.trimmed().isEmpty()) {
			return false;
		}
		result.endpoints = ParseServers(value, log);
		if (result.endpoints.empty()) {
			Report(log, QStringLiteral("Error: no valid servers in %1, "
				"ignoring").arg(source));
			return false;
		}
		sourcesList.push_back(QStringLiteral("servers from %1").arg(source));
		return true;
	};
	const auto runtimeServers = tryServers(
		sources.envServer,
		QString::fromLatin1(kEnvServer))
		|| (json
			&& tryServers(json->servers.join(QChar(u',')), json->path));
	tryServers(sources.buildServer, QStringLiteral("build defaults"));
	if (result.endpoints.empty()) {
		result.endpoints.push_back({
			.dcId = kDefaultDcId,
			.ip = "127.0.0.1",
			.port = kDefaultPort,
			.ipv6 = false,
		});
		sourcesList.push_back(QStringLiteral("servers from fallback"));
	}

	// Pinning.
	result.pinned = runtimeServers;
	if (json && json->pin) {
		result.pinned = *json->pin;
	}
	if (!sources.envPin.isEmpty()) {
		if (const auto pin = ParseBool(sources.envPin)) {
			result.pinned = *pin;
		} else {
			Report(log, QStringLiteral("Error: bad %1 value '%2'")
				.arg(QString::fromLatin1(kEnvPin), sources.envPin));
		}
	}

	// Public keys.
	const auto tryKeys = [&](const QByteArray &pem, const QString &source) {
		if (!result.publicKeys.empty() || pem.trimmed().isEmpty()) {
			return;
		}
		result.publicKeys = ParsePublicKeys(pem, log);
		if (result.publicKeys.empty()) {
			Report(log, QStringLiteral("Error: no valid RSA keys in %1, "
				"ignoring").arg(source));
			return;
		}
		sourcesList.push_back(QStringLiteral("keys from %1").arg(source));
	};
	if (!sources.envRsaKey.isEmpty()) {
		const auto inlined = sources.envRsaKey.contains(
			QStringLiteral("-----BEGIN"));
		tryKeys(
			inlined
				? sources.envRsaKey.toUtf8()
				: ReadFile(sources.envRsaKey, log),
			inlined
				? QString::fromLatin1(kEnvRsaKey)
				: QStringLiteral("%1=%2").arg(
					QString::fromLatin1(kEnvRsaKey),
					sources.envRsaKey));
	}
	if (json) {
		auto pem = json->keys.join(QChar(u'\n')).toUtf8();
		for (const auto &file : json->keyFiles) {
			pem += '\n' + ReadFile(file, log);
		}
		tryKeys(pem, json->path);
	}
	tryKeys(sources.buildRsaKeys, QStringLiteral("build defaults"));

	for (const auto &endpoint : result.endpoints) {
		const auto ip = QString::fromStdString(endpoint.ip);
		Report(log, QStringLiteral("DC %1 endpoint %2:%3")
			.arg(endpoint.dcId)
			.arg(endpoint.ipv6 ? QStringLiteral("[%1]").arg(ip) : ip)
			.arg(endpoint.port));
	}
	result.source = sourcesList.join(QStringLiteral(", "));
	Report(log, QStringLiteral("%1, endpoints %2")
		.arg(result.source)
		.arg(result.pinned
			? QStringLiteral("pinned")
			: QStringLiteral("not pinned")));
	return result;
}

const Config &Current() {
	static const auto result = [] {
		auto sources = Sources();
		sources.envServer = qEnvironmentVariable(kEnvServer);
		sources.envRsaKey = qEnvironmentVariable(kEnvRsaKey);
		sources.envPin = qEnvironmentVariable(kEnvPin);
		sources.configPath = qEnvironmentVariable(kEnvConfig);
		sources.buildServer = QString::fromUtf8(kBuildServer);
		sources.buildRsaKeys = QByteArray(kBuildRsaKeys);
		if (QCoreApplication::instance()) {
			sources.exeDir = QCoreApplication::applicationDirPath();
		} else {
#ifdef Q_OS_LINUX
			const auto exe = QFileInfo(QStringLiteral("/proc/self/exe"));
			sources.exeDir = QFileInfo(exe.canonicalFilePath()).absolutePath();
#endif // Q_OS_LINUX
		}
		auto log = QStringList();
		auto config = Load(sources, &log);
		for (const auto &line : log) {
			LOG((QStringLiteral("teled: %1").arg(line)));
		}
		return config;
	}();
	return result;
}

} // namespace MTP::teled
