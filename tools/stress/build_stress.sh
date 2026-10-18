#!/bin/bash
# Build the stress client outside the production target list.
# docs/PROMPT_STRESS_TEST.md 禁止事项: the probe must not be mixed into the shipped
# build, so it is compiled here instead of being added to CMakeLists.txt.
set -euo pipefail

cd "$(dirname "$0")"

for want in g++; do
    command -v "$want" >/dev/null || { echo "missing $want (needs build-essential)" >&2; exit 1; }
done
[ -d /usr/include/boost ] || { echo "missing /usr/include/boost (needs libboost-dev)" >&2; exit 1; }

g++ -std=c++17 -O2 -Wall -Wextra -pthread ws_stress.cpp -o ws_stress
echo "built $(pwd)/ws_stress"
