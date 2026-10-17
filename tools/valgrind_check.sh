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
# Which tool answers which question - and in what order
#
#   memcheck   the commit gate. Uninitialised reads, invalid frees, definite
#              leaks. Nothing about concurrency: memcheck does not schedule
#              threads differently from the bare run, so a clean memcheck says
#              nothing at all about races.
#   tsan       the FIRST sieve for a data race or a deadlock. It is a compiler
#              instrumentation, so it sees the real interleaving of the asio
#              strand and of the lock-free table instead of guessing from the
#              call order. It needs its own build (build-tsan/) because the
#              runtime has to be linked in, and it needs vm.mmap_rnd_bits=28 on
#              the host - if it dies with "unexpected memory mapping" the
#              sysctl did not take, and the fallback below is setarch -R.
#   drd        a cross-check only, and never the first pass. DRD models mutexes
#              and happens-before from the call sequence, so it reports a
#              structural false positive on anything asio dispatches through a
#              strand and on anything the engine publishes without a lock; the
#              engine has both. A row DRD flags has to be confirmed under tsan
#              before it is called a defect - which is exactly why tsan runs
#              first and drd, if at all, runs after it.
#   callgrind  instruction counts, so that "faster" is a machine-independent
#              number.
#   massif     heap peak.
#
# Valgrind and a load run are mutually exclusive. Under memcheck the server is
# 20-50x slower, so a client that reconnects on failure turns connect timeouts
# into a feedback loop and the machine dies; sampling tools of any kind are
# forbidden in the same window as any of these runs.
#
# Usage:
#   bash tools/valgrind_check.sh memcheck test_board test_search
#   bash tools/valgrind_check.sh memcheck --all
#   bash tools/valgrind_check.sh tsan test_training test_server
#   bash tools/valgrind_check.sh drd test_training
#   bash tools/valgrind_check.sh callgrind test_search
#   bash tools/valgrind_check.sh massif gomoku_train --rounds 1 --method hill \
#       --hill-iterations 1 --depth 1 --timeout-ms 1 --fitness-games 1 \
#       --verification-games 1 --no-data
#   Everything after the first target starting with -- is passed to the binary as
#   arguments; --max-nodes is not one of them (see gomoku_train --help), and an
#   unknown option exits 1 in under a second, which reads like a crash.
#
# `--all` is the eight ctest suites plus the two programs, and only means that
# for memcheck; a race sweep wants the threaded targets (test_training,
# test_server, gomoku_train), not the eight.
#
# The gate is "no new reports against the recorded baseline", not "no reports":
# REVIEW_LOG.md (轮次121 B2/B3 sections) records what the tree already produced,
# per class, with the build directory and the -UNDEBUG state named. A report is
# only dismissed as a false positive against a stated reason - "strand dispatch"
# or "lock-free publish" has to name the code path that makes it one.
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

tool="${1:-}"
if [ -z "$tool" ]; then
    echo "usage: valgrind_check.sh <memcheck|tsan|drd|callgrind|massif> <target...>" >&2
    exit 2
fi
shift

# tsan is not a valgrind tool; it is the compiler runtime, and it replaces the
# wrapper rather than adding to it. Everything else needs valgrind itself.
bindir="$root/build-vg"
configure_type=Debug
configure_flags=("-UNDEBUG")

case "$tool" in
    # Uninitialised reads, invalid frees, definite leaks: the correctness check.
    memcheck)
        vg=(valgrind --tool=memcheck -q --error-exitcode=9 --leak-check=full
                --show-leak-kinds=definite --errors-for-leak-kinds=definite
                --track-origins=yes --num-callers=16) ;;
    # Data races and deadlocks. -O1, not -O0: the search's race windows are wide
    # enough to hit at any optimisation, and -O0 makes the suites take an hour.
    # RelWithDebInfo is what keeps the -g that makes the report readable.
    tsan)
        vg=()
        bindir="${TSAN_BUILD:-$root/build-tsan}"
        configure_type=RelWithDebInfo
        configure_flags=("-UNDEBUG" "-fsanitize=thread")
        export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=0 second_deadlock_stack=1}" ;;
    # Cross-check only - see the ordering note above. DRD reports false positives
    # on std::mutex, on asio strands and on the engine's lock-free publishes; a
    # row it flags is not a defect until tsan agrees.
    drd)
        vg=(valgrind --tool=drd -q --error-exitcode=9 --read-var-info=yes
                --num-callers=16) ;;
    # Instruction counts, so that "faster" is a machine-independent number.
    callgrind)
        vg=(valgrind --tool=callgrind -q --collect-atstart=no
                 --callgrind-out-file=/dev/null) ;;
    # Heap peak, so that memory cost can be shown to stop tracking concurrency.
    massif)
        vg=(valgrind --tool=massif -q --massif-out-file=/dev/stderr
                  --pages-as-heap=no) ;;
    *) echo "unknown tool '$tool'" >&2; exit 2 ;;
esac

if [ "$tool" != tsan ] && ! command -v valgrind >/dev/null 2>&1; then
    echo "valgrind is not installed; the gate cannot run" >&2
    exit 2
fi

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

if [ ! -f "$bindir/CMakeCache.txt" ]; then
    echo "-- configuring $bindir ($configure_type, asserts enabled)"
    cmake -S "$root" -B "$bindir" -DCMAKE_BUILD_TYPE="$configure_type" \
          -DCMAKE_CXX_FLAGS="${configure_flags[*]}" >&2
    if [ "$tool" = tsan ]; then
        # The compiler flag alone is not enough: the sanitizer has to be on the
        # link line, and CMAKE_EXE_LINKER_FLAGS is the one place that guarantees it.
        cmake -S "$root" -B "$bindir" -DCMAKE_BUILD_TYPE="$configure_type" \
              -DCMAKE_CXX_FLAGS="${configure_flags[*]}" \
              -DCMAKE_C_FLAGS="${configure_flags[*]}" \
              -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
              -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread" \
              -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g -fno-omit-frame-pointer" >&2
    fi
fi
echo "-- building"
cmake --build "$bindir" -j "$(nproc)" >&2 || { echo "build failed" >&2; exit 1; }

overall=0
for t in "${targets[@]}"; do
    bin="$bindir/$t"
    if [ ! -x "$bin" ]; then
        echo "SKIP $t (no such target)"
        continue
    fi
    log=$(mktemp)
    # The suite's own stdout is kept separate: a test that aborts mid-run still
    # leaves every earlier case memchecked, and the last line names where it died.
    start=$(date +%s)
    if [ "$tool" = tsan ]; then
        # No valgrind wrapper, and ASLR entropy: if the kernel's mmap randomisation
        # is wider than the sanitizer's shadow can reserve around, TSan refuses to
        # start. The sysctl cannot be read from inside WSL, so the retry is driven
        # by the run's own output.
        timeout 3600 "$bin" ${extra+"${extra[@]}"} >"$log.out" 2>"$log"
        rc=$?
        if grep -q "unexpected memory mapping" "$log"; then
            timeout 3600 setarch "$(uname -m)" -R "$bin" ${extra+"${extra[@]}"} >"$log.out" 2>"$log"
            rc=$?
            echo "-- retried under setarch -R (vm.mmap_rnd_bits did not take)"
        fi
    else
        timeout 3600 "${vg[@]}" "$bin" ${extra+"${extra[@]}"} >"$log.out" 2>"$log"
        rc=$?
    fi
    secs=$(( $(date +%s) - start ))
    # Reported per class, because "reports=N" cannot be compared to a baseline:
    # a new invalid read and a new definite leak are different defects and the
    # gate has to say which one moved. -q suppresses the ERROR/LEAK SUMMARY
    # lines, so these count the error entries themselves. The classes are asked
    # of the tool that produces them: a DRD row that printed memcheck's four
    # counters would read like a memcheck row that found nothing.
    reports=$(grep -c "^==.*==" "$log" 2>/dev/null); reports=${reports:-0}
    invalid_read=0; invalid_write=0; uninit=0; leak_def=0
    conflicts=0; races=0; deadlocks=0
    if [ "$tool" = memcheck ]; then
        invalid_read=$(grep -c "Invalid read of size" "$log" 2>/dev/null); invalid_read=${invalid_read:-0}
        invalid_write=$(grep -c "Invalid write of size" "$log" 2>/dev/null); invalid_write=${invalid_write:-0}
        uninit=$(grep -c "uninitialised" "$log" 2>/dev/null); uninit=${uninit:-0}
        leak_def=$(grep -c "definitely lost" "$log" 2>/dev/null); leak_def=${leak_def:-0}
    elif [ "$tool" = drd ]; then
        conflicts=$(grep -c "Conflicting accesses" "$log" 2>/dev/null); conflicts=${conflicts:-0}
    elif [ "$tool" = tsan ]; then
        races=$(grep -c "ThreadSanitizer: data race" "$log" 2>/dev/null); races=${races:-0}
        deadlocks=$(grep -c "ThreadSanitizer: deadlock" "$log" 2>/dev/null); deadlocks=${deadlocks:-0}
    fi

    if [ "$tool" = tsan ]; then
        if [ "$races" != 0 ] || [ "$deadlocks" != 0 ]; then
            verdict="TSAN REPORTS"
        elif [ "$rc" != 0 ]; then
            verdict="ABORTED rc=$rc"
        else
            verdict="clean"
        fi
        printf '%-14s %s  races=%s deadlocks=%s rc=%s %ss\n' \
               "$t" "$verdict" "$races" "$deadlocks" "$rc" "$secs"

        # The pairs, not just the count: a race is only adjudicable by the two
        # accesses and the two functions, and strand/atomic involvement is what
        # decides whether it is real.
        if [ "$races" != 0 ]; then
            grep -A24 "^==.*== WARNING: ThreadSanitizer: data race" "$log" \
                | grep -E "^ *([0-9]+ +(go| pthread)|(Read|Write|Atomic|Prev|Location) )" \
                | head -40
        fi
    else
        if [ "$rc" = 9 ]; then
            verdict="VALGRIND ERRORS"
        elif [ "$rc" != 0 ]; then
            verdict="ABORTED rc=$rc"
        else
            verdict="clean"
        fi
        if [ "$tool" = memcheck ]; then
            printf '%-14s %s  reports=%s  invalid_read=%s invalid_write=%s uninitialised=%s definite_leak=%s  %ss\n' \
                   "$t" "$verdict" "$reports" "$invalid_read" "$invalid_write" "$uninit" "$leak_def" "$secs"
        elif [ "$tool" = drd ]; then
            printf '%-14s %s  reports=%s  conflicting_accesses=%s  %ss  (cross-check: tsan is the first sieve)\n' \
                   "$t" "$verdict" "$reports" "$conflicts" "$secs"
        else
            printf '%-14s %s  reports=%s  %ss\n' "$t" "$verdict" "$reports" "$secs"
        fi
    fi
    if [ "$reports" != 0 ] && [ "$tool" != tsan ]; then
        grep -m3 -E "Invalid read|Invalid write|uninitialised|definitely lost|Conflicting accesses|Segmentation" "$log"
    fi
    [ "$rc" != 0 ] && tail -2 "$log.out"
    { [ "$rc" != 0 ] || [ "$verdict" = "TSAN REPORTS" ] || [ "$verdict" = "VALGRIND ERRORS" ]; } && overall=1
    rm -f "$log" "$log.out"
done

exit "$overall"
