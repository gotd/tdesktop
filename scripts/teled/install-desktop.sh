#!/usr/bin/env bash
# Install a build from dist/ as a desktop application for the current user,
# with a recolored icon and its own data directory, so it never touches the
# data of an official Telegram install.
#
# Usage: scripts/teled/install-desktop.sh [dist/<name>.tar.gz]
#        (default: the newest dist/tdesktop-*.tar.gz, the production build)
#
# Environment:
#   APP_NAME     menu name (default: "Telegram Trace")
#   APP_ID       desktop file id; must match the client's app id so the dock
#                shows this icon (default: org.telegram.desktop, the app id
#                of builds with the updater disabled, which ours are)
#   ICON_HUE     ImageMagick -modulate hue for the icon (default: 6, orange)
#   INSTALL_DIR  where binaries go (default: ~/.local/opt/tdesktop-trace)
#   WORKDIR      client data directory, passed as -workdir; traces go to
#                <WORKDIR>/trace (default: ~/.local/share/TelegramTrace)
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

APP_NAME=${APP_NAME:-Telegram Trace}
APP_ID=${APP_ID:-org.telegram.desktop}
ICON_HUE=${ICON_HUE:-6}
INSTALL_DIR=${INSTALL_DIR:-$HOME/.local/opt/tdesktop-trace}
WORKDIR=${WORKDIR:-$HOME/.local/share/TelegramTrace}
DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

log() {
	echo "install-desktop: $*" >&2
}

tarball=${1:-}
if [ -z "$tarball" ]; then
	tarball=$(ls -t dist/tdesktop-*.tar.gz 2>/dev/null | head -1 || true)
fi
if [ -z "$tarball" ] || [ ! -f "$tarball" ]; then
	log "no package; build one with PRODUCTION=1 scripts/teled/build-linux.sh"
	exit 1
fi
if [ -f "$tarball.sha256" ]; then
	(cd "$(dirname "$tarball")" && sha256sum -c --quiet "$(basename "$tarball").sha256")
fi
if ! command -v magick >/dev/null 2>&1; then
	log "ImageMagick (magick) is needed for the icon"
	exit 1
fi

name=$(basename "$tarball" .tar.gz)
mkdir -p "$INSTALL_DIR"
rm -rf "${INSTALL_DIR:?}/$name"
tar -C "$INSTALL_DIR" -xzf "$tarball"
ln -sfn "$name" "$INSTALL_DIR/current"
binary="$INSTALL_DIR/current/Telegram"
log "installed $name to $INSTALL_DIR"

mkdir -p "$WORKDIR"
chmod 700 "$WORKDIR"

icon_name=tdesktop-trace
icon_dir="$DATA_HOME/icons/hicolor/256x256/apps"
mkdir -p "$icon_dir"
magick Telegram/Resources/art/logo_256.png -modulate "100,110,$ICON_HUE" \
	"$icon_dir/$icon_name.png"

apps="$DATA_HOME/applications"
mkdir -p "$apps"
desktop="$apps/$APP_ID.desktop"
cat > "$desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$APP_NAME
Comment=Telegram Desktop build with wire tracing ($name)
Exec="$binary" -workdir "$WORKDIR" -- %u
Path=$WORKDIR
Icon=$icon_name
Terminal=false
StartupWMClass=TelegramDesktop
Categories=Chat;Network;InstantMessaging;Qt;
Keywords=tg;chat;im;messaging;messenger;sms;tdesktop;trace;
SingleMainWindow=true
X-GNOME-UsesNotifications=true
EOF
log "wrote $desktop"

update-desktop-database "$apps" >/dev/null 2>&1 || true
gtk-update-icon-cache -q -t "$DATA_HOME/icons/hicolor" >/dev/null 2>&1 || true
log "data and traces: $WORKDIR (traces in $WORKDIR/trace)"
