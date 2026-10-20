#!/bin/bash
# Stress runner for gomoku_server, implementing docs/PROMPT_STRESS_TEST.md.
#
#   ./run_stress.sh preflight   - self-check + tier plan, sends no load
#   ./run_stress.sh smoke       - 1 connection held 12s, paced pings, clean close
#   ./run_stress.sh run         - case A ladder: 50 -> 100 -> 200 -> 400
#   ./run_stress.sh caseb       - case B handshake: B_RATE/s for B_SECS (S3: <=100, <=60)
#   ./run_stress.sh stop        - tear the server down and confirm the port is free
#
# Safety clauses this script is responsible for, as opposed to ws_stress.cpp which
# owns S3-S5 and S7-S8: S1 (single task), S2 (declared, capped ladder), S6 (total
# wall clock), S9 (nothing is written to sysctl or netsh), S10 (process tree and port
# released on every exit path). docs/STRESS_TEST_RULES.md 一 also makes Valgrind and
# load mutually exclusive, so a running valgrind/tsan job aborts this before anything
# else happens.
set -uo pipefail

STRESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$(dirname "$STRESS_DIR")")"
BUILD_DIR="$REPO_DIR/build"

PORT="${PORT:-8080}"
TIERS="${TIERS:-50 100 200 400}"
HOLD="${HOLD:-60}"          # 每档保持 60 秒
RAMP_UP="${RAMP_UP:-10}"    # 档内建连斜坡, 受 S3 的 100 conn/s 上限约束
GAP="${GAP:-30}"            # 档间爬坡 30 秒
OBSERVE="${OBSERVE:-10}"    # 每档结束后观察 10 秒
MAX_TIER="${MAX_TIER:-500}" # S2 hard ceiling
BUDGET_S="${BUDGET_S:-900}" # S6: 单次任务总时长 <= 15 分钟
RECLAIM_WAIT="${RECLAIM_WAIT:-130}"
INTERVAL_MS="${INTERVAL_MS:-1000}"
OUT_DIR="${OUT_DIR:-/tmp/gomoku_stress/$(date +%Y%m%d-%H%M%S)}"
SERVER_BIN="${SERVER_BIN:-$BUILD_DIR/gomoku_server}"
CLIENT="${CLIENT:-$STRESS_DIR/ws_stress}"

SERVER_PID=""
SERVER_STARTED_HERE=0
START_TS=$(date +%s)

mkdir -p "$OUT_DIR"

log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*"; }

note() {
    log "$@"
    echo "$@" >>"$OUT_DIR/notes.log"
}

# Exact names only: a -f pattern would match the shell that launched this script,
# which contains the words in its own command line.
peer_processes() {
    pgrep -x gomoku_server 2>/dev/null || true
    pgrep -x ws_stress 2>/dev/null || true
}

busy_instruments() {
    for name in valgrind drd helgrind massif callgrind test_server test_training; do
        pgrep -x "$name" 2>/dev/null || true
    done
}

elapsed_s() { echo $(( $(date +%s) - START_TS )); }

teardown() {
    local rc=$?
    if [ "$SERVER_STARTED_HERE" = 1 ]; then
        note "teardown: killing the client and the server started here"
    else
        note "teardown: killing the client; a server this script did not start is left alone"
    fi
    pkill -TERM -x ws_stress 2>/dev/null || true
    sleep 1
    if [ "$SERVER_STARTED_HERE" = 1 ] && [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        for _ in 1 2 3 4 5; do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    note "port $PORT listeners after teardown: $(ss -tln "sport = :$PORT" 2>/dev/null | tail -n +2 | wc -l)"
    note "TIME_WAIT now: $(time_wait_count)"
    note "output kept in $OUT_DIR"
    return $rc
}
trap teardown EXIT
trap 'note "SIGINT received"; exit 130' INT
trap 'note "SIGTERM received"; exit 143' TERM

time_wait_count() {
    ss -tn state time-wait 2>/dev/null | tail -n +2 | wc -l
}

tcp_stat_line() { ss -s 2>/dev/null | grep -i "^TCP:" | head -1; }

ensure_server() {
    if curl -s -o /dev/null --max-time 3 "http://127.0.0.1:$PORT/"; then
        SERVER_PID=$(pgrep -x gomoku_server | head -1)
        SERVER_STARTED_HERE=0
        note "server already answering on $PORT (pid ${SERVER_PID:-unknown}); not touching it"
        return 0
    fi
    [ -x "$SERVER_BIN" ] || { note "missing $SERVER_BIN - build the project first"; return 1; }
    note "starting $SERVER_BIN --port $PORT from $BUILD_DIR"
    ( cd "$BUILD_DIR" && ./gomoku_server --port "$PORT" ) >"$OUT_DIR/server.log" 2>&1 &
    SERVER_PID=$!
    SERVER_STARTED_HERE=1
    for _ in $(seq 1 20); do
        curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/" && break
        sleep 1
    done
    if ! curl -s -o /dev/null --max-time 2 "http://127.0.0.1:$PORT/"; then
        note "server did not answer within 20 s; see $OUT_DIR/server.log"
        return 1
    fi
    note "server up: pid=$SERVER_PID http_code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 "http://127.0.0.1:$PORT/")"
}

preflight() {
    note "=== 开跑前自检 (PROMPT_STRESS_TEST.md 自检清单) ==="
    local ulimit_n; ulimit_n=$(ulimit -n)
    local need=$(( 2 * MAX_TIER ))
    note "1. ulimit -n = $ulimit_n  (要求 >= 2 x 最大并发 = $need)"
    if [ "$ulimit_n" != "unlimited" ] && [ "$ulimit_n" -lt "$need" ]; then
        note "   NOT satisfied - the ladder must drop below $(( ulimit_n / 2 )) concurrent connections"
    fi
    note "2. ip_local_port_range = $(cat /proc/sys/net/ipv4/ip_local_port_range | tr '\t' ' ')  -> $(( $(cat /proc/sys/net/ipv4/ip_local_port_range | awk '{print $2}') - $(cat /proc/sys/net/ipv4/ip_local_port_range | awk '{print $1}') )) 个临时端口"
    note "3. net.core.somaxconn = $(cat /proc/sys/net/core/somaxconn)"
    note "4. ss -s 基线: $(tcp_stat_line)  | state time-wait = $(time_wait_count)"
    local code; code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 3 "http://127.0.0.1:$PORT/" 2>/dev/null)
    case "${code:-000}" in
        2*|3*) note "5. 服务端 :$PORT 已响应 http_code=$code" ;;
        *)     note "5. 服务端 :$PORT 未响应 (http_code=${code:-000}) - ensure_server 会起一台再复核" ;;
    esac
    note "6. S1 独占: 现存 gomoku_server/ws_stress 进程 = [$(peer_processes | tr '\n' ' ')]"
    note "   规则互斥: 现存 valgrind/tsan 进程 = [$(busy_instruments | tr '\n' ' ')]"
    note "档位表: $TIERS  每档 ${HOLD}s  档内建连斜坡 ${RAMP_UP}s  档间爬坡 ${GAP}s  档后观察 ${OBSERVE}s"
    note "客户端预算: 每连接 ${INTERVAL_MS}ms 一条消息, 单档最长 $(elapsed_s >/dev/null; echo $(( MAX_TIER * 60 ))) 条/分钟, 固定读缓冲 16KB -> 400 并发 = 6.4MB"
    note "计划总时长: 4 档 x (ramp ${RAMP_UP} + hold ${HOLD}) + 3 x gap ${GAP} + 4 x observe ${OBSERVE} + 冒烟 + 回收等待 ${RECLAIM_WAIT} = $(( 4 * (RAMP_UP + HOLD) + 3 * GAP + 4 * OBSERVE + RECLAIM_WAIT + 40 )) s  (S6 上限 ${BUDGET_S} s)"
}

guard_exclusive() {
    local others; others=$(peer_processes)
    local vgs; vgs=$(busy_instruments)
    if [ -n "$vgs" ]; then
        note "ABORT (S1 / STRESS_TEST_RULES 一): valgrind or tsan is running: $vgs"
        return 1
    fi
    local strangers
    strangers=$(pgrep -x ws_stress || true)
    if [ -n "$strangers" ]; then
        note "ABORT (S1): another stress client is already running: $strangers"
        return 1
    fi
    return 0
}

run_tier() {
    local tier="$1"
    local rc=0
    note "--- tier $tier: ramp ${RAMP_UP}s + hold ${HOLD}s @ 1 msg/s/conn ---"
    local out
    out=$("$CLIENT" --tier "$tier" --port "$PORT" --ramp-up "$RAMP_UP" --hold "$HOLD" \
        --interval-ms "$INTERVAL_MS" --server-pid "${SERVER_PID:-0}" --csv "$OUT_DIR/tiers.csv" \
        2>>"$OUT_DIR/client.err")
    rc=$?
    printf '%s\n' "$out" | tee -a "$OUT_DIR/notes.log"
    # Long-connection capacity is only a server measurement if the sockets stayed up.
    # connect_ok far above the target means the client churned, and then the latency
    # numbers describe handshakes rather than a steady state.
    local ok peak
    ok=$(printf '%s\n' "$out" | sed -n 's/.*connect_ok=\([0-9]*\).*/\1/p' | tail -1)
    peak=$(printf '%s\n' "$out" | sed -n 's/.*conns_peak=\([0-9]*\).*/\1/p' | tail -1)
    if [ -n "$ok" ] && [ -n "$peak" ] && [ "$peak" -gt 0 ] && [ "$ok" -gt $(( peak * 3 / 2 )) ]; then
        note "CHURN tier $tier: connect_ok=$ok against conns_peak=$peak (>= 1.5x) -> 该档数字是握手churn, 不是稳态容量"
    fi
    note "tier $tier rc=$rc  TIME_WAIT=$(time_wait_count)  已用时 $(elapsed_s)s"
    return $rc
}

run_handshake() {
    local secs="$1" rate="$2"
    local opens=$(( secs * rate ))
    note "--- handshake: ${opens} 次建连分布在 ${secs}s (= ${rate}/s)，建连后立即优雅关闭，不发消息 ---"
    local out rc=0
    out=$("$CLIENT" --handshake-only --tier "$opens" --port "$PORT" --ramp-up "$secs" \
        --hold 1 --server-pid "${SERVER_PID:-0}" --csv "$OUT_DIR/tiers.csv" 2>>"$OUT_DIR/client.err")
    rc=$?
    printf '%s\n' "$out" | tee -a "$OUT_DIR/notes.log"
    return $rc
}

case "${1:-preflight}" in
    preflight)
        preflight
        ;;
    smoke)
        guard_exclusive || exit 1
        preflight
        ensure_server || exit 1
        note "=== 单点冒烟: 1 连接 / 12 秒内每秒 1 条 ping / 收齐 pong / 正常关闭 ==="
        "$CLIENT" --smoke --port "$PORT" --server-pid "${SERVER_PID:-0}" --csv "$OUT_DIR/tiers.csv" \
            2>>"$OUT_DIR/client.err" | tee -a "$OUT_DIR/notes.log"
        local_rc=${PIPESTATUS[0]}
        note "smoke rc=$local_rc"
        exit "$local_rc"
        ;;
    run)
        guard_exclusive || exit 1
        preflight
        ensure_server || exit 1
        note "=== 单点冒烟 ==="
        "$CLIENT" --smoke --port "$PORT" --server-pid "${SERVER_PID:-0}" --csv "$OUT_DIR/tiers.csv" \
            | tee -a "$OUT_DIR/notes.log"
        smoke_rc=${PIPESTATUS[0]}
        if [ "$smoke_rc" != 0 ]; then
            note "ABORT: smoke did not pass (rc=$smoke_rc); no ladder will be run"
            exit "$smoke_rc"
        fi
        for tier in $TIERS; do
            if [ "$tier" -gt "$MAX_TIER" ]; then
                note "ABORT (S2): tier $tier exceeds the declared ceiling $MAX_TIER"
                exit 1
            fi
            if [ "$(elapsed_s)" -gt "$BUDGET_S" ]; then
                note "STOP (S6): task budget ${BUDGET_S}s reached before tier $tier"
                break
            fi
            run_tier "$tier"
            tier_rc=$?
            note "档后观察 ${OBSERVE}s"
            sleep "$OBSERVE"
            if [ "$tier_rc" -eq 2 ]; then
                note "STOP: 熔断命中 (S5)，后续档位全部取消"
                break
            fi
            if [ "$tier_rc" -eq 3 ]; then
                note "STOP: 连续建连失败 20 次 (S4)，后续档位全部取消"
                break
            fi
            if [ "$tier_rc" -ne 0 ]; then
                note "STOP: tier $tier rc=$tier_rc, 后续档位全部取消"
                break
            fi
            note "档间爬坡 ${GAP}s"
            sleep "$GAP"
        done
        note "=== 收尾：等 TIME_WAIT 自然回收 ${RECLAIM_WAIT}s ==="
        sleep "$RECLAIM_WAIT"
        note "结束态 ss -s: $(tcp_stat_line) | state time-wait = $(time_wait_count)"
        note "读数表: $OUT_DIR/tiers.csv"
        ;;
    caseb)
        # 用例 B 的守卫是 S3 的速率与时长，而不是 S2 的并发数：握手即关时同时存活的连接
        # 只有一两毫秒量级，真正消耗资源的是 TIME_WAIT 累积，所以这里换算的是总建连数。
        B_RATE="${B_RATE:-100}"
        B_SECS="${B_SECS:-60}"
        if [ "$B_RATE" -gt 100 ] || [ "$B_SECS" -gt 60 ]; then
            note "ABORT (S3): 用例 B 上限是 100 次/秒 且 <= 60 秒，收到 rate=$B_RATE secs=$B_SECS"
            exit 1
        fi
        guard_exclusive || exit 1
        preflight
        ensure_server || exit 1
        note "=== 用例 B: 握手压测（建连后立即优雅关闭，不发消息）==="
        note "S3 声明: rate=${B_RATE}/s x ${B_SECS}s = $(( B_RATE * B_SECS )) 次建连; 预期 TIME_WAIT 峰值 ~= 该数 (客户端主动关闭)"
        note "对照: S5 熔断线 TIME_WAIT=8000, 本机临时端口 28231"
        for secs in 10 "$B_SECS"; do
            if [ "$secs" = "$B_SECS" ] && [ "$B_SECS" = "10" ]; then continue; fi
            if [ "$(elapsed_s)" -gt "$BUDGET_S" ]; then
                note "STOP (S6): 预算 ${BUDGET_S}s 已用完，剩余时长档不再跑"
                break
            fi
            local_rc=0
            run_handshake "$secs" "$B_RATE" || local_rc=$?
            note "handshake ${secs}s rc=$local_rc  TIME_WAIT=$(time_wait_count)  已用时 $(elapsed_s)s"
            if [ "$local_rc" -eq 2 ]; then
                note "STOP: 熔断命中 (S5)，剩余握手档取消"
                break
            fi
            if [ "$local_rc" -eq 3 ]; then
                note "STOP: 连续建连失败 20 次 (S4)，剩余握手档取消"
                break
            fi
            if [ "$secs" != "$B_SECS" ]; then
                note "试点通过，等 30s 让 TIME_WAIT 回落后再放满 ${B_SECS}s"
                sleep 30
            fi
        done
        note "=== 收尾：等 TIME_WAIT 自然回收 ${RECLAIM_WAIT}s ==="
        sleep "$RECLAIM_WAIT"
        note "结束态 ss -s: $(tcp_stat_line) | state time-wait = $(time_wait_count)"
        note "读数表: $OUT_DIR/tiers.csv"
        ;;
    stop)
        teardown
        ;;
    *)
        echo "usage: $0 {preflight|smoke|run|caseb|stop}" >&2
        exit 4
        ;;
esac
