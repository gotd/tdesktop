#!/usr/bin/env bash
# Build the gotd/tdesktop teled fork for Linux in upstream's Rocky Linux 8
# docker environment (Telegram/build/docker/centos_env), the same way
# .github/workflows/linux.yml does, then package it into dist/.
#
# Usage: scripts/teled/build-linux.sh [extra cmake -D args...]
#
# Environment:
#   TELED_RSA_KEY_FILE  PEM with teled RSA public key(s) to embed
#                       (default: teled testing key, in-tree)
#   TELED_SERVER        default endpoints, "[dc@]host:port,..."
#                       (default: 127.0.0.1:10443,[::1]:10443)
#   BUILD_CONFIG        Release (default) or Debug
#   IMAGE               docker image (default: tdesktop:centos_env)
#   BUILD_IMAGE         auto (default: build if missing), always, never
#   TDESKTOP_API_ID / TDESKTOP_API_HASH
#                       api credentials; default: upstream test ones
#                       (-D TDESKTOP_API_TEST=ON), teled accepts any
#   CCACHE_DIR          host ccache dir to mount
#                       (default: ~/.cache/tdesktop-ccache; set empty to
#                       build without ccache)
#   WARNINGS_AS_ERRORS  ON to fail on compiler warnings, as upstream CI does
#                       (default: OFF; newer GCC warns on upstream code)
#   PACKAGE            1 (default) to create dist/<name>.tar.gz + .sha256
#   DIST_NAME           package name (default: teled-<version>-<short sha>)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

TELED_SERVER=${TELED_SERVER:-127.0.0.1:10443,[::1]:10443}
TELED_RSA_KEY_FILE=${TELED_RSA_KEY_FILE:-Telegram/SourceFiles/mtproto/teled/teled_test_public_key.pem}
BUILD_CONFIG=${BUILD_CONFIG:-Release}
CCACHE_DIR=${CCACHE_DIR-$HOME/.cache/tdesktop-ccache}
WARNINGS_AS_ERRORS=${WARNINGS_AS_ERRORS:-OFF}
IMAGE=${IMAGE:-tdesktop:centos_env}
BUILD_IMAGE=${BUILD_IMAGE:-auto}
PACKAGE=${PACKAGE:-1}
CENTOS_ENV=Telegram/build/docker/centos_env

log() {
	echo "teled-build: $*" >&2
}

case "$BUILD_CONFIG" in
	Release|Debug) ;;
	*) log "BUILD_CONFIG must be Release or Debug, got '$BUILD_CONFIG'"; exit 2 ;;
esac

if [ ! -f Telegram/lib_base/CMakeLists.txt ]; then
	log "submodules are missing, run: git submodule update --init --recursive"
	exit 1
fi

# The key is passed to cmake inside the container, so copy it under the
# mounted source tree.
if [ ! -f "$TELED_RSA_KEY_FILE" ]; then
	log "TELED_RSA_KEY_FILE '$TELED_RSA_KEY_FILE' not found"
	exit 1
fi
if ! fingerprints=$(python3 -I scripts/teled/rsa_fingerprint.py "$TELED_RSA_KEY_FILE"); then
	log "TELED_RSA_KEY_FILE must hold RSA public key(s) only, for a teled key:"
	log "  openssl rsa -in key.pem -RSAPublicKey_out > key.pub.pem"
	exit 1
fi
mkdir -p .teled
if [ "$(realpath "$TELED_RSA_KEY_FILE")" != "$ROOT/.teled/rsa_public_key.pem" ]; then
	cp "$TELED_RSA_KEY_FILE" .teled/rsa_public_key.pem
fi

gen_dockerfile() {
	(
		cd "$CENTOS_ENV"
		if command -v poetry >/dev/null 2>&1; then
			poetry install >&2
			DEBUG='' LTO='' poetry run gen_dockerfile
		else
			# gen_dockerfile.py only needs jinja2.
			DEBUG='' LTO='' python3 gen_dockerfile.py
		fi
	)
}

if [ "$BUILD_IMAGE" = always ] \
	|| { [ "$BUILD_IMAGE" = auto ] && ! docker image inspect "$IMAGE" >/dev/null 2>&1; }; then
	log "building docker image $IMAGE (takes hours on the first run)"
	dockerfile=$(gen_dockerfile)
	rm -rf "$CENTOS_ENV/__pycache__"
	DOCKER_BUILDKIT=1 docker build -t "$IMAGE" - <<<"$dockerfile"
fi

api_args=(-D TDESKTOP_API_TEST=ON)
if [ -n "${TDESKTOP_API_ID:-}" ] || [ -n "${TDESKTOP_API_HASH:-}" ]; then
	api_args=(-D "TDESKTOP_API_ID=${TDESKTOP_API_ID:?}" -D "TDESKTOP_API_HASH=${TDESKTOP_API_HASH:?}")
fi

docker_args=(
	--rm
	-u "$(id -u)"
	-v "$ROOT:/usr/src/tdesktop"
	-e "CONFIG=$BUILD_CONFIG"
)
run_prefix=()
if [ -n "${CCACHE_DIR:-}" ]; then
	mkdir -p "$CCACHE_DIR"
	docker_args+=(
		-v "$CCACHE_DIR:/var/cache/ccache"
		-e "CCACHE_SLOPPINESS=pch_defines,time_macros"
	)
	run_prefix=(env -u CCACHE_DISABLE)
fi
if [ -t 0 ]; then
	docker_args+=(-it)
fi

log "config $BUILD_CONFIG, servers '$TELED_SERVER', key $TELED_RSA_KEY_FILE ($fingerprints)"
docker run "${docker_args[@]}" "$IMAGE" \
	"${run_prefix[@]}" /usr/src/tdesktop/$CENTOS_ENV/build.sh \
	-D "CMAKE_CONFIGURATION_TYPES=$BUILD_CONFIG" \
	-D "CMAKE_COMPILE_WARNING_AS_ERROR=$WARNINGS_AS_ERRORS" \
	-D DESKTOP_APP_USE_PACKAGED=OFF \
	-D DESKTOP_APP_DISABLE_AUTOUPDATE=ON \
	-D DESKTOP_APP_DISABLE_CRASH_REPORTS=ON \
	-D TDESKTOP_TELED_RSA_KEY_FILE=/usr/src/tdesktop/.teled/rsa_public_key.pem \
	-D "TDESKTOP_TELED_SERVER=$TELED_SERVER" \
	"${api_args[@]}" \
	"$@"

binary="out/$BUILD_CONFIG/Telegram"
if [ ! -f "$binary" ]; then
	log "build finished but $binary is missing"
	exit 1
fi
log "built $binary ($(stat -c %s "$binary") bytes)"

if [ "$PACKAGE" != 1 ]; then
	exit 0
fi

version=$(awk '$1 == "AppVersionStr" { print $2 }' Telegram/build/version)
sha=$(git rev-parse --short=10 HEAD)
name=${DIST_NAME:-teled-$version-$sha}
stage="dist/$name"
rm -rf "$stage" "dist/$name.tar.gz" "dist/$name.tar.gz.sha256"
mkdir -p "$stage"
cp "$binary" "$stage/Telegram"
cp .teled/rsa_public_key.pem "$stage/rsa_public_key.pem"
cat > "$stage/teled.json.example" <<EOF
{
  "servers": ["127.0.0.1:10443", "[::1]:10443"],
  "rsa_key_file": "rsa_public_key.pem",
  "pin": true
}
EOF
{
	echo "name: $name"
	echo "version: $version"
	echo "commit: $(git rev-parse HEAD)"
	if git rev-parse -q --verify upstream/dev >/dev/null; then
		echo "upstream: $(git merge-base HEAD upstream/dev)"
	fi
	echo "config: $BUILD_CONFIG"
	echo "servers: $TELED_SERVER"
	echo "rsa_fingerprints: $(echo "$fingerprints" | tr '\n' ' ')"
	echo "docs: https://github.com/gotd/tdesktop/blob/dev/docs/teled.md"
} > "$stage/BUILDINFO"
tar -C dist -czf "dist/$name.tar.gz" "$name"
(cd dist && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
log "packaged dist/$name.tar.gz"
cat "dist/$name.tar.gz.sha256"
