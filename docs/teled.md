# Telegram Desktop for teled

This is the [gotd/tdesktop](https://github.com/gotd/tdesktop) fork of Telegram Desktop. It connects to
[teled](https://github.com/gotd/teled), a testing MTProto server written in Go, instead of the
Telegram servers. It is a development tool: don't use it with production Telegram, and don't
distribute it under the Telegram name or branding.

The fork is upstream `dev` plus a few commits on top (`git log upstream/dev..dev`):

| Commit | What |
| --- | --- |
| `feat: adjust for teled` | Disables the special config loader, DNS-over-HTTPS and HTTP unixtime sync, and sets `kDefaultMainDc = 1`. |
| `feat(mtproto): configure teled endpoints and RSA keys at runtime` | Moves endpoints and RSA keys into `Telegram/SourceFiles/mtproto/teled/` and `Telegram/cmake/teled.cmake`. |
| `build(teled): …`, `ci(teled): …`, `docs(teled): …` | Build script, workflows, this document. |

To keep rebases cheap, fork code goes into new files under `Telegram/SourceFiles/mtproto/teled/`,
`Telegram/cmake/teled*.cmake` and `scripts/teled/`. Upstream files only get short hook lines:

- the `include(teled.cmake)` line at the end of `Telegram/cmake/td_mtproto.cmake`;
- the `teled::` calls in `mtproto_dc_options.cpp`.

## Endpoints and RSA keys

The client takes its built-in DC endpoints and the server RSA public key(s) from
`MTP::teled::Current()` (`mtproto/teled/teled_config.cpp`). The same values are used for the
production and the test environment. For each field (servers, keys), the first source that has a
valid value wins:

1. **Environment**
   - `TDESKTOP_TELED_SERVER`: list of endpoints separated by commas, semicolons or spaces. Each one is
     `host:port`, `[ipv6]:port`, `ipv6` or `host`, with an optional `<dc>@` prefix.
     - The port defaults to `10443` and the DC to `1`.
     - Hostnames are resolved once at startup.
     - Example: `TDESKTOP_TELED_SERVER='127.0.0.1:10443,[::1]:10443'`.
   - `TDESKTOP_TELED_RSA_KEY`: a path to a PEM file, or the PEM text itself.
     - The file may hold several `RSA PUBLIC KEY` (PKCS#1) or `PUBLIC KEY` blocks.
     - teled's private key file (`RSA PRIVATE KEY`) is accepted too. Only its public part is used.
   - `TDESKTOP_TELED_PIN`: `1` or `0`, see [pinning](#pinning).
2. **`teled.json`**: the file named by `$TDESKTOP_TELED_CONFIG`, else `./teled.json` in the working
   directory, else `teled.json` next to the executable:

   ```json
   {
     "servers": ["127.0.0.1:10443", "[::1]:10443"],
     "rsa_key_file": "rsa_public_key.pem",
     "pin": true
   }
   ```

   - `server` is accepted as an alias of `servers`, and `servers` may also be a single string.
   - `rsa_key_file` may be a list. Relative paths are resolved against the JSON file's directory.
   - `rsa_key` holds inline PEM text (a string or a list).
3. **Build-time defaults** (CMake, see [Building](#building)):
   - `-D TDESKTOP_TELED_SERVER=...`: default `127.0.0.1:10443,[::1]:10443`.
   - `-D TDESKTOP_TELED_RSA_KEY_FILE=/path/key.pub.pem`: default is teled's testing key,
     `Telegram/SourceFiles/mtproto/teled/teled_test_public_key.pem`.

   The PEM is read at configure time and embedded into a generated header,
   `out/Telegram/gen/teled/teled_build_defaults.h`. CMake reconfigures when the file changes. Private
   keys are refused at build time.

If a source is set but invalid (for example a missing file or a bad address), the client logs an
error and falls back to the next source.

### Pinning

tdesktop normally replaces its built-in endpoints with the DC list that the server returns
(`help.getConfig`, `updateDcOptions`), and later starts from the list it stored in `tdata`. When the
endpoints are **pinned**, both of those lists are ignored, so the client always connects to the
configured endpoints. That is what you want when the teled address changes between runs.

- Endpoints from the environment or `teled.json` are pinned by default.
- Build-time defaults are not pinned.
- `TDESKTOP_TELED_PIN=0|1` or `"pin": false|true` overrides the default.

Without pinning, teled's `--host`/`--port` must be an address the client can reach, because teled
advertises them in `help.getConfig`.

### Fingerprints

The key fingerprint is computed from the key: the lower 64 bits of SHA1 over the TL-serialized `n`
and `e`, the same as gotd's `crypto.RSAFingerprint`. No other code needs to change when the key
changes. The client logs what it loaded to `log.txt`:

```
teled: RSA key fingerprint 147d61eba7bd646a (1476443917606806634)
teled: DC 1 endpoint 127.0.0.1:10443
teled: DC 1 endpoint [::1]:10443
teled: servers from build defaults, keys from build defaults, endpoints not pinned
```

`scripts/teled/rsa_fingerprint.py key.pem` prints the same value offline. The value above belongs to
teled's testing key.

### Changing the key

- **Locally, no rebuild:**

  ```console
  TDESKTOP_TELED_RSA_KEY=/path/to/teled/key.pem ./Telegram
  ```

  The private key file that teled uses works here.
- **Baked into a build:** export the public part with
  `openssl rsa -in key.pem -RSAPublicKey_out > key.pub.pem`, or with `teled keys public --key key.pem`
  if teled has that command. Then build with `TELED_RSA_KEY_FILE=key.pub.pem scripts/teled/build-linux.sh`.
- **In CI:** set the `TELED_RSA_PUBLIC_KEY` secret or variable (see [Workflows](#workflows)) and run
  `teled.` manually, or wait for the weekly run.

The client keeps the auth key it created in `tdata`. After changing the server key or teled's
database, start with a fresh working directory (`./Telegram -workdir /tmp/tg-teled`) or delete
`tdata`.

## Running against teled

```console
# teled (https://github.com/gotd/teled)
go install github.com/gotd/teled/cmd/teled@latest
teled --key _testdata/test.key.pem --host 127.0.0.1 --port 10443

# client
TDESKTOP_TELED_SERVER=127.0.0.1:10443 ./Telegram -workdir /tmp/tg-teled -debug
```

- **Login:** any phone number works and the code is `12345`. Phones `+99966XYYYY` are test accounts
  whose code is `X` repeated five times.
- **api_id / api_hash:** teled does not check them, since it unwraps `initConnection` without
  validation. The builds use upstream's test credentials (`-D TDESKTOP_API_TEST=ON`, api_id
  `17349`). To use your own, pass `TDESKTOP_API_ID` and `TDESKTOP_API_HASH` to
  `scripts/teled/build-linux.sh`, or set the repository variables of the same names.
- **`-debug`:** writes the MTProto dumps to `DebugLogs/`.

## Building

Linux, with upstream's Rocky Linux 8 docker environment (`Telegram/build/docker/centos_env`), the
same way as upstream `.github/workflows/linux.yml`:

```console
git clone --recursive -b dev https://github.com/gotd/tdesktop.git
cd tdesktop
scripts/teled/build-linux.sh
```

- **Docker image:** the first run builds the `tdesktop:centos_env` image, which takes hours. The
  script renders the Dockerfile with `poetry run gen_dockerfile`, or with `python3` and `jinja2`.
  Set `BUILD_IMAGE=never IMAGE=ghcr.io/gotd/tdesktop/centos_env:<hash>` to reuse the image that CI
  pushed instead.
- **Output:**
  - `out/Release/Telegram`;
  - `dist/teled-<version>-<sha>.tar.gz`, which holds the binary, the embedded public key,
    `teled.json.example` and `BUILDINFO`;
  - a `.sha256` file next to the tarball.
- **Settings** (environment variables):

  | Variable | Default | Meaning |
  | --- | --- | --- |
  | `TELED_RSA_KEY_FILE` | teled testing key | Public key PEM to embed. |
  | `TELED_SERVER` | `127.0.0.1:10443,[::1]:10443` | Default endpoints. |
  | `BUILD_CONFIG` | `Release` | `Release` or `Debug`. |
  | `CCACHE_DIR` | `~/.cache/tdesktop-ccache` | Host ccache directory to mount; empty disables ccache. |
  | `WARNINGS_AS_ERRORS` | `OFF` | `ON` fails the build on compiler warnings, like upstream CI. |
  | `PRODUCTION` | `0` | `1` builds upstream tdesktop for production Telegram in a worktree (`.production`), without teled servers and keys; needs your own `TDESKTOP_API_ID`/`TDESKTOP_API_HASH`. |
  | `PRODUCTION_REF` | upstream base of this fork | Commit, tag or branch to build with `PRODUCTION=1`. |
  | `TELED_BUILD_ENV` | `~/.config/teled/build.env` | File with your defaults for these variables, one `NAME=value` per line; variables set in the environment win. |
  | `TDESKTOP_API_ID`, `TDESKTOP_API_HASH` | upstream test credentials | api credentials. |

  Extra arguments go to cmake, for example `-D DESKTOP_APP_USE_PACKAGED=OFF`.
- **Fast rebuilds:** keep the docker image, `out/` (teled) and `.production/out/` (production); the
  script never deletes them, so rebuilds are incremental. Both trees mount at the same container
  path and share `~/.cache/tdesktop-ccache` with `CCACHE_BASEDIR`, so ccache reuses objects across
  them. Keep api credentials in `~/.config/teled/build.env` so every build uses the same values.
- **Fixed flags:** the script always passes `-D DESKTOP_APP_DISABLE_AUTOUPDATE=ON` and
  `-D DESKTOP_APP_DISABLE_CRASH_REPORTS=ON`, so the fork never updates itself to official Telegram
  and never sends crash reports to Telegram.

Other platforms use upstream's instructions plus `-D TDESKTOP_TELED_RSA_KEY_FILE=...` and
`-D TDESKTOP_TELED_SERVER=...`. The fork logic is platform-independent, except that on Linux it
falls back to `/proc/self/exe` to find the executable directory.

## Workflows

| Workflow | Triggers | What it does |
| --- | --- | --- |
| `teled.` (`.github/workflows/teled.yml`) | push to `dev`, weekly (Mon 05:00 UTC), manual (`config`, `release`) | Builds Linux with `scripts/teled/build-linux.sh`, uploads the tarball as an artifact and, on `dev`, publishes the release `teled-<upstream version>-<short sha>` with the tarball and its sha256. |
| `teled sync.` (`.github/workflows/teled-sync.yml`) | daily (03:30 UTC), manual (`push`) | Rebases `upstream/dev..dev` onto upstream `dev` with `scripts/teled/sync.sh` and pushes `dev` with `--force-with-lease`, which triggers `teled.`. On a conflict it opens or updates an issue labeled `teled-sync` that lists the conflicting files, and fails. The issue is closed after the next clean sync. |

### Docker image in `teled.`

Upstream's `docker.yml` pushes `ghcr.io/<repo>/centos_env:latest` from the default branch. Upstream's
`linux.yml` doesn't use that image: it rebuilds the image with a buildx cache kept in `actions/cache`.
The `teled.` workflow does the following:

1. Tries to pull `ghcr.io/<repo>/centos_env:<hashFiles(centos_env/**)>`.
2. On a miss, builds the image like `linux.yml` does (buildx, local cache, cache-dance) and pushes it
   under that tag. The tag changes whenever upstream changes the environment, so a stale image is
   never used.
3. Keeps the ccache in `actions/cache`, as `linux.yml` does.

The push needs `packages: write`, which the workflow requests. If org policy blocks it, the step
fails without failing the build, and the next run rebuilds the image from the buildx cache. The first
run without a cache can take several hours, within the 6 h job limit.

### Secrets and variables

| Name | Kind | Used by | Meaning |
| --- | --- | --- | --- |
| `TELED_SYNC_TOKEN` | secret, **required** for sync | `teled sync.` | See [the token for sync](#the-token-for-sync). |
| `TELED_RSA_PUBLIC_KEY` | secret or variable | `teled.` | PEM of the teled public key to embed. The secret wins over the variable. Default: teled's testing key. |
| `TELED_RSA_PUBLIC_KEY_URL` | variable | `teled.` | URL to fetch the PEM from, used when `TELED_RSA_PUBLIC_KEY` is unset. |
| `TELED_SERVER` | variable | `teled.` | Default endpoints baked into the build. |
| `TDESKTOP_API_ID`, `TDESKTOP_API_HASH` | variables | `teled.` | Own api credentials. Default: upstream test credentials. |

`GITHUB_TOKEN` covers the release (`contents: write`), the ghcr image (`packages: write`) and the
sync issue (`issues: write`).

#### The token for sync

`TELED_SYNC_TOKEN` is used to push the rebased `dev`. Use one of:

- a fine-grained PAT for `gotd/tdesktop` with **Contents: read and write** and **Workflows: read and
  write**;
- a classic PAT with the `repo` and `workflow` scopes.

`GITHUB_TOKEN` doesn't work for two reasons:

1. Upstream commits regularly change `.github/workflows/*`. Pushing those changes needs the
   `workflow` permission, which `GITHUB_TOKEN` can't have.
2. Pushes made with `GITHUB_TOKEN` don't trigger other workflows, so `teled.` wouldn't run.

### Repository settings

- The default branch must be `dev`, because scheduled workflows run from the default branch.
- Enable **Issues**, which forks have disabled by default, so the sync can report conflicts.
- Allow GitHub Actions to create releases. Under Settings > Actions > General, set workflow
  permissions so the `permissions:` blocks are honored.

### Upstream workflows to disable in the fork

These workflows are noise in the fork, need secrets the fork doesn't have, or would act on teled
releases. Disable them once (they stay disabled across syncs):

```console
gh -R gotd/tdesktop workflow disable stale.yml                 # closes stale issues/PRs
gh -R gotd/tdesktop workflow disable lock.yml                  # locks threads
gh -R gotd/tdesktop workflow disable issue_closer.yml          # closes new issues
gh -R gotd/tdesktop workflow disable cant-reproduce.yml        # issue triage
gh -R gotd/tdesktop workflow disable needs-user-action.yml     # issue triage
gh -R gotd/tdesktop workflow disable waiting-for-answer.yml    # issue triage
gh -R gotd/tdesktop workflow disable pr-evidence-check.yml     # upstream PR policy
gh -R gotd/tdesktop workflow disable winget.yml                # runs on our releases, needs WINGET_TOKEN
gh -R gotd/tdesktop workflow disable master_updater.yml        # runs on our releases, rewrites master
gh -R gotd/tdesktop workflow disable snap.yml                  # snap build
gh -R gotd/tdesktop workflow disable changelog.yml             # GitHub Pages changelog on push to dev
gh -R gotd/tdesktop workflow disable copyright_year_updater.yml
gh -R gotd/tdesktop workflow disable unused_styles_updater.yml
gh -R gotd/tdesktop workflow disable user_agent_updater.yml
gh -R gotd/tdesktop workflow disable canary.yml                # upstream canary, needs signing secrets
gh -R gotd/tdesktop workflow disable canary-bot-api.yml
gh -R gotd/tdesktop workflow disable private-ci.yml
gh -R gotd/tdesktop workflow disable docker.yml                # teled. caches its own image
gh -R gotd/tdesktop workflow disable linux.yml                 # duplicated by teled.
gh -R gotd/tdesktop workflow disable win.yml                   # optional: no teled Windows/macOS builds yet
gh -R gotd/tdesktop workflow disable mac.yml                   # optional
gh -R gotd/tdesktop workflow disable mac_packaged.yml          # optional
```

`winget.yml` and `master_updater.yml` trigger on `release` events, so disable them **before** the
first `teled.` release.

## Rebasing by hand

```console
scripts/teled/sync.sh          # fetch upstream dev, rebase upstream/dev..origin/dev on a detached HEAD
# on conflict: redo the rebase, resolve, continue
git rebase --onto teled-upstream/dev "$(git merge-base origin/dev teled-upstream/dev)"
git push --force-with-lease=refs/heads/dev:"$(git rev-parse origin/dev)" origin HEAD:dev
```

Keep fork changes in the teled files and hook lines described above. New features, such as wire
tracing, go into new files under `mtproto/teled/` behind a `TDESKTOP_TELED_*` CMake option declared in
`Telegram/cmake/teled.cmake`.
