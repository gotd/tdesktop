# Control server

The gotd/tdesktop fork has a local control server, so a script or an AI agent can drive the client
and capture frames: start it against a local [teled](teled.md), log in, open a chat, scroll, record
30 fps of the window, read the widget tree, then line the frames up with the wire trace.

- **Opt-in.** The server starts only when `TDESKTOP_CONTROL=<unix socket path>` is set or
  `-control <path>` is passed. Otherwise it does nothing: no socket, no event filter, no timers.
- **Local.** It listens on a unix socket (mode `0600`, in a `0700` directory it creates) and never
  opens a network connection. Screenshots and frames show account content: they go under
  `<working dir>/control/` (or the path you give), as `0600` files in `0700` directories.
- **Main thread.** Requests run on the Qt main thread, in order, one at a time per connection.
  Only the JPEG encoding of recorded frames runs on worker threads.

The code is fork-owned: `Telegram/SourceFiles/control/` and `Telegram/cmake/control.cmake`. The only
upstream change is the `include(control.cmake)` line at the end of
`Telegram/cmake/telegram_options.cmake`; there is no C++ hook, the server starts from a
`Q_COREAPP_STARTUP_FUNCTION`. CMake options:

| Option | Default | Meaning |
| --- | --- | --- |
| `TDESKTOP_CONTROL` | `ON` | Compile the server in. |
| `TDESKTOP_CONTROL_OFFSCREEN` | `ON` | Link Qt's `offscreen` platform plugin (Linux, static Qt), for headless runs. |

The client is `tdctl` (`cmd/tdctl` in the private `gotd/wiretrace` repository):

```console
go install github.com/gotd/wiretrace/cmd/tdctl@latest
```

## Running headless

The static Qt of the Linux build has the `offscreen` platform plugin, and `control.cmake` links it in.
`QT_QPA_PLATFORM=offscreen` then runs the client without any display: windows are "shown" into an
offscreen screen (the main window is 818×642 logical, DPR 1), get paint events and real input, and
can be captured. Xvfb is not needed. Keep the client away from your desktop session too: unset
`DISPLAY` and `WAYLAND_DISPLAY`, and point `DBUS_SESSION_BUS_ADDRESS` at a socket that doesn't
exist, so notifications, the tray icon, portals and the secret service never reach your desktop.
(With `dbus-run-session` instead, startup stalls for 25 s on a D-Bus call to a service the private
bus doesn't have; with no bus at all the window is up in about 0.5 s.)

```console
# teled on a local port (teled >= fix/tdesktop-compat: layer 233, in-memory DB by default;
# add --postgres-uri for persistence)
teled --key _testdata/test.key.pem --host 127.0.0.1 --port 18443 \
  --object-store-dir /tmp/teled-objects

# client: fresh working dir, local teled only, control socket
mkdir -m 700 -p /tmp/tdctl
env -u DISPLAY -u WAYLAND_DISPLAY \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
  QT_QPA_PLATFORM=offscreen \
  TDESKTOP_TELED_SERVER=127.0.0.1:18443 \
  TDESKTOP_TELED_RSA_KEY=/path/to/teled/_testdata/test.key.pem \
  TDESKTOP_CONTROL=/tmp/tdctl/control.sock \
  ./Telegram -workdir /tmp/tdctl/work

export TDESKTOP_CONTROL=/tmp/tdctl/control.sock
tdctl wait window --timeout 60s
tdctl state
```

`log.txt` in the working dir has one line when the server is up:

```
Control: listening on '/tmp/tdctl/control.sock' (platform: offscreen).
```

### OpenGL and capture sources

`screenshot` and `record_start` take `source`:

- `grab` renders the window with `QWidget::grab()`. It is complete: it includes `QOpenGLWidget`
  content (Qt reads the GL framebuffer). It costs a full repaint of the window.
- `backingstore` copies the window's raster backing store (`QPlatformBackingStore::toImage()`). It
  is a memcpy of what was last painted, so it is cheap enough for 60 fps, but `QOpenGLWidget`
  content (media viewer, calls, some animations when OpenGL is on) is not in it.
- `auto` (the default for recordings) uses `backingstore` unless the window has a visible
  `QOpenGLWidget`/`QQuickWidget`/`QRhiWidget`, then `grab`. `screenshot` defaults to `grab`.

With `QT_QPA_PLATFORM=offscreen` and no `DISPLAY` there is no OpenGL at all: Ui::GL falls back to
raster (`Renderer: [Raster]` in `log.txt`) and both sources see everything. With `DISPLAY` set, the
offscreen plugin can create GLX contexts through that X server without showing any window; the
client then picks the QRhi renderer (`Renderer: [QRhi]`), and `grab` and `backingstore` still
capture the window correctly (checked on the intro screens; GL-only widgets such as the media
viewer were not exercised). If you run against a real display (xcb or wayland) and see
black or stale GL areas in `backingstore` frames, use `source:"grab"`, or turn hardware acceleration
off for that working dir (Settings > Advanced > Hardware acceleration, stored in `tdata`), which
makes every widget raster.

Popups (menus, tooltips, the emoji panel) are separate top-level windows. With `composite` (default
`true`), `screenshot` and recordings draw the visible popups that overlap the window on top of it.

## Protocol

One JSON object per line in each direction. A request has `"cmd"` and the arguments at the top level;
an optional `"id"` is echoed back. Every reply has `"ok":true` with result fields, or
`"ok":false,"error":"..."`.

```json
{"cmd":"click","x":120,"y":300,"id":7}
{"ok":true,"id":7,"window":0,"x":120,"y":300}
```

**Coordinates** are logical pixels of a window (`window` id, default: the primary window), the same
as `rect` in `tree`. Images are in device pixels (`dpr` in the reply). **Windows** have small integer
ids, stable for the process lifetime; the primary window is usually `0`. **Widget paths** are
`<window>/<child>/<child>...`, child indices among `QWidget` children (hidden ones included), stable
while the tree doesn't change.

**Targets.** Mouse commands (`click`, `move`, `press`, `release`, `scroll`) take one of:

- `x`, `y` (+ `window`);
- `path` (from `tree`), optionally `item` (an accessibility item index, from `tree items:true`) to
  hit a row of a painted list such as the chat list or the message list;
- `find`: an object with the `find` arguments below plus `nth`, for example
  `{"find":{"type":"Ui::RoundButton","text":"Start Messaging"}}`.

With `path`/`find` the point is the center of the visible part of the widget (or item), moved by
`dx`/`dy` if given.

| Command | Arguments | Result |
| --- | --- | --- |
| `ping` | | `pong`, `t` (trace clock), `pid` |
| `help` | | `commands` |
| `state` | | `platform`, `app_state`, `launched`, `locked`, `idle_ms`, `paint_ago_ms`, `accounts`, `account_index`, `authorized`, `user`, `user_name`, `window` (id, geometry, active, dpr, `gl_widgets`), `sections` (visible `Window::AbstractSectionWidget` types), `boxes` (visible `Ui::BoxContent` types), `intro_step`, `popup`, `layer`, `chat` (`peer`, `name`, `kind`), `recording`, `trace` |
| `windows` | | visible windows: `id`, `type`, `geometry`, `dpr`, `active`, `primary`, `title` |
| `tree` | `window` or `path`; `text`, `items`, `items_max` (200), `all` (hidden too), `flat`, `depth` | `windows`: nested nodes `{path, type, rect:[x,y,w,h], name, visible, enabled, focus, role, text, value, items:[{i,name,rect}], children}` (or flat `widgets`), `nodes` |
| `find` | `type` (substring, or exact with `exact`), `text` (case-insensitive substring of the accessibility name / label / button text), `name` (objectName), `role`, `within` (path), `window`, `visible` (default true), `limit` | `widgets: [{path, type, rect, text}]`, `count` |
| `click` | target, `button` (`left`/`right`/`middle`), `count` (2 = double click), `mods` | `window`, `x`, `y`, `path`, `type` |
| `move` / `press` / `release` | target, `button`, `mods` | same |
| `scroll` | target, `dy`/`dx` (pixels, sent as both pixel and angle delta; positive `dy` scrolls up, Qt convention) or `notches` (wheel clicks of 120), `steps`, `interval_ms` (16), `mods` | same + `steps`; replies after the last step |
| `key` | `key`: a `QKeySequence` in portable text (`Ctrl+F`, `Escape`, `Return`, `Up`, `Alt+Shift+Down`, `Ctrl+K, Ctrl+X`), `window` | `window` |
| `type` | `text`; `mode`: `im` (default: one input-method commit, any Unicode) or `keys` (key events per character) | `window`, `length` |
| `activate` | `window` | makes it the focus window |
| `resize` | `w`, `h`, `x`, `y`, `window` | `geometry` |
| `open_peer` | `peer`: `self`, `user:ID`, `chat:ID`, `channel:ID` or a Bot API id (`-100…` channel, other negative: chat); `msg` | `peer`, `name`, `kind`. The peer must be loaded (in the chat list, or seen) |
| `back` | | `SessionController::showBackFromStack()` |
| `shortcut` | `name`: `search`, `next_chat`, `previous_chat`, `first_chat`, `self_chat`, `show_archive`, `show_contacts`, `folder1`, `all_chats`, `read_chat`, `lock_telegram`, … (unknown names list the known ones) | `handled` |
| `chats` | `limit` | `chats: [{peer, name, kind}]` in chat list order |
| `wait` | `ms`; or `for`: `idle` (no widget painted for `quiet_ms`, default 300), `authorized`, `window`, `widget` / `gone` (with `find` arguments); `timeout_ms` (10000) | `waited_ms`, or `ok:false` on timeout |
| `screenshot` | `window`, `path` (`.png`, or `.jpg` with `quality`), `source` (`grab`), `composite` | `path`, `width`, `height`, `dpr`, `source`, `bytes`, `capture_ms`, `t` |
| `record_start` | `window`, `fps` (30, 1–60), `quality` (80), `scale` (1, down to 0.1), `source` (`auto`), `composite`, `dir`, `queue` (16), `threads` (4), `max_s` (600) | `dir`, … |
| `record_status` | | `frames`, `dropped`, `elapsed_s` |
| `record_stop` | | `frames`, `encoded`, `dropped`, `failed`, `duration_s`, `fps`, `fps_target`, `capture_ms_avg`, `capture_ms_max`, `encode_ms_avg`, `bytes`, `width`, `height`, `source`, `t_start` |
| `marker` | `label` (≤ 200 chars) | `n`, `t`, `trace` (written to the wire trace), `frames` (written to `frames.jsonl`) |
| `quit` | | quits the client |

### Input

Input goes through `QWindowSystemInterface`, the entry point the platform plugin uses for real
input, followed by `flushWindowSystemEvents()`, so the command returns after the event was handled.
Enter/leave, hover, the implicit mouse grab, popups, double clicks and `QShortcut` matching behave as
with a real mouse and keyboard. `click` moves the mouse first: tdesktop chooses the click handler
under the cursor on mouse move, before the press. Key events go to the focus window; `key` and
`type` activate the target window first if it isn't focused.

Handlers that tdesktop defers (`crl::on_main`, animations, network) run after the reply, so follow an
action with `wait idle`, or `wait widget`/`wait gone` for the expected result. `wait idle` never
settles on screens that animate continuously (the QR login spinner); wait for a widget there.
Section switches slide in a cached picture first: the new section's widget exists before its
children are visible, so wait for the child you are about to click (`wait widget --type
Ui::CountryCodeInput`), not for the section.

### Recording

`record_start` starts a precise `QTimer` at `fps` on the main thread. Each tick captures the window
(see sources above) and queues the image for the encoders, which write
`<dir>/frames/NNNNNN.jpg`. When `queue` frames are already waiting, the frame is dropped and counted.
`<dir>/frames.jsonl` gets one line per event:

```json
{"start":true,"t":12.502114,"fps":30,"quality":80,"scale":1,"source":"auto","window":0}
{"i":0,"t":12.502301,"ms":0.2,"file":"frames/000000.jpg","geometry":[0,0,800,600],"dpr":1,"w":800,"h":600,"cap_ms":0.9}
{"drop":1,"t":13.1,"ms":598.4}
{"marker":"before scroll","n":2,"t":13.4,"ms":901.0}
{"stop":true,"t":15.51,"frames":90,"dropped":1}
```

`t` is the wire trace clock: the same seconds as `t` in `<working dir>/trace/<run>/events-NNNN.jsonl`
(`Trace::details::Writer::Now()`), so a frame can be matched with the requests around it. Without the
trace module it is seconds since process start. `marker` writes `ui.marker` (`src:"control"`, `n`,
`label`) into the trace when tracing is on, and a `marker` line into `frames.jsonl` while recording.

Measured headless (Release build, 818×642 window, JPEG quality 80, about 35 KB per frame):
capturing from the backing store takes 0.25–0.5 ms per frame and `grab` about 2 ms; encoding takes
about 50 ms per frame per thread (the build's libjpeg is mozjpeg). 30 fps runs at 30.4 fps with no
drops; 60 fps reaches 59 fps with 4 encoder threads (the default) and drops about 20% with 2.
`scale 0.5` cuts encoding to 15 ms per frame.

To turn frames into a video: `ffmpeg -framerate 30 -i frames/%06d.jpg -pix_fmt yuv420p out.mp4`.

## tdctl

```console
tdctl [-s SOCKET] [-timeout 60s] COMMAND [flags]    # socket defaults to $TDESKTOP_CONTROL
```

Every command prints the JSON reply. The exit status is 1 when a reply is `ok:false`, 2 on usage
errors.

| Command | Request |
| --- | --- |
| `tdctl ping`, `state`, `windows`, `help`, `back`, `quit` | the same `cmd` |
| `tdctl tree [--text] [--items] [--flat] [--all] [--depth N] [--path P\|--window N]` | `tree` |
| `tdctl find --type T --text S [--role R] [--within P] [--limit N]` | `find` |
| `tdctl click --x X --y Y` / `--path P [--item I]` / `--type T --text S [--nth N]` `[--button right] [--count 2] [--mods ctrl]` | `click` (`move`, `press`, `release` take the same flags) |
| `tdctl scroll <target> --dy 120 [--steps 10 --interval 16ms]`, `--notches -3` | `scroll` |
| `tdctl key Ctrl+F Escape` | one `key` per argument |
| `tdctl type [--keys] TEXT` | `type` |
| `tdctl open [--msg ID] PEER`, `tdctl shortcut NAME`, `tdctl chats` | `open_peer`, `shortcut`, `chats` |
| `tdctl resize --w W --h H`, `tdctl activate` | `resize`, `activate` |
| `tdctl wait 500ms`, `wait idle [--quiet 300ms]`, `wait authorized`, `wait window`, `wait widget --type T`, `wait gone --type T` (`--timeout 10s`) | `wait` |
| `tdctl shot [-o FILE.png] [--source grab\|backingstore\|auto]` | `screenshot` (relative paths are made absolute) |
| `tdctl rec --fps 30 --for 3s [--dir D] [--quality 80] [--scale 0.5]`, `rec start`, `rec stop`, `rec status` | `record_start` + `wait` + `record_stop` (prints the stop stats) |
| `tdctl marker LABEL` | `marker` |
| `tdctl call '{"cmd":"tree","depth":2}'` | raw request |
| `tdctl script [-k] < FILE` | one command or JSON request per line, `#` comments; stops at the first failure unless `-k` |

## Example session

Log in to teled with a new account, then record scrolling Saved Messages. Desktop starts with QR
login; the phone form is behind a link. A new account needs both first and last name.

```console
$ export TDESKTOP_CONTROL=/tmp/tdctl/control.sock
$ tdctl script <<'EOF'
wait widget --type Intro::details::StartWidget --timeout 60s
click --type Ui::RoundButton --text "Start Messaging"
wait widget --role link --text "phone number" --timeout 20s
click --role link --text "phone number"
wait widget --type Ui::CountryCodeInput
click --type Ui::CountryCodeInput
key Ctrl+A Backspace
type --keys 1
type --keys 5550001234
marker "submit phone"
click --type Ui::RoundButton --text Next
wait widget --type Ui::CodeInput --timeout 20s
type --keys 12345
wait widget --type Intro::details::SignupWidget
type Agent
click --type Ui::InputField --text "Last name"
type Test
click --type Ui::RoundButton --text "Sign Up"
wait authorized --timeout 30s
EOF
$ tdctl state | jq -c '{authorized, intro_step, chat, sections}'
$ tdctl open self && tdctl wait idle
$ tdctl rec start --fps 30
$ tdctl scroll --type HistoryView::ListWidget --dy 240 --steps 15 --interval 50ms
$ tdctl rec stop
$ tdctl shot -o after.png
```

`tdctl tree --text --flat` shows what is on screen, for example on the phone step
(path, type, rect, role, text):

```
0/1/1/0/3 Intro::details::PhoneWidget [9,32,800,600] dialog "Your Phone Number"
0/1/1/0/3/2 CountryInput [259,225,300,61] button "USA"
0/1/1/0/3/3 Ui::CountryCodeInput [259,292,64,61] focus
0/1/1/0/3/4 Ui::PhonePartInput [334,292,225,61]
0/1/1/0/4/0 Ui::RoundButton [259,395,300,42] button "Next"
```

This flow was run headless against teled. With teled `fix/tdesktop-compat` (layer 233) the client
also gets past sign-up: chat list, Saved Messages, sending and history paging, global search,
BotFather and a two-client DM were driven with tdctl. Older teled (layer 228) makes the client fail
`users.getFullUser` with `CLIENT_RESPONSE_PARSE_FAILED`.
