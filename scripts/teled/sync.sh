#!/usr/bin/env bash
# Rebase the fork's teled commits (upstream/dev..BRANCH) onto the latest
# upstream dev. Used by .github/workflows/teled-sync.yml, works locally too.
#
# Usage: scripts/teled/sync.sh [--push]
#
# Environment:
#   BRANCH         fork branch to rebase (default: dev)
#   REMOTE         fork remote to push to (default: origin)
#   UPSTREAM_URL   upstream repository
#                  (default: https://github.com/telegramdesktop/tdesktop.git)
#   UPSTREAM_REF   upstream branch (default: dev)
#
# Exit status: 0 for up to date, rebased or pushed; 1 for a conflict or
# error. When GITHUB_OUTPUT is set, writes: status (up-to-date, rebased,
# pushed, conflict, error), old, new, upstream, commits, conflict_commit and
# conflict_files (multiline).
set -euo pipefail

BRANCH=${BRANCH:-dev}
REMOTE=${REMOTE:-origin}
UPSTREAM_URL=${UPSTREAM_URL:-https://github.com/telegramdesktop/tdesktop.git}
UPSTREAM_REF=${UPSTREAM_REF:-dev}
PUSH=0
if [ "${1:-}" = --push ]; then
	PUSH=1
fi

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

log() {
	echo "teled-sync: $*" >&2
}

output() {
	if [ -n "${GITHUB_OUTPUT:-}" ]; then
		echo "$1=$2" >> "$GITHUB_OUTPUT"
	fi
}

output_multiline() {
	if [ -n "${GITHUB_OUTPUT:-}" ]; then
		local delimiter
		delimiter="EOF_$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
		{
			echo "$1<<$delimiter"
			echo "$2"
			echo "$delimiter"
		} >> "$GITHUB_OUTPUT"
	fi
}

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	log "working tree is dirty"
	exit 1
fi

git fetch --no-tags "$REMOTE" "+refs/heads/$BRANCH:refs/remotes/$REMOTE/$BRANCH"
git fetch --no-tags "$UPSTREAM_URL" "+refs/heads/$UPSTREAM_REF:refs/remotes/teled-upstream/$UPSTREAM_REF"
upstream=$(git rev-parse "refs/remotes/teled-upstream/$UPSTREAM_REF")
old=$(git rev-parse "refs/remotes/$REMOTE/$BRANCH")
output old "$old"
output upstream "$upstream"

if git merge-base --is-ancestor "$upstream" "$old"; then
	log "$REMOTE/$BRANCH already contains upstream $UPSTREAM_REF ($upstream)"
	output status up-to-date
	output new "$old"
	exit 0
fi

if ! base=$(git merge-base "$old" "$upstream"); then
	log "$REMOTE/$BRANCH and upstream $UPSTREAM_REF have no common history"
	output status error
	exit 1
fi
commits=$(git rev-list --count "$base..$old")
log "rebasing $commits commit(s) of $BRANCH from $base onto $upstream"
git log --oneline "$base..$old" >&2
output commits "$commits"

# Work on a detached HEAD so local branches are left alone.
git checkout -q --detach "$old"
if ! git rebase --onto "$upstream" "$base"; then
	files=$(git diff --name-only --diff-filter=U)
	stopped=$(git rev-parse -q --verify REBASE_HEAD || true)
	subject=""
	if [ -n "$stopped" ]; then
		subject=$(git log -1 --format='%h %s' "$stopped")
	fi
	git rebase --abort || true
	log "conflict while applying '$subject' in:"
	echo "$files" >&2
	output status conflict
	output conflict_commit "$subject"
	output_multiline conflict_files "$files"
	exit 1
fi

new=$(git rev-parse HEAD)
output new "$new"
log "rebased $BRANCH: $old -> $new"

if [ "$PUSH" != 1 ]; then
	log "not pushing, HEAD is detached at the result"
	output status rebased
	exit 0
fi

# Refuse to overwrite the branch if it moved since the fetch above.
git push --force-with-lease="refs/heads/$BRANCH:$old" "$REMOTE" "HEAD:refs/heads/$BRANCH"
output status pushed
