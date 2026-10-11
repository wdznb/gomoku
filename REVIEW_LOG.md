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

