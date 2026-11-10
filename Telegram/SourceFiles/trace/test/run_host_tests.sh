#!/usr/bin/env bash
# Builds and runs the host-side test of the Qt-free trace parts with ASan and
# UBSan: trace_ui_core (ids, stamps, coalescing, markers, records) and
# trace_writer. Needs a C++20 compiler and OpenSSL (libcrypto) headers.
#
# Usage: Telegram/SourceFiles/trace/test/run_host_tests.sh [compiler]
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
src=$(cd "$here/../.." && pwd)
cxx=${1:-${CXX:-c++}}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

"$cxx" -std=c++20 -g -O1 -Wall -Wextra \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer \
	-I "$src" \
	"$here/trace_ui_core_test.cpp" \
	"$src/trace/trace_ui_core.cpp" \
	"$src/trace/trace_writer.cpp" \
	-lcrypto -pthread \
	-o "$out/trace_ui_core_test"

mkdir -p "$out/data"
"$out/trace_ui_core_test" "$out/data"
if command -v python3 >/dev/null 2>&1; then
	# Every written line must be valid JSON.
	python3 -I -c '
import json, sys
for path in sys.argv[1:]:
    for line in open(path, encoding="utf-8"):
        json.loads(line)
print("trace_ui_core: JSON lines parse")
' "$out/data/run/events-0000.jsonl" "$out/data/lines.jsonl"
fi
