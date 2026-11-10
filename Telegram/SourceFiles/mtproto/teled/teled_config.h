/*
This file is part of the gotd/tdesktop fork for teled
(https://github.com/gotd/teled), a testing MTProto server.

Runtime configuration of the built-in DC endpoints and RSA public keys.
See docs/teled.md for the user-facing description.
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <string>
#include <vector>

namespace MTP::teled {

struct Endpoint {
	int dcId = 0;
	std::string ip;
	int port = 0;
	bool ipv6 = false;
};

struct Config {
	// Built-in DC endpoints, used for both production and test environments.
	std::vector<Endpoint> endpoints;

	// PEM blocks ("BEGIN RSA PUBLIC KEY" or "BEGIN PUBLIC KEY"), each
	// accepted by MTP::details::RSAPublicKey.
	std::vector<QByteArray> publicKeys;

	// When true, endpoints from the server config (help.getConfig,
	// updateDcOptions) and from local storage are ignored, so the client
	// always connects to the endpoints above.
	bool pinned = false;

	// Human-readable description of where the values came from (for logs).
	QString source;
};

// Defaults used when nothing is configured, and for unset fields.
inline constexpr auto kDefaultDcId = 1;
inline constexpr auto kDefaultPort = 10443;

// Resolution order, first match wins for each field:
//   1. Environment: TDESKTOP_TELED_SERVER, TDESKTOP_TELED_RSA_KEY,
//      TDESKTOP_TELED_PIN.
//   2. JSON file: $TDESKTOP_TELED_CONFIG, else ./teled.json, else
//      teled.json next to the executable.
//   3. Build-time defaults (CMake TDESKTOP_TELED_SERVER and
//      TDESKTOP_TELED_RSA_KEY_FILE).
// Loaded once, thread-safe.
[[nodiscard]] const Config &Current();

[[nodiscard]] inline const std::vector<Endpoint> &Endpoints() {
	return Current().endpoints;
}
[[nodiscard]] inline const std::vector<QByteArray> &PublicKeys() {
	return Current().publicKeys;
}
[[nodiscard]] inline bool EndpointsPinned() {
	return Current().pinned;
}

// Exposed for tests.
// Parses "host:port", "[v6]:port", "v6", "host" with an optional "<dc>@"
// prefix. Lists may be separated by commas, semicolons or whitespace.
// Hostnames are resolved (blocking). Invalid entries are reported in errors.
[[nodiscard]] std::vector<Endpoint> ParseServers(
	const QString &value,
	QStringList *log = nullptr);

// Extracts public key PEM blocks from PEM data. A PKCS#1 private key
// ("BEGIN RSA PRIVATE KEY", as used by `teled --key`) is converted to its
// public part.
[[nodiscard]] std::vector<QByteArray> ParsePublicKeys(
	const QByteArray &pem,
	QStringList *log = nullptr);

// Builds a config from explicit sources, used by Current() and tests.
struct Sources {
	QString envServer;
	QString envRsaKey;
	QString envPin;
	QString configPath; // Empty: look up teled.json.
	QString exeDir;
	QString buildServer;
	QByteArray buildRsaKeys;
};
[[nodiscard]] Config Load(const Sources &sources, QStringList *log);

} // namespace MTP::teled
