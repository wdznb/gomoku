#!/usr/bin/env bash
# 题库门禁（设计文档 ENGINE_REDESIGN_V2.md §6.7 c）。
#
# 三档预算各报一次 solve_rate；与上一 tag 的读数比，任何一档掉超过 2 个百分点即红。
# 它不进 ctest（ctest 已 ~500 s，再塞一个门禁会让人开始跳过 ctest），而是 build.sh
# 之后的独立一步，与 readme_anchor_check / web_frontend_check 并列。
#
# 用法：
#   tools/tactics_bench.sh [题库目录]        默认 data/tactics
#   BUDGETS="2000 8000 32000" tools/tactics_bench.sh
#
# 题库怎么来：gomoku_analyze --replay <records.jsonl> --review --budget 200000 \
#                              --depth 8 --cards data/tactics/judged/normal.jsonl
set -euo pipefail
cd "$(dirname "$0")/.."

BENCH_DIR="${1:-data/tactics}"
BUDGETS="${BUDGETS:-4000 8000 32000}"
BIN=./build/gomoku_analyze

if [ ! -x "$BIN" ]; then
    echo "gomoku_analyze not built: cmake --build build -j\$(nproc)" >&2
    exit 2
fi
if [ ! -d "$BENCH_DIR" ]; then
    echo "no tactics directory at $BENCH_DIR" >&2
    echo "produce one first: $BIN --replay <records.jsonl> --review --budget 200000 --depth 8 --cards $BENCH_DIR/judged/normal.jsonl" >&2
    exit 2
fi

for budget in $BUDGETS; do
    echo "== solve_rate@$budget =="
    # --layer easy 是「全都要」：easy 是地板，题库里比它高的层也会被算进来。
    "$BIN" --bench "$BENCH_DIR" --budget "$budget" --layer easy | grep -E "solve_rate|layer |WARN" || true
done

cat <<'NOTE'

判据（§6.7 c）：把上面三档与上一 tag 的读数逐档比，任何一档掉 > 2 个百分点即红。
0 % / 100 % 的告警不算红：那是「预算选错了」，换一档再读（§3.3）。
NOTE
