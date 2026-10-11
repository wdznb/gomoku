# Gomoku/Renju AI — Vulnerability Review Log

Standings goal under review: *严格基于需求文档，审查所有性能漏洞、逻辑漏洞并修复，每个修复在 WSL 环境编写测试用例验证，通过后提交并重复此流程，直到没有漏洞。*

Every fix below was built in WSL (`cmake --build build -j4`) and verified with `ctest`
(**4/4 passing**: `test_board`, `test_pattern`, `test_evaluator`, `test_search`) before its
commit. Branch: `phase2-advanced-search`. Rounds 49–50 continue the same loop over the Phase 3
training modules on branch `phase3-self-training`, where `ctest` is **5/5** (they add
`test_training`).

## Round 44 — `f680a72`

**Clean shutdown on SIGINT/SIGTERM.** Signals were handled by a raw `std::signal` handler
that called `std::cout` / `g_server->stop()` (async-signal-unsafe) and never stopped
`ioc_.run()`, so Ctrl-C hung while a client connection kept a pending io op. Moved handling
onto the io thread via an Asio `net::signal_set` registered in `GameServer::start()`; `stop()`
now closes the acceptor, cancels the signal set, and calls `ioc_.stop()`.

- Verified: server exits status 0 with a client connected, printing "Shutting down...".

## Round 45 — `f171a8b`

**Resign-probe overflow + undo board race.**

- The resign probe scaled its time budget into a node cap with `usable_ms * 400` in a signed
  `int`; a large client-supplied `timeout_ms` overflowed it into a negative cap, tripping
  `node_limit_` on the first node and **silently disabling resignation** (plus UB). Compute the
  cap in 64-bit, clamp at `resign_node_limit_`, and clamp the client-controlled `timeout_ms`
  to `[100, 30000]` at the GameManager boundary (`set_config` / `request_hint`).
- `undo()` re-armed the AI worker and `handle_undo` then read `board()`/`status()` lock-free
  while the worker writes them under `thinking_mutex_`. `undo()` now returns the post-undo
  `move_count`/`status` captured before re-arming.

- Verified: new `test_search_engine_resigns_with_large_timeout` (resigns at `timeout_ms=6000000`).

## Round 46 — `606d9c5`

**Biggest defect found in the whole loop — the shipped server could not serve the web UI.**
The single binary documented at `http://localhost:8080` never read the HTTP request;
`handle_http_request` / `load_file` / `get_content_type` were declared but had no definitions,
so a browser `GET /` failed the WebSocket handshake and the UI never loaded.

- Implemented the Beast hybrid HTTP + WebSocket path: read the request first, `async_accept`
  the upgrade, **or** serve a static file.
- `resolve_file` resolves strictly under `web_dir_` via a canonical-prefix check plus rejection
  of `..`, `.`-segments, backslash, `%`, `//` and fragments (path-traversal safe).
- `GameManager` (and its multi-tens-of-MB transposition table) is now created only on a
  successful WS handshake, so static-asset and probe connections never each allocate one.
- `choose_side` rejects unknown/missing colors instead of silently meaning WHITE.
- Opening book clamps each weight and accumulates the weighted pick in 64-bit (removed a
  signed-int overflow from a malformed book file).

- Verified with raw-socket integration tests: `GET /`, `/app.js`, `/style.css` → 200;
  `/../../etc/passwd`, `/web/../../CMakeLists.txt`, `//etc/passwd` → 404; WebSocket → 101;
  live `choose_side` → `state_sync` → `thinking_progress`; invalid color → error.
  New unit test `test_opening_book_large_weight_no_overflow`.

## Round 47 — `afb7de6`

**Portability + socket hygiene.** `resolve_file` compared the canonical path against the web
root using a hard-coded `'/'` boundary; that never matches a Windows-preferred separator, so it
now uses `fs::path::preferred_separator`. A WebSocket handshake that fails after the upgrade
request is read now closes the socket explicitly instead of relying on destructor timing.

- Verified: HTTP serving + traversal rejection + WS 101 all re-confirmed on WSL.

## Round 48 — full-scope audit: no genuine defects

Audited the previously-uncovered modules — board win detection (`has_five_in_a_row` /
`check_winner`, BLACK exactly-five vs overline, WHITE five-or-more), `undo_last_move` /
`move_history` consistency, greedy legality + immediate-win, evaluator overflow/signs,
`types.h` / `protocol.h` enum aliasing and `opponent()`, `main.cpp` arg parsing, and CMake /
`build.sh` wiring. Confirmed clean by executable probes and the committed suites.

## Convergence

The last two rounds surfaced only a portability nit on a non-supported native-Windows build
(R47) and nothing genuine (R48). Every module has now been freshly deep-audited across rounds
45–48 with a clean final pass; reviews are returning false positives rather than new defects.
**The review→fix→test→commit loop has converged.**

## Deliberately deferred (not correctness bugs; each gated on needing a strength/stress harness)

- Gap-pattern matcher (R34 F3)
- Overline-locked-four threat classification (R35 F3)
- Engine perf refactors: incremental eval, `sort_moves` pattern threading, `find_defense` board
  copy (R41 F1/F2/F3)
- Blocking worker join on the io thread (R42 F3)
- Verified false positive: VCF/VCT live-four early win (R43 F1) — correct because
  `find_block_move` returns the unique forced block

## Phase 3 — self-training (`8cc5ac6`, `118e921`)

### Round 49 — audit of the new training modules

**Silent integer truncation at every training file boundary.** JSON keeps integers in 64 bits and
nlohmann's `get<int>()` only casts, so `"five": 4294967396` was applied as `100` — a mate score the
file never stated, which also slipped past the `five <= 100000` guard because that check ran on the
already-truncated value. Board coordinates were worse: `row: 4294967299` narrowed to `3` and
replayed a different square. Added `training/json_int.h` (`read_json_int`, `read_json_uint`) and
used it at each boundary with the policy the value deserves: parameters and coordinates are
**rejected** with a message (`CoefficientVector::from_json_string`, `GameRecord::from_json`,
`TrainingSample::from_json`), while informational counters fall back to their default
(record metadata, `EloSystem::load_json` — those only choose the K-factor bracket).

- Verified: `gomoku_server --params` and `gomoku_train --params` both exit non-zero on an
  out-of-range `five` / `live_four`; new `test_readers_reject_out_of_range_numbers`.

**Data race on `games_played_`.** `Trainer::generate_data_games` incremented the shared, non-atomic
counter from every collection worker while the main thread incremented it for match games. The
per-worker record lists are now merged after `join()` and counted once.

**An unusable record could still be scored.** `is_decisive()` excluded only `MOVE_LIMIT`, so a
parsed record carrying `winner` with `reason: "invalid"` produced ±1 from `outcome_for` — the same
direction-of-error rule the search side enforces (UNKNOWN must not read as a result). A record that
cannot be replayed now reports no outcome, and `DataCollector` reads through it.

**Checkpoint resume applied partial state.** The counters were `get<int>()`'d straight into the
members, so a rejected file could leave the trainer running on the checkpoint's versions and
ratings, and a full-width `--seed` came back narrowed. Reads now go into locals and are applied only
after the whole file validates; out-of-range or negative counters and a mismatched champion index
are rejected.

- Verified: new `test_trainer_checkpoint_seed_and_counters` (seed `0xFFFF…FF` round-trips;
  `rounds_completed: -2` is rejected and leaves the trainer's state intact).

### Round 50 — measured the shipped training defaults

One hill-climbing round at the documented defaults (`--depth 4 --timeout-ms 400 --max-moves 60`,
resign on) does produce decisive games: both data games ended `five` for BLACK at 27 plies, 256
samples, champion and candidate each 0-0-1. Fitness and verification still land on exactly 0.500,
because the pairing is symmetric — each side wins the game in which it holds the first move. That is
what colour-swapping is for (a candidate must win **with white** to clear `--accept-score 0.56`
instead of being credited for the opening advantage), but it means `--fitness-games 2` hands the
optimiser a 0 / 0.5 / 1 plateau rather than a gradient.

Documented in the README instead of changed: raising the default match size multiplies the cost of
every round, and turning the fitness signal into something strength-graded is the deferred
strength-harness work listed under "Deliberately deferred" above.

### Phase 3 status

`ctest` is 5/5 (`test_board`, `test_pattern`, `test_evaluator`, `test_search`, `test_training`),
`make -j4` is warning-free, and the round-50 run exercised the full pipeline end to end
(records, samples, checkpoint, promoted champion file, `--params` load into `gomoku_server`).
