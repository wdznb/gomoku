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

- ~~Gap-pattern matcher (R34 F3)~~ — done in round 54
- ~~Overline-locked-four threat classification (R35 F3)~~ — done in round 54
- ~~Engine perf refactors: `sort_moves` pattern threading, `find_defense` board copy (R41 F1/F2)~~ —
  done in round 55. Incremental whole-board evaluation (R41 F3) was measured there and rejected: a
  make/undo delta costs about as much as the re-scan it replaces.
- ~~Blocking worker join on the io thread (R42 F3)~~ — measured in round 56 and closed without a
  code change: the wait is bounded by one search node (worst observed 314 µs at depth 32 / 30 s),
  it cannot deadlock because the callbacks post to the session executor, and removing it would
  require per-worker transposition tables plus detached-thread lifetime management.
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

## Phase 4 — optimization (`98a85e3`, `dfa90a6`)

### Round 51 — audit of the bitboard recognizer and the root dedup

**The rewrite had to be proven equal, not argued equal.** Shape semantics cannot be re-typed by hand
and kept honest, so the 2^18 lookup table is *generated* by running every `(own << 9) | blocked` key
through the untouched window decoder — the table is the old matcher by construction, and the scalar
path survives only as a test oracle. On top of that: `test_bitboard_matches_scalar` compares both
paths over all 225 squares × 4 directions × both colours on 60 random boards plus structured
edge/corner/diagonal boards (111,600 windows), `test_line_bitboard_tracks_cells` replays 600
interleaved place/undo steps and checks that every stone's bit lives only in its own colour's line
words in all four families (copy-construct and `reset()` included), and `test_run_length_matches_walk`
diffs `run_length` against an independent `get()`-based walk 108,000 times. `run_length` needed its
own oracle because `finish_direction` — the Renju demotion of WHITE `LONG_FIVE` and the promotion of a
BLACK `FIVE` whose real run is ≥ 6 — is shared by both recognizer paths, so a differential test alone
could not see a `run_length` bug.

- Verified: identical search output before and after (559 nodes, same best move, same score, same
  evaluator sinks); measured 216 → 48 ns per direction, 29.5 → 5.2 µs per `evaluate_board`,
  35k → 207k root nodes/s. Table build costs 4.7 ms once and 256 KB of a static member.

**Colour became an array index.** With per-colour line words, a stray colour value stopped being a
no-op and became out-of-bounds read/write, so `set_stone_bits`, `run_length` and `recognize_direction`
now reject anything outside BLACK/WHITE (and `recognize_direction` also rejects an out-of-range
direction family). The wall bits outside a line's 15 real cells are never set by a legal stone, which
is what lets off-board window cells come from the precomputed mask instead of per-cell bounds tests.

**Root dedup must not change the answer.** Only mirror-equivalent candidates are dropped — one
lowest-index cell per orbit under the position's own D4 self-symmetries — and the identity variant is
always in that set, so a non-empty root never empties. The evaluator's position weights
(`|r-7|+|c-7|`) and the pattern recognizer are D4-equivariant, and the Zobrist keys are not, so
mirrored branches cannot cross-talk through the transposition table. Positions with no self-symmetry
skip the pass entirely; measured on a symmetric position: 14,838 → 5,241 nodes at identical move and
score. `test_root_symmetry_dedup` checks the 8-variant empty board yields exactly 36 orbit
representatives (Burnside), that the chosen move is always a representative, and that a horizontal and
a vertical mirror position score alike.

## Phase 4 (continued) — replay analysis and frontend (`phase4-ui-replay`)

### Round 52 — audit of the replay path

**The analysis list is a second copy of the move history, so it has to be edited everywhere the
history is.** `GameManager` appends on the human's `make_move`, on the worker's committed stone (only
inside the same `thinking_mutex_` critical section that places it), pops exactly the stones `undo()`
gives back, and clears with `reset()`. A resign adds no stone and therefore no entry — asserted
against `board().move_count()` in `test_game_manager_replay_snapshot`, which is the invariant the test
holds on rather than a hardcoded size.

**Reading the record from the io thread while the worker commits.** `replay_snapshot()` copies the
list, winner, reason and status under `thinking_mutex_` in one lock, so the client can never receive
a move list from before the ending paired with the ending's own fields. The io thread is single
(`ioc_.run()` on one thread), which is what makes the un-locked `make_move` append safe; the worker is
the only other writer and it holds the lock.

**The client's own turn state can be stale after a rebuild.** `handleReplay` rebuilds the move list
from the server's authoritative snapshot, which may have been taken before an in-flight AI stone was
committed. Re-deriving `currentTurn` from parity and `lastMove` from the rebuilt list (the same
reconciliation `handleUndoSuccess` already does) keeps the view self-consistent; the missing stone
arrives as the next `ai_move`.

**A loaded record silently goes out of date in two different ways**, and the first version of the
enable rule only caught one: a new move makes the record shorter than the board, while an ending that
arrives *after* a complete load changes no move count at all. The button now re-arms on either, and an
undo drops the record outright because every analysis index after the popped plies would point at the
wrong stone.

- Verified in a real browser against the WSL server: play → load → step / auto-play / jump-to-move →
  exit, resign → reload (ending text restored after exiting the replay), undo (record invalidated),
  and the console stayed clean. Screenshots were used to catch the layout regression where the
  full-width `.btn` rule stacked the replay controls one per row.

**Two pre-existing frontend defects surfaced while wiring this up.** The 超时时间 / 搜索深度 inputs
were only ever sent as `request_hint` parameters, so the AI's own moves always ran on the server's
defaults while the UI claimed to configure it — they now send `set_config` on change and before
choosing a side. And the game-over message's `reason` was dropped on the floor; the status line now
names the ending (五连 / 认输 / 满盘和棋).

**Curve convention.** The engine scores each move for the side that just played, so plotting the
record directly would zig-zag with whose turn it was and drag through zero on every human ply
(those entries carry no evaluation at all). The graph plots engine moves only, negated for WHITE, and
says so on the canvas.

### Round 53 — dead code left behind by the JSON protocol

Swept every function declared in `src/**/ *.h` for call sites across `src/`, `tests/` and `web/`.

**The whole of `src/server/protocol.h` was unused.** Its `ClientMessage` / `ServerMessage` /
`MoveJson` structs described an earlier typed-message layer; the handlers parse and emit `nlohmann::json`
directly, so nothing ever populated them. `game_manager.h` still included the header, which kept it alive
in the dependency graph without any consumer. `GameServer::game_state_json` was the same story one layer
up: `choose_side` builds its `state_sync` inline, so the helper had no caller and only duplicated the
board/turn/status serialization. `move_to_json` was never called either.

**Two more were misleading rather than merely unused.** `AlphaBetaSearch::set_move_time_limit` wrote
`move_time_limit_ms_`, but `search()` overwrites that member from its own `time_limit_ms` argument on
every call, so a value set through the public setter could not survive into a search.
`RetryExplorer::record_new_plies` was declared and never defined — legal for an unused member, but any
future call would be a link error rather than a compile error, so the stale declaration went.

**Deliberately kept:** the `SearchEngine::set_vcf_depth` / `set_vct_depth` / `set_alphabeta_*` /
`set_resign_node_limit` and `DataCollector::set_keep_undecided` setters, and the read-only
`exhausted()` / `current_line()` / `transposition_table()` accessors. Each fronts a member the code does
use, so they are an unexercised configuration and introspection surface rather than dead logic; deleting
them would have removed capability without fixing anything.

- Rebuilt warning-free and `ctest` 5/5 after the removals; `build/web` matches `web/`, so the server
  started from the build directory serves the current UI.

### Phase 4 status

All four items of the phase are implemented: 4.1 bitboard and 4.2 root dedup (released as
`v0.4.0-phase4`), 4.4 per-move analysis capture plus `request_replay` / `replay`, and 4.3 the replay
UI, eval curve, clickable move list, ending reason and working AI settings.

## Deferred items (rounds 34/35) — gap shapes and overline-locked fours

### Round 54 — teaching the matcher about holes

The user re-approved the deferred list, so R34 F3 (gap/broken shapes) and R35 F3 (overline-locked
four) are now closed together. Both live in the same two functions, and both were verified rather
than argued.

**`match_pattern` only ever counted the run through the placed stone**, so every single-hole shape was
read one or two grades too weak: `XX_XX` was a two, `X_XXX` a three, and `XXX_X` nothing at all,
because the scan stops at the first empty cell. The fix splits the classification into the contiguous
run and a one-hole extension of it, and returns the stronger label — the `PatternType` enum is already
ordered by threat strength, so that is a plain `max`. Scope is deliberately narrow:

- four stones around one hole (`XXX_X`, `XX_XX`, `X_XXX`) are a **rush four**, because the hole is
  their only five point and blocked outer ends cannot take that away;
- three stones around one hole (`X_XX`, `XX_X`) are a **broken three**, live only when the hole fill
  would make a *live* four (both cells outside the whole shape empty);
- two stones around a hole (`X_X`) are left unlabelled, and shapes holding five or more stones are
  left to the contiguous label — `X_XXXX` is a four story, not a hole story, and relabelling it would
  hide that it has two completion points.

Because `fill_pattern_lut` generates the table by running every key through `match_pattern`, the
bitboard path and the scalar oracle moved together for free: `test_bitboard_matches_scalar` still
passes over 111,600 windows after the change.

**Two consequences had to be chased down.** `VCFSearch::find_block_move` does its own contiguous walk
and had no idea a hole could be a forced block, so gap support there was added (stones beyond exactly
one empty cell, four in total, block the hole) — and the new `test_vcf_gap_four_sequence_wins` proves
the payoff: a VCF whose *only* win begins with `1.(7,6)` making `XX_XX`, where the contiguous
alternative `1.(7,4)` is dead-ended by a white stone at `(7,1)`. That assertion could not have been
satisfied before this change, because the move was never generated as a candidate.

**R35 F3 was the riskier half, and the first attempt was wrong.** A BLACK four only threatens if some
completion point makes *exactly* five, so `XXXX_X` (live four by the window: both ends empty) is
really a rush four, and `X_XXXX_X` (every fill is a six) is not a four at all. The first version
measured the completion points with `Board::run_length`, which counts the run on the board as it
stands — the square under recognition is still empty, so the walk stopped at it and every plain live
four looked like a three. `test_recognize_live_four` failed immediately. The correction is a local
`fill_run` walk that treats the square under recognition as already BLACK; only shapes whose run is
exactly four are in scope, so gap fours keep their label.

- Verified: `ctest` 5/5, and a short training run confirms the matcher is not merely quieter — all four
  recorded data games now end `five` with **WHITE** winning, where the same configuration previously
  ended with BLACK at 27 plies. The reading is that BLACK now runs into stricter forbidden detection
  (broken threes count toward 三三) and that WHITE's holes count as the threats they are; it is a
  one-run observation, not a strength measurement.
- One test expectation was relaxed on purpose: `test_vcf_defense_own_live_four_survives` hard-coded
  the defence cell, and with holes recognised `(10,2)` / `(10,8)` are decisive tempi too, so the
  ordering legitimately changes. It now asserts the property that matters — that WHITE answers with a
  four-or-better of its own rather than a passive block.

## Deferred item (round 41) — engine performance refactors

### Round 55 — threading one recognition pass, and measuring the third refactor

R41 F1/F2/F3 are three separate claims: `sort_moves` re-recognizing patterns, board copies in the
threat searches, and incremental whole-board evaluation. The first two were implemented; the third was
measured and rejected. All three were settled with numbers rather than argument, which is exactly the
harness this list was gated on.

**The verification gate came first.** These refactors must not change what the engine plays, so
`build/fingerprint.cpp` (scratch, not built by CMake) prints method / score / nodes / chosen move /
resign flag for 25 positions × depths 2, 4, 6 × threats on and off × resign on and off, plus a
`find_defense` loss proof for both colours at every position — 350 lines. Compiling it against
`git archive HEAD`'s tree and against the working tree and diffing gives a byte-level behaviour check
far stronger than the unit tests: node counts included, so any change in move ordering shows up even
when the chosen move does not.

**F2: `sort_moves` ran eight direction scans per candidate.** It scored each candidate with
`evaluate_position(board, r, c, color) + evaluate_position(board, r, c, opp)` — two full four-direction
recognitions of the *same* two bitboard lines, because a BLACK stone is exactly a blocked cell in
WHITE's window and vice versa. `PatternRecognizer::recognize_position_both` now walks the four
directions once, takes the two line loads per direction and derives both colours' windows from them,
and `Evaluator::evaluate_position_pair` / `evaluate_board` / `evaluate_position_both` use it. The
scoring rule (mate first, then pattern score plus centre bias) moved into one private
`candidate_value`, so the pair path and the single path cannot disagree.

- Verified: new `test_recognize_position_both_matches_pair` compares the shared pass against two
  single-colour `recognize_position` calls field-by-field on every square of 42 boards — 18,900 squares
  including occupied ones and both colours, on packed rows, columns and both diagonals so the
  colour-specific corrections are covered on every square at once. It failed on its first run for the
  wrong reason (a copied `assert(checked >= 100000)` threshold sized for the four-direction-scan
  scalar test, while this one counts squares), which is a reminder to size the coverage assertion to
  the loop the test actually runs.

**F1: only one of the three "board copies" was a loop copy.** `vcf.cpp` and `vct.cpp` recursion already
make/unmake on a `Board&`; the copies at `VCFSearch::search_impl`, `VCTSearch::search` and
`AlphaBetaSearch::search` are once per search or once per depth iteration. The real loop copy was
`find_defense`'s `Board after = board;` inside the reply loop — and it was paying *two* copies per
reply, because `search_impl` then copied its argument again. `search_impl` now takes `Board&` (its
caller owns the copy) and the resign probe pays one instead of two.

**Reusing the board across replies was tried and reverted.** `undo_last_move()` restores `active_empty_`
by swap-remove, so a board that has played and unplayed stones holds its candidate list in a different
order than a fresh copy — and `sort_moves` breaks score ties against that order. Timing the fingerprint
proved it: hoisting AlphaBeta's per-iteration copy out of the loop kept every score and every chosen
move but moved node counts by ~4% (13,987 → 14,847 at two stones, depth 4), i.e. a different search.
Same for `find_defense`: one shared board across the defender's replies would silently re-order the
threat generators. Both copies are load-bearing for reproducibility and now say so at the call site.
After that revert the fingerprint is `IDENTICAL (350 lines)` — the accepted refactors change speed
only.

**F3: incremental evaluation does not pay for itself here.** Measured on the working tree:
`evaluate_board` is 0.58 calls per node at 12 stones / depth 6 and each call costs ~3.7 µs, so the full
re-scan is ~50% of search time — worth attacking if a delta were cheap. It is not. A pattern at cell X
reads the 9-cell window along each direction through X, so one stone changes the candidates on the four
lines through it: measured over every candidate of a 12-stone position, **15 cells (27% of the 56
candidates)** must be refreshed per placement, 38% at four stones and 18% at 26. A make plus its undo
therefore costs ~54% of the full re-scan that the node's single `evaluate_board` call costs 58% of —
break-even in the midgame, a loss in the early game, and only a win past ~20 stones, and that ignores
the bookkeeping for candidates entering and leaving the set. Not implemented.

| position | depth | nodes (before = after) | before | after |
| --- | --- | --- | --- | --- |
| 12 stones | 4 | 39,990 | 222 ms | 185 ms |
| 12 stones | 6 | 722,966 | 3,764 ms | 3,159 ms (192k → 229k nps) |
| 16 stones | 6 | 921,214 | 4,527 ms | 3,704 ms (203k → 249k nps) |
| `evaluate_board` at 12 stones | — | — | 4.89 µs/call | 3.75 µs/call |

- Rebuilt warning-free, `ctest` 5/5 (now 6 pattern binaries' worth of cases including the new
  differential test), and the pre/post fingerprint diff identical, after each accepted step.

## Deferred item (round 42) — joining the AI worker on the io thread

### Round 56 — what the blocking join actually costs

R42 F3 flagged that every abort path in `GameManager` (`reset`, `resign`,
`request_hint`, `start_ai_thinking`'s re-arm of a running worker, the destructor) `join()`s
the AI worker thread on the caller's thread — and in the server that caller is the single io
thread, which holds `games_mutex_` while it does so. A slow join would therefore stall every
session, not only the one being reset. It was deferred because sizing it needed a threaded
harness. `build/joinprobe.cpp` (scratch, not built by CMake) is that harness: it arms a worker
at a given depth/budget, waits, and times the aborting call itself.

**The wait is bounded by one search node, not by the search budget**, and the numbers show it
rather than argue it. Every search loop re-reads `stop_flag_` per node
(`alphabeta.cpp:83/133/160/209/291/306`, `vcf.cpp:121/130/244/407`, `vct.cpp:109/123/213/223/325`)
and `SearchEngine::search` re-checks it between the book / VCF / VCT / AlphaBeta / resign-probe
phases (`search_engine.cpp:101/120/138/151/171`), so an aborting caller waits for the node
already in flight and nothing more. Latency is flat across a 300× budget range, which is exactly
what that bound predicts:

| root | depth | timeout | worst of the four join sites | samples provably mid-search |
| --- | --- | --- | --- | --- |
| empty (225 candidates) | 4 | 100 ms | 260 µs (hint) | 15/15 |
| empty | 8 | 30 s | 247 µs (resign) | 15/15 |
| empty | 16 | 30 s | 274 µs (restart) | 12/12 |
| empty | 24 | 30 s | 251 µs (resign) | 18/18 |
| empty | 32 | 30 s | 314 µs (restart) | 15/15 |

Depth 32 / 30 s is far past anything a client can ask for (`clamp_timeout_ms` caps the deadline
at 30 s regardless of what `set_config` or `request_hint` sends), and the join still costs a
third of a millisecond. The four sites — `resign`, `reset`, re-arming over a running worker, and
`start_ai_thinking` joining a live hint worker — agree with each other to within the noise.

**Two measurement traps had to be removed before these numbers meant anything.** (i) The engine's
transposition table belongs to the `GameManager` and survives `reset()`, so repeating one position
let the depth-16 search finish *before* the abort landed; the probe then timed `join()` on an
already-exited thread and reported 1–10 µs — a measurement of nothing, and reassuringly wrong.
Varying the stone layout per rep and printing the mid-search sample fraction is what exposed it.
(ii) A 12-stone warm-up walk completes a five, and on a finished game the worker returns
immediately: same false reading, different cause. Even at live 8-stone positions the abort landed
inside the search in only 6 of 12 samples, because those dense fill patterns are solved by the
threat searches almost at once. The empty board is the widest root the engine really sees, is
reliably mid-search, and is therefore the pessimistic case — reported above.

**There is no deadlock and no second thread hiding behind the join.** All four `GameManager`
callbacks funnel into `WebSocketSession::send`, which does `net::post(ws_.get_executor(), …)`
(`server.cpp:128-129`): the worker queues its messages for the io thread instead of running them
inline, so the io thread never waits on a send and the worker never needs the io thread to make
progress — which is the cycle a blocking join could deadlock on. `SearchController`
(`src/search/controller.cpp`), which owns a worker thread of its own, is referenced only by
`tests/test_search.cpp`; the server path is one thread per session, so this join has no
transitive chain.

**Not implemented, and what implementing it would actually cost.** Dropping the join means an
aborted worker must become incapable of touching shared state once the caller moves on. Two
concrete obstacles, both real: `engine_` is one object per session holding `tt_alphabeta_` and the
sub-searches' `nodes_searched_` / deadline state, so an old worker and its replacement searching
concurrently would write the same `std::vector<TTEntry>` (a benign-looking race, but `lookup`
returns a `const TTEntry*` into it while `insert` writes it) and corrupt each other's counters —
giving each worker its own engine costs the ~64 MB table per session that
`search_engine.h:74-77` deliberately avoids allocating twice; and a detached worker outliving
`GameManager` needs the committed state (`board_`, `analyses_`, `status_`) moved into a
reference-counted block that every call site then has to respect. That is a restructure of the
session's concurrency model, with a new class of shared-engine bugs to prove the absence of, to
save ~0.3 ms of io-thread time on a user-initiated abort while that client is already waiting for
the AI. Closed on the measurement; the deferred list is now empty apart from the recorded false
positive (R43 F1).


## 摆设棋局（自定义开局）— feature round 57 (`phase4-ui-replay`)

**What shipped.** A position can now be installed instead of played out: `src/core/position_setup.*`
parses the three forms (15×15 / flat-225 / text grid, a 4×`uint64` black+white mask pair, an alternating
coordinate list), detects the black/white/empty marks over five alphabets (`1/2/0`, `1/-1/0`, `B/W/.`,
`X/O/.`, `黑/白/空`), and validates the result (count difference, an existing five, and every BLACK
stone re-tested through `is_forbidden` by removing it, which is also what catches 长连 — `check_winner`
deliberately does not treat an overline as a win). `src/server/position_json.*` adds the document/file
layer and the canonical emitter; `GameManager::set_position` installs the board; `check_position` /
`set_position` carry it over the socket; the web panel paints, imports, detects, confirms and exports.
`ctest` is **6/6** (`test_position` covers all three layers plus the five shipped `data/` templates).

**An unrecognisable grid is refused, never guessed.** The detection loop demands that one alphabet
cover *every* distinct token in the file, so `./2/B` yields "无法识别棋盘标志 (./2/B)" rather than a
reading that treats the stray symbol as empty and quietly deletes a stone. An explicitly supplied
`encoding` is trusted as stated but still has to cover every token, for the same reason.

**The confirmation rule fits a human and a script through one code path.** A grid-shaped document must
state its mapping — either in the file (which is why the templates carry `encoding`) or in the message
after the player confirms the reading `check_position` echoed. Bitboard and move-list forms have no
symbols, so they owe nothing. No client-type flag was added; a trainer pointing at the same template
simply loads it directly.

**只摆局面，不开新局.** `set_position` aborts and joins both workers, drops them, clears `analyses_`,
leaves `status_ = WAITING` and sets `position_pending_`. Only `set_player_color` and a played stone clear
that flag, which is also why `choose_side`'s guard could be relaxed to
`move_count() > 0 && !position_pending()` without letting a mid-game client re-pick a side. The turn
needs no extra state: `current_turn()` derives it from `move_count() % 2`, exactly as the loader's
count rule does.

`build/wspos.py` (scratch stdlib WebSocket harness, not built by CMake) is what proves the socket half
of that: 22 checks over detect → confirm → load → choose_side → human move → AI answer, including that a
load arms no worker, that a loaded position replaces a live game, and that each refusal reaches the
client as the right message (`position_preview{ok:false}` for a dry run, `error` for a refused load).

**Three defects found while verifying, all on the preview side.** (i) The server attached
`encoding` to every `position_preview`, so a bitboard or move-list position came back as
`{"black":"","white":"","empty":""}` — meaningless strings that read as "the marks are blank". It is now
sent only for the `board` form (`server.cpp:522`). (ii) That wart was load-bearing for a crash: the
client computed the mapping line unconditionally, so pasting a *bitboard document as text* threw a
`TypeError` inside `handlePositionPreview` and the preview was silently dropped — the 载入 button never
enabled and no reason was shown. Both branches now key off `needsConfirm = fromText && !!msg.encoding`,
which is also the exact condition under which `set_position` requires the box ticked. (iii) A preview
repaints the board (a dry run should show what the file means), but `state_sync` — the one message that
carries the server's authoritative board, and the reply to `choose_side` — was handled by setting a
status string and nothing else. So "识别 → 忘了点载入 → 直接选边" left the player looking at five stones
while the session played an empty board, with the turn and the parity disagreeing. `handleStateSync` now
applies `msg.board`, adopts `msg.current_turn`, drops the preview and redraws, which makes the server's
board the only thing a started game can display.

Verified in a real browser: pasted bitboard JSON loads with no checkbox, a pasted `B/W/.` grid shows
黑=B 白=W 空=. and stays disabled until confirmed, and the choose-the-non-moving-side path arms the AI
immediately on the loaded position (the socket harness saw the same sequence as
`thinking_progress{depth:5}` → `ai_move`, and the board the client renders matches it).

**Test fixtures were the main source of failures, not the parser.** A hand-written `white[1]` of
`0x0003000000000000` sets bits 48 and 49, i.e. (7,7) and (7,8), and (7,7) is BLACK — the overlap
assertion fired on the test's own arithmetic, not on `word_of`. Recording the cell→bit derivation in the
comment (`(7,7)=112 → word 1 bit 48`) is what makes the next reader re-derive instead of trusting it.

**Deliberate omissions.** No client-side reconnect exists for the 60 s idle socket timeout, so a page
left open on the position panel drops its WebSocket and must be reloaded — pre-existing behaviour, out
of scope here.

## 摆设棋局（自定义开局）— feature round 58 (`phase4-ui-replay`)

**What shipped.** The grid form now also accepts one long string of the whole board, separated or
unseparated, from a file or from a socket message with no extra field: 225 tokens split on
comma / semicolon / tab / space / newline (one line or many, breaks may fall anywhere), or 225 symbols
written back to back (`...BW....`, `01020…`, even a `黑/白/空` run). `data/position_flat_template.txt`
(one 225-character `B`/`W`/`.` line) and `data/position_flat_template.csv` (one line of 225 comma-
separated `1`/`-1`/`0`) are the two new shipped templates, so the loader has seven to prove itself
against.

**The flat reading is a fallback, not a rewrite.** `parse_grid_text` still tries the row layout first
(`split_row` reads a 15-character ASCII line as one symbol per cell, otherwise it splits on separators),
and only when the document is not 15 rows of 15 cells does it re-read everything it collected as a
single run. Keeping the row bookkeeping in a `ragged` flag is what preserves each existing complaint
verbatim — 16 uniform rows are still "棋盘有 16 行，需要 15 行" and a three-cell line is still
"第 1 行有 3 格，需要 15 格", because blaming a length for what is a layout problem sends the reader to
the wrong place. A 15-character-per-cell line is also still the only unseparated row shape the row
parser takes, since a 30-character numeric line really is ambiguous between 15 `-1` cells and 30 marks.

**Symbol kinds are judged before the cell count.** A run is exploded by UTF-8 codepoint
(`codepoints_of`, so 225 Chinese marks are 675 bytes and not 675 cells; a lead byte missing its
continuation bytes yields itself alone, which matches no alphabet instead of corrupting its neighbour),
and the distinct-kind check runs before the length check: a fourth kind of symbol cannot become a board
at any length, so it is reported as "连续棋盘含 N 种符号 (…)，只能表示黑/白/空三种" whether the file was
200, 300 or 225 symbols long. Only a run that does have at most three kinds gets the length complaint
("棋盘有 N 格，需要 225 格（15x15，分隔或连写皆可）") — truncation and mixed alphabets need different
fixes, and the first thing said should be the thing that decides the answer. `1/-1/0` remains a
separated-only dataset by definition: unseparated, `-1` is two marks.

**`read_position_payload` had to stop trusting JSON.** A numeric flat run whose first symbol is not `0`
is not discarded text — `100…` is a legal 225-digit JSON *number*, so the old "did it parse? then it is a
document" branch handed the server a scalar and answered "局面文档必须是 JSON 对象" for a perfectly
readable board. Anything that is not an object or an array now goes to the parser as `{"board": text}`,
which is the same route a CSV paste takes.

**Verification.** `ctest` 6/6 with `test_flat_strings` added (ASCII run, numeric run, 675-byte Chinese
run, separated runs with breaks every 50 cells, four-kind refusals at three lengths, wrong-length
refusals, and the row diagnostics intact). `build/wspos.py` is up to 28 checks, covering the same forms
over the socket plus the shipped flat file; `read_position_payload` is exactly what those six new
checks exercise. In the browser, pasting the 225-character `B`/`W`/`.` run into 摆棋 and pressing 识别
returned "确认标志：黑=B 白=W 空=.", 载入 stayed disabled until the box was ticked, and the loaded board
showed 3 黑 2 白 with 白方 to move. (The socket did drop once mid-verification, for the known 60 s idle
timeout and not for anything in this round: a reload re-ran the sequence in one script.)

## Documentation audit — round 59 (`phase4-ui-replay`, before merging to master)

**`build.sh`'s non-CMake fallback had been left behind by three phases of growth, and README
recommended the script as 方法1.** Its compile line listed no `transforms.cpp`, `position_setup.cpp` or
`position_json.cpp`, still cloned and included **websocketpp** (the transport has been Boost.Beast for
the whole time), and linked `-lboost_system -lboost_thread` — which the CMake path never needs, because
`find_package(Boost REQUIRED)` there resolves to headers only and `-lboost_system` is not even installed
on this host. So the documented "recommended" build would fail with undefined references on any machine
without CMake. The list now mirrors `GOMOKU_SOURCES`, the websocketpp download is gone, and the link is
`-lpthread`; verified by running exactly that command (compiles clean under `-Wall -Wextra`, and the
binary it produced serves HTTP and loads `data/opening_book.json` on a scratch port).

**README corrections.** The dependency section named websocketpp as an auto-downloaded dependency; the
"手动编译" example was a stale nine-file command that could not link, and now says plainly that the
source list belongs to `CMakeLists.txt` and that cherry-picking files only produces missing symbols. The
训练 defaults paragraph still reported round 50's measurement ("27 手内黑棋成五") as the current
behaviour, which round 54's stricter forbidden / gap reading overturned — it now gives both measurements
and states the conclusion that actually matters for the CLI: verification pairs each colour once, so a
colour-decided outcome lands the score at exactly 0.500 and `--accept-score 0.56` demands the candidate
win the game where it does *not* hold the advantageous colour. The 服务器→客户端 protocol block listed
only seven of the twelve message types the server can send, missing `state_sync` — the one message that
carries the server's authoritative board, and the reason round 57's phantom-board defect mattered — so
`state_sync`, `move_made`, `undo_success`, `hint_result` and `game_restarted` are documented, with the
note that `restart` deliberately answers `game_restarted` rather than `state_sync`. The 版本 section now
says which merged content still has no tag.

**What checked out.** The `src/` and `tests/` trees, the five server CLI flags, every trainer default in
the options table (rounds 3 / depth 4 / 400 ms / 60 moves / fitness 2 / verification 8 / 0.56 / data 4 /
threads 2 / population 8), the 100–30000 ms server-side timeout clamp, the 2^18 shape-table numbers and
the round-51 performance table. The front-end's timeout input allows 1000–60000 while the manager clamps
to 100–30000, which is the documented server range — a UI/server mismatch, not a documentation error, and
left alone.

## 前端极简重排 — feature round 60 (`master`)

**What shipped.** The page lost its title, its gradient cards and its thinking-progress line, and is now
laid out geometrically: the board is a centred square with its coordinates drawn on the canvas edges, and
the rest of the viewport is cut into four regions by the lines from each board corner to the nearest page
corner — status on top, controls below, replay at left, move list at right. A view control (旋转 90° steps
and 镜像) re-orients the board as the player sees it. All geometry is computed in `layout()` from
`innerWidth/innerHeight` with device-pixel-ratio-scaled canvases, and re-runs on resize.

**The regions take the inscribed rectangle of their trapezoid.** The dividing lines are one
`pointer-events: none` SVG, so they can be drawn exactly where the spec puts them, but the content is
placed in the rectangle that fits inside — width = board width for the top/bottom bands, height = board
height for the side bands. Anything else gets clipped by the diagonal or overlaps the board.

**The view is display-only, and that has to be true in both directions.** `displayOf` / `logicalOf` map
logical ↔ shown cells; stones, star points, the last-move mark, the hint ring and the manual-paint brush
all go through the display side, and a click is inverted before it becomes a `make_move`, so the server,
the transcript and the exported position keep real coordinates. Two bugs came from writing that mapping
as prose instead of checking it:

- `logicalOf` first did `r = 14 - c; c = r;` — the second half read the value the first half had just
  overwritten, so three of the eight views were not inverses at all. Extracting the two methods out of
  `app.js` and round-tripping all 225 cells × 8 views found it immediately (840 failures), and the fix is
  a destructuring swap.
- The edge labels assumed the top edge names columns. At 90° the top edge runs along *rows*, so every
  label on it collapsed to "A". `columnLabel` / `rowLabel` now ask the transform which component is
  constant along the line and label with the other one; the check is that the two labels read at a
  stone's position still compose to that stone's real name (`H8` reads `H8` in all eight views).

**The 摆棋 drawer is a layer, not a fifth region.** Reserving bottom-band height for it looked like the
honest answer and was not: at a 785 px-tall window the board dropped from 471 to 300 px and the drawer
still overflowed. It is now a fixed card anchored 6 px under the button row, sized from the row's
measured bottom, scrolling inside itself — so opening it leaves the board size, the four regions and the
dividing lines untouched, and the controls in the bar stay reachable.

**One message surface.** `#thinking-info` is gone; the AI's verdict (`ai_move` score and time) and the
hint result now go to the status line, which carries the severity class (`ok` green, `warn` amber,
`error` red, `idle` grey), and `alert()` on a server `error` became a red status line — a modal that
blocks the page has no place in a layout where the board is the page. The eval curve sizes itself from
its own client box, so it follows the left band instead of a hard-coded 260 px.

**Verified in the browser** (server on :8080, 813×785 window): region rects do not overlap the board and
the SVG has exactly four corner-to-corner lines; live play through a rotate and a rotate+mirror landed on
H8 / C11 / M12, i.e. the logical cells the clicks addressed under those views, and the move list names
match; 载入本局分析 → 下一手 → click record #3 → 退出回放 → 悔棋 all behave (3 stones in the replay view
while the live board holds 6); the 摆棋 drawer recognises the 225-character flat run, echoes
黑=B 白=W 空=., keeps 载入 disabled until confirmed, then loads 3 黑 2 白 with the green status line; a
mixed-alphabet grid is refused in red inside the drawer; the 60 s idle disconnect now surfaces as
"连接断开，请刷新页面重连" in red. Two screenshots captured the base layout and the rotated+mirrored
board before the in-app browser surface went hidden; the drawer geometry is verified by measured rects
rather than a third screenshot.

**Known gaps.** Below roughly a 900 px-tall window the drawer scrolls internally rather than showing all
five controls at once. There is still no client-side reconnect for the server's 60 s idle socket timeout
(carried over from round 57); with the new always-visible status bar it is at least now clearly reported.

## 连接寿命缺陷 — bug round 61 (`master`)

**Reported as:** "才下了几个子，显示连接断开". Rounds 57, 58 and 60 all recorded this as a *60 s idle
socket timeout* and deliberately left it alone. That diagnosis was wrong, and the wrong label is exactly
why it survived three rounds: an idle timeout sounds like intended behaviour, a lifetime cap is a bug.

**The connection had a fixed 60-second life from the moment it opened.** `WebSocketSession::start()` set
`ws_.next_layer().expires_after(60s)` once, before the HTTP read. Beast's `tcp_stream` expiry is a single
absolute deadline that every later async operation on that stream is checked against — it is not an idle
timer and nothing re-armed it. So 60 s after the page connected, the next `async_read` (the player's next
click) aborted with "The socket was closed due to a timeout" and the session was dropped, no matter how
active the game had been. `timeout::suggested(server)` on the websocket layer only governs control
frames, and the client never sent any, so it never interfered. Measured before changing anything: a
session pinging every 10 s survived six round trips and died at **t = 60.0 s**, while the server process
stayed up (so: session dropped, not a crash).

**Fix: re-arm on traffic, and name the window.** `arm_idle()` is called at the top of both `do_read()` and
`do_write()`, so liveness in either direction slides the deadline forward; the value is now
`kIdleTimeoutSeconds = 120`. Both calls run on the one io thread that owns the stream (writes are posted
to `ws_.get_executor()` and `ioc_.run()` is single-threaded), so this adds no synchronisation. A silent
session is still reaped, which is the only thing the deadline was ever for.

**`ping` → `pong`.** A player thinking over the board sends nothing for minutes, which a 120 s idle window
would otherwise punish, so the server answers a `ping` with a `pong` and the browser sends one every 30 s.
The reply is not needed to keep the socket alive — receiving anything already re-arms it — but it gives the
client something to notice if the peer goes half-open. Unknown types used to answer
`Unknown message type`, so a keepalive needed a real branch rather than a fallthrough.

**Client reconnect.** Backoff 1, 2, 4 … capped at 15 s, with the attempt count and the wait shown in
amber; while down, `sendMessage` reports "消息未发出" instead of dropping the click silently. A successful
reconnect is a **new server session** — the game, the replay record and the 摆棋 pending flag all lived in
the old one — so `resyncAfterReconnect()` clears the local board to what the server actually has, re-sends
`set_config`, and asks for a side choice in green. Making a game outlive its socket would mean moving
session state off the connection, which is a design change and not part of this fix.

**Verified.** Active-session probe: alive through 150 s of traffic (was 60 s). Silent probe: reaped at
exactly **t = 120.0 s**, so the reaper still works. `ping` → `pong` plus "session still alive after ping"
are now two of the 30 socket-harness checks; `ctest` 6/6. In the browser: killed the server mid-game → the
page counted up to "4 秒后重连（第 3 次）" in amber → restarted it → the page recovered with no refresh,
board cleared, green "连接已恢复…请重新选边", and a fresh 执黑 + click on I7 was answered by the AI at H8
with a real score. (The first probe run had no `sleep`, so it accidentally pushed 554,038 ping/pong round
trips through the loop socket in 150 s; kept as a throughput data point, cadence fixed afterwards.)

## 会话与断线续局 — feature round 62 (`master`)

**Asked for:** "服务器 session token，且定时（如每隔10秒）将会话表的落子记录永久保存在硬盘，这也便于
随时复盘或继续下以及后续训练用" — the follow-up round 61 explicitly refused: making a game outlive its
socket moves session state off the connection, which is a design change. Two decisions were taken before
writing anything: **a token must still resume after the server restarts**, and **one file per game under
`data/sessions/<token>.json`, reusing the training `GameRecord` format**, with finished games also appended
as one line to `data/training/records.jsonl` so the trainer's existing entry point reads them unchanged.

**The table, not the socket, owns the game.** `GameServer` keeps `sessions_: token → TrackedGame{game,
attached, created/updated/detached, side_chosen, exported, saved_payload}` plus `socket_tokens_` for the
reverse map, and `games_` (socket → game) stays exactly as the eleven handlers use it. A handshake no
longer builds anything: `GameManager` is created by the first message that needs a game, or by `resume`.
`on_disconnect` became `detach_socket`: halt the search that was writing into that socket, clear the
callbacks, flush, and leave the game in the table for 30 minutes. `resume` of a hot token reattaches; of a
cold one, rebuilds. A token claimed by a second tab is taken by the newcomer.

**Startup loads the files, but not as games.** Every `data/sessions/*.json` is read into
`cold_sessions_: token → SessionSnapshot` and *no* `GameManager` is built for it — each engine carries its
own tens-of-MB transposition table, so a server that restarts with five hundred saved games must not pay
five hundred boards. A cold token is materialised only when someone resumes it, and a hot session that
waits out the reattach window is demoted back to a snapshot (its file is already current, so nothing is
lost but memory). The token therefore outlives the process, the restart, and the window.

**`base` is derived, not remembered.** A session started from a 摆棋 position has stones the move list does
not cover, and undo can walk *those* off the board too, so any separately-stored copy of "the position as
installed" would drift. `Board::move_history()` is already `base ++ played` (the loader's `place_stone`
calls are the same stack the later moves append to), so `ReplaySnapshot::base` is simply the history minus
the last `analyses_.size()` entries, taken under `thinking_mutex_`. `restore_game` places that list first
and the played moves after, which reproduces the same split on the next flush — round-trip stable, and
`test_snapshot_round_trip` asserts it through an undo.

**Nothing on the flush path reads the board off-lock.** `ReplaySnapshot` gained `status` (replacing the
`game_over` bool, which it subsumes), `move_count` and `base`; `session`'s own board is rebuilt from the
snapshot by `board_of()` rather than reading `board_`, because a *stolen* session may still have a worker
running. Dirtiness is decided by comparing the serialised payload with its timestamps zeroed against
`saved_payload`, so a session nobody has moved in is written once, not every tick; an untouched session
(no stones, no chosen side) is never written at all, which is what keeps 重开 from littering the directory.

**`halt_search()` replaces four copies of the abort-and-join sequence** (destructor, `reset`,
`set_position`, `restore_game`) and is now public, because detach and the steal path need it too. The
steal has to halt *before* rewiring callbacks: the searching thread reads `progress_callback_` while it
searches, so swapping the `std::function`s underneath it is a race, and the re-armed search on the resume
path is what picks the position back up.

**A bug the probe found by freezing the server.** `WebSocketSession::close()` was Beast's closing
handshake — `ws_.close(code, ec)` — which *blocks waiting for the peer to echo the close frame*. Called
from the steal path on the one io thread, it stopped the whole server: the probe's second socket got no
reply at all and timed out at 30 s. `close()` now shuts the socket down directly, so the outstanding read
comes back as an error and that handler releases the session like any other disconnect. A tab that is
already gone, or one that never answers, can no longer take the server with it.

**Training export stays honest.** A finished game is appended once (`exported`, seeded from the loaded
snapshot so a resumed ending is not exported a second time by a new process) and only when `base` is empty
— a record that began from a hand-set position is not replayable from an empty board, and quietly feeding
it to the trainer would be worse than not feeding it at all. Those games stay in the session file, for
review and for resuming.

**`session` is a second authoritative frame, and the client has to treat it as one.** The protocol's
"side chosen means playing" rule (choose_side's reply already said so) is applied in `send_session_state`,
otherwise a game the human opens — side taken, board still empty, nothing armed — comes back as `waiting`
and the page refuses the first click. `replay` gained `base`, which also fixes a carried-over round-58
bug: clicking 回放 on a 摆棋 game rebuilt the board from the move list alone and erased the installed
stones from the screen.

**Client.** The token lives in `localStorage`; every socket open sends `resume` for it, and the status bar
says 正在恢复对局… → 已恢复对局：N 手 · 轮到X方 / 已恢复摆好的局面：N 子… / 已恢复会话：棋盘还是空的，
请选择执子颜色 (green), or 上一局找不回来了，已开新局 (amber). `resumed:false` stores the token and applies
*nothing else* — found in the browser, where the fresh-session announcement (which arrives before
`choose_side`'s own reply) cleared the side the player had just picked. The replay that answers a resume
paints the live board rather than entering the scrub view; the 回放 button still does what it always did.
Round 60's known gap ("no client-side reconnect … the board is lost on refresh") is closed by this round.

**Who holds a side.** Sessions now have to answer "has anyone chosen a side?" (`side_chosen`), because
`GameManager::player_color_` always has a default and a saved "" is what distinguishes the two. That made
three places have to say so out loud: `set_position` clears the claim (the client does too, and a loaded
position has no side until someone takes it), `restart` clears it, and `resetGame()` on the client now
clears `playerColor` — previously a restarted page still believed it was WHITE while the server's fresh
`GameManager` was BLACK, so a click before the next 执黑/执白 placed a stone for the other side. Carrying
the old colour across 重开 was tried first and dropped: it leaves a game that reports itself as playing,
refuses every click and waits for a side choice that the UI no longer asks for.

**Verified.** `ctest` 7/7 — new `test_session` (token validation incl. path-traversal rejection, envelope
round-trip with `base`, damaged-file rejection, atomic write leaving no `.tmp`, listing and truncated
JSON), plus `test_restore_game` and `test_snapshot_round_trip` in `test_position`. `build/sessions_probe.py`
(40 checks, raw WebSocket + reading the real files): token issued on the first game message; the 10 s tick
creating the file with 4 plies, the side, the config and the engine numbers; a second socket stealing a live
token while the first is closed; detach flushing immediately (mtime moves without waiting for a tick); a
brand-new socket resuming and *continuing* to 8 plies with a continuous move list; unknown and malformed
tokens opening fresh sessions and writing nothing for untouched ones; 重开 issuing a new token while the
abandoned game stays on disk; a 摆棋 game saved with 3 base + 2 played stones and resuming at 5 stones with
`replay.base` intact; resign ending the game and appending exactly one `records.jsonl` line readable as a
game; **server killed and restarted → the same token resumes at 8 plies, plays on, and keeps its numbers**;
a `game_over` session resuming as finished without a duplicate export. `wspos.py`'s 30 position checks
still pass. In the browser: reload resumed a 4-stone game (stones, move list with `book` annotations, side
buttons locked, the session's own 800 ms / depth 2 echoed into the AI settings), a second reload resumed
6, play continued after the reload, 载入本局分析 + 跳到最后一手 + 退出 still work, a reloaded *pending*
position came back as "已恢复摆好的局面：3 子，轮到白方，请选择执子颜色" and accepted a side and a stone,
and a reloaded restarted-empty session came back as "已恢复会话：棋盘还是空的，请选择执子颜色" with the
side buttons live. Screenshot: `build/tmp/session_resume.png`.

## 网页摆棋重做 — feature round 63 (`master`)

**Asked for:** "网页摆棋优化：点击摆棋之后，可以在现有棋盘上直接手动摆棋，也可以临时清空棋盘再摆棋
（不清空落子记录）；网页右侧改为显示当前摆棋记录（自动将当前落子记录写入到当前摆棋记录，可直接点击摆棋
记录的任意步骤切换棋盘落子状态）；清空：临时清空棋盘，不清空落子记录；旋转/镜像：将棋盘旋转/镜像；导入：
出现弹窗，点击上传或拖拽导入棋盘文件，或者直接粘贴内容，自动识别棋盘；交换：出现弹窗，三个选项，可将当前
棋盘上的黑/白/空两两交换，主要用于修正自动识别的结果；确认/取消：确认后，将棋局信息发给后台，然后开始后续
流程（进行选边，选边后自动清空落子记录，按新局运行）；返回后，不清空落子记录，还原棋盘。'确认/取消'这两个
按钮任一点击后，与摆棋有关按钮消失，清空当前摆棋记录"

**One working copy, one server write.** Round 62's editor painted onto `this.board` behind a 手动摆棋
toggle, so the copy and the live game were the same array and leaving the mode without 载入 silently kept
the painted stones on screen. Setup now owns `setup = {active, board, record, view, preview,
confirmInFlight, sideLock, gameButtons}`, seeded from what is on screen at the moment 摆棋 is clicked, and
**确认 is the only message that touches the session**. That is what makes 取消 free: there is nothing to
restore, because the live board was never edited. `displayView()` answers with the copy while the mode is
open, and the click handler answers with `paintSetupCell` before any of the live-game guards — a board
being set up is not a game in progress.

**The 摆棋记录 is seeded, not rebuilt.** Entering setup copies the game's own move list into the record and
puts the stones that list does not explain (the ones a hand-set position installed) in front of it, so the
record and the board always describe the same position — which is what lets 确认 send only a board and let
选边 rebuild the record afterwards. Clicking step *N* shows the position after *N* stones; clicking the
highlighted step again drops back to the whole position. Painting ends a preview (editing onto a board you
are not looking at is worse than no preview) and edits the record in place: a cell already in it is
recoloured or spliced out, a new cell is appended.

**清空 clears the copy, not the game.** The game's 落子记录 is what 取消 goes back to and what 确认 keeps
until a side is chosen, so it is untouched; the 摆棋记录 describes the copy's board and goes with the
stones, otherwise clicking a step would repaint exactly what was just cleared.

**导入 is a modal whose payload is the textarea.** A picked or dropped file is read by `FileReader` into
the box, and the box's contents are sent verbatim as `check_position` — the server's recogniser stays the
only decoder of grids, long strings, bitboards and move lists. Only a grid comes back with an `encoding`,
so only a grid has symbols to confirm; the checkbox gates 导入, which writes the recognised board into the
copy. A grid carries no order, so the record is rebuilt row-major: the position is the same either way, and
the record's job is stepping through stones, not recovering history.

**交换 has two behaviours because only one of them is safe.** 黑↔白 recolours the record and keeps its
order — every occupied cell stays occupied, so the move order it was seeded from still reads correctly.
A swap involving 空 changes *which cells are occupied at all*, so the record has to be rebuilt from the
board. The modal stays open after a swap because fixing a mis-read board is often two swaps.

**选边 is the commit, and the server's `move_count` is every stone on the board.** `position_set` hides the
setup buttons and clears the 摆棋记录 but keeps 落子记录 on screen — the player has not committed to the new
position yet — and re-arms the side buttons; `state_sync` (choose_side's reply, the only place that frame is
emitted) is where the record is cleared and every stone then on the board becomes `base`. That split is also
what makes 悔棋 correct from a hand-set position: the stack is `base ++ played`, so once the played stones
run out an undo eats the installed position too. `handleUndoSuccess` now divides `msg.move_count` into the
kept base prefix and the remaining plies and rebuilds the board from both, instead of popping one local move
and comparing a played-only length against a total count.

**A refused 确认 stays inside 摆棋.** The server answers `error` (illegal position, AI thinking), and the
copy is still there with 确认 re-armed and the side buttons handed back from `sideLock` — the player fixes
what was objected to and confirms again. Confirmed in the browser with a 2-black position: "黑 2 子、白 0
子：黑先手，黑白差只能是 0 或 1", record and panel intact, retry enabled.

**What disappeared**: 手动摆棋 (painting *is* the mode now), 载入 (导入 into the copy replaced it), the
drawer's inline file input and textarea (they are the modal's), and the `editMode` / `positionPreview` /
`localCounts` state that only existed to arbitrate between an edited board and a live one. The drawer lost
its textarea and gained nothing, so it is one button row plus a status line; its padding went 8→6 and the
status margin 6→4 so that row fits the 62 px left under the bottom bar at an 813×785 viewport instead of
scrolling 5 px. 旋转/镜像 are unchanged and still work during setup — they are display-only, so they rotate
what the player sees without touching the position.

**Verified.** `ctest` 7/7 (no C++ changed this round; the tree was rebuilt to confirm). Browser
end-to-end: a 4-ply game entered setup with 摆棋记录 = those 4 moves and the game buttons (including the
replay cluster) held; painting appended step 5 and clicking step 2 showed 2 stones, clicking it again
showed 5; 清空 emptied the copy while the game kept its 4 plies; the 导入 modal recognised a pasted
`B/W/.` grid (黑=B 白=W 空=., 3/2, 白方), refused 导入 until the mark was ticked, then wrote 5 stones
row-major; a dragged CSV file took the same path; 黑↔白 recoloured the record in place, 黑↔空 rebuilt it
(220 黑 + 3 白, row-major); 旋转 90° + 镜像 kept clicks landing on the true cells; 取消 restored the live
4-stone board with 落子记录 intact and the toggle's pressed state cleared; a refused confirm stayed
editable; a confirmed 1黑1白 position left the old record on screen until 执黑, after which 落子记录 was
empty, `base` held 2 stones and the next exchange played on top of them; 悔棋 from 2 base + 2 played gave
2 stones then 0 (the base eaten from its end, as the server's stack does it); 载入本局分析 + ⏭ + ✕ still
scrub a base+played game; a page reload resumed the same session with `base` 2 + 2 plies; the newest
`data/sessions/*.json` after the flush tick held `base` 4 and 2 moves with the engine numbers. Console
clean. Screenshots: `build/tmp/r63_setup.png`, `r63_import.png`, `r63_swap.png`.

## 摆棋界面重排与超时改用秒 — feature round 64 (`master`)

**Asked for:** "摆棋界面上方状态栏增加醒目提示，比如'点击棋盘任意位置后依次标记为黑/白/空'；摆棋时的按钮
位置调整，既然上面一排按钮都被禁用了，那么直接把摆棋相关的按钮放到他们位置上；点交换按钮后的弹窗挡住无法看到
棋盘界面了，改成点击之后不再弹窗而是将这三个按钮放到原来在深度和超时的位置（摆棋时这两个输入框也不需要改的）；
导入棋盘界面，移除'确认标志'的选框，按钮为：识别，确认，取消，在'确认标志'提示信息的附近加上交换黑白空的三个
按钮；每个深度的超时时间都不一样，因此修改超时单位从毫秒改成为秒，且超时最小值设置为深度的2倍，最大值为深度的
4倍，比如深度为4，超时最小为8秒最多16秒，默认用最小值"

**The bar has two faces.** 摆棋 disables the side and play buttons, so the drawer that used to float over
the bottom band was covering board to host buttons that had nowhere else to go. They now have somewhere:
`group-side` + `group-play` swap out for `group-setup` (清空/导入/导出) + `group-commit` (确认/取消), and
`group-config` (深度/超时) swaps for `group-swap` (黑↔白/黑↔空/白↔空) — the inputs have nothing to set
while a position is being painted either. `setSetupBar(on)` flips the six spans and the 摆棋/旋转/镜像
group never moves, so the board is no more covered during setup than during play. The drawer, its
`position: fixed` anchoring in `layout()`, and `.panel` are gone; `#position-status` became an ordinary
second line of `#region-bottom`, and the only overlay left is 导入, which needs the space (paste box plus
file drop). Only the left-hand replay cluster still has to be disabled by hand, and `setup.gameButtons`
shrank to exactly that list.

**A latent CSS bug the new layout exposed.** `[hidden] { display: none }` lives in the UA sheet, so any
author rule that sets `display` outranks it regardless of specificity — and `.group`, `.confirm-row` and
`.modal-card`'s siblings all do. The round-63 交换 modal happened to work because `.modal` carries its own
`[hidden]` rule, written for exactly this reason, but `#position-confirm-row` (`display: flex`) never
actually hid: a bitboard or move-list read would have shown an empty 确认标志 line. Fixed once, globally,
with `[hidden] { display: none !important }`, which is also what the screenshot caught on the first try at
this round's layout — both faces of the bar on screen at once.

**The swap trio has two targets because it fixes two different things.** The bar's edits the 摆棋 copy
(record included: 黑↔白 recolours in place, anything involving 空 rebuilds row-major). The modal's edits
the *preview*, which is where a mis-read file is still cheap: the recognised marks are swapped alongside
the board, so the line under them keeps reading truthfully (`黑=W 白=B 空=.` after 黑↔白), and the counts
and side to move are recomputed from the swapped matrix. A form that came in as a bitboard or a move list
has no marks to be wrong about, so the modal's trio is disabled for it. With the trio in the modal there is
nothing left for the checkbox to gate, and 确认 simply applies what is shown.

**Timeouts are in seconds and belong to their depth.** The box is bounded by 2× to 4× the depth and a
depth change pulls it to the floor of that range (`syncTimeoutBounds('min' | 'clamp' | 'bounds')`); an
edited value is clamped back inside, and a garbage one falls to the floor rather than sending nothing.
The depth itself is pinned to 2–20 before the bounds are derived, because it is read from a number input a
player can type into. `set_config` and `request_hint` convert back to `timeout_ms` on the way out.

**Which meant raising the server's cap.** `clamp_timeout_ms` topped out at 30 s, so depth 8's stated
maximum (32 s) and anything past depth 15 would have been silently truncated — a UI that promises a range
it cannot get is worse than a smaller range. The cap is now 120 s, which still bounds the wall-clock, the
resign-probe node budget and the worker's wait the original comment was guarding. A resumed session keeps
the deadline it was saved with (the resume path uses `'bounds'`, refreshing only the range) rather than
being rewritten to the new floor on reconnect.

**Verified.** `ctest` 7/7. Browser: both bar faces measure 64 px inside the 149 px bottom band with no
overflow and no drawer; the amber setup hint reads 点击棋盘任意位置后依次标记为 黑 / 白 / 空; the import
modal's buttons are 识别/确认/取消, a `X/O/.` grid came back as `识别到的标志：黑=X 白=O 空=.`, its 黑↔白
flipped both the board and that line, and 确认 wrote the recoloured four stones into the copy; the bar's
黑↔白 recoloured the copy's record in place; 确认 installed the position, flipped the bar back and left
落子记录 for 选边 to clear; a 2-black position was refused with the counts in the error and 确认 re-armed;
depth 4/8/20/50 produced 8..16 / 16..32 / 40..80 / 40..80 with the depth pinned to 20, and the frames on
the wire read `timeout_ms: 8000 / 16000 / 40000`; 求招, a move, and an undo that ate both plies and kept
`base` 4 all still work. Screenshots: `build/tmp/r64_setup_bar.png`, `r64_import.png`, `r64_bar.png`.

## 轮次71 十三项缺陷批修 — bugfix round 71 (`phase5-bugfix`)

**Asked for:** 每轮严格基于需求文档分析所有性能缺陷与逻辑漏洞，交修改会话（fix-code）定向修复，再由测试会话在 WSL
编写用例回归；全绿才提交推送。本轮由四条审计线（服务端/引擎/前端摆棋/训练）产出 20 个候选，主会话逐条核验后
留 13 项下发，其余判为误报或既有设计。

**Search.** `VCTSearch::generate_defenses` only consulted the Chebyshev≤4 proximity blocks when the
defender had no counter-threat anywhere on the board, so a single distant reply excluded the real
answering square and `vct_defend/vct_attack` minned over an incomplete defense set — fabricated
`100000+depth` wins that `search_engine` commits without AlphaBeta. The proximity candidates now merge
unconditionally (deduplicated, additions only, same monotone-safe argument as the existing note).
Resignation had the mirror problem of a different kind: the VCF loss probe is gated on
`probe_budget_ms > 500`, but AlphaBeta almost always consumes the whole time limit, so in real games the
probe never unboxed; an AlphaBeta score that is itself a proven mate loss (≤ -100000, side-to-move
perspective) now resigns directly, probe kept as the extra path. And `remaining_time * 200` stayed int
on the AlphaBeta node budget while the trainer's `--timeout-ms` has no upper bound — 1.5e9 wrapped
negative, tripped the limit on the first node and silently fell back to `root_moves[0]`; computed in
64-bit and clamped now.

**Server.** A stone played without `choose_side` starts the AI for real, yet `side_chosen` stayed false,
so the c154add resume guard refused to re-arm after a reconnect or takeover and the board sat on the
AI's turn forever — an accepted `make_move` now claims the side, and **r69's F3-A expectation is
superseded**: `make_move → disconnect → resume` must re-arm (T3a probes `thinking_progress` within
15 s). Settlement raced the 10 s tick: ending a game and confirming 摆棋 inside the window wiped
`winner` before any flush, losing the game from both the session file and `records.jsonl` — the
GAME_OVER callback posts an immediate deduped flush, and `set_position` settles the outgoing game
before overwriting it. The flush guard keyed off `side_chosen`, letting a 0-stone choose_side→resign
game write a file and append a decisive blank line; persistence is content-decided now (base and moves
both empty → nothing is written, per 一子未落的空会话不写 — three legacy `sessions_probe` assertions
that expected such files are obsolete by design). `choose_side "red"` used to reset a finished board
before answering the error; validation happens before any state change now.

**Trainer / position / frontend.** `--accept-score 0.5` slipped through a `< 0.5` rejection although the
README pins the threshold strictly above the draw score — `<= 0.5` now, help text aligned. INVALID
self-play games were excluded from samples but still appended to `--records` and counted in
`games_played`; they are dropped before the push. Bitboard string words went through `std::stoull`,
which wraps `"-1"` to 2^64-1 while the numeric path rejects negatives — strings reject `-` too.
In `web/app.js`: `sendMessage` returns whether the frame left the socket, and a confirm whose message
never went out rolls `confirmInFlight` back instead of disabling 确认 forever; 载入本局分析 is disabled
while a hand-set position is unclaimed (its replay is `moves:[]` and would wipe 落子记录 before 选边,
which the README reserves for the side claim); undo exits the replay view unconditionally, so no stale
「回放 · 第 N 手」 pointer survives a popped ply.

**Verified.** Test session (WSL): three new cases in `tests/test_search.cpp`
(`test_vct_far_counter_does_not_fabricate_win`, `test_search_engine_resigns_when_alphabeta_proves_mate`,
`test_alphabeta_huge_timeout_does_not_overflow_node_budget`); differential harness against
`git show HEAD:` — T2 and T7 discriminate strongly (HEAD plays on at 300 ms / searches 0 nodes at
1.5e9), T1 came out verdict-inert across ~1600 fuzz positions so it is recorded DISCRIMINATING=weak.
Socket probes 20/20 (T3a/b/c, T4 settle-within-1s and no double export, T5, T12, T13), `gomoku_train`
accept-score four gates, `wspos.py` 30/30, adapted `sessions_probe` green except the three obsolete
expectations above, `ctest` 7/7 with only the pre-existing `server_reason_of` warning. One caveat: the
T2 case budgets 300 ms wall-clock and aborted once while a CPU-saturating fuzzer ran beside it — green
on an idle machine; keep it in mind if it ever flakes under load.

## 轮次72 会话导出与落盘健壮性 — bugfix round 72 (phase5-bugfix)

**Found.** Round 71's guards closed the individual holes but left the bookkeeping around them inverted in
five places. `exported` was a one-way stamp: once a token's game appended a line to `records.jsonl`, any
later game on that same token — after 悔棋 back to the middle of the board, or after 摆棋 replaced it —
was never exported again, silently starving the training stream. The same 悔棋 path showed the other face
of round 71's 一子未落的空会话不写 rule: `flush_entry` skipped the write but left the previous game's file
on disk, so a restart resurrected a board the player had just erased. `saved_payload` was committed
*before* `save_session` ran, so a failed write was remembered as a success and the next tick's content
comparison skipped the retry forever. On the trainer side `games_played_` still counted the discarded
INVALID verifications that round 71 had removed from the data, inflating the checkpoint's game tally, and
both `save_checkpoint` and `CoefficientVector::save` wrote with `ofstream(trunc)` in place — one kill or
power loss mid-write and 断点续训 could no longer parse its own checkpoint.

**Fixed.**  Undo that pops a stone and `set_position` both clear `exported`; the resume path in
`open_session` keeps deriving it from the restored record. The empty-board branch of `flush_entry` now
deletes the residue via `delete_session` and only commits the dedup stamp when that delete succeeds, so a
failure retries on the next tick; the write branch commits `saved_payload` after `save_session` returns
true. `games_played_` increments only for `reason != INVALID`, matching the data side. Both trainer writes
mirror `save_session`: full payload to `<path>.tmp`, then `std::filesystem::rename`, with the temp removed
if the rename fails — the previous file survives any interruption intact.

**Verified.** Test session added `tests/test_server.cpp` (a white-box target that un-privates
`GameServer` for one translation unit and calls the handlers with no io thread):
`test_undo_reopens_export` and `test_set_position_reopens_export` assert `records.jsonl` reaches 2 lines
after a second decisive game; `test_emptied_board_deletes_residue` blocks the sessions directory with a
regular file to prove the stamp holds and the retry deletes; `test_failed_flush_retries_and_keeps_dedup`
does the same for the write branch and re-checks round 71's no-rewrite-on-identical-content.
`test_session` gained `test_delete_session_is_idempotent`, `test_training` gained the count and the two
atomic-write cases. `ctest` 8/8 on an assert-live build; the R3 INVALID half is NOT-COVERED because a
healthy engine cannot produce one. One process note: a `RelWithDebInfo` reconfigure had quietly defined
`NDEBUG` and made an earlier "green" meaningless — the build directory is back to plain `cmake ..`.


## 轮次73 活三判定与持久化重试 — bugfix round 73 (phase5-bugfix)

**Found.** Four audit domains surfaced twenty candidates; seven were refuted or deferred and fifteen
were dispatched. The headliner is a rule bug, not a plumbing one: `match_contiguous_run` demanded
`left_open && right_open` before calling a contiguous three live. Both adjacent cells are already empty
in that branch, so a single open cell one step further out is enough — playing that side yields a four
with both ends free. A real 活三 was being scored as 眠三, which under-counts 400 points in the
evaluator, hides the threat from `generate_attacks`, weakens the alphabeta static extension, and
silently misses 三三 forbidden (README:27-29). The rest were bookkeeping: `check_position` ran through
`ensure_attached` and so manufactured a fresh session for what README:517 calls a dry run, handing the
client a new token and losing the game it was about to set a position in; `flush_entry` committed
`saved_payload` before appending to `records.jsonl`, so a failed append was remembered as done and the
export was lost forever (round 72 had fixed only the file-write half); `delete_session` shared one
`error_code` across two `remove` calls, and the second — deleting a `.tmp` that usually does not exist —
reset it to success, so a real failure on `<token>.json` returned true and round 72's
"commit the stamp only when the delete worked" was bypassed. Front end: a resumed session frame during
摆棋 rewrote `this.board` behind a `displayView` that still rendered the working copy, leaving the user
looking at stones the server never had, with 确认 stuck and the replay buttons lit on a board that was
not the game's; cancelling 摆棋 dropped back into a stale replay view; undo-to-empty latched
`gameStatus='waiting'` while the server kept accepting moves, locking the client out; `chooseSide`
applied its optimistic state before checking that the frame left the socket; `requestHint` sent `null`
for a cleared timeout/depth box and silently ran 5000ms/depth 6. Trainer: the rewind explorer accepted a
branch point at `prefix == max_moves`, producing games where the engine never decided a single ply —
every move `method:"replay"`, still counted and still exported; a data round ran in full with no
`--records`/`--samples` target to write to; `game_id_base` was derived from `rounds_completed_`, which
is checkpointed *after* the data append, so a kill in between re-issued an overlapping id range;
`--fitness-games`/`--verification-games` rounded down, contradicting both the header comment and
README:191-192; and every write failure warning was gated on `log_`, so `--quiet` swallowed them.
Memory: each `SearchEngine` owns a 33MB transposition table, three concurrent sessions already break
README:611's <100MB target.

**Fixed.** `||` in the contiguous-three branch (the gapped-run AND stays — filling its hole makes a
contiguous four, so both ends genuinely must be free; README:29's `XXXX_X` rush-four is the same
argument). `check_position` joins `ping` on the no-session-needed list. `saved_payload` is committed
after the export block. `delete_session` gets two error slots and reports only the main file's failure.
`handleSession` exits 摆棋 unconditionally on a resumed frame before applying the authoritative board,
`refreshReplayControls` gained `|| this.setup.active`, and `enterSetup` clears `replayIndex`. Undo
promotes `waiting` to `playing` when a side is already chosen, mirroring `game_manager.cpp`.
`chooseSide` and `requestHint` check the send and the inputs respectively. `RetryExplorer` requires
`ply + 3 <= max_moves`; `generate_data_games` returns 0 with no output paths; `game_id_base` reads
`games_played_ + 1 + t * 1000`; pairs round up via `(games + 1) / 2`; write failures go to `std::cerr`
regardless of `--quiet`. Default TT size dropped to `1 << 18` (~8.4MB/table).

**Not defects / deliberately left.** Unbounded `cold_sessions_` growth is the design README:542-543
describes; `--resume` overriding `--seed` is reproducible-continuation semantics; the second-tab
takeover loop is specified behaviour (README:544-545) with no reliable client signal to guard on;
`match_gapped_run`'s `lo_open && hi_open` is correct as argued above; clicking a move in the record list
while the AI thinks is real but low-value, deferred.

**Verified.** Test session (WSL) added `test_asymmetric_live_three` (A1) and confirmed the gapped-run
cases at `tests/test_pattern.cpp:367-383` still behave, `test_transposition_default_size` (A2, RSS delta
8316 KiB for one engine), `test_check_position_skips_session_creation` and
`test_failed_export_retries_records_append` in `tests/test_server.cpp`,
`test_delete_session_reports_main_failure` (A5), the five training cases `test_retry_rewind_keeps_engine_decisions`,
`test_generate_data_games_requires_outputs`, `test_data_game_ids_unique_across_batches`,
`test_play_match_rounds_up_to_even`, `test_write_failures_report_to_stderr`, and a new Node harness
`tests/web_frontend_check.js` covering A6-A10 (5/5, with the A6 control confirmed to FAIL once the fix is
removed). A13's original assertion was wrong on its own terms — the eight-fold symmetric augmentation
repeats `move_index` inside a game by design — so it asserts `game_id` non-reuse with a real checkpoint
round-trip instead of weakening the fix. Main session re-ran the whole gate independently: plain
`cmake ..` (asserts live, `CMAKE_BUILD_TYPE` empty), `make -j4` clean apart from the known
`server_reason_of` warning, `ctest` 8/8, `node tests/web_frontend_check.js` 5/5.

## 轮次74 参数防线、token 身份与规则常量 — bugfix round 74 (phase5-bugfix)

**Found.** Four audit domains (search/engine, pattern/shape, server/session, training/frontend) plus a
main-session pass over the layers no agent held — `main.cpp`, `CMakeLists.txt`, `position_setup`,
`position_json`, `transforms` — produced thirty-odd candidates. Ten survived verification and were
dispatched in two batches. Two of them are not code bugs but missing defences: `gomoku_server` had no
`else` on its option chain, so `--prts 9090` or a bare `--params` started a server on 8080 with built-in
coefficients and exit 0, silently contradicting the rule `main.cpp:49-51` states for `--params`
(README:132-161 document these as the operator interface); and every one of the 984 `assert()`s in the
eight suites compiles away under `-DCMAKE_BUILD_TYPE=Release/RelWithDebInfo` while `ctest` still prints
8/8 green — the exact trap that already burned round 72, until then guarded only by the verbal
"plain `cmake ..`" rule. The rest: `open_session` minted a fresh token even when rebuilding from a
snapshot, so a cold resume broke README:543's "token 依旧有效" — the old URL landed on a blank game while
the same board occupied two files on disk; the optimisers rebuild candidates through
`CoefficientVector::from_vector`, which only writes the tunable fields, so `five` and
`forbidden_penalty` — rule constants per README:583-584 — came back as compile defaults, making the
verification match pit two different rule sets against each other and rewriting the parameter file on
promotion; `OpeningBook::query` keyed itself on landing squares and only checked the parity of
`history.size()`, so a hand-set position whose base stones were placed out of alternation
(white@(7,7), black@(7,8)) matched a black-first book line and returned 花月's third move as `method:"book"`,
skipping VCF/VCT/AlphaBeta entirely (README:39); `handle_resign` settled a game nobody had started,
awarding the win over zero moves against README:283/321; `require_int` accepted 0 and negatives, so
`--verification-games 0` trained forever with nothing promoted; offline `restart()` cleared the local
board the server never heard about; a second 求招 click while the first was in flight produced a
spurious "No hint available"; and `ZobristHash::init` was a check-then-set on a `static bool`, so the
concurrent first `Board` of each training thread could refill the table while another thread read it.

**Fixed.** `else` on the option chain rejects unknown/valueless tokens; a `foreach` over the eight test
targets appends `-UNDEBUG` (`/UNDEBUG` for MSVC) after the build-type flags, so asserts no longer depend
on `CMAKE_BUILD_TYPE` while `gomoku_server`/`gomoku_train` stay untouched. `open_session` keeps
`from->token` for any session rebuilt from disk and mints a new one only for a genuinely fresh session;
the trainer copies `five`/`forbidden_penalty` from the champion into the candidate before
`play_match`; the book now rejects any history whose per-stone colours disagree with BLACK-WHITE-BLACK;
`handle_resign` requires `side_chosen` or at least one stone and otherwise answers "Game not started";
`require_int` gained a lower bound (default 1, `--rounds` 0, `--timeout-ms`/`--max-moves` unbounded);
`restart()` only resets locally when the frame actually left the socket; 求招 arms an in-flight flag and
disables its button until the reply or a covering frame lands; `ZobristHash::init` fills the table under
`std::call_once` with the values and seed unchanged.

**Not defects / deliberately left.** `match_contiguous_run`'s two-stone branch keeps `&&`: a 活二 needs
two free cells on both sides to ever become a 活三, and README:29 only constrains the three. The gapped
four not proving its hole makes *exactly* five is unreachable as a wrong verdict — a fill that would make
six is itself a 长连 at that point, and six still wins for white. `recognize_combined`'s OR-ed
`four_four`/`three_three` fields feed nothing (`is_forbidden` derives its answer from black's own pattern
alone). `set_position` leaving `player_color_`/`ai_color_` alone only bites if the client allowed a click
on a white-to-move pending board without choosing a side, which it does not. The loss-proof
`probe_budget_ms > 500` gate is the documented budget reservation with resignation's live path being the
mate score (README:40 holds), and the un-aged quiescence mate is a play-strength enhancement, not a rule
defect. Carried over from round 73: unbounded `cold_sessions_` growth, the second-tab takeover,
`--resume` overriding `--seed`, clicking the record list mid-search, and the per-session `1<<18` tables.

**Verified.** Test session (WSL) added `test_opening_book_rejects_swapped_colours` (C3, both branches:
normal alternating order still answers from the book and the swapped-colour board now returns false —
the control that fails once the per-ply loop is removed), `test_trainer_keeps_rule_constants_through_promotion`
(C2, driven through the *promotion* path with a deviation-forcing fitness, so `five`/`forbidden_penalty`
are asserted on candidate, champion and the emitted file), the C8 Zobrist `call_once` case in
`tests/test_board.cpp`, and three `tests/web_frontend_check.js` cases for C6/C7 (restart refused while the
socket is down, hint sent once and released by `hint_result`, resumed session releases an in-flight hint).
Two CLI cases were run as end-to-end scripts: B1's five bad-option forms plus the `--port 0/70000` range
guard and a healthy `--port 9099 --no-book --no-sessions` boot; C1's cold round-trip — kill the server,
restart on the same `--sessions` dir, resume with the original token, and assert `token == T`,
`resumed == true`, unchanged `move_count` and still exactly one file in the directory; C4 asserted
`Game not started` for a bare resign and a normal `game_over (reason: resign)` after `choose_side`.
B2 was proved at build-system *and* compiler level: in a separate Release configure dir every one of the
eight test targets shows `-DNDEBUG` followed by `-UNDEBUG` in `flags.make`, and a minimal program aborts
under `-DNDEBUG -UNDEBUG` while surviving under bare `-DNDEBUG`. The external dir was deleted afterwards.
Three rows the brief listed as expected-FAIL are expectation errors in the brief, not code defects:
`--optimizer` is really `--method`, `--accept-score -10` is refused by the pre-existing 0.5-1.0 guard
(`main_trainer.cpp:219`), and `--rounds 0` / `--timeout-ms 0` / `--max-moves 0` are refused by the
downstream semantic guards at `main_trainer.cpp:213` and `:224` — which is the README-consistent
behaviour, so `require_int` deliberately leaves them to those guards. Main session re-ran the gate
independently: plain `cmake ..` (asserts live), `make -j4` clean apart from the known `server_reason_of`
warning, `ctest` 8/8, `node tests/web_frontend_check.js` 9/9.

## 轮次75 整数截断、会话身份与不可悔的 base — bugfix round 75 (phase5-bugfix)

**Found.** Three read-only audit domains (snapshot/record integrity, protocol/input boundary, frontend
display layer) plus the orchestrator's own pass produced twenty-plus candidates; the headliner class is
`get<int>()` truncation. `json_int.h:10-14` already documents that a JSON integer wider than 32 bits
"reads back as 3", and `src/training/params.cpp` uses `read_json_int` for exactly that reason - yet the
server's own `get_int` (server.cpp:21-24), `session_from_json`'s `version`/`timeout_ms`/`max_depth`/base
row-col, and `position_json`'s coordinate reader all still cast. So `{"row":4294967304}` became row 8,
passed the 0-14 check at server.cpp:822 and put a stone on a square the client never named; a session
file with `version: 4294967297` was accepted as v1; a hand-edited base stone silently moved a cell before
being resumed as the player's own position. Second class: the write-then-rename sites
(`trainer.cpp:396`, `params.cpp:210`) and the four plain writers (`data_collector.cpp:241`,
`elo.cpp:176`, `game_record.cpp:152` and `:177`) checked `file.good()` on an unflushed stream, so a
buffered failure that only surfaces at close was reported as success - the truncated `.tmp` then renamed
over the good checkpoint, and a failed `append_to_jsonl` let `flush_entry` mark the game `exported`,
losing the record forever. Third class: `open_session`'s restore-failure branch promised in its own
comment that the unreadable file "stays on disk for a human", but left `saved_payload` empty, so the next
`flush_entry` took the no-stones branch and deleted exactly the file it meant to preserve; `undo` popped
base stones whenever `board_.move_count() > 0` even with `analyses_` empty, mutilating a hand-set position
after `choose_side`; a cold rebuild overwrote `created_ms` with the resume instant; and
`handle_set_config`/`handle_request_hint` fed missing keys through hardcoded fallbacks, so asking to
change only `max_depth` silently reset the session's timeout to 5000. Front end: an undo aborts an
in-flight hint whose callback is then suppressed, and nothing on the undo path cleared `hintInFlight`,
locking 求招 off for the rest of the game; a live stone arriving during a replay view left the board
looking current while clicks were swallowed by `app.js:845` with no message.

**Refuted before dispatch.** The audits' highest-severity frontend row - "a resumed 摆棋 game greys out
both side buttons and cannot be started" - is a polarity misread: `setSideButtons(blackDisabled,
whiteDisabled)` (`app.js:1346-1349`) takes disable flags, so `gameStatus==='playing' && chosen` yields
`false,false` = enabled. `valid_token`'s 1-64 length is deliberate back-compat (README:476 states the
generator's 16), `five`'s upper bound already guards the mate threshold, and the io-thread flush cost,
`validate_position`'s per-stone rebuild, opening-book line bounds and the search's per-node allocations
are recorded as performance items for a later round, not dispatched here.

**Fixed.** E1-E3: all three integer readers now go through `read_json_int` and refuse an out-of-range
value instead of reinterpreting it (missing/non-integer keeps the existing fallback semantics). E4: the
restore-failure branch stamps `saved_payload` with what this flush would otherwise have written, so the
dedup returns before the delete branch - and once the player actually moves, the payload differs again and
the session is saved normally. E5: `analyses_.empty()` returns false and `undo_count` is clamped to
`analyses_.size()`, so board stones and analysis entries are always given back in pairs. E6: `flush()`
before `good()` at all six sites. E7: `created_ms` is inherited on a successful rebuild only. F1: a
non-object `coefficients` is an error, so `--params` cannot claim success while running defaults. F2: a
new `get_int_or` applies only keys the message actually carries, taking the rest from the game's current
`timeout_ms()`/`max_depth()`. F3-F4: undo releases the hint control, and `handleMoveMade`/`handleAIMove`
call the existing `exitReplay()` while a swallowed replay click now warns. F5: `load_session` compares the
token inside the file with the token the filename claims and refuses the mismatch, so a renamed file can no
longer make a resume serve a different game than its URL promised - and the refusal leaves the user's file on
disk. G1: the position document's `version` is read through `read_json_int`, so `4294967297` is a refusal
instead of a truncation into `1`. H1: the opening-book reader refuses a line whose coordinate or weight does
not fit an int rather than building a shifted entry from the truncated pair, and only that line - the other
lines of the same file still load.

**Verified.** Batch E ran first in the WSL test session: `test_rejects_wide_integer_fields` (E2), the
rewritten `test_snapshot_round_trip` (E5 - the four assertions that had recorded "undo eats a base stone" as
expected now assert the refusal, plus a base+1-move control group), `test_buffered_write_failures_are_caught`
with a `/dev/full` control probe that shows `good()` is true before the flush and false after (E6), a
`--records /dev/full` server probe that shows the failed export is reported and retried on later ticks while
`exported` is never set (E6), and the E1/E3/E4/E7 WS probes: a wide `row` answers `Invalid move position` and
leaves `(8,7)` empty instead of truncating into a stone, a wide restore failure keeps the session file
byte-identical across three flush ticks and then overwrites it normally once a stone is played, and two cold
rebuilds keep the original `created_ms` while `updated_ms` advances. Batch F/G/H then ran in the same session:
`test_params_rejects_non_object_coefficients` (F1, with the CLI proof that both `gomoku_server --params` and
`gomoku_train --params` exit 1 and stop claiming a loaded parameter set, and the partial-key control group
still loading), the F2 probe (a `set_config` that only carries `max_depth` keeps the session's 60000 ms
timeout, and a keyless `request_hint` runs the session budget instead of resetting to 5000),
`test_load_refuses_token_mismatch` (F5, file left on disk), `test_opening_book_rejects_wide_numbers` (H1, the
bad line refused while the same file's legal line still loads and the built-in book stays at 45 lines / 109
prefix keys), the G1 probe (`version 4294967297` refused where the previous round's run had measured it being
accepted as v1, `version 2` still refused, a missing key still accepted), and the F3/F4 harness cases. For the
negative proof the test session copied `web/app.js` and the harness into `/tmp` and deleted the fix lines
there, because the round-76 batch was editing `web/app.js` concurrently: 11/11 with the lines, `FAIL F3 ...`
and two distinct `FAIL F4 ...` without them, and `web/app.js`'s md5 unchanged throughout. Main session
re-ran the gate over the tree that also holds the round-76 J batch: plain `cmake ..` (asserts live), clean
build with only the known `server_reason_of` warning, `ctest` 8/8, `node tests/web_frontend_check.js` 11/11.

One brief-writing error is worth recording: the F3 instruction told the test session to delete a line in
`web/app.js` to prove the case fails, which contradicts its own "never touch web/" constraint and would have
clobbered the concurrent batch. It was retracted mid-run and the `/tmp` copy method above substituted; the
test session had already confined every negative proof to `/tmp`, so no repo file was ever written by it.

## 轮次76 搜索时序、回放视图与不再沉默的求招 — bugfix round 76 (phase5-bugfix)

**Found.** Two audit domains (search timing/ordering behaviour, frontend interaction plus the hint protocol
that drives it) produced nine dispatched defects. Search side: `SearchEngine::search` already owned one
absolute `search_deadline` and handed it to the VCF and VCT phases, but the AlphaBeta phase was given only
a slice (`remaining_time`, floored at 100 ms) and its `is_timeout()` measured that slice from its own
`start_time_` - so a move whose tactics phases had already spent the whole budget still bought another
slice, and `timeout_ms` was a suggestion rather than a wall (`alphabeta.cpp:13`, `search_engine.cpp:154`).
Second: `sort_moves` pinned the transposition-table move only when it had to swap it forward
(`if (it != moves.begin() && it != moves.end())`), so in the common case - the TT move already sitting first
because the previous iteration just promoted it - `tt_swapped` stayed false and the heuristic re-ordering
started at index 0 and was free to push it back; because the clock-out path answers with `root_moves[0]`,
that also changed which stone the AI hands over when time runs short. Front end: `handleSession` rebuilt
`baseStones`, `analysis` and `replayIndex` from the server's frame but never cleared `moveHistory`, so a
resumed page kept the previous game's ply list under the new board; `resumedStatusText()` then reported
that local list as the resume line even though the same frame carries `move_count`; 悔棋/求招 were
enabled and disabled from four different places with three different rules, so an ended game could show
armed buttons and a finished search could leave them greyed; the board click handler turned raw pixels into
a cell with `Math.round((y - pad) / cell)` and only range-checked the result, so a click inside the
coordinate-label ring - pixels outside the grid that are not a cell at all - rounded to row/column 0 (or 14)
and installed a real stone where the player had pointed at nothing; the replay view leaked four ways
(auto-play at the last ply called `stopReplayAuto()`, which leaves `replayIndex` on the final move, so the
board looked live while every click was dropped; a dropped replay click said nothing; a live `move_made` or
`ai_move` arriving during a replay never left it; `game_over` composed its ending text over a historical
view); and `hint_result` was painted unconditionally, so a hint searched on one position and answered after
the position changed marked a square that was occupied by then, or one that no longer existed. Server side:
a hint interrupted by 终止 answered nothing at all, because the worker's delivery predicate was
`!stop_flag_.load()` while `stop_thinking()` only raises `stop_flag_` - the client's `hintInFlight` stayed
true and 求招 was dead until a reload (round 75 had only freed it on the undo path).

**Refuted before dispatch.** The audits' two highest-severity search rows were wrong on the code: "the
shared deadline can strand the AI with no move" - `alphabeta.cpp:360-362` still falls back to
`root_moves[0]`, so the worst case is a shallow move, not silence (kept as a test case instead); and "the
hint thread and the AI thread race on the engine transposition table" - `start_ai_thinking` calls
`stop_thinking()` and then joins *both* `search_thread_` and `hint_thread_` (`game_manager.cpp:319-326`), so
the two searches never overlap. Also refuted: a `ai_move` frame before the player has chosen a side (the
server arms the AI only from `choose_side`/`set_position`), unclamped client `timeout_ms`/`max_depth`
(`clamp_timeout_ms`/`clamp_max_depth` at `game_manager.cpp:288-289`, re-clamped in `handle_request_hint`),
the io thread blocking while a search is joined (measured and accepted in round 42), a race on the websocket
write (`WebSocketSession::send` posts onto the stream executor, `server.cpp:318-336`), and duplicate
coordinate keys in the opening book.

**Fixed.** J1: `AlphaBetaSearch::set_deadline` plus a `has_deadline_` branch in `is_timeout()`, wired to the
same `search_deadline` the tactics phases use - the slice stays as the no-deadline fallback, and the floor
and the `remaining_time * 200` node budget are untouched. J2: the pin now tests only `it != moves.end()` and
swaps inside, so an already-first TT move keeps `tt_swapped` true and stays at index 0. J3-J8 in
`web/app.js`: `handleSession` clears `moveHistory`; the resume line takes the server's `move_count` (with a
`Number.isInteger` guard, so a missing field falls back to the local list); the undo/hint enable rule is now
one expression (`playerColor === null || gameStatus === 'game_over'`) applied by `handleSession`,
`handleThinkingProgress`, `handle_ai_move`/stop, `handleUndoSuccess` and `handleHintResult`; clicks outside
the grid rectangle (`x < pad`, `x > size - pad`, same for `y`) return before anything else, in play mode and
in 摆棋 alike; auto-play's last ply calls `exitReplay()`, a replay click now warns 正在回放历史，先退出回放才能落子,
`handleMoveMade`/`handleAIMove`/`handleGameOver` leave the replay view first, and `hint_result` is dropped
as 建议已失效 when the ply count no longer matches the one recorded by `requestHint`. K1: the hint worker
delivers on `!discard_pending_.load()` instead of `!stop_flag_.load()`, so a user stop still answers
(`server.cpp:1463-1475` turns a valid move into `hint_result` and an invalid one into
`error: No hint available`, which the client already releases the button on) while reset / resign / the
destructor - the paths that set `discard_pending_` through `halt_search()` - stay silent, and
`hint_active_.store(false)` still runs on every branch.

**Verified.** The test session ran the J1-J8 + K1 regression in WSL against the plain `cmake ..` build and
the orchestrator re-ran the whole gate over the same tree: `ctest` 8/8 (test_board, test_pattern,
test_evaluator, test_search, test_position, test_session, test_server, test_training) and
`node tests/web_frontend_check.js` **17/17**, which now includes `J3 resumed session drops the previous move
record`, `J4 resumed status reports the server ply count`, `J5 undo/hint buttons follow the chosen-side rule
on every path`, `J6 clicks on the label ring never reach the board`, `J7 replay view exits on end-of-play and
live moves`, `J8 stale hint replies are refused` and the three `C7` cases that pin K1 (the hint is sent once
and released by `hint_result`, a resumed session releases an in-flight hint, a non-resumed session recovers
through `chooseSide`). The J1 deadline and the J2 pin are covered inside `test_search`, which stayed green
with its 4.97 s of search cases.

The same session then ran two read-only evidence probes for the *next* round, and both came back 属实, so
they are recorded here as round-77 candidates rather than as regressions of this batch:

- **K2 - a stale `discard_pending_` swallows 求招 forever.** On one socket and one token, a
  `set_position` → `choose_side` (human to move, so no `start_ai_thinking`, which is the only other site that
  clears the flag) → `request_hint{20000,8}` sequence printed **zero** frames in 15 s and still none after
  `stop_thinking`: the search ran (one `thinking_progress` at 3,133,223 nodes) and K1's
  `!discard_pending_.load()` predicate at `game_manager.cpp:308` discarded the answer without an `error`
  reply. `restart` → `reset()` reproduced it the same way, while a fresh manager on the same port answered in
  10.67 s with the identical 20000/8 request - the single-factor control the brief asked for. Rescue paths
  exist but only via other frames (`error: Cannot undo` during 摆棋, `game_restarted` + a rotated-token
  `session` after restart), so the silence itself never releases the button.
- **R1 - a gap shape whose hole fills to an overline is still scored as a BLACK four.** With black stones at
  (7,2)-(7,5) and (7,6) empty, recognising (7,7) answers `RUSH_FOUR`, and playing (7,7) then filling (7,6)
  measures `run_length == 6` - the completion point is a 长连, which README:29 says is not a four at all
  (`X_XXXX_X 不算四`). Removing (7,2) makes the same probe a true rush four (`run_length == 5`), so the
  labelling is what is wrong, not the geometry. `pattern.cpp:416-419` exempts gap shapes from the
  exactly-five completion scan on the strength of a comment claiming `match_gapped_run` already proved it,
  while `match_gapped_run` (`:257-259`) only counts stones: `stones == 4` becomes `RUSH_FOUR` with no board
  check. The consequence is not limited to the evaluation - a phantom four feeds `four_count`, so a legal
  black move can be rejected as 双四/四三.

## 轮次77 缺口四的成五点、被吞掉的求招与规则常量的下界 — bugfix round 77 (phase5-bugfix)

**Found.** Round 76's two evidence probes came back 属实 and were joined by three more read-only audits
(core rules, server/session lifecycle, training/CLI). R1: the black-four correction in
`pattern.cpp:393-437` was gated on `if (run == 4)` (`:419`), so gap shapes never got the board-level
exactly-five check - `match_gapped_run` (`:257-259`) only counts stones - and the comment at `:416-418`
asserted a proof that function never makes. K2: `request_hint()` (`game_manager.cpp:286-296`) cleared
`stop_flag_` but never `discard_pending_`, the only flag K1's delivery predicate reads, and every path that
goes through `halt_search()` (reset / set_position / restore_game / resign / destructor) leaves it raised,
so a 求招 after 摆棋 or 重开 searched to completion and was then dropped without even an `error` frame.
L1: `params.cpp:183` guarded `five` only from above and `forbidden_penalty` not at all, so a file could set
`five: 0` (making `evaluator.cpp:158-162`'s `cap = five - 1` flatten or invert the whole static
evaluation) or `forbidden_penalty: 0` (turning a 禁手 into an incentive) or `>= 100000` (making
`alphabeta.cpp:247/:357` read our own rule penalty as a proven mate loss and `search_engine.cpp:191`
resign). L2: `require_double` (`main_trainer.cpp:80-88`) only checked `used != text.size()`, and `stod`
consumes `nan`/`inf` wholly, so `--accept-score nan` passed the `(0.5, 1.0]` guard at `:219` - NaN
compares false against everything - leaving every Elo comparison false and the run silently never
promoting. L3: `--no-sessions` cleared both persistence paths inside the parse loop (`main.cpp:27-29`), so
a later `--records` re-armed half of what README:161 calls 完全关闭落盘. L5: README:115-118 enumerated the
manual-compile source set as core/search/optimization/server "外加 `src/training/params.cpp`", but
`GOMOKU_SOURCES` also carries `src/training/game_record.cpp`, which `server.cpp:157`'s `GameEndReason`
needs. L7: `stopThinking()` (`app.js:954-966`) re-enabled 求招 without releasing `hintInFlight`, the very
flag `requestHint()` short-circuits on (`:928`) - and a hint really can be running when 终止 is clickable,
because the server emits `thinking_progress` for a hint search too - so the button looked free and did
nothing; `handleUndoSuccess`'s comment meanwhile claimed an undo aborts the hint search, which
`GameManager::undo` (`:189-192`) never does.

**Refuted before dispatch.** The core audit's "the whole-board evaluation credits black's forbidden
shapes" is unreachable: every move-generation path filters them first (`alphabeta.cpp:33/:125`,
`greedy.cpp:32`, `search_engine.cpp:46/:88`, `vcf.cpp:16`, `vct.cpp:16`, `opening_book.cpp:236`,
`self_play.cpp:11`), so `evaluate_board` never sees an illegal black stone. "The default-empty
`--records`/`--samples` make README:175's step 4 silently produce nothing" is README-consistent - README:199
documents both defaults as 空 and README:195 makes `--no-data` the opt-out, and games with no destination
would only burn the round's budget. The `--resume` overriding `--seed` row is the round-73/74 refutation
re-surfaced. The crash-between-append-and-checkpoint duplicate `game_id` finding is real but its fix is an
idempotent-resume design (persist a high-water mark before appending, or dedup on load), not a point defect,
so it is recorded as deferred rather than dispatched; the same goes for the trainer's `game_id` base hole and
the redundant forbidden check from round 76.

**Fixed.** R1: the `run == 4` gate is gone, so the exactly-five scan runs for every black four - the walk
past the group stops on the first empty cell, which *is* the gap shape's hole, and `fill_run` on the board
decides whether filling it makes five; zero five-points drops the label to NONE, one demotes a LIVE_FOUR to
RUSH_FOUR, and white is untouched because a six there is a win. K2: `request_hint()` now clears
`discard_pending_` immediately after joining the previous hint thread and before spawning the new one. L1:
`five` must stay above zero, `forbidden_penalty` must stay strictly inside `(0, 100000)`; a missing key still
keeps the compile default, so `five: 100000` / `forbidden_penalty: 50000` files and the 88888/77777
pass-through round-trip both still load. L2: `require_double` rejects non-finite values, so `nan`, `inf` and
`-inf` exit 1 like any other bad number. L3: `--no-sessions` only records the flag and clears both paths
after the loop, making it order-independent. L5: README now lists both training sources. L7: `stopThinking()`
keeps 求招 disabled while `hintInFlight` is true, and the undo comment now says what actually happens - the
in-flight answer still arrives and is voided by the ply-count check.

**Verified.** The test session wrote the cases and the orchestrator re-ran the gate on the same tree: plain
`cmake .. && make -j4` with no new warnings, `ctest` **8/8**, `node tests/web_frontend_check.js` **18/18**.
New in-suite cases: `test_window_edge_gapped_four` (R1 - edge shape NONE, whole-position `four_count == 0`,
the three-stone control still RUSH_FOUR, a real live four still LIVE_FOUR, a single-five-point shape still
demoted to RUSH_FOUR, and white unchanged), `test_request_hint_clears_stale_discard` (K2 white-box), and the
L1 loader rejections. Negative proofs were all done off-repo - relinking the pre-fix `pattern.cpp`,
`params.cpp`, trainer and server in `/tmp`, or a copied harness - and each fired exactly the expected
failure (`Assertion \`edge.type == PatternType::NONE' failed` printing `RUSH_FOUR`; the HEAD loader
accepting all five broken constant files; the HEAD trainer accepting `nan`/`inf`/`-inf`; the HEAD server
creating the session dir and re-opening persistence for `--no-sessions --records`; one FAIL / 17 pass with
`|| this.hintInFlight` removed). K2 was re-checked end to end against the live binary: the 摆棋 and restart
sequences that previously printed zero frames in 15 s now answer in 11.2 s and 19.9 s. The repo diff is
confined to `tests/` (+318/-2 in four files); no `src/`, `web/`, `README.md`, `CMakeLists.txt` or `data/`
file was touched by the test session, and no `gomoku_server` process was left behind.

## 轮次78 摆棋开局的那一手、被中止推翻的认输与读文件的最后一扇门 — bugfix round 78 (phase5-bugfix)

**Found.** Eight verified defects, all read line by line by the orchestrator before dispatch. N1: a
`position` string that parses as a JSON *array* was moved into the position document as-is, so merging the
top-level `encoding` indexed an array with a string and threw a `type_error` straight out of the handler -
the client got no frame at all for a 识别/确认 click (README:517/519). N2: README:283-284 allows the player to
start a hand-set position by *playing a stone* instead of choosing a side, but `player_color_` was still the
constructor default BLACK, so a white-to-move template refused every white move with a generic
`Invalid move`. N3: the AI worker's `result.resigned` branch required `!stop_flag_`, so pressing 终止 during a
proved loss neither resigned nor played nor answered - the session was left with `ai_color_` to move and every
later `make_move` refused. M1: `CoefficientVector::from_json_string` validated only the twelve keys it
recognised, so a file of nothing but typos loaded "successfully" as pure defaults (the code's own comment at
:142-144 described the hole). M2: a verification game discarded as INVALID returned 0.5 and stayed in
`play_match`'s denominator, i.e. a candidate that lost the board got a draw credit toward the promotion bar
(README:190/193). M3: `GameRecord::from_json` defaulted a missing/non-string `winner` to a draw and
`reason_from_name` mapped any typo to INVALID, silently rewriting outcomes - README:209 permits defaults for
metadata only. M4: README:176/:201 documented `--resume` as a bare switch while `main_trainer.cpp:52` and
`:206-207` require a PATH. M5: `load_checkpoint` compared the elo player list and the version list by
*length*, and `EloSystem::add_player` aliases duplicate names, so a shifted name column kept the same length
and attributed later results to the wrong version.

**Refuted before dispatch.** "A cold resume of an unreadable session file returns `resumed:false` while
keeping the old token, so the next flush overwrites the record round-75 E4 preserved" - `open_session`
(`server.cpp:1126-1137`) stamps `saved_payload` with exactly the empty-game payload so the dedup returns
early, and README:475-477's "new token" branch is about an unknown or malformed token, not about a record
that cannot be rebuilt. Recorded; a related hole did survive and is listed under 轮次79.

**Fixed.** N1: an array text payload now becomes `{"board": <array>}`, so the document is always an object
before `encoding` is merged. N2: `make_move` claims the side (`player_color_ = current_turn()`,
`ai_color_ = opponent`) while `position_pending_` is set, before the colour guard; the existing
clear-pending/`WAITING→PLAYING`/AI-arming tail at :181-192 then does the rest, and `handle_make_move` reads
`player_color()` after the move instead of guessing it beforehand. N3: the resignation gate is now
`!discard_pending_ && status_ == PLAYING`, so interruption cannot overturn a refutation while reset / resign /
destruction stay silent. M1: zero recognised keys is an error, and an unrecognised key names the first
offender (our own `to_json_string` writes exactly the twelve, so the round trip is unaffected). M2:
`play_game` gained a `played` out-parameter, an INVALID game returns 0.0 and leaves the denominator, and a
match with no valid game returns 0.0; `games_played_` and the `fitness_override_` shortcut are unchanged.
M3: `winner` and `reason` must be present, be strings, and be names the writer can produce, with `"invalid"`
still legal as itself. M4: both README lines now say `--resume PATH`. M5: every restored player must equal
`version_name(i)`, not merely match the count.

**Verified.** The test session wrote the cases and the orchestrator re-ran the gate on the same tree: plain
`cmake .. && make -j4`, no new warnings, `ctest` **8/8**, `node tests/web_frontend_check.js` **18/18**.
New cases: `test_position_array_string_payload_answers` (N1, both `check_position` and `set_position` plus the
document-string and text-grid controls), `test_pending_position_direct_move_claims_side` (N2, including the
already-chosen-side rejection path that must not regress), `test_resign_survives_interruption` (N3),
`test_game_record_requires_outcome` (M3), `test_params_rejects_unrecognised_keys` (M1),
`test_play_game_counts_only_valid_games` (M2 - it pins the valid side, the null out-parameter, the
denominator identity and the injected-objective path, and records honestly that an INVALID game is not
reachable through `play_game`, since every move generator filters forbidden points), and M5's shifted-name
checkpoint rejection inside `test_trainer_rejects_bad_checkpoint`. Two pre-existing session expectations broke
under M3's stricter reader; the test session gave those fixtures a legal outcome instead of loosening the
assertion, so each case still exercises its own check (status, base, config bounds, wide integers). Repo diff
is confined to the eight named files plus `tests/` (+595/-24); `data/` untouched, no `gomoku_server` left
running.

## 轮次79 空候选表认输、两遍棋型扫描与读文件的最后一道缝 — bugfix round 79 (phase5-bugfix)

**Found.** Eight verified defects (A1-A8), each read line by line by the orchestrator before dispatch. A1:
`SearchEngine::search` treated an *empty candidate list* as "BLACK has nowhere legal left" and resigned
(`search_engine.cpp:48-56`), but the list is `board.active_empty()` - only cells within Chebyshev distance 2 of
a stone - while forbidden-ness is itself local, so an emptied active list says nothing about the other 200
squares; and with `--no-resign` (README:196) the branch fell *through* with a zero-length root list, which the
later phases answer from, so the caller could receive a move the root filter had already refused. A2: both
threat generators called `is_forbidden(board, ...)` and then `recognize_position(...)` on the same cell
(`vcf.cpp:13-19`, `vct.cpp:13-19`) - two full 4-direction window scans per candidate inside the hottest loop of
a VCF/VCT pass, when `is_forbidden` is exactly `recognize_position` + `is_forbidden_from_pattern`. A3:
`greedy_.search()` ran unconditionally in the prologue (`search_engine.cpp:64`) although every phase after it
returns before the one line (`:256`) that consumes its result, so most searches paid for a full candidate
ranking and threw it away. A4: `session_from_json` read `position_pending` only `if is_boolean()`, so
`"position_pending":1` loaded as *false*, and `player_color` / `winner` were copied with no value set at all
(only `status` was checked), so `"player_color":"red"` resumed a session and announced a side nobody chose. A5:
`TrainingSample::from_json` accepted a line with no `move` (keeping the default `Move()` - (0,0) BLACK, a label
from no game), a `features` array of any length (README:46 fixes the layout at 52; another length is another
model), and a missing `value` / `weight` (silently 0.0 / 1.0, a draw label attached to an unknown outcome). A6:
round-75 E4's "keep the unreadable record" stamp survived only until the next flush that *changed* the payload -
and choosing a side changes it, so `flush_entry`'s empty-board branch deleted the very file the token names, the
residue README:553 says is left for a human. A7: a forced encoding from **确认** was trusted as stated and the
coverage test ran *after* it (`position_setup.cpp:329-345`), so `black == white` read every white stone as black
and an `empty` colliding with either colour deleted a side - producing counts that then "proved" the position
legal. A8: `word_of` parsed with `std::stoull(text, &consumed, 0)`, and base 0 takes a leading zero to mean
*octal*, so the documented form "字可以是十进制整数" (README:221) was wrong for every zero-padded word -
`"010"` became 8 and moved the stone off (0,1) while the position still validated.

**Refuted before dispatch.** Three of the audit's own candidates did not survive the read: `confirmInFlight`
does not soft-lock on a `resumed:false` reply (every path that sets the flag clears it in the same handler or is
followed by a `session` frame, and 摆棋 stays reachable); `validate_position`'s per-stone board rebuild is ~ms on
a full board and runs only on **确认**, where the API has to know *which* stone is forbidden to report it; and
`flush_entry`'s per-tick re-serialisation is bounded, because `on_flush_tick` evicts detached sessions into
`cold_sessions_` (`server.cpp:1427-1428`) instead of letting `sessions_` grow. The flagship rule-layer defect -
no board-level LIVE_THREE correction, so a `±4` window that cannot see the stone which makes a four *live*
reports a false 三三 forbidden (README:28/29) - was confirmed by the test session's scratch probe and the
orchestrator's own window arithmetic, and is dispatched as 轮次80 B1 rather than patched inside this batch.

**Fixed.** A1: the empty-candidate branch now scans the whole board for non-forbidden empty cells and plays
from that set; only when that too is empty does it end the game - resignation when `resign_detection_` is on,
otherwise `method == "no_legal_move"` with an invalid `best_move`, which `ai_thinking_worker`'s
`best_move.pos.is_valid()` guard already treats as "commit nothing". A2/A3: recognition is computed once per
candidate and tested through `is_forbidden_from_pattern`; the greedy search moved to the single line that
consumes it. A4: a present `position_pending` must be boolean, and `player_color` / `winner` must be in the
writer's own set (`""`/black/white, plus draw) with the offending value named - checked against
`server.cpp:1298-1330` so nothing the server writes is ever refused. A5: `move`, `value`, `weight` and the
52-long vector are mandatory; `to_json` always writes all four, so our own files are unaffected. A6:
`TrackedGame` gained `unreadable_record` (set in `open_session`'s unreadable branch) and the empty-board branch
keeps the file and commits the stamp while it is set - the file is still replaced the moment the game has
content. A7: the three tokens must name three different things, compared *after* normalisation (so `b`/`B`
clash too), each message quoting the clashing symbol. A8: only `0x` / `0X` is hexadecimal, everything else is
decimal.

**Verified.** The test session's first two turns failed mid-way; a fresh session continued from the surviving
`tests/` diff instead of redoing it, and the orchestrator re-ran the gate on the same tree: plain
`cmake .. && make -j4`, no new warnings, `ctest` **8/8**, `node tests/web_frontend_check.js` **18/18** (this
batch touches no `web/` file, confirmed from `git status`). New cases:
`test_search_engine_no_legal_move_and_resign_switch` (A1, on the nearly-full board where the only empty cell is
an overline point, so the far scan genuinely finds nothing - both switch positions pinned),
`test_generators_keep_five_points_and_drop_forbidden` (A2: a double-four cross stays out of both generators and
an exact-five point stays in both), `test_greedy_method_and_move_unchanged` (A3: method `greedy` with exactly
the move `GreedySearch` returns, mid-game and simple), `test_envelope_rejects_damaged_flags` (A4, including the
absent-keys-keep-defaults side and a writer round trip), `test_sample_from_json_rejects_damaged` (A5),
`test_unreadable_record_file_survives` (A6: `resume` -> `choose_side` -> `detach_socket`, and the damaged file
must still be there byte for byte), `test_forced_encoding_rejects_collisions` (A7, three clashes plus the
`b`/`B` normalised clash and the two unaffected paths), `test_bitboard_words_are_decimal` (A8, pinning the
*cells* rather than the numbers, on the first and the last word). One `tests/`-only compile fix was needed: A3's
pin calls `build_midgame`, defined lower in the file, so it is forward-declared. HEAD negative proofs were run
in a `/tmp` copy by re-linking one test target against `git show HEAD:<file>` at a time (no fix line was deleted
in the repo): A1 `test_search.cpp:993 Assertion '!result.resigned' failed`, A4 `test_session.cpp:184`, A5
`test_training.cpp:1286`, A6 `test_server.cpp:888 Assertion 'exists(dir/"deadbeef.json")' failed`, A7
`test_position.cpp:753`, A8 `test_position.cpp:833 Assertion 'padded.ok()' failed` (`"010"` read as octal 8, the
white stone then colliding). A2 and A3 passed at HEAD *by design*: they are behaviour-preserving pins of the
observable set and the observable move, not fix detectors. Repo diff is 9 `src/` files plus 5 `tests/` files
(+665/-40); the only file outside the dispatch list is `src/server/server.h`, which A6 needs for the new
member. `data/`, `web/`, `README.md` and `CMakeLists.txt` untouched, no `gomoku_server` left running.

## 轮次85 跳键拒绝收窄、分层进度帧与会话接管告知 — bugfix round 85 (phase5-bugfix)

**Found.** Eleven verified defects, dispatched individually after the orchestrator read each site. F1 (round
84) had narrowed too far the other way: the loader refused any partial params file whose *inherited
defaults* violated the ladder or the combination floors, so `{"live_four":40000}` failed on the default
`double_three`/`four_three` it never wrote - the refusal named keys the file did not contain. F2:
`--resume ck.json` without `--checkpoint` kept writing the default file, so ck.json never advanced and the
next resume replayed the same rounds and re-appended the same games to `records.jsonl` / `samples.jsonl`.
F3: the only progress frame was the one at the end of the move, so a 16 s think was silent and the client's
终止 button never armed. F4: VCF/VCT shared the whole deadline, leaving AlphaBeta the 100 ms floor and a
depth-0 fallback into `root_moves[0]` still labelled `alphabeta` in the replay panel. F5: a game started by
playing a stone on a hand-set position never claimed the colour, so `playerColor` stayed null, 悔棋/求招
stayed disabled and a black win read as a loss. F6: the `games_played_` id base's comment claimed crash
protection the write order does not give. G1: 摆棋 over a finished hand-set game overwrote that game's only
file (its `base` keeps it out of `records.jsonl`). G2: a cold rebuild that failed still reused the token,
contradicting README:476-478's "resumed:false + 新 token" and exposing the unreadable file to the next
flush. G3: `moves` upload had no size cap before the per-coordinate walk. G4: 摆棋 inherited the replaced
game's `created_ms`. H1: two tabs resuming one token evicted each other in a loop, every takeover re-running
`halt_search` plus a flush, so the AI never got a stone down.

**Fixed.** F1/F1b: the eight cross-key checks fire only when the rewritten end was explicitly in the file
(presence bits recorded in the 12-key parse loop); an out-of-range written value is still refused, inherited
defaults are left to `sanitize()`, whose body is untouched. F2: `--resume` is also the write-back target;
an explicit `--checkpoint` naming a different file exits 1 at startup, and `--help` says so. F3:
`AlphaBetaSearch` gained a progress callback invoked on the completed-layer branch only; the engine forwards
its own callback (copied to a local first) and the trailing frame is unchanged. F4: named
`kVcfBudgetPercent = 35` / `kVctBudgetPercent = 55` slices anchored at the search start; AlphaBeta keeps the
full deadline, and a depth-0 AlphaBeta result reports `method == "greedy"` (README:520 lists greedy as a
method). F5: `handleMoveMade` claims the side on the `waiting && playerColor === null && baseStones > 0`
state, mirroring `chooseSide`'s success path and dropping the held-back 落子记录 the way `handleStateSync`
does. F6: comment-only rewrite stating what the id base really buys. G1: before the wipe, a previous game
with content is rotated onto its own token via the `handle_restart` mechanism (`detach_socket` +
`open_session` + `attach_socket`), the new token announced through `send_session_state`; untouched boards
keep the plain flush. G2: an unreadable record gets a fresh token through the same `make_token` avoidance
loop; the old file stays on disk untouched. G3: `moves` beyond 225 coordinates is refused before the walk,
`reserve` moved after the check. G4: the replacing position re-stamps `created_ms`/`updated_ms` to now, the
same pair the cold-rebuild path sets. H1: the takeover branch sends
`{"type":"session_taken_over","token":...}` before dropping the loser; `close()` now defers the TCP
shutdown behind the write queue (Beast's own close handshake preempts queued writes - its close op takes the
write lock before the write completion handler runs), and the client sets a taken-over flag that blocks
`scheduleReconnect`, clears the timer, greys the controls and keeps the localStorage token. README was
synced in the same round (the frame and its client handling, the 摆棋 token rotation, the unreadable-record
wording, the `--resume` write-back and exit-1 rule).

**Verified.** Fixer-side only at entry time: each item passed its permitted syntax self-check
(`g++ -fsyntax-only` on the touched translation unit, `node --check web/app.js`), all clean; the F1
regression that motivated F1b is on record (`test_training.cpp` failed on
`from_json_string(R"({"live_four":40000})")` before the narrowing). The test session then ran the gate:
`ctest` **8/8**, `node tests/web_frontend_check.js` **30/30**, and the `--resume`/`--checkpoint` CLI script.
Negative proofs against `c052f59` (`build/neg85`): F1 `test_training.cpp:2257`, F3 `test_search.cpp:552`,
G1 `test_server.cpp:1093`, G2 `:1198`, G3 `test_position.cpp:307`, G4 `test_server.cpp:1152`, H1 `:1257`,
F1b's probe table (HEAD silently rewrote the stated values `live_two 1000→100`, `rush_four 15000→10000`,
`sleepy_two 500→50`), F2-HEAD (the resumed checkpoint stayed at `rounds_completed:1` while the default file
was written), and F4 only at a 10 ms budget (HEAD `alphabeta depth 0` vs `greedy depth 0`). Two reds turned
out to be **the tests' own assumptions**, not the code: `test_set_position_reopens_export` cached
`h.entry` at construction and G1 legitimately replaces the entry once the previous game has content, and
`test_opening_book_large_weight_no_overflow` pinned a single draw when the ordered key `(7,7),(8,8)` legally
holds the identity *and* the transposed reply - `(6,6)/(6,7)/(7,6)` in a 49.8/24.9/25.3 split over 100k
draws, which `c052f59`'s per-move seeding exposed. Both were rewritten to the new contract; the remaining
six `handle_set_position` call sites were audited and are unaffected (empty pre-state → in-place reuse).

## 轮次86 文本字段的类型门、越界的配置哨兵与被吞掉的临时文件 — bugfix round 86 (phase5-bugfix)

**Found.** All four sit in the loaders the page and the trainer read back from disk, and each one is a
present-but-unusable value being treated as an absent one:

`session_store.cpp:111-113` read a `player_color`/`status`/`winner`/`reason` key only when it was a string,
so a damaged `"winner": null` resumed as "" - the writer's own "not decided yet" - and the page announced
白棋胜 for a game that ended differently (`web/app.js:1823` falls through to the white branch for anything
that is not `black`/`draw`). `session_store.cpp:166-177` claimed its bounds matched the message-boundary
clamps but used `< 0` / `> 120000` and `< 0` / `> 20`, so a file stating `timeout_ms: 50` loaded and was
installed straight through `game->set_config` at the resume path, below the 100 ms floor README:338 and the
1-20 depth rule README:340 promise. `save_session` removed the `.tmp` when the *rename* failed
(`:240-245`) but returned early on a mid-write failure (`:235-238`) without removing it, leaving
`<token>.json.tmp` behind forever - README:450 states the atomic write leaves no temporary file.
`data_collector.cpp:222` turned any colour string that was not exactly `"black"` into WHITE, including
`"BLACK"`, `"b"` and `""`, loading a sample whose label blames the wrong player for the move - while the
writer at `:160` only ever emits `black`/`white` and the sibling loader at `game_record.cpp:138-144`
*refuses* an unknown colour for the same field.

**Fixed.** `read_string` now returns a verdict: absent stays absent (the writer's "not set yet" is legal),
present-but-not-a-string refuses the whole file, which is the rule `position_pending` and `base` already
followed - and the refused file then takes the documented unreadable-record path (fresh token, file kept
for inspection, README:474-478). The config bounds became `!= 0 && (out of [100,120000] / [1,20])`, with 0
kept as the "never set" sentinel `SessionSnapshot`'s default and `server.cpp`'s `> 0` test both depend on,
and the comment rewritten to say exactly that. The mid-write failure branch now
`std::filesystem::remove(tmp, ec)` before returning. The sample colour read accepts only `"black"`/`"white"`
and refuses anything else, with no new cross-module dependency.

**Refuted this round.** `elo.cpp:153-164` defaulting a damaged wins/losses/draws/games counter to 0: the
comment at `:146-147` pins the intent, README promises nothing about that file's strictness, and the only
consequence is the K-factor bracket - hardening, not a defect. The same reasoning kept `created_ms` /
`updated_ms` out of the type gate (they are re-stamped on every flush, so refusing a file over them would
cost the player a playable game), and it rules out a "status == game_over implies non-empty winner"
cross-field rule (round-75 settled EMPTY↔"draw" as notation). From the frontend audit: the 摆棋记录 seeded
with the old game's 落子记录 is README:356's prescribed behaviour (进入摆棋时自动把当前落子记录写进摆棋记录),
the status line printing `分值 100000` is README:320 asking for the score (the 胜/负 mapping is the list and
curve's job), and an unguarded `localStorage` in a sandboxed iframe is an untargeted environment, not a
reachable defect.

**Verified.** `ctest` **8/8** and `node tests/web_frontend_check.js` **30/30** (re-run by the orchestrator at
the commit), new cases `test_rejects_non_string_envelope_fields`, `test_session_config_range_edges`,
`test_save_session_leaves_no_tmp`, `test_sample_move_color_must_be_exact`. Negative proofs against
`891df39` in `build/neg86`: P1 `test_session.cpp:263` (HEAD accepts `"winner": null`), P2 `:362` (HEAD
accepts `timeout_ms: 50`), P3 `test_training.cpp:1478` (HEAD reads `"BLACK"` as WHITE and returns true).
P5 has **no observable difference** in these cases and was left without a negative proof on purpose: the
branch needs a real mid-write failure (`file.good()` after the flush), which no test can provoke through
`save_session(dir, snap, error)` - inventing one would mean opening a failure back door in `src`, and the
degraded case (a normal save leaves no `.tmp`, an illegal token writes nothing) passes on HEAD exactly as it
should.

## 轮次86（批 Q）被 `--quiet` 漏掉的那一行、被宣布又被丢弃的 `--params`、算了八遍的那半边特征 — bugfix round 86 batch Q

**Found.** Three training-domain rows from a read-only audit of `game_record.cpp` / `data_collector.cpp` /
`trainer.cpp` / `main_trainer.cpp` / `game_manager.cpp` and the server's replay-restart-append paths; I
re-read every cited line before dispatching.

`trainer.cpp:166` printed `skipping data games this round: no --records or --samples output path was given`
through `std::cout` - the only bare `std::cout` in the file, where every other progress line goes through
`if (log_) log_(...)` (`:295 :311 :353 :358 :368`) and `main_trainer.cpp:281` installs that callback only
when `!quiet`. README:199 makes both output paths default-empty, so a bare `gomoku_train --quiet` printed one
line per round against `--help`'s own "Only print the final result" (`main_trainer.cpp:55`).

`main_trainer.cpp:259-267` loaded `--params` fatally (a bad file exits 1), announced `Starting from <path>`,
passed the result as the Trainer's `base` (which `trainer.cpp:29` pushes as `versions_[0]`) - and then
`load_checkpoint` replaced `versions_`, `elo_` and `best_version_` wholesale (`trainer.cpp:537-540`), so under
`--resume` the announced start point is not the one used, and a file the run will never read can still stop it
from starting. `base` has no other use in `trainer.cpp`.

`data_collector.cpp:127-135` called the whole `extract_features` once per symmetry variant, so the
`active_empty()` loop with its two `recognize_position` calls per candidate cell (`:42-68`) ran eight times
per sampled ply. Slots **0-24** are identical for all eight orientations - the shape counts are summed over
the four directions and scaled by `actives.size()`, which a dihedral map preserves bijectively; the stone
counts, active count, ply and side are counts; `centre_control` uses Chebyshev distance to `(7,7)`, and
`transform_board` (`transforms.cpp:15-22`) replays `move_history` through `place_stone`, so it is an exact
board isometry that also preserves `move_count`/`last_move`. Only **25-51** (last-move row/col and the 5x5
window) follow the transform, which is what `data_collector.h:30-32` already says the augmentation multiplies.

**Fixed.** The skip notice went to `if (log_) log_(...)` with its text unchanged; the four `std::cerr`
write-failure lines were left alone (settled: those print regardless of `--quiet`). `--params` and `--resume`
now refuse each other at startup, in the same shape as the existing `--checkpoint`/`--resume` rule, so neither
a wasted fatal check nor a false announcement is possible, and `--help` says so on both rows.
`extract_features` keeps its public signature and its return byte-for-byte: its body is now
`fill_invariants` (0-24) + `fill_orientation` (25-51) over a zero-initialised vector, and `collect()` computes
the invariants once per ply inside the emit branch, then copies them and refills only 25-51 per accepted
variant. Sample counts, the `seen` dedup, the `FEATURE_*` constants and the writers are untouched.

**Refuted this round.** `game_record.cpp:158-173`'s plain `ofstream(trunc)` single-record save is not a
production writer (only `test_training.cpp:242`/`:1921` call it; the server appends through
`append_to_jsonl`, `server.cpp:1453`) and README:561's "不会留下半个文件" describes the 10-second session
flush, which is atomic. A 摆棋 `base`-backed record reaching `DataCollector::collect()` from an empty board
needs someone to hand-copy a session file's `record` into a trainer path, and README:565 says exactly that
those games live only in the session file and stay out of the training stream (`server.cpp:828` implements
it) - the ignored `place_stone` result at `:149` cannot diverge for a record that path can actually produce.
From the frontend audit three candidates were dropped the same way, for 0 dispatchable items.

**Verified.** `ctest` **8/8** (bare `cmake ..`, new case `test_augmented_samples_match_manual_transforms`
inside `test_training`) and `node tests/web_frontend_check.js` **30/30**, the web harness re-run by the
orchestrator. Q1 and Q2 are CLI-observable, so they were proven by a script rather than a unit case:
against the worktree, `gomoku_train --quiet --rounds 1 --data-games 1` prints the skip notice **0** times
while the same run without `--quiet` prints it **1**, and `--resume A --params B` exits **1** naming both
flags while each alone still exits 0 (`Starting from` for `--params`, `Resumed` and **0** `Starting from`
for `--resume`). In `build/neg86q` (`git archive 980983b` + the current `tests/`), the same script shows
both defects verbatim: `--quiet` prints the notice, and the combination runs to completion announcing
`Starting from /tmp/r86qn_b_params.json` followed by `Resumed /tmp/r86qn_ck.json`. Q3 has **no negative
proof by design** - it is a behaviour-preserving reordering, so the gold-standard case (every kept sample
equals `extract_features(transform_board(board_i, v), side)` bit for bit, identity orientation present, all
eight distinct orientations surviving, one sample per ply when augmentation is off) passes on `980983b`
too; the evidence for the change is the call shape instead: `980983b` still had
`sample.features = extract_features(oriented, side)` inside the variant loop, while the worktree calls
`fill_invariants` once per ply (`:136`) and only `fill_orientation` per variant (`:149`).
No run wrote into `data/training/` (the default checkpoint is still absent).

## 轮次87 只撒到下半格的播种、被当成禁用的空路径 — bugfix round 87 batch R

**Found.** **R1** `src/training/genetic.cpp:51` built the "redrawn across the whole box" half of the seeded
population with `bound.min_value + noise(rng_) * span`, reusing the `std::uniform_real_distribution<double>
noise(-0.5, 0.5)` from `:38`. Half of those draws are negative, so they land *below* `min_value` and are
clamped back by `sanitize_vector` (`params.cpp:71-90`, the clamp at `:76`); the reachable band was
`min .. min + span/2`, not the box the comment at `:45-46` claims, and roughly half of every reseeded member's
genes sat pinned at the floor of their `ParamBound` (`params.cpp:18-31`). Measured on `c14854a`: 23-33 of the
60 reseeded genes per 12-member population hit a `min_value`, and no member ever exceeded the midpoint of the
`double_four` box (10 seeds). The founder half is untouched, so this was purely about where the *search*
starts - the upper half of each box, which is exactly where a wider coefficient search has to be able to
begin, was never sampled. **R3** `src/main.cpp` took `--web-dir ""` / `--book ""` / `--sessions ""` /
`--records ""` / `--params ""` as "feature off": an empty shell variable therefore silenced the opening book,
dropped the 10-second session flush and the finished-game record stream, or replaced the trained params with
the built-in defaults, all without a word - while README:159-161 documents `--no-book` / `--no-sessions` as
the only off switches and the file's own fail-loud principle (`:70-72`, `:83-85`) refuses a path the caller
did not really give.

**Fixed.** `:39` adds a separate `uniform(0.0, 1.0)` and `:51` uses it for the redraw branch only, so
`min + U(0,1) * span` covers the box without a clamp; the `noise` distribution stays for the nudge branch at
`:49`, and nothing else in the optimizer (tournament, crossover, mutation, elitism) was touched. The five
path flags each refuse an empty value with `Invalid <flag> (must not be empty)` and exit 1, next to the
existing `--port`/unknown-option refusals; README now states the rule where the flags are documented.

**Test-side.** `tests/test_training.cpp` gains `test_genetic_seed_covers_the_whole_box` (two fixed seeds,
`max_generations = 0` so `population()` is exactly what `seed_population` wrote: seat 0 equals the base gene
for gene, odd seats are the redrawn members and no gene may sit on its `min_value`, even seats stay within
`nudge_budget`, and at least one redrawn `double_four` is above the box midpoint - the old draw's ceiling was
50000-1). `test_genetic_elite_single_evaluation` was **rewritten as 用例假设错, not as an implementation
regression**: its `assert(calls[winner_key] == 1)` counted fitness calls per *gene-vector string*, so it
measured "this vector happens to be unique" rather than "the elite is not re-scored", and any change to the
seeding distribution could flip it. The probe settles it - on both trees and all eight seeds
`evaluations() == 15 == pop + gens * (pop - 1)`, i.e. the elite is genuinely never re-scored, while duplicate
vectors are re-scored on `c14854a` too (seed 3 a non-winner vector 4 times, seed 11 the founder 3 times,
seed 4242 the winner 2 times). The case now pins the observable form (`population().back().fitness == best`,
the appended elite's coefficients equal to the returned champion, the population size and the evaluation
count formula unchanged) and the vector-keyed map is gone.

**Refuted this round.** "must_block throws away a proven VCF win" - in the cited position black's live-four
creation at (5,6) hands the move to white, who completes five at (10,7), so the block is the only move; a
five-completion point is always adjacent to a friendly stone and therefore always inside `active_empty()`, so
no five point can be missing from that scan. Multiplicative mutation (`genetic.cpp:107-117`) cannot move a
gene already at 0 (`POSITION_WEIGHT_SCALE` has `min_value = 0`): a real limitation, but the seeding fix
removes the mass of floor-pinned genes that made it common, and README promises no additive fallback. The
static-serving/CLI audit cleared `server.cpp:403-442` (`/` → `index.html`, `%`/backslash/`//` rejected,
every `..`/`.` segment rejected, `weakly_canonical` + root-prefix check, `is_regular_file` so no directory
listing), `server.cpp:444-458` (all three shipped assets have explicit content types), `main.cpp` `--port`
(`used != text.size()` rejects trailing junk, `stoi` failures exit 1, the 1-65535 check runs before the
`uint16_t` endpoint), duplicate/unknown flags, and `elo.cpp:60-94` / `game_record.*` / `self_play.cpp`
counters and labels. One item from that audit is **carried to the next batch**: `params.cpp:344-347` returns
on a mid-write failure without removing `<path>.tmp`, while its own comment at `:329-331` claims it mirrors
`save_session`, which removes the temp on that branch (`session_store.cpp:244-248`) and `params.cpp:352`
removes it when only the rename failed.

**Verified.** Gate over the identical tree: `ctest` **8/8** (bare `cmake ..`; `test_genetic_seed_covers_the_whole_box`
and `test_genetic_elite_single_evaluation` both pass in `test_training`) and `node tests/web_frontend_check.js`
**30/30**. Negative proofs in `build/neg87r` (`git archive c14854a` + the current `tests/`): the seeding case
aborts at `tests/test_training.cpp:1052` `Assertion 'genes[g] != bound.min_value' failed` (exit 134) with
`min_hits=23` / `31` and `double_four_above_mid=0`, while the elite case **passes** at HEAD - the rewrite is
behaviour-neutral, only R1 is proven by it. R3 is CLI-proven both ways: on the worktree all five empty values
exit **1** naming the flag (`Invalid --params (must not be empty)` is the log tail), legal paths and
`--no-book` / `--no-sessions` alone still reach `Server started` (exit 124 from the harness' own timeout) with
**0** empty-path refusals, and no run wrote into the repo's `data/`; on `c14854a` the same four flags
`--book`/`--sessions`/`--records`/`--params` given `""` print `Server started` and never name the flag (only
`--web-dir ""` exited 1, and for the pre-existing "not a directory" reason).

