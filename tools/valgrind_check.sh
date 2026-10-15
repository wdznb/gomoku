#!/usr/bin/env bash
# Valgrind gate for the engine redesign.
#
# Memory hygiene is a per-commit gate here, not a one-off audit: the redesign
# repacks the transposition table entries, walks the table back to extract a
# principal variation, caches candidate scores across place/undo and hands the
# search a Board by reference. Each of those is a memory bug waiting to be
# invisible in a passing test run.
#
# It builds into build-vg/, never into build/: the gate build is the Release-ish
# tree ctest runs against, and -O3 optimises uninitialised reads away. build-vg/
# is Debug so memcheck reports real line numbers and catches reads of padding
# and of table slots that were never written. -UNDEBUG keeps assert() alive, the
# same reason the ctest targets carry it.
#
# Usage:
#   bash tools/valgrind_check.sh memcheck test_board test_search
#   bash tools/valgrind_check.sh memcheck --all
#   bash tools/valgrind_check.sh drd test_training --only test_shared_tt_concurrent_probe_store
#   bash tools/valgrind_check.sh callgrind test_search
#   bash tools/valgrind_check.sh massif gomoku_train --rounds 1 --max-nodes 5000
#
# `--all` is the eight ctest suites plus the two programs. The gate is "no new
# reports against the recorded baseline", not "no reports": see REVIEW_LOG.md for
# what the pre-redesign tree already produced.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build-vg"

tool="${1:-}"
if [ -z "$tool" ]; then
    echo "usage: valgrind_check.sh <memcheck|drd|callgrind|massif> <target...>" >&2
    exit 2
fi
shift

if ! command -v valgrind >/dev/null 2>&1; then
    echo "valgrind is not installed; the gate cannot run" >&2
    exit 2
fi

case "$tool" in
    # Uninitialised reads, invalid frees, definite leaks: the correctness check.
    memcheck) vg=(valgrind --tool=memcheck -q --error-exitcode=9 --leak-check=full
                    --show-leak-kinds=definite --errors-for-leak-kinds=definite
                    --track-origins=yes --num-callers=16) ;;
    # Data-race check for the shared training table. DRD reports false positives
    # on std::mutex; anything it flags has to be confirmed under -fsanitize=thread
    # before it is dismissed.
    drd) vg=(valgrind --tool=drd -q --error-exitcode=9 --read-var-info=yes
                --num-callers=16) ;;
    # Instruction counts, so that "faster" is a machine-independent number.
    callgrind) vg=(valgrind --tool=callgrind -q --collect-atstart=no
                     --callgrind-out-file=/dev/null) ;;
    # Heap peak, so that memory cost can be shown to stop tracking concurrency.
    massif) vg=(valgrind --tool=massif -q --massif-out-file=/dev/stderr
                  --pages-as-heap=no) ;;
    *) echo "unknown tool '$tool'" >&2; exit 2 ;;
esac

targets=()
extra=()
seen_extra=0
for arg in "$@"; do
    if [ "$seen_extra" = 1 ]; then extra+=("$arg"); continue; fi
    if [ "$arg" = "--all" ]; then
        targets+=(test_board test_pattern test_evaluator test_search test_position
                  test_session test_server test_training gomoku_train)
    elif [ "${arg:0:2}" = "--" ]; then
        seen_extra=1; extra+=("$arg")
    else
        targets+=("$arg")
    fi
done

if [ "${#targets[@]}" = 0 ]; then
    echo "no targets given ('--all' runs the full set)" >&2
    exit 2
fi

if [ ! -f "$build/CMakeCache.txt" ]; then
    echo "-- configuring $build (Debug, asserts enabled)"
    cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-UNDEBUG" >&2
fi
echo "-- building"
cmake --build "$build" -j "$(nproc)" >&2 || { echo "build failed" >&2; exit 1; }

overall=0
for t in "${targets[@]}"; do
    bin="$build/$t"
    if [ ! -x "$bin" ]; then
        echo "SKIP $t (no such target)"
        continue
    fi
    log=$(mktemp)
    # The suite's own stdout is kept separate: a test that aborts mid-run still
    # leaves every earlier case memchecked, and the last line names where it died.
    start=$(date +%s)
    timeout 3600 "${vg[@]}" "$bin" ${extra+"${extra[@]}"} >"$log.out" 2>"$log"
    rc=$?
    secs=$(( $(date +%s) - start ))
    reports=$(grep -c "^==.*==" "$log" 2>/dev/null); reports=${reports:-0}
    if [ "$rc" = 9 ]; then
        verdict="VALGRIND ERRORS"
    elif [ "$rc" != 0 ]; then
        verdict="ABORTED rc=$rc"
    else
        verdict="clean"
    fi
    printf '%-14s %s  reports=%s  %ss\n' "$t" "$verdict" "$reports" "$secs"
    if [ "$reports" != 0 ]; then
        grep -m3 -E "Invalid read|Invalid write|uninitialised|definitely lost|Segmentation" "$log"
    fi
    [ "$rc" != 0 ] && tail -2 "$log.out"
    [ "$rc" != 0 ] && overall=1
    rm -f "$log" "$log.out"
done

exit "$overall"
