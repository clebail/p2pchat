#!/bin/sh
# Tests sous AddressSanitizer + UndefinedBehaviorSanitizer (fuites, accès mémoire, comportements indéfinis).
# Usage : tests/run-sanitizers.sh [dossier de compilation, par défaut build-asan]
set -e
src=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$src/build-asan}
mkdir -p "$build"
cd "$build"
qmake "$src/tests/tests.pro" CONFIG+=debug CONFIG+=sanitizer CONFIG+=sanitize_address CONFIG+=sanitize_undefined
make -j"$(nproc 2>/dev/null || echo 4)"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export LSAN_OPTIONS=suppressions=$src/tests/lsan.supp
make check
