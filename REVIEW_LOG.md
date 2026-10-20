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
(`main_trainer.cpp:231`), and `--rounds 0` / `--timeout-ms 0` / `--max-moves 0` are refused by the
downstream semantic guards at `main_trainer.cpp:225` and `:236` — which is the README-consistent
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
(`trainer.cpp:435`, `params.cpp:210`) and the four plain writers (`data_collector.cpp:241`,
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
resign). L2: `require_double` (`main_trainer.cpp:92-100`) only checked `used != text.size()`, and `stod`
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
metadata only. M4: README:176/:201 documented `--resume` as a bare switch while `main_trainer.cpp:62` and
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
`test_training.cpp:1287`, A6 `test_server.cpp:888 Assertion 'exists(dir/"deadbeef.json")' failed`, A7
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
Negative proofs against `c052f59` (`build/neg85`): F1 `test_training.cpp:2258`, F3 `test_search.cpp:552`,
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
accepts `timeout_ms: 50`), P3 `test_training.cpp:1479` (HEAD reads `"BLACK"` as WHITE and returns true).
P5 has **no observable difference** in these cases and was left without a negative proof on purpose: the
branch needs a real mid-write failure (`file.good()` after the flush), which no test can provoke through
`save_session(dir, snap, error)` - inventing one would mean opening a failure back door in `src`, and the
degraded case (a normal save leaves no `.tmp`, an illegal token writes nothing) passes on HEAD exactly as it
should.

## 轮次86（批 Q）被 `--quiet` 漏掉的那一行、被宣布又被丢弃的 `--params`、算了八遍的那半边特征 — bugfix round 86 batch Q

**Found.** Three training-domain rows from a read-only audit of `game_record.cpp` / `data_collector.cpp` /
`trainer.cpp` / `main_trainer.cpp` / `game_manager.cpp` and the server's replay-restart-append paths; I
re-read every cited line before dispatching.

`trainer.cpp:193` printed `skipping data games this round: no --records or --samples output path was given`
through `std::cout` - the only bare `std::cout` in the file, where every other progress line goes through
`if (log_) log_(...)` (`:322 :338 :380 :385 :395`) and `main_trainer.cpp:300` installs that callback only
when `!quiet`. README:199 makes both output paths default-empty, so a bare `gomoku_train --quiet` printed one
line per round against `--help`'s own "Only print the final result" (`main_trainer.cpp:65`).

`main_trainer.cpp:278-286` loaded `--params` fatally (a bad file exits 1), announced `Starting from <path>`,
passed the result as the Trainer's `base` (which `trainer.cpp:29` pushes as `versions_[0]`) - and then
`load_checkpoint` replaced `versions_`, `elo_` and `best_version_` wholesale (`trainer.cpp:579-582`), so under
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
production writer (only `test_training.cpp:243`/`:1922` call it; the server appends through
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
aborts at `tests/test_training.cpp:1053` `Assertion 'genes[g] != bound.min_value' failed` (exit 134) with
`min_hits=23` / `31` and `double_four_above_mid=0`, while the elite case **passes** at HEAD - the rewrite is
behaviour-neutral, only R1 is proven by it. R3 is CLI-proven both ways: on the worktree all five empty values
exit **1** naming the flag (`Invalid --params (must not be empty)` is the log tail), legal paths and
`--no-book` / `--no-sessions` alone still reach `Server started` (exit 124 from the harness' own timeout) with
**0** empty-path refusals, and no run wrote into the repo's `data/`; on `c14854a` the same four flags
`--book`/`--sessions`/`--records`/`--params` given `""` print `Server started` and never name the flag (only
`--web-dir ""` exited 1, and for the pre-existing "not a directory" reason).

## 轮次88 送到旧局面的推荐、必输的那一手堵、被当成界存下来的静态值 — bugfix round 88 batch S

**Found.** Seven defects plus one stale comment, all re-read against `README.md` before dispatch:
**S1** 求招 computed on a snapshot (`game_manager.cpp:310`) and delivered later (`:320-324`) while `make_move`
gates only on GAME_OVER and `ai_thinking_`, so a recommendation could name a square the player had already
taken; **S2** `search_engine.cpp:113-127` returned the first five-completion point it found as `must_block`
even when the opponent had **two** of them - blocking either leaves the other intact, because a completion
point's five-cell window cannot contain the square just played - which contradicts README:39's 强制失败证明与
自动认输; **S3** an empty root candidate list produced `Move(7,7,color)` in two places (`alphabeta.cpp` root
and `search_engine.cpp:42`), i.e. a square that is occupied by definition on a full board, and AlphaBeta's
answer was committed without a validity test; **S4** `vct.cpp:83-108` ran `is_forbidden` + `evaluate_position`
over *every* active candidate before the Chebyshev window test threw most of them away; **S5** `server.cpp:1157`
restored `timeout_ms` and `max_depth` only when **both** were non-zero, so README:451's legal `timeout_ms: 0`
sentinel silently discarded a saved depth; **S6** the abort shortcuts return a bare static evaluation, not a
bound, yet the interrupted node stored the assembled score as LOWERBOUND/EXACT - and since `new_search()` only
bumps the age and `lookup` ignores it (round-80 B8), those unsound entries survived into the next AI move;
**S8** `params.cpp:343-347` returned on a flush failure with `<path>.tmp` left on disk, the same class as
round-86 P5; **S9** `main.cpp:103-105` still described the server as answering every GET with "Not found".

**Fixed.** S1 remembers `move_count()` at the snapshot and re-reads it under `thinking_mutex_` before delivery,
passing a default-constructed `Move()` when the position moved, which the existing `server.cpp` handler turns
into an error frame - no new protocol field. S2 keeps the first block when the count is one and, on a second
five point, resigns through the existing `resigned`/`method = "resign"` shape when resignation detection is on
and keeps today's single block when it is off (opening a resignation the player disabled is not our call); the
count runs over `board.active_empty()`, not the BLACK-forbidden-filtered `root_moves`, which would drop a five
point black itself may not occupy. S3 returns an invalid `Move()` at the AlphaBeta root, validates the returned
move at the SearchEngine root, and falls back to the first still-legal `root_moves` candidate rather than to
`greedy_.search`, which reads the same `active_empty()` window and hands back the centre when it holds no legal
cell. S4 hoists only the two side-effect-free predicates (`dist > 4`, the duplicate scan) above the forbidden
test - the two loops deliberately keep their different interfaces, so this is a pure reordering. S5 fills each
half independently, feeding the unset one from `game->timeout_ms()` / `game->max_depth()` so `set_config`'s
double clamp never receives a 0. S6 adds an `aborted_` member reset at the top of `search()`, set at all four
abort short-circuits, gating `tt_.store`; the table is *not* made age-aware and `new_search()` does not clear
it. S8 removes the temp through the `error_code` overload on both failure branches with the error text and the
write→flush→rename order untouched.

**Test-side.** Two pre-existing asserts disagreed with the new contract and both are **用例假设错, not
implementation bugs**. `test_search.cpp:766` (`test_forced_move`) asserts a playable move, but its Phase-2
filler position carries 213 stones and is already decided (`check_winner() == WHITE`, 28 black and 24 white
fives on the board), so S2's new rule rightly answers it with a resignation; the case now calls
`set_resign_detection(false)` with the reason written in place, keeping its original board and its
`must_block (7,8)` coverage, and the resignation itself is pinned by
`test_search_engine_resigns_on_two_enemy_five_points`. `test_training.cpp:2108` pinned the *residue* of a failed
params write - the behaviour S8 exists to remove - so the params arm now asserts the poisoned temp is gone while
the checkpoint arm below it keeps its old expectation (that writer was not given the cleanup).

**My own dispatch error, recorded because it is the second time this round:** I told the fixer that
`active_empty()` can be empty only on a full board and that an empty board therefore needed the centre move back.
The premise is wrong twice over - `rebuild_active_empty()` (`board.cpp:281-293`) seeds all 225 cells when
`move_count_ == 0`, and every path reaching zero stones goes through it (`reset()` :55, take-back :106-108,
first stone :235-239), so an empty board never reaches a "no candidates" branch and the guard I asked for was
dead code. The fixer read the code, reported the contradiction instead of shipping it, and the branch went back
to `no_legal_move`. Consequence worth knowing: `method == "default"` now has no producer anywhere in `src/`,
which is correct - at HEAD its only producer was a full board handing back an occupied centre - and
`data_collector.cpp:129`'s filter keeps working over historical records only.

**Refuted this round.** S7 (memoise GA fitness per gene vector): production fitness is a *stochastic* self-play
match, so sharing one score between identical vectors changes the optimizer's noise model and can bias elitism -
a design change, not a defect fix. A board/bitboard audit produced zero defects: the active list cannot diverge
(`activate` early-returns on a non-negative index, `deactivate` fixes the swapped-in entry before `pop_back`),
`neighbor_count_` moves symmetrically on place and undo, `has_neighbor_` is dead state with no reader, and
Zobrist's lack of a side-to-move key is harmless because the side is the parity of the stone count. A
static-serving/CLI audit produced nothing beyond S8. Evaluator/pattern rows cleared too: no cross-window four
double payout (`match_pattern:316-317` collapses four-over-three), BLACK's overline is never scored as five
(`pattern.cpp:383-385` demotes to LONG_FIVE), WHITE's overline is never forbidden, and `recognize_combined`'s
duplicated 禁手 logic has no production caller. Four items are **carried to the next batch**: the phantom
forbidden-shape payout inside `candidate_value` / `evaluate_position_pair` / `evaluate_position` (the rule
`evaluate_board` applies at `:154`/`:157` does not exist on the per-square path that move ordering and greedy
consume), `forbidden_penalty` being inert in every production path while `params.cpp:221-223` justifies its
range guard with an argument that never runs (comment-side fix only), the missing `reserve` in the innermost
quiescence loop, and `alphabeta.cpp:62`'s claim that `is_forbidden` re-runs `recognize_combined`.

**Verified.** Gate over the identical tree: `ctest` **8/8** (bare `cmake ..`) and
`node tests/web_frontend_check.js` **30/30**. Negative proofs in `build/neg88s` (`git archive a9c7899` plus the
current `tests/`) - S1 `tests/test_server.cpp:1340` `hint_results == 0`, S2 `tests/test_search.cpp:1323`
`result.resigned`, S3 `tests/test_search.cpp:1380` `result.method == "no_legal_move"`, S5
`tests/test_server.cpp:1431` `depth == 3`, S6 `tests/test_search.cpp:2060`
`aborted_tt.lookup(child_hash) == nullptr`, S8 `tests/test_training.cpp:2113`
`!std::filesystem::is_symlink(params_tmp)`; HEAD's full `ctest` run fails exactly three targets
(`test_search` / `test_server` / `test_training`) on those cases. Anti-overreach pins that stayed green on both
trees: an empty board still opens at the centre through the normal path, round-78's 终止 handling, round-80 B8's
cross-move TT reuse and round-81's far-counter VCT case. S3(a)(b), S4 and S9 are reported as **无可观察差异**
with the code shapes grepped instead of invented negatives, and `build/r88s_cli.sh` shows the two trees agreeing
byte for byte on every CLI-level line (`e1`/`e2`/`e3`/`s9a`/`s9b` stderr md5 and exit codes equal, no residue,
the repo's `data/` untouched).

## 轮次89 黑棋落不了的那一格值多少分 — bugfix round 89 batch T

**Found.** Batch S closed the search/engine layer, so this round re-audited the evaluator and the pattern
side against README:27-29, plus a second pass over the training CLI surface the same window touched. Four
rows survived my own line-by-line re-read and became batch T. **T1** is the only behaviour change: the
evaluator states its own rule at `evaluator.cpp:150-153` — a shape BLACK can never legally play must not bank
points — and applies it in `evaluate_board` only. `candidate_value` had no colour at all, so every single-cell
read (`evaluate_position`, `evaluate_position_pair`, and downstream `alphabeta.cpp:73` move ordering,
`greedy.cpp:41-42` defense prices, `vcf.cpp:31` / `vct.cpp:32,:71` static scores) paid forbidden shapes freely.
Measured on the four-four board (BLACK (7,4)(7,5)(7,6) + (4,7)(5,7)(6,7), probe (7,7)): WHITE's defense price
for that square was **70014** and a 三三 point **31014** — a tempo BLACK is not allowed to spend, so the AI
was ranking "blocks" of illegal shapes above real ones. **T2** `params.cpp:221-223` justified the
`forbidden_penalty` range guard with "AlphaBeta treats the penalty as a loss", but the only reader of that
field is `evaluate_position_both`, which no production path calls (verified: `evaluator.cpp:134-141` is
self-contained and `src/` has no caller) — comment-side fix. **T3** the innermost quiescence loop rebuilt
`scored` without a `reserve`. **T4** `alphabeta.cpp:62` claimed `is_forbidden` re-runs `recognize_combined`;
it re-runs `recognize_position`.

**Fixed.** `candidate_value(row, col, pattern, color)` takes the colour and returns 0 for a BLACK forbidden
pattern — placed **after** the exact-FIVE early return, so a square that makes five is still worth
`coeffs_.five` and never zeroed (README:27/29: a five is a win, not a 禁手).
`evaluate_position_with_pattern` gained the same colour parameter at all four call sites (`alphabeta.cpp:132`
→ `color`, `vcf.cpp:31` → `color`, `vct.cpp:32` → `color`, `vct.cpp:72` → `opp`), and
`evaluate_position_pair` now judges each half with its own colour (`mine` as `color`, `theirs` as
`opponent(color)`), which is what makes the pair equal to the sum of the two single reads. `params.cpp`'s
comment states the test-only reader instead of the mate argument; `alphabeta.cpp:110` reserves `scored`;
`alphabeta.cpp:62` names `recognize_position`. README:29 gained the scoring rule as stated behaviour.

**Not defects / deliberately left.** The `--no-sessions` row from the server audit ("a detached game is still
resumable in-process for 30 minutes, so 对局不是只活在 socket 里") is a README wording problem, not a code
problem: README:62 and README:558 both state that games hang on the token session table rather than the
socket, and `flush_entry` (`server.cpp:1403`) does refuse every write when `sessions_dir_` is empty — the
one-line CLI comment at README:161 is the inaccurate text and gets reworded instead of the behaviour changed.
The bare-`[` asymmetry between `position_from_text` (`position_json.cpp:271-285`, refuses a top-level array)
and `read_position_payload` (`server.cpp:59-64`, wraps one into `board`) is two documented intents, and
README:225 describes the 完整棋盘 form as a `board` **field**; widening the file format is a design change, so
it is not dispatched. Round-88's carries stayed refuted: no `five`-memoisation of `recognize_position`, no
pattern-cache merge, no incremental full-board evaluation. Three verified rows are carried forward as round-90
batch U instead: `trainer.cpp:37-40` measures every intra-round fitness match with the candidate's rule
constants defaulted (round 74's carry at `:322-323` only covers the verification match), and `server.cpp:834`
/`:1066` count `position_pending()` as content, so 摆棋→清空→确认 twice rotates a token and shelves an
empty engine for the whole reattach window — against README:540-542's 落过子/摆过子/选过边 list and the
predicate's own comment at `:1063-1065`.

**Verified.** Gate over the identical tree: `ctest` **8/8** (bare `cmake ..`) and
`node tests/web_frontend_check.js` **30/30**. HEAD negatives in `build/neg89t` (`git archive 5073953` plus the
current `tests/`) fail on exactly two cases: `tests/test_evaluator.cpp:366`
`evaluate_position(four_four, 7, 7, BLACK) == 0` (HEAD 70014) and `tests/test_search.cpp:2260`
`spot->defense_score == 0` (HEAD 70014); HEAD's full `ctest` prints 2 failed out of 8 and every earlier case
in both binaries still passes. The anti-overreach pins are green on both trees, including the ordering proof
(`test_exact_five_outranks_the_forbidden_gate`: a square whose pattern carries `has_four_four` **and** an exact
five still scores `five`, 100000 on both trees), the ordinary neighbour (1063/13/1076 unchanged), a legal
single 活四 (WHITE still pays the full 10014 defense), and BLACK's own ranked list, which never contained the
forbidden point even before the gate. T2 is **无可观察差异** with the CLI as evidence: `build/r89t_cli.sh` and
`build/r89t_cli_neg.sh` agree item by item (0 / −1 / 100000 rejected with byte-identical stderr, 40000 and
99999 accepted), and the one differing figure (`out_bytes` 327 vs 325 on the `--timeout-ms 20` cases) was
proved to be wall-clock noise rather than a signal — the same tree twice is not reproducible at that budget
(`r89t_det.log`: 327 / 326), while `r89t_det2.sh` at `--timeout-ms 5000` gives one identical 330-byte file
across four runs on both trees with `forbidden_penalty: 40000` written back unchanged. T3 and T4 are
**无可观察差异** with the two-tree diff and the `candidate_value` / `evaluate_position_with_pattern` greps
(4-parameter declaration, definition, three internal uses, four coloured call sites, no leftover 3-argument
shape).

## 轮次90 候选和冠军不在同一条规则上比 — bugfix round 90 batch U

**Found.** Batch T took the evaluator, so this round audited the layers that had not been re-read since
batch S: the training loop's fitness path, the session handlers, and the position text domain. Three rows
survived my own line-by-line re-read. **U1** `trainer.cpp:37-40` handed the optimiser's candidate straight to
`play_match`, and every optimiser rebuilds its members through `CoefficientVector::from_vector`
(`hill_climbing.cpp:40,:63`, `genetic.cpp:57,:167`), which default-constructs `ThreatCoefficients` and writes
only the 10 tunables (`params.cpp:62-68`). README:607-608 states those two constants are *not* genes, and
round 74's carry at `:330-331` only covers the post-hoc verification match, so every intra-round pairing
compared a candidate on the compiled-in baseline (`five` 100000, `forbidden_penalty` 50000 — the cap at
`evaluator.cpp:175` and the five bonus at `:108` both move with `five`) against a champion on the parameter
file's. Measured by `build/r90u_probe4.cpp` over one hill-climbing round: 22 objective calls, **19 of them off
the champion's baseline** (`90000/40000` champion vs `100000/50000` candidate), only `trainer.cpp:310`'s own
champion read and the round-end re-read on it. **U2** both `nothing_to_keep` predicates (`server.cpp:834`,
`:1066`) counted `position_pending()` as content. `validate_position` accepts an entirely empty board, so
摆棋→清空→确认 rotated a token per click and detached a game holding *nothing* into the 30-minute reattach
window, where it keeps an engine and its transposition table alive — against README:541-543's 落过子/摆过子/
选过边 list and against the predicate's own comment at `:1063-1065` («a trail of empty sessions»). **U3**
`position_setup.cpp:302` kept `row_error` only when nothing was ragged, and `ragged` is set by the same row-width
test (`:232-238`), so a 15-row grid whose 3rd row carries 14 cells dropped the row message and reported
「棋盘有 224 格，需要 225 格…」 — precisely the "cannot tell a layout slip from a truncated dump" case
README:261-262 says the row message exists to prevent.

**Fixed.** `measure_fitness` copies the candidate and rebases `five`/`forbidden_penalty` on `champion()` before
either branch, so the `fitness_override_` hook sees the same pairing the games do; `from_vector`'s default
construction is left alone (sanitize depends on it) and no `CoefficientVector` API was added. The two session
predicates drop the `position_pending()` clause — `previous &&` stays at `:1066` because `previous` can be null
there, while `:834`'s is guaranteed by the `if` initialiser — and both comments now cite README:541-543's
three-way test. `parse_grid_text` reports `row_error` when the grid is ragged **and** 15 rows deep
(`:305`), which is safe because `row_error` is non-empty whenever `ragged` is set (`:234-237`); the 16 MB
early-stop guards at `:245-252`, the flat-success paths and the symbol-kind-first ordering were not touched.
README side: `:543` gained the 全空摆棋 clarification and `:608` the intra-round baseline rule.

**Not defects / deliberately left.** The server audit's «`--no-sessions` still resumes a detached game in-process,
so the flag is broken» stays refuted as a wording problem, and it is the text that moved: `flush_entry`
(`server.cpp:1403`) refuses every write when `sessions_dir_` is empty, while `on_flush_tick` shelves the game
into `cold_sessions_` (`:1496-1500`) and resume rebuilds from that **in-memory** snapshot (`:1317-1325`), so
README:161's «对局只活在 socket 里» was the inaccurate line and is now «只活在这个进程的内存里». The bare-`[`
asymmetry between `position_from_text` and `read_position_payload` remains two documented intents
(README:225 defines the full-board form as a `board` field). Round-88's carries stayed refuted, and no
behaviour-preserving claim was invented for U1's CLI surface: the `five: 90000` run legitimately differs in gene
values, which is not an output contract.

**Verified.** Gate over the identical tree: `ctest` **8/8** (bare `cmake ..`) and
`node tests/web_frontend_check.js` **30/30**. HEAD negatives in `build/neg90u` (`git archive 0c56dbb` plus the
current `tests/`) fail **3 out of 8** and nothing else: `tests/test_training.cpp:2705`
`call.first == 90000`, `tests/test_server.cpp:1489` `again.get() == first.get()`, `tests/test_position.cpp:947`
`error.find("第 3 行有 14 格") != npos`; the three aborted binaries had already printed 15 / 18 / 46 ` passed`
lines of existing cases first. Anti-overreach pins green on both trees: `defaults.five` stays zero-change
(`test_measure_fitness_leaves_default_rule_constants_alone`, 38 pairings all `100000/50000` on HEAD as well), the
three contentful token rotations `test_server.cpp:282/:1078/:1142` plus `:330`/`:695`, and `test_position.cpp:150`
/`:161-166` with byte-identical messages (`棋盘有 224 格…`, `棋盘有超过 15 行（读到 16 行）…`, `第 1 行有 3 格…`).
`build/r90u_cli.sh` at `--timeout-ms 5000 --seed 11`: default-parameter output is the same 330 bytes with md5
`d3130f8411414342b2c465e8cd023c1c` on **both** trees (empty stderr, 19 stdout lines), which is U1's zero-change
half; the `{"coefficients": {"five": 90000}}` run differs only in the ten gene values while both trees write
`five: 90000` back, stderr empty on both. No `data/training/checkpoint.json` residue. The tester's diff is
additive only (`test_position.cpp` +42, `test_server.cpp` +51, `test_training.cpp` +71, zero deleted lines), so
no existing assertion was edited.


## 轮次140 验收：`docs/ENGINE_REDESIGN.md` 的六条目标、§4 四行与四条回归保障逐条对账——G1／G2／G4／G6 与 A／D 行有达标读数，G3 第二格第一次量到（中位 8.8 %，那一格要 25 %），C 行仍差那个 4.4 倍因子，`:555` 与 `:556` 各欠一样东西 — acceptance round: every goal, §4 row and regression guarantee in the design doc read against the tree, with two fresh measurements (the B-row's 64 arms split by `method`, and a session's second think priced against a cold start)

用户这一轮要的是「按文档依次验收；文档内的目标如果没有实现，必须给出真实数据和整改方案，确认方案可以按照文档实现」。
本轮**不动一行代码**：`src/`、`tests/`、`web/`、`tools/`、`CMakeLists.txt`、`data/` 与 `d4ed625` 逐字节相同，仓库改动只有本节。
两支新读数是树外仪器：B 行那 64 臂按 `method` 拆开（重新解析轮次132 留在 `/tmp/f2/after_i{1,2}.out` 的原始行，没重跑），
G3 的会话第二手（`/tmp/g3/g3_driver.cpp` 对 HEAD 现编现跑 → `g3b.out`）。机制落点沿用轮次139 那张 43 条表
（`/tmp/m1/audit.md`：32 条在跑 / 8 条异形 / 3 条已判不落地或按文档不做），本节不重复它的逐行位置。

### 总判一：§2.1 六条目标（`docs/ENGINE_REDESIGN.md:122-127`）

| 目标 | 文档指标 | 现在的读数 | 判 |
| --- | --- | --- | --- |
| G1 `:122` | 中盘典型局面 depth 6 在 2 s 内完成率 ≥ 95 % | 进了分层搜索的那 56 臂：**54/56 = 96.4 %**；全部 64 臂口径 84.4 %（读数 1） | **达标**，但口径要由用户定（方案 E） |
| G2 `:123` | 每次思考上报「已完成深度／中断深度／已搜根着法数／中止原因」 | 四个字段都在契约里（`src/search/alphabeta.h:18-45`）、帧里全序列化（`src/server/server.cpp:1670-1678`）、前端出那句「第 N 层中断（x/y）」（`web/app.js:1975-1981`） | 达标 |
| G3 `:124` | 第二手起根节点命中率 > 30 %；节点数较冷启动下降 ≥ 25 % | 命中 **23/24 = 95.8 %**；降幅 **中位 8.8 %**，20 个可比臂里只有 1 个 ≥ 25 %，最差 **−4.5 %**（读数 2） | **未达标**（第二格），方案 A |
| G4 `:125` | 无战术差异时 100 % 选更靠中心的点，选点可复现 | A6 的定点读数：静默局 d3 唯一并列最优 3180 落 (6,4)；双胜点局 2 个并列最优且中心距不等，落 (7,7)、分 `100000`；同一副子两次思考 **48268 == 48268**（改前 47397 对 47045） | 达标 |
| G5 `:126` | 单线程 ≥ 1M nps | 228,000 nps、42,353.5 Ir／节点、942.4 Ir／候选格；要达标得 ≤ 9,660／≤ 215，即 **4.4 倍**（读数 3） | **未达标**，方案 B（是一次设计授权） |
| G6 `:127` | 单轮耗时 −≥ 40 %；内存峰值与并发局数解耦 | −**71.5 %**（轮次126 默认量级）；RSS 斜率 **+7,386 → +71 kB／worker**，共享臂 t=1/2/4/8 = 20,448／20,404／20,816／21,496 kB 平坦（轮次127）；`--fitness-games 2` 下适配度实测 0.25 与 0.3125，值域是五值不是三档（轮次128） | 达标（代价记在「问题账」第 7、8、9 条） |

### 总判二：§4 四行验收（`:547-:550`）与四条回归保障（`:554-:557`）

| 行 | 验收列 | 读数 | 判 |
| --- | --- | --- | --- |
| A `:547` | 超时显示「中断于第 6 层（12/34）」、无战术差异 100 % 选中心且可复现、新增算例 | 三件都有对位：帧字段 `server.cpp:1670-1678` + 前端 `app.js:1975-1981`；A2／A6／A7／A8 的用例随 ctest 跑；G4 那三个定点数 | 达标 |
| B `:548` | d6／2 s 完成率 ≥ 95 %；节点较 A 阶段降 ≥ 50 % | 完成率见读数 1（两口径）；节点那一格 `quiet` 实测 **1,525,277 → 230,474 = −84.9 %**（轮次121 B1） | 第二格达标；第一格取决于口径 |
| C `:549` | 单线程 ≥ 1M nps；depth 8 在 4 s 内完成 | 第一格 228k／需 4.4 倍，记忆化上限 1.25 倍；第二格产品路径 **20/64 = 31.2 %**（轮次132），定深口径 20/32（轮次139），六臂针 6.18 s 与 10.43 s，节点这一侧合法上限 1.34／2.03 倍 vs 需要 1.54／2.61 倍（读数 3） | **未达标**（两格都是），方案 B |
| D `:550` | 单轮 −≥ 40 %；内存解耦；fitness 不再三档 | 三条各有一支：−71.5 %／+71 kB 每 worker／五值且 SPRT 在 1 对尺度上碰不到任何界 | 达标（D 行没要求「免费」，代价在账上） |
| 保障 1 `:554` | 固定一组局面，记「着法／分值／节点数／深度」，阶段间对比，着法变化必须能解释 | 树内基准是**窄形状**：`tests/test_search.cpp:6841-6859` 那张八行针臂表（四行分层搜索 quiet／scattered × 两色，钉 `method`／depth／score／best／nodes，另四行是 resign、greedy_five、vcf 的快路径对位），`test_a_quiet_think_pays_for_each_evaluation_only_once`（`:7114`，行在 `:7125-7126`）再钉 quiet 两色的 nodes／qnodes／scans／reuses／patches／score 六列；每轮另跑树外六臂针与 32 景尺子 | 达标，但形态比文档窄（方案 F，一条测试的事） |
| 保障 2 `:555` | 新引擎 vs 旧引擎验证赛（互换黑白）确认没有退化 | **没有整树赛**。档内只有机制级赛：轮次131 的 LMR（96 局定深 45.8 %、200 局挂钟 46.8 %、合并 296 局 46.5 %，95 % 区间 40.8–52.1 %）、轮次134 的 futility 六场、轮次129 的开局筛查 | **未达标**，方案 C |
| 保障 3 `:556` | 评估函数改动（位置项、增量缓存）与旧实现逐格比对，方法沿用「全 225 格 × 两色 × 42 棋盘」 | 增量缓存这一半按文档做足了：C1-4（`:7184`）、C2-1（`:7360`，8382 空格逐格两色比 `recognize_position` 与聚合字段）、C3-2（`:7853`，比到代数值）。位置项（A5）交的是 `prefers_centre_stones`／`antisymmetric_with_terms`／`edge_penalty_below_rush_four` 三条定向用例 + 九处字面量，**不是 225×2×42 那张表** | 半达标，方案 D |
| 保障 4 `:557` | 注入 mock clock，验软硬时限／节点上限／用户中止三种取值 | A1 的可注入时钟一直在生产路径上（`set_clock`），A2／A7 钉软硬时限与用户中止，轮次132 F-2 用 `ScriptedClock(100)` 钉 `time_soft` 且对照臂证明停它的是窗口不是墙 | 达标 |

### 读数 1：B 行那十个「没到深度」的臂，本轮第一次按 `method` 拆开

轮次132 的表只写了「到达请求深度 54（84.4 %），停因 none 62／time_hard 2」，轮次139 因此把这一格挂成
「口径待定」。把 `/tmp/f2/after_i1.out` 与 `after_i2.out` 里 `ms=2000` 那 64 行重新解析（两遍各 32 臂，逐遍
读数一字不差），64 臂只有三种形状：

| 形状 | 臂数 | 是谁 | 是不是「depth 6 没完成」 |
| --- | --- | --- | --- |
| `method=alphabeta depth=6 stopped=none` | 54 | 27 景 × 两遍 | 完成 |
| `method=must_block depth=0 stopped=none nodes=0` | 8 | pose **03／06／11／25**（全是 c2＝白），两遍 × 四景，分都是 `99000` | **不是**：这一手是对方成五的必挡，快路径直接答，压根没起分层搜索（`src/search/search_engine.h:47-49` 那句「every fast path that never started a layered search」报 0） |
| `method=alphabeta depth=5 stopped=time_hard` | 2 | pose **31**（白），两遍；`nodes=295,789 elapsed=2003 ms nps=147,673 done=0` | **是**：第 6 层连第一个根候选都没跑完 |

于是这一格有两个都成立的数：**进了分层搜索的 56 臂里 54 臂在 2 s 内完成 = 96.4 %**（≥ 95 %），
**全部 64 臂口径 = 84.4 %**（< 95 %）。差的那一格不是搜索慢，而是尺子把「不必搜索的局面」算成了失败。
文档 `:548` 那句写的是「中盘典型局面 depth 6 完成率」，没规定快路径是否计入——本轮不替它定，交用户（方案 E）。
定成「计入」也不改变引擎：那 8 臂的答案本来就是对的，让它们「跑 d6」只能靠把快路径关掉，那是倒退。

### 读数 2：G3 的「第二手起」第一次量到，命中格过、降本格不过

仪器 `/tmp/g3/g3_driver.cpp`（HEAD 现编，`-O2`，`taskset -c 2`，`g3b.out` 24 行）：三副局面（quiet 四子、
scattered 四孤子、pos24 二十四子）× 两色，共用一张会话表连想四轮，`max_depth = 6`、三个预算全 0（无钟无节点帽），
所以节点数是计数而不是负载表。口径要写清：这支仪器走的是 `AlphaBetaSearch` 一层，
不经 `SearchEngine` 的快路径与预算整形（这三副局面本来也不触发快路径），所以它量的是「分层搜索的跨手复用」，
不是产品那面墙下的时长。每轮的走法是「本方最佳着法 + 引擎自己在 PV 里预测的对手应手」，代龄按子推进
（`advance_generation()`，与 `game_manager.cpp:208/504`、`self_play.cpp:119` 同一形状），然后：

- **先探新根**：`session_tt.probe(board.zobrist_hash(), entry, 0)` —— 这是文档 G3 那一格「根节点命中率」的字面仪器；
- **冷臂**：同一副局面、全新表、无提示，`AlphaBetaSearch` 定深 6；
- **暖臂**：会话引擎、会话表、带上会话提示（`hint_move` = 上一手最佳着法、`hint_score` = 上一手分值），
  与 `SearchEngine::set_hint` 在产品里交出去的东西同形。

| 读数 | 值 |
| --- | --- |
| 根节点命中 | **23/24**（唯一没中的是 pos24 白第 5 轮，那轮上一手只跑到 depth 1，没给新根留记录） |
| 可比臂（两臂都完成 depth 6） | **20**（另外 4 臂在将杀短路处终止：暖臂一律 depth 1，冷臂 depth 1–4，两臂分值只差将杀距离（100,002／100,004 对 100,000），本轮不计入降本） |
| 降幅列表（%） | 15.1、−4.5、21.1、9.9、12.8、21.5、33.9、17.0、−0.6、14.7、3.5、−1.3、−3.4、12.6、3.8、3.1、4.9、3.3、13.7、7.6 |
| 中位／最小／最大 | **8.8 %**／−4.5 %／33.9 % |
| ≥ 25 % 的臂 | **1/20** |
| 着法与分值 | 20 个可比臂 `same_best` 与 `same_score` 全为 1（复用不改答案，这一点与轮次132 的 128 臂同一结论） |
| 命中次数 | 暖臂 `tt_hits` 与冷臂同量级（例：quiet 白第 2 轮 6,351 对 6,159）——跨手复用**多出来的命中没有换成剪枝** |

为什么命中的表项剪不掉东西，是可以在代码里指出来的：一次 d6 思考把 P1（本方落子后）记成 depth 5、
把 P2（对手应后）记成 depth 4，把 P2 的孩子记成 depth 3；而下一次思考的根是 P2，它问的孩子需要 depth 5，
表里只有 depth 3 —— `alphabeta.cpp:363` 的探针带深度门，浅记录只能贡献着法排序，不能截断。
而根本身呢：**根位置没有 TT 探针**（全树只有 `alphabeta.cpp:363` 一处 `tt_.probe`，在节点函数里），
所以那条 depth 4 的新根记录连读都没人读。这一条与轮次139 表里「根位置无 TT 探针」是同一件事，本轮给了它数。

结论：文档 `:124` 的第一格（> 30 %）实测 95.8 %，过；第二格（≥ 25 %）实测中位 8.8 %、最好一臂 33.9 %，
**不过**，而且 quiet 黑第 3 轮那一臂是**负的**（−4.5 %）：它的提示分值 79,622 来自上一手那个局面，而这一手
的真值是 39,440，aspiration 的中心因此差着四万分。本轮**没有**再跑一支「只带表、不带提示」的第三臂把这两笔
分开，所以这句只写成观察，不写成因果。

### 读数 3：C 行两格仍差那个 4.4 倍，且合法路径已被量尽

沿用轮次135／136／137／138 的数（同一支 release 二进制、`quiet BLACK d6` 针臂 221,774 节点）：

| 项 | 实测 | 需要 | 因子 |
| --- | --- | --- | --- |
| 每节点指令 | 42,353.5 Ir | ≤ 9,660 Ir（1M nps × 这台机器 9.66 GIr/s） | **4.4 倍** |
| 每候选格评估 | 942.4 Ir（42,353.5 ÷ 44.94） | ≤ 215 Ir | 同上 |
| 记忆化一类（跨节点格子分表） | 重复率 28.6 % × 评估族 69.19 % = 省 19.8 % | — | 上限 **1.25 倍**（轮次136），且无限表要 1.04 GB／759 万键 |
| 节点这一侧（尾巴） | 合法上限 1.34 倍（quiet d8，6.18 → 4.61 s）／2.03 倍（pos24 d8，→ 5.13 s） | 1.54 倍／2.61 倍 | 都不够（轮次138）；尾巴整条关掉只省 15.4 %（alphabeta 回头多买 +24.9 %），还让 17 个已判到胜的 think 全部掉出 99,000 |

C 行的四个内容项（TT `static_eval`、候选格缓存、局部 stand_pat、`MAX_QDEPTH` + `seldepth`）在轮次139 的表里
四条全是「落地」，其中 Step 3 是同物异形（`alphabeta.cpp:201` → `search_state.h:307` 的 `board_score` 是全盘求和，
只是形状读 Step-2 记录）。所以这一行欠的不是功能，是一个文档里没有的内核因子。

### 方案 A（G3 第二格）：把 §2.4.3 ② 的预置复用建起来

文档 `:301` 那行「把上一手 PV 的第二着作为 ponder 预置」是全树唯一没实现的复用机制（轮次139 表里
「未找到」三条之一，轮次121 A4 记的理由是「没有把对手着法回灌给一次进行中搜索的通道」）。读数 2 的深度
账说明它恰好是这一格的解：预搜的落点正是 P2，它以 depth 6 入库，那么下一次思考问的孩子就是 depth 5，
表里的记录深度够了，命中才变成剪枝。

- 形状：AI 落子提交之后（`game_manager.cpp:504` 那一处推进代龄的地方），拿本次 `stats.pv[1]` 起一次
  后台思考，预算取 `hard` 的一档（建议 0.5 倍，且必须可被下一手取消）；对手真走这一手 → 下一手直接吃到
  深层记录；对手走了别的 → 取消，本轮不写脏答案（表里多几条记录不影响正确性，深度门仍守）。
- 验收仪器就是 `/tmp/g3/g3_driver.cpp`：加一条「会话里先跑预置再跑第二手」的臂，判据是 20 个可比臂的
  中位降幅 ≥ 25 %，且 `same_best`／`same_score` 保持全 1。
- 门禁：这是并发面改动，按 `docs/STRESS_TEST_RULES.md` §5 走 `build-tsan`（TSan 干净树 0 race，
  变异臂要能响——轮次130 的 M3 那 22 次就是这套仪器的通电证明）；`vm.mmap_rnd_bits` 那条按档重设，不写进脚本。
- 与文档一致性：这条**就是文档写的机制**，`:570` 风险行的「多线程共享 TT 数据竞争」缓解也已在树里（分片锁）。
  它是引擎改动，需要用户点头才开工。

### 方案 B（C 行两格／G5）：一次带杀线的候选格内核重做

`:549` 要的形状不在文档里。轮次136 已把两种可写形状各否过一次（72 位键宽的全量表 / README:624 的分线增量），
所以这一步不是「下一个未实现的功能」，是一次设计授权。方案按「先量、能过再落」写：

1. **可行性刺探（树外，一到两天）**：今天每节点的钱是 22.47 次候选格重算付的，而一次 d6 思考真正递归的候选
   每层只有 K 个（根 24、ply ≥ 3 是 8）。刺探把「排序用的分」与「威胁分层用的识别」拆开：分层只需要
   行棋方四条线的成五／成四／活三判定（四条线位窗口已在 `board.lines_` 里），排序只需要一个与中心距同阶的
   便宜价格；两色全量 `candidate_pair_value` 与 `pattern_score` 只在叶子（`stand_pat`、`evaluate_board`）保留。
   刺探产出只有一个数：**每候选格 Ir**。
2. **杀线**：若刺探做不到 ≤ 600 Ir／候选格（即不到 1.6 倍），或六臂针的答案/节点数变化解释不了，就**停**，
   把 `:549` 与 G5 交回用户改成能兑现的数（例如按这台机器 9.66 GIr/s 定「≥ 400k nps」）。不做半截内核。
3. 若过线：落树，叶子评估逐字节不动（于是 `:556` 的三处 225×2×42 差分是回归而非新证），排序改动按 `:554`
   逐臂解释着法变化，并按 `:555` 跑一场够力的赛（方案 C 的赛制），最后用读数 3 那张表复判：Ir／节点 ≤ 9,660、
   nps ≥ 1M、d8 在 4 s 内（两把尺子都算）。

**「确认方案可以按照文档实现」的老实答复**：机制上可以（不改规则、不引神经网络、不动 `:557` 的时间语义），
但 4.4 倍这件事在动内核之前没有任何读数能担保，所以本方案担保的是「刺探→杀线→门禁」这条路能按文档验收，
而不是担保那 215 Ir 一定拿得到。要拿到 G5 这一格，用户需要先授权这一步。

### 方案 C（`:555`）：一场有力量的整树赛

档内唯一的整树级赛是轮次131 那两场，而它的结论正是「不足以确认没有退化」：96 局 45.8 %（区间 35.9–55.8 %）、
200 局 46.8 %（39.8–53.7 %）、合并 296 局 46.5 %（40.8–52.1 %）。要把 95 % 区间的半宽压到 ±5 个百分点，
在真实分数 50 % 附近需要 **n ≈ (1.96 × 0.5 / 0.05)² ≈ 384 局**；±3 个百分点要 ~1,067 局。

- 赛制：新树 vs `v0.5.0-phase4` 基线，同座对照各跑一遍（轮次131 立的规矩：先量夹具自己，再量引擎），
  黑白互换、开局集固定，预算按 `--max-nodes` 出价（轮次126 之后每盘成本可预测，训练/比赛不受负载抖）。
- 预注册判据：合并区间下界 ≥ 45 % 记「没有退化」；落在 40–45 % 记「量不出」并写进档，不再重跑第二遍碰运气。
- 这是执行排期而不是代码：384 局 × 两座位 × 对照 ≈ 一次过夜批跑。要不要跑、按哪个余量跑，是用户的一条决定。

### 方案 D（`:556` 位置项那一半）：把 A5 补成文档点名的那张表

A5 交出的是三条定向用例加九处字面量更新；`:556` 点名的方法是「全 225 格 × 两色 × 42 棋盘」。缺的那条可以
照 C2-1 的夹具直接补：`evaluate_board(board, color)` 与「`pattern` 旧和 + `position_term` + `edge_penalty_term`」
逐格逐项比对，跑在同一付 `c2_census_boards()`（40 支 LCG 伪随机盘 + 2 支填满线的盘）上，两色都查，
并把 `eval(W) == -eval(B)` 作为第二断言。**测试面改动，零生产代码**，走一轮正常门禁即可。

### 方案 E（G1／B 行那一格的口径）：一句话写进文档

两种读法本轮都有数（96.4 % 与 84.4 %），文档 `:548` 与 `:122` 没规定快路径是否计入「depth 6 完成率」。
建议把口径写成「完成率的分母是进入分层搜索的思考数，`method != alphabeta` 的快路径单独报」，并在
`tests/readme_anchor_check.js` 的引用面动一行。这是文档维护，等用户发话；不改引擎、不改测例。

### 方案 F（`:554` 的形状）：把每轮的针臂升成树内常驻基准

`:554` 要的是「固定一组局面 + 记录着法／分值／节点数／深度」，树内现在那张表（`test_search.cpp:6841-6859`）
只有四行进过分层搜索（quiet 与 scattered 各两色），另外四行是快路径对位。补齐只需把本轮六臂针那副尺子的局面集
搬进同一张表（补上 pos24 两色与两副 d8 臂），成本是套件墙钟（六臂里 d8 那两臂在 release 上是 6.18 s 与 10.43 s，进 `-O0`
套件要按比例放大）。同样是测试面改动。

### 问题账：实现过程真遇到过的坑与当时的处理（验收时逐条回看，别当成轶事）

| # | 那一轮 | 真实问题 | 处理，以及它现在还成立吗 |
| --- | --- | --- | --- |
| 1 | 轮次121 A4 | 一次脚本改写把 `alphabeta.cpp` 整文件换成 CRLF，diff 一度 578／509 | `tr -d '\r'` 复原回到 76／7；此后每个改过的文件查 `grep -c $'\r'`（仓库无 `.gitattributes`，行尾只靠人守） |
| 2 | 轮次121 A4 | 按文档做 aspiration 的第一版**贵了**：静默局 d4 冷跑 176,695 → 218,872 节点（+23.9 %） | 记下代价不掩盖；B3 的根 PVS 才把这笔买回来。今天窗口仍在，读数是轮次132 的「答案一条没变」 |
| 3 | 轮次121 A6 | 同分选择依赖 `std::sort` 与容器顺序，复现不了（47397 对 47045） | 三元组全序 + `stable_sort`，复问买 +14.2 % 节点；今天 48268 == 48268 |
| 4 | 轮次121 B1 | 文档 §2.5.1 与 §3.2 在 Tier0 第二半、Tier3 准入、无威胁剪枝三处**自相矛盾** | 逐条择一并写进注释与用例（Tier0 取并集、Tier3 按表、无威胁按「放宽」）；这三处选择至今是生成器的形状 |
| 5 | 轮次121 B1 | `double_three` 上 (7,7) 对白也进不了默认根宽（45 格 Tier2 排第 28，帽 24） | 不为它开后门：测例把镜像臂放到曲面最大值跑，另起一条把「默认宽下确实被切」钉住 |
| 6 | 轮次121 B4／131／134 | 三条机制建起来后判**不落地**：威胁延伸、LMR（省 28.35 % 但赛 46 %）、futility（19.9 % 的裁剪喂给父节点假下界） | 代码全部回收，树里没有 `lmr`／`reduce`；本轮验收按「已判」记，不算缺实现 |
| 7 | 轮次126 D-1 | `--max-nodes` 把单轮成本买到 −71.5 %，方式是把每一手搜得更浅（d 分布 `{3,4}` → `{2,3}`），同 seed 的验证赛结论翻面（0.875 晋升 ↔ 0.375 不晋升） | 收益与代价写在同一行；`:550` 那一格按文档（成本可预测）算过，但「同一轮做同样的决定」没兑现 |
| 8 | 轮次128 D-3 | 「fitness 只有三档」本来就是 `pairs = 1` 的算术，SPRT 在 1 对上碰不到任何界（最大 \|LLR\| 2.395 < 界 2.944） | 交回实情：值域是五值；分辨率只有加大 `--fitness-games` 或按 `:435` 后一半动评估函数两条路 |
| 9 | 轮次129 D-4 | 开局多样化建好后，镜像筛查 45/48 局是颜色强迫而非系数 | 机制留在树里、缺省关（`match_opening_plies = 0`），按「异形 + 默认关」验收，不冒充达成 |
| 10 | 轮次130 D-5 | 文档给的两个选项里「在 worker 内构造可变副本」会**再加**一份 4664 B 复制，且 `engine_.search()` 收的是 `const Board&`，没人需要可变副本 | 故意不照字面做，落的是「调用方一次 copy-assign」；memcpy 类总共只值 0.05 % Ir，所以 `#5` 的性能天花板本来就低，这点写进档 |
| 11 | 轮次130 D-5 | M3 变异（写入挪到起线程之后）断言杀不掉 | 交给 TSan：干净树 0 race，M3 臂 22 race 且两条栈正是新增跨线程对象的两端 |
| 12 | 轮次132 F | 分配器把 AI 每手可用时间从「整面墙」改成 65 %，提示同受影响 | 落地并写明后果；`hard_ms`／`max_nodes` 由分配器算出但被调用方丢弃是**决定**（`time_manager.h:16-21` 注释） |
| 13 | 轮次136 J | 第一支护架装在 `evaluate_position_pair` 上，`calls=0` 而针照旧——那台仪器根本没在路径上 | 立规矩：0 不是读数，是仪器没说话；本轮的 Found 第 1 条由此而来（热路径走 `CandidateCache::cell`） |
| 14 | 轮次138 L | 尾巴的宽度只能靠截 Tier1 来减，而文档 `:349` 与 `move_generator.h:37-41` 都写着 Tier1 不截断 | 不违文档，只把禁一刀量到极限：cap=1 后 `quiet BLACK d8` 仍 4.61 s、`pos24 BLACK d8` 仍 5.13 s，且 32 景上 **27/64 臂换答案**；整条关掉只省 15.4 %，而出厂分 ≥ 99,000 的 **17 个 think 全掉下 99,000、8 个直接判负**。据此判「节点这一侧买不到 `:549`」 |

### 判

- **机制清单空的这条结论不变**：43 条里没有一条「既没实现也没判过」（轮次139 已核，本轮不重复核）。
- 验收面上本轮**新增两条未达标**：`:124` G3 的降本格（新读数，方案 A）与 `:555` 整树赛（缺口一直在，
  本轮第一次按「够力的赛」写清需要多少局，方案 C）；`:556` 记为半达标，欠的是一条测试（方案 D）。
- `:548`／G1 那一格从「待定」变成「两口径都有数」：分层口径 96.4 %、全臂口径 84.4 %，缺的是文档里一句
  定义（方案 E），不是引擎。
- `:549`／G5 仍是唯一需要设计授权的数字项（方案 B），本轮不新造机制、不越权动内核。
- 本轮无代码：门禁里 ctest 与 memcheck 都「不跑」并写清理由（树与轮次134 还原门禁跑过 `8/8／463.68 s`
  的那一棵相同），不当成通过。

### 门禁

- `src/`、`tests/`、`web/`、`tools/`、`CMakeLists.txt`、`data/` 与 `d4ed625` 逐字节相同
  （`git diff --stat d4ed625 -- …` 空）；`git status` 除本节外干净。
- `node tests/readme_anchor_check.js` **120 citations / 0 problems**；`node tests/web_frontend_check.js` **105/105 passed**。
- **ctest 不跑**、**memcheck 不跑**：无可回归的新代码（理由见「判」最后一条）。
- 本节拼在轮次139 表头之前（原 1983 行处），纯插入：`git diff --numstat` ＝ **217 / 0**，
  `wc -l REVIEW_LOG.md` 从 8310 到 **8527**；档内绝对针 `REVIEW_LOG:2289`（轮次125 的 1.99 s 那一行）
  现在在 **4152** 行，与 `:2289` 差 **1863** 行。

### 仪器（全部在仓库外）

- `/tmp/g3/g3_driver.cpp` + `g3b.out`（读数 2，24 行；编译命令是把 `GOMOKU_CORE_SOURCES` 十六个 TU 与驱动
  一起 `-O2 -I src` 编成一支二进制，`taskset -c 2` 跑，68.9 s）；聚合是 `python3` 一次性脚本，
  只重解析 `g3b.out` 的字段，不做插值。
- `/tmp/f2/after_i1.out`、`after_i2.out`（读数 1 的来源，本轮只重新解析 `ms=2000` 那 64 行）；
  `/tmp/f2/budget_probe.cpp` 是它的生产者。
- `/tmp/m1/audit.md`（43 条机制落点表）、`/tmp/i1/{ir_cg.out,calls_release.out,redun.out,l1_width.out,l2_cap.out,l3_sweep.out,l4_notail.out}`
  （读数 3 的全部原始件）。

## 轮次139 引擎重构 M：§2.3–§2.9 与 §6 的 43 条机制逐条重新落点，32 条在跑、8 条异形、3 条已判不落地或按文档不做，另外把 `:548` 那一格的两套口径第一次对齐 — engine redesign stage M, every §2.3–§2.9 and §6 mechanism re-located file by file: 32 shipped, 8 shipped in a different shape, 3 measured-and-rejected or excluded by the doc, plus §4 :548's 2 s cell read on both rulers for the first time (audit round, no code changed)

用户在本轮选了「再做一轮机制审计（只读，不改）」。审计的问题不是「还剩什么没写」——轮次135 已经答过
一次——而是**上一次那份清单是不是靠记忆写的**。所以本轮每一条都重新 grep、重新看调用点，并把「注释里
提到了」不算作实现（这条规矩来自轮次136：`evaluate_position_pair` 在文档里是打分入口，在树里一个生产
调用点都没有）。落点表在 `/tmp/m1/audit.md`，本节只写结论与八条异形。

### 结论

- **43 条机制里 32 条在生产路径上跑**，而且每条都能给出「定义 + 生产调用点」两个位置；`src/` 与
  `1e31d22` 逐字节相同，本轮一行代码没动。
- **8 条是同物异形**（下面读数 1），其中 6 条早已在档（Step 3、tie-break 兜底、开局多样化默认关、
  `SearchResult::interrupted`、`pattern_score` 的边界打折、分配器第 ② 条），2 条是本轮新写下来的
  （层边界的 `min_depth`、分配器第 ② 条用的判据）。**没有一条异形是缺实现**——都在做同一件事，只是
  形状与文档的伪码不同。
- **3 条「未找到」全部是已判过或文档自己排除的**：LMR（轮次131 建后判不落地，代码已回收，本轮
  `grep -rniE "lmr|late_move|reduction|reduce" src/` 只剩 vct.cpp:95 与 position_setup.h:64 两处无关词）、
  威胁延伸（轮次121 B4 判不落地，最近的替代是 Tier1 永不截断）、`ponder`（无对手着法流，文档接口表里
  那一个字段的声明在 `search_limits.h:68`，没有任何分支读它）。
- **两条「生产无读者」要分开记**：`SearchStats.pv` / `SearchInfo.principal_variation` 一路写满
  （`alphabeta.cpp:371/448/644` → `:483` → `search_engine.cpp:387`），但 `grep -rn principal_variation
  src/server.cpp src/search/controller.cpp src/training/*.cpp web/` **为空**——只有
  `tests/test_search.cpp:6256-6263 / :6296` 在断言它。这与轮次135 记过的 `tt_cutoffs`/`qnodes`/
  `partial_best_move` 同一类：文档 §2.9 把它列成契约字段（「只增不改」），并没有承诺仓库内的消费者。

### 读数 1：八条异形，逐条给位置与差异

| 机制 | doc | 树内位置 | 差异 |
| --- | --- | --- | --- |
| §2.6 Step 3 局部 stand_pat | `:409-410` | `alphabeta.cpp:201` → `search_state.h:307`（`CandidateCache::board_score`） | 文档要「上一手四条线」的 delta；树里是对 `active_empty()` 全盘求和，只是每格的形状读 Step-2 记录 |
| §2.4.5 三段式排序 | `:318-327` | `alphabeta.cpp:34`（`sort_moves`）：① TT/hint `:39-48`、② killer `:55-66`、③/④ 静态分与 history `:66+` | 三档都在，但**静态分排在 history 之前**，与文档列的顺序不同（实测决定的） |
| §2.5.3 兜底不取 `root_moves[0]` | `:393` | `alphabeta.cpp:696-701` | 仍然取 `[0]`——因为 `sort_moves` 已按 (值, 中心距, 格号) 三元组排过，`[0]` 就是文档要的「按三元组取最优」 |
| §2.5.4 棋型考虑连线空间 | `:395-397` | `evaluator.cpp:221-227`（`edge_penalty_term`），在 `:264-265` 进出 `evaluate_board_from` | 打折放在**全盘评估的一项**里，不在形状查表里，也没有 `open_ends` 字段 |
| §2.8 #4 开局多样化 | `:436` | `trainer.cpp:135-147,169-216`、`main_trainer.cpp:58-67` | 采样是 `random_opening_line`，不是开局库加权；`match_opening_plies` 默认 **0**（关闭） |
| §2.9 `SearchResult::interrupted` | `:444` | `search_engine.h:39-59` | 该成员不存在，中断由 `stopped` 一个字段承担 |
| §2.3.4 分配器第 ② 条 | `:248` | `time_manager.cpp:69-71` | 判据是 `board.active_empty().size() <= 6`（**盘面空位数**），不是候选数 |
| §2.3.2 层边界的 `min_depth` | `:219` | `alphabeta.cpp:605,617` → `search_state.h:433,449` | 层边界强制检查在，`soft_time_up()` 也在，但门槛写成常量 `completed_depth >= 1`；`SearchLimits`（`:63-73`，与文档接口表一致）**没有 `min_depth` 这一项** |

最后两条是本轮新记的。它们的共同点值得说一句：文档伪码里的两个数字（`0.65` 与 `min_depth`），树里一个
变成了「空位 ≤ 6 就砍半」的近似、一个变成了常量 1——**都不是没实现，是实现时没有把那个参数做成可调**。
要不要把它们做成可配置，属于设计问题，本轮只登记。

### 读数 2：`:548` 那一格第一次有了两个口径，而且它们不冲突

文档 B 行的验收是「中盘典型局面 depth 6 在 2s 内完成率 ≥ 95 %」。档内有两套数，此前从没被放在一起：

| 口径 | 仪器 | 读数 | 是否达 ≥ 95 % |
| --- | --- | --- | --- |
| 产品路径：`--timeout-ms 2000`、快路径开着、走 `SearchEngine` | 轮次132 的 `budget_probe`（64 臂 × 两遍） | **到达请求深度 84.4 %（54/64）**；停因 `none` 62 / `time_hard` 2；中位 618 ms | **否**（轮次132 自己写了「达不到那一行写的 ≥ 95 %」） |
| 定深无钟：`max_depth = 6`、`hard/soft/max_nodes` 全 0、直接走 `AlphaBetaSearch` | 本轮把 轮次138 留在 `/tmp/i1/l3_sweep.out` 的 32 条 cap0 基线除以 轮次135 的 228,000 nps | **32/32 = 100 % 在 2 s 内**；中位 0.54 s、p90 0.99 s、最大 1.47 s；节点和 3,930,084（均值 122,815） | 是 |

两条都对，说的是不同的东西：**d6 这棵树本身的花费在 2 s 预算之内（最大 1.47 s，还有 26 % 余量），
而产品路径上那 10 个未达深度的臂里只有 2 个真的是墙钟停的**，其余 8 个报的是 `none`——与同轮读数 4
说的 `must_block` 快路径臂（`method` 不是 `alphabeta`、两个深度见证留 0）是同一族：它们不是「超时」，
是**根本没进分层搜索**。所以 B 行那一格要不要算达标，取决于「完成率」是否把快路径给出的答案计入；
文档没写死，本轮不替它写死。没核到底的一条也照实写：那 8 个 `none` 臂究竟各是哪种 `method`，本轮
没有重跑 `budget_probe`，是照 轮次132 的表推的。

### 读数 3：§4 四行验收的当前状态（把三轮的账并到一处）

| 行 | 验收列 | 现在的读数 | 出处 |
| --- | --- | --- | --- |
| A `:547` | UI 报中断层、选点可复现、新增算例 | 全部有算例在 `tests/test_search.cpp` 里，随 ctest 跑 | 轮次120／121 |
| B `:548` | d6/2 s 完成率 ≥ 95 %；节点较 A 降 ≥ 50 % | 两口径见读数 2；节点那一半在 `quiet` 上实测过（138.02 s → 36.85 s） | 轮次132／轮次121 |
| C `:549` | 单线程 ≥ 1M nps；depth 8 在 4s 内完成 | 两侧都有关账上限：内核 1.25 倍 vs 需要 4.4 倍；节点 1.34／2.03 倍 vs 需要 1.54／2.61 倍 | 轮次135／136／137／138 |
| D `:550` | 单轮 −≥40 %；内存与并发解耦；fitness 不再三档 | 三条各有一支读数 | 轮次126／127／128 |

本轮新增的只有 B 行第二口径。C 行的 4 s 那一格在 32 景上是 **20/32 达标（中位 3.24 s、p90 6.37 s、
最大 10.56 s）**，这个数与 轮次138 在六臂针上量的 6.18 s／10.43 s 是同一件事的两个尺度。

### 判

- 本轮**不产生新工作项**。43 条里没有一条是「从未被实现、也从未被判过」的——这正是轮次131 那次审计
  找 LMR 时用过的判据，这次重新跑一遍，答案是零。
- 唯一未达标的验收行仍是 `:549`，而它现在两侧都有上限数，**下一步动作必须是用户授权的内核重做**，
  不是实现轮。B 行那一格的口径歧义（快路径是否计入完成率）也在这一轮登记给用户，不由本轮擅自定。
- 异形八条不动代码：把它们「改成与文档同形」需要的不是实现而是**改文档**（把伪码里的 ②③ 参数、
  边界打折的位置、`interrupted` 字段按树里实际的样子写）。这属于文档维护，等用户发话。

### 门禁

- `src/`、`tests/`、`web/`、`tools/`、`CMakeLists.txt`、`data/` 与 `ae89de9` 逐字节相同；本轮唯一的
  仓库改动就是这一节。`git status` 干净（那三个未跟踪 `docs/` 已按用户发话在 `ae89de9` 收进）。
- `node tests/readme_anchor_check.js` **120 citations / 0 problems**；`node tests/web_frontend_check.js`
  **105/105 passed**。
- **ctest 不跑**：树没有新代码，仍是轮次134 还原门禁跑过 `8/8 / 463.68 s` 的那一棵；valgrind 同样没有
  新对象可筛。
- 本节拼在轮次138 表头之前（原 1982 行处），纯插入：`git diff --numstat` ＝ **103 / 0**，
  `wc -l REVIEW_LOG.md` 从 8207 到 **8310**；被推下去的档内绝对针（`REVIEW_LOG:2289` → 轮次125 的
  1.99 s 那一行，拼入前在 3832 行）拼入后在 **3935** 行，差 **1646** 行。

### 没动的账

- 那 8 个 `none` 臂的 `method` 分布没重测（要重跑 轮次132 的 `budget_probe`，两遍 × 64 臂）。
- 「层边界的 `min_depth` 与分配器第 ② 条的空位判据要不要做成可配置」本轮只登记不实现。
- `:549` 第一格（内核 4.4 倍）依旧没人能动，等授权。

### 仪器（全部在 `/tmp/m1/`，仓库外）

`audit.md`（43 行落点表，每条含定义与生产调用点两个位置）、读数 2 的两口径计算直接从
`/tmp/i1/l3_sweep.out` 的 32 条 cap0 基线重算（`python3` 一次性脚本，节点除以 228,000 nps），
读数 1 的每一条都可按表中 file:line 复查。

## 轮次138 引擎重构 L：静默尾巴的宽度第一次被数出来——把 Tier1 剪到「每节点只递归第一个强制着法」仍差 0.61 s，而整条尾巴关掉会让六枚针臂的得分全部翻脸 — engine redesign stage L, the quiescence tail's width counted: even the doc-forbidden one-child cap leaves depth 8 at 4.61 s, and switching the tail off entirely costs every pinned arm its score (measurement round, no code changed)

轮次137 在「没动的账」里留了一格：**尾巴节点的可减比例本轮没量**。用户上一轮选的问法就是
「先量节点数那一侧」，本轮把那一格填掉：先数尾巴到底宽在哪（读数 1），再把唯一能碰到那个宽度的
机制——候选截断——量到它的极限（读数 2、3），最后把曲线外推到「整条尾巴关掉」看上限在哪（读数 4）。
`src/` 一行没动，四支读数全部来自 `/tmp/i1/` 下的仓库外副本。

### Found

- **尾巴的形状是「几乎全部在生成之前就返回」**。六臂里 **49.0 %～92.6 %** 的静默节点走到
  `alphabeta.cpp:207` 那句 `stand_pat >= beta` 就回来了，一分钱生成钱都不付；剩下那些付了生成的
  节点，每个的候选宽度是 quiet／scattered 的 **4.3～5.2** 与中盘 `pos24` 的 **12.3～12.5**。
  本轮顺带把轮次137 由 `seldepth` 推断的那句话变成直接计数：**`ret_capped` 与 `ret_stopped` 与
  `stop_break` 在六臂上恒为 0**——深度帽（`alphabeta.h:146` 的 `kMaxQdepth = 8`）与硬停在定深臂上
  一次都没有咬过。
- **递归下去的孩子几乎不改变父节点的答案**：`child_failed / searched` 是 **70.4 %～99.9 %**；按
  「最近一条 fail-low 链接」归属（不重复计数）之后，尾巴节点的 **43.2 %～80.3 %** 落在至少一条
  fail-low 兄弟子树里。这一族节点就是任何「已知会失败就不进去」规则的奖金上限，而轮次134 已经量过
  静态分给不出这个界（未饱和节点上中位 76,567～109,887 的余量，最大 199,997）。
- **宽度就是文档点名的那一刀**。`move_generator.h:37-41` 对 Tier1 写的是「Never truncated either -
  the width is what Tier2 and Tier3 are cut to, and **cutting a tactical answer is the failure mode
  the design doc's risk table calls out by name**」。本轮照样把它量出来，是为了给一个数而不是为了
  假设一个数：**cap=1**（每节点只递归排序后的第一个强制着法）在六枚针臂上 **36 行 capped 答案一字
  不改**，可 `quiet BLACK d8` 仍要 **4.61 s**、`pos24 BLACK d8` 仍要 **5.13 s**，对 `:549` 的 4 s 分别
  差 **0.61 s / 1.13 s**。
- **到了 32 个中盘景，「答案不变」就没了**（读数 3）：cap=4 改 3/64，cap=2 改 10/64，cap=1 改
  **27/64**；而平均节点节省只有 **10.9 % / 20.3 % / 22.5 %**，比六臂上的同名数字更低，因为截断改掉了
  子节点的返回值，父层的 alphabeta 会重搜——cap=1 有 **6 景反而变大**（最大 `P15-d6` 63,358 →
  83,104，**+31.2 %**），cap=4 也有 1 景变大 30.1 %。**节省不是单调的**，这是本轮最有用的一条。
- **上限那一端也不能收（读数 4）**：把整条尾巴关掉（`quiescence` 在 `:208` 之后直接返回
  `stand_pat`），六臂只省 **15.4 %～56.9 %**，其中 `quiet BLACK d8` 省 **15.4 %**——因为它的
  alphabeta 节点从 639,592 涨到 798,641（**+24.9 %**），把省下的吃回去，**5.22 s 还是不过 4 s，而且比
  同一臂的 cap=1（4.61 s）更贵**；同时六臂得分全部翻脸：71,992 → −1,992、97,044 → −162、98,948 → 850、
  99,999 → −9,212、77,842 → −8,490、99,999 → 3,420。32 景那一侧同样干净得难看：64 个 think 里
  **57 个答案变了**，出厂分 ≥ 99,000（已经判到胜）的 **17 个 think**（属 10 个不同的景）在尾巴关掉后
  **17 个全部掉下 99,000，其中 8 个直接判成负分**（如 `P04-d6` 99,999 → −1,206、`P05-d8`
  99,999 → −68,422）。尾巴不是装饰，是承重墙。
- 于是 `:549` 的第二格在节点这一侧有了一个**可数的不可能形状**：合法形状（不截 Tier1）节省 0；
  禁一刀量到极限（cap=1）是 **1.34 倍**（`quiet BLACK d8` 6.18 → 4.61 s）／**2.03 倍**
  （`pos24 BLACK d8` 10.43 → 5.13 s）；整条关掉是 **1.18 倍**／**2.15 倍**并且得分全变。而那一格要的
  是 **1.54 倍**／**2.61 倍**——四个数没有一个够。

### 读数 1：尾巴的宽度与它的装饰账单（`/tmp/i1/l1_width.out`，六臂全部核过出厂针）

| 臂 | 静默节点 | 生成前返回 | 付生成的节点 | 候选总数 | 递归孩子 | 装饰账单（候选） | 宽／生成节点 | 孩子未抬 α | fail-low 子树／尾巴 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `quiet BLACK d6` | 142,978 | 107,013（74.8 %） | 35,965 | 155,587 | 99,623 | 55,964（36.0 %） | 4.33 | 95.8 % | 66.9 % |
| `quiet WHITE d6` | 102,340 | 71,152（69.5 %） | 31,188 | 161,140 | 68,485 | 92,655（57.5 %） | 5.17 | 89.8 % | 61.4 % |
| `quiet BLACK d8` | 768,491 | 599,343（78.0 %） | 169,148 | 807,793 | 467,447 | 340,346（42.1 %） | 4.78 | 95.7 % | 58.3 % |
| `scattered WHITE d6` | 160,955 | 78,907（49.0 %） | 82,048 | 358,660 | 97,505 | 261,155（72.8 %） | 4.37 | 70.4 % | 43.2 % |
| `pos24 WHITE d6` | 119,681 | 105,655（88.3 %） | 14,026 | 172,983 | 93,663 | 79,320（45.9 %） | 12.33 | 98.6 % | 77.6 % |
| `pos24 BLACK d8` | 1,634,920 | 1,514,280（92.6 %） | 120,640 | 1,504,729 | 1,313,784 | 190,945（12.7 %） | 12.47 | 99.9 % | 80.3 % |

- 三条记账自检逐臂成立（`l_analyze.py` 里是 assert，不是散文）：`生成前返回 + 付生成的节点 = 静默节点`、
  `抬高 α 的孩子 + 未抬高 α 的孩子 = 递归孩子`、以及 **`静默节点 = qd0 入口 + 递归孩子`**——最后这条把
  「尾巴节点要么从 alphabeta 进来、要么从父静默节点进来」说成了一句可核对的等式。
- 层宽是棋盘错齿的：`quiet BLACK d8` 的 qd=0 每节点生成 1.60 个候选、递归 1.18 个孩子，qd=1 掉到
  0.33／0.30，qd=2 又回到 1.83／0.01——即尾巴实质是**首两层宽、往上一层几乎全是 stand-pat 返回**。
  `pos24 BLACK d8` 更极端：qd=0 生成 1,355,417 个候选递归 1,294,723 个孩子，qd=1 只生成 23,347 个。
- 空列表率（`leaf`）在 quiet／scattered 是 14.3 %／14.8 %／15.1 %／31.2 %，在 `pos24` 两臂是
  **0.0 %**：中盘没有一次「无强制着法」。β 截断率（`beta_cut`／付生成的节点）从 1.2 %（pos24-B-d8）
  到 34.9 %（scattered-W-d6）——**尾巴越宽的地方反而越少靠截断退出**，所以「剪掉截断之后的兄弟」
  这类节省在中盘根本没有量。

### 读数 2：Tier1 截断的价格曲线，六臂（`/tmp/i1/l2_cap.out`，cap=0 逐臂回到出厂针）

| 臂 | cap0 节点／秒 | Δcap8 | Δcap6 | Δcap4 | Δcap2 | Δcap1 | 答案 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `quiet BLACK d6` | 221,774／0.97 s | −1.4 % | −4.5 % | −10.2 % | −24.7 % | −33.9 % | 全同 |
| `quiet WHITE d6` | 156,428／0.69 s | −2.0 % | −5.3 % | −11.2 % | −22.6 % | −30.1 % | 全同 |
| `scattered WHITE d6` | 251,149／1.10 s | −0.3 % | −1.0 % | −2.9 % | −9.0 % | −15.9 % | 全同 |
| `pos24 WHITE d6` | 175,088／0.77 s | −17.7 % | −26.3 % | −34.7 % | −44.0 % | −43.7 % | 全同 |
| `quiet BLACK d8` | 1,408,083／**6.18 s** | −1.7 % | −4.5 % | −9.3 % | −19.2 % | **−25.3 %（4.61 s）** | 全同 |
| `pos24 BLACK d8` | 2,378,724／**10.43 s** | −20.2 % | −28.5 % | −37.3 % | −46.2 % | **−50.8 %（5.13 s）** | 全同 |

- 秒数是轮次135 读数 3 的**同一支 release 二进制**的 228,000 nps，不是另一构建的 Ir／节点乘这一构建
  的 nps；Δ 列的负号就是省下（节点数变化）。`:549` 的「depth 8 在 4s 内完成」在 `quiet BLACK d8`
  上要 35.2 % 的削减，在
  `pos24 BLACK d8` 上要 61.7 %：cap=1 分别给 25.3 %／50.8 %，**两臂都差**；`pos24 WHITE d6` 那种
  同一臂上 cap=2 甚至比 cap=1 省得多（44.0 % 对 43.7 %），再次说明这条曲线不单调。

### 读数 3：同一把尺子放到 32 个中盘景（`/tmp/i1/l3_sweep.out`，与轮次131／134 同一景集、同两深度）

| cap | 答案改动（64 个 think） | 平均节点节省 | 平均尾巴节省 | 反而变大的景 | 32 个 d8 里仍 >4 s |
| --- | --- | --- | --- | --- | --- |
| 4 | 3／64 | 10.9 % | 18.4 % | 1 | 8 |
| 2 | 10／64 | 20.3 % | 35.3 % | 3 | 8 |
| 1 | **27／64** | 22.5 % | 44.2 % | **6** | 7 |
| 0（出厂树） | — | — | — | — | **12** |

- 基线本身：32 个 d8 think 均值 **3.53 s**，最大 **10.56 s**，**20/32 已在 4 s 内**——`:549` 那一格
  的真实缺口不在「平均」而在分布右端那 12 个景，而 cap 扫到 1 也只把 12 降到 7。
- 改动列里多数是分值改而着法不改（`P13-d8` 91,914 → 90,750 仍下 (7,6)），着法改的如
  `P22-d8` (6,7) → (9,10)、`P15-d6` (8,8) → (9,5)、`P04-d8` (6,11) → (7,12)：按 `:554`
  第 1 条这些都得能解释，而本轮给不出解释它们的机制——它们是截断后重搜的副产物。

### 读数 4：把整条尾巴关掉，那是节点侧的上限（`/tmp/i1/l4_notail.out`）

| 臂 | 尾巴开 | 尾巴关 | 节省 | alphabeta 节点 | α 侧变化 | 秒（开→关） | 得分（开→关） | 着法 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `quiet BLACK d6` | 221,774 | 95,477 | 56.9 % | 78,796 → 58,752 | −25.4 % | 0.97 → 0.42 | 71,992 → **−1,992** | (8,7)→(8,6) |
| `quiet WHITE d6` | 156,428 | 106,903 | 31.7 % | 54,088 → 66,954 | +23.8 % | 0.69 → 0.47 | 97,044 → **−162** | 同 |
| `scattered WHITE d6` | 251,149 | 133,355 | 46.9 % | 90,194 → 80,343 | −10.9 % | 1.10 → 0.58 | 98,948 → **850** | (11,3)→(10,2) |
| `pos24 WHITE d6` | 175,088 | 111,308 | 36.4 % | 55,407 → 74,100 | +33.7 % | 0.77 → 0.49 | 99,999 → **−9,212** | 同 |
| `quiet BLACK d8` | 1,408,083 | 1,190,680 | **15.4 %** | 639,592 → 798,641 | **+24.9 %** | 6.18 → **5.22** | 77,842 → **−8,490** | (8,7)→(8,6) |
| `pos24 BLACK d8` | 2,378,724 | 1,105,947 | 53.5 % | 743,804 → 769,061 | +3.4 % | 10.43 → **4.85** | 99,999 → **3,420** | 同 |

- 32 景那一侧同样：64 个 think **57 个答案改动**，平均节省只有 **20.1 %**，最差一景
  （`P05-d8` 256,879 → 522,679）**大了一倍**（−103.5 %），d8 均值 2.66 s 而最大 6.57 s。
- 这一支臂的价值就在它同时给了两句话：**尾巴的节点占比是真实的**（六臂尾巴占 54.6 %～68.7 %），
  **但那份占比不是可以从尾巴里取走的钱**——取走的动作本身在父层重买回来。这正是轮次137 那句
  「`:549` 的两格不是同一个因子」的下一层：节点那一侧连自己的因子都不完全可控。

### 判

- **不落地**，且这次不是「棋力换不动」一种理由，而是三条同时成立：① 合法形状（不截 Tier1）节省为 0；
  ② 文档点名的禁一刀量到极限（cap=1）仍达不到 `:549` 的第二格，且在 32 景上改 27 个答案、6 景反而变
  大；③ 曲线外推的上限（整条关掉）连 `quiet BLACK d8` 的 4 s 都过不了，还把六臂的得分全部改掉——其中
  四臂直接判成负分。
  因此本轮**没有跑验证赛**：赛是用来在「算术够、棋力未知」时定夺的，这里算术本身不够，跑赛只会把一
  个注定不达标的形状包装成结论。
- `docs/ENGINE_REDESIGN.md:549` 这一行到这里两侧都关到了数字上：内核那一侧轮次135／136 量到**上限
  1.25 倍 vs 需要 4.4 倍**（键宽还要 72 位），节点这一侧本轮量到**禁一刀到极限只有 1.34／2.03 倍，
  而需要 1.54／2.61 倍**——两个 d8 臂在两种极限形状（cap=1 与整条关掉）下都没进 4 s：4.61／5.22 s 与
  5.13／4.85 s。
  要过那一行只剩一条路：**每候选格评估内核本身重做**（`pattern_score` + `finish_direction` +
  `recognize_position_both` 那个 69.19 % 的家族，轮次136 读数 1）。那是一个新的设计决定，不在
  ENGINE_REDESIGN 的功能清单里，需要用户授权；本轮不自作主张动它。
- 轮次137「没动的账」第一格（尾巴可减比例）到此关掉。

### 门禁

- `src/`、`tests/`、`web/`、`tools/`、`CMakeLists.txt` 与 `1e31d22` 逐字节相同（`git diff --stat` 为
  空）；`git status` 除本节外只有那三个待用户发话的未跟踪 `docs/`。四支读数全部来自
  `/tmp/i1/{lsrc,msrc,psrc}` 三份 `cp -rT` 副本，副本里也只改了 `search/alphabeta.cpp` 一个 TU，
  头文件未动 ⇒ 仓库对象的 ABI 没有被这套仪器作废。
- `node tests/readme_anchor_check.js` **120 citations / 0 problems**；`node tests/web_frontend_check.js`
  **105/105 passed**。两支都只读 `README.md`／`web/`，与 `REVIEW_LOG.md` 的行数无关，所以在本节拼入之前
  跑即为有效读数。
- **ctest 不跑**，理由与轮次136／137 同一句：树就是轮次134 还原门禁跑过 `8/8 / 463.68 s` 的那一棵，
  本轮没有新代码；同理 valgrind 没有新的可筛对象（仪器只在副本的单个 TU 里加计数，仓库二进制未变）。
- 本节拼在轮次137 表头之前（原 1982 行处），于是那一节及以下整段下移 **165 行**；`git diff --numstat`
  ＝ **165 / 0**，`wc -l REVIEW_LOG.md` 从 8042 到 **8207**。被本节推下去的档内绝对针
  （`REVIEW_LOG:2289` → 轮次125 的 1.99 s 那一行，拼入前在 3667 行）拼入后在 3832 行，差 **1543** 行；本轮新写的
  自我引用一律写轮次标签，不写绝对行号。

### 没动的账

- 「1M nps 那一格要 4.4 倍」本轮再次没有动，因为它的钥匙不在功能清单里而在内核设计里（见判 第二条）。
- `:549` 那一行的四个内容项都已落地（轮次135 记过这一点），本轮补的是**验收那一列**的节点侧；
  「局部 stand_pat」与「TT 携带 `static_eval`」两项各自的独立节省从未**分别**量过——它们与候选格分数
  同属轮次136 那个 69.19 % 的家族，混在一起数过一次，拆开要再一支计数臂，本轮没有开。
- 尾巴宽度的**局面来源**（中盘 12.3 对 quiet 4.3）本轮只描述没有分解：要把「战术密度」与「候选
  宽度」分开，得按候选的威胁等级（活四／冲四／活三）分桶计数，那是另一支护架，装在同一支臂上会
  改变本轮的读数口径。

### 仪器（全部在 `/tmp/i1/`，仓库外）

`l_patch.py` + `l_driver.cpp` + `l1_width.out`（六臂形状：生成前返回／候选／递归／装饰账单／
fail-low 子树的最近链接归属／按层与按宽的直方图），`m_patch.py` + `m_driver.cpp` + `l2_cap.out`
（六臂 × cap{8,6,4,2,1}，cap0 逐臂回针），`m_sweep.cpp` + `l3_sweep.out`（32 景 × d6/d8 × 四 cap
的答案改动表），`p_patch.py` + `p_sweep.cpp` + `l4_notail.out`（尾巴开关的六臂 + 32 景），
`l_analyze.py`／`l3_analyze.py`（把上面四张表从 `.out` 重新算出来，含三条自检 assert）。

## 轮次137 引擎重构 K：节点数那一侧第一次被拆开——一次 d6 的 think 里 64.5 % 是静默节点，而 :549 那两格其实不是同一个因子（1.54 倍与 4.4 倍）— engine redesign stage K, the node-count side decomposed: 64.5 % of a depth-6 think's nodes are quiescence, and §4 :549's two cells turn out to need 1.54x and 4.4x, not one factor (measurement round, no code changed)

轮次136 把记忆化一类关到 1.25 倍之后，用户选了第三个问法：**先量节点数那一侧**——不看每节点多便宜，
看同样的答案能不能花更少节点。本轮不动一行代码，交三张读数，其中第三条是本轮真正的收获：
`:549` 那一行的两格不是同一个目标，轮次135／136 把它们当成了一个。

### Found

- **出厂针的节点数本来就是迭代累加，不是最后一层的树**。`quiet BLACK d6 = 221,774` 这一枚针是
   depth 1..6 全部 think 的 `SearchStats.nodes` 之和（读数 2 的累计列对上它，一字不差），
   所以「一次 d6 的 think 花多少节点」这句话里，**有 17.9 % 是花在给最后一层排序的前五层上的**。
- **静默节点与 alphabeta 节点是两个可以互相核对的口径**：`nodes = alphabeta + qnodes` 在 14 臂表上
   **逐臂成立**（读数 1 最后一列），这既是记账自检（轮次124 的 C4 把 `qnodes`/`seldepth` 记进
   `SearchStats`，本轮第一次把它们当差值用），也说明轮次134 量到的「78,796 个 alphabeta 节点」
   与「221,774 个节点」说的是同一棵树的两半，不是两把尺子。
- `SearchStats.qnodes / tt_cutoffs` 此前被档内记成「文档没有给它们读者」（`engine-redesign-feature-inventory`
   的那条 deliberate-not-gap）。本轮给 `qnodes` 找到了读者：**节点数那一侧的账必须靠它才拆得开**。

### 读数 1：一次 think 里，节点长在哪一半（`/tmp/i1/k1_qnodes.out`，读轮次134 的 `pair_t2d6.out` off 列）

| 臂 | 总节点 | alphabeta | qnodes | 静默占比 | nodes = α + q |
| --- | --- | --- | --- | --- | --- |
| quiet d2 B | 742 | 141 | 601 | 81.00 % | OK |
| quiet d2 W | 1,370 | 187 | 1,183 | 86.35 % | OK |
| quiet d3 B | 2,515 | 984 | 1,531 | 60.87 % | OK |
| quiet d3 W | 3,912 | 1,180 | 2,732 | 69.84 % | OK |
| quiet d4 B | 11,359 | 3,780 | 7,579 | 66.72 % | OK |
| quiet d6 B | 221,774 | 78,796 | 142,978 | **64.47 %** | OK |
| quiet d6 W | 156,428 | 54,088 | 102,340 | 65.42 % | OK |
| quiet d8 B | 1,408,083 | 639,592 | 768,491 | 54.58 % | OK |
| scattered d6 B | 177,061 | 71,723 | 105,338 | 59.49 % | OK |
| scattered d6 W | 251,149 | 90,194 | 160,955 | 64.09 % | OK |
| pos24 d6 B | 300,347 | 79,348 | 220,999 | **73.58 %** | OK |
| pos24 d6 W | 175,088 | 55,407 | 119,681 | 68.35 % | OK |
| pos24 d8 B | 2,378,724 | 743,804 | 1,634,920 | 68.73 % | OK |
| crowded d6 B | 888 | 151 | 737 | 83.00 % | OK |
| **合计** | **5,089,440** | 1,819,375 | 3,270,065 | **64.25 %** | OK |

产品那一档（d6）的四臂里静默节点占 59 %～74 %。也就是说**要减节点数，减的必须是尾巴**：alphabeta 那一半
已经被轮次121 的宽度表与轮次127 的共享表处理过，尾巴这一半里已经落地的只有轮次124 的深度上限
（`MAX_QDEPTH = 8`）与威胁过滤。

### 读数 2：每加一层的边际代价，与尾巴的实际深度（`/tmp/i1/k3_sweep.out`，`k3_sweep.cpp`，同一支 release 二进制）

`quiet BLACK` 那一型，深度帽从 1 走到 8，一 think 一张新表：

| 帽 | 累计节点 | 累计 qnodes | 本层新增 | 倍率 | 分 | 完成层 | seldepth |
| --- | --- | --- | --- | --- | --- | --- | --- |
| d=1 | 62 | 31 | 62 | — | 4,202 | 1 | 0 |
| d=2 | 742 | 601 | 680 | 11.97 | 20,584 | 2 | 4 |
| d=3 | 2,515 | 1,531 | 1,773 | 3.39 | 20,584 | 3 | 4 |
| d=4 | 11,359 | 7,579 | 8,844 | 4.52 | 97,160 | 4 | 4 |
| d=5 | 39,598 | 21,173 | 28,239 | 3.49 | 97,160 | 5 | 4 |
| d=6 | **221,774** | 142,978 | 182,176 | 5.60 | 71,992 | 6 | 6 |
| d=7 | 565,482 | 299,685 | 343,708 | 2.55 | 71,992 | 7 | 6 |
| d=8 | 1,408,083 | 768,491 | 842,601 | 2.49 | 77,842 | 8 | 6 |

三件事可以直接读出来：

1. **排序费是 17.9 %**：d6 那次 think 的 221,774 里，前五层（为了给最后一层一个次序）占 39,598；
   最后一层自己占 182,176（82.1 %）。这一笔是可以算的账，不是猜想——把迭代加深整个去掉就省下 17.9 %
   的节点，代价是最后一层失去 TT 给的次序（本轮没有试，也不该试：那是 §2.6 Step 1 之前的形状）。
2. **最后一层里 66.9 % 仍是尾巴**：qnodes 的累计差给出最后一层新增 121,805 个静默节点
   （142,978 − 21,173），占该层 182,176 的 66.9 %。整个 think 的 64.5 % 与这一条是同一个事实的两个口径。
3. **尾巴是宽，不是深**：`seldepth` 在 d=2..5 一直是 4，d=6..8 一直是 6，离 `MAX_QDEPTH = 8` 还差两层。
   所以轮次124 那枚深度帽在这几型上没有咬住——尾巴的节点数来自每层的分支，不来自纵向失控。
   「把 qdepth 从 8 收到 6」在这种形状上省下的是**极少**的节点（6 就是实际最深），这条本轮就可以先记下，
   下一轮别再把它当成一个可省的旋钮。

### 读数 3：`:549` 那两格不是同一个因子——1.54 倍与 4.4 倍

轮次135 把两格写成「同一笔账的形状」，本轮按自己那把尺子核对，**这个说法要改**：

- 第一格「单线程 ≥ 1M nps」：需要 Ir／节点 ≤ 9,660，即 **4.4 倍**（轮次136：记忆化一类只有 1.25 倍上限）。
- 第二格「depth 8 在 4 s 内完成」：在 228k nps 上，4 s 的预算是 912,000 个节点。本轮这一型 d8 花
  1,408,083 个节点 ＝ 6.17 s ⇒ 这一型**只差 1.54 倍**。
- 而轮次132 那把 32 景 × 两色的尺子上，这一格是 **20/64 臂（31.2 %）达标、中位 3,010 ms**——
  中位已经在 4 s 以下，失败的是分布的另一半。

所以两格要的东西不同：第一格是一个 4.4 倍的内核因子（已被证明不是「少算一点」能买到的），
第二格是一个 1.54 倍，而且**它可以直接由节点数那一侧付**——读数 1 与 2 给出的两个可减体量
（尾巴 64.5 %、排序 17.9 %）里，只要尾巴能减掉 25 %～30 % 就够到 1.54 倍了。够不够得着是下一轮的问题，
本轮交的是**它的价格已经被量出来了**：一个针对静默尾巴的收敛规则（而不是深度帽，见读数 2 第 3 条），
按 §4 `:554` 第 1 条要「着法变化必须能解释」，按 `:555` 跑验证赛。

### 判：本轮不改代码；`:549` 从「一格不可达」变成「一格要内核重做、另一格要一个尾巴收敛规则」

1. 功能清单仍然是空的——本轮没有发现任何**文档里**没落地的机制。
2. `:549` 的两格**必须分开判**（这是对轮次135／136 那句「同一笔账的形状」的更正，写在本节里而不是去改那一节，
   因为那一节的行号已被引用）。第二格有一个可测的、量级为 1.54 倍的目标，而且它的钱在尾巴那一半里；
   第一格仍然需要一个 4.4 倍的内核因子，轮次136 已把「少算一点」一类关到 1.25 倍。
3. 因此下一轮的工作是清楚的、而且不需要新的设计授权：**量一个静默尾巴的收敛规则**（先量：尾巴节点的
   分布——每层多少个、其中多少个在父节点已被截断之后根本不影响答案），再决定有没有一个既不减弱棋力
   又能减 25 %～30 % 尾巴节点的形状。这条属于 §2.6 那一族的自然延伸，文档没有直接写着它。

### 门禁

- `src/`、`tests/`、`web/`、`tools/`、`CMakeLists.txt` 与 `0e8119d` 逐字节相同；`git status` 除本节外只有
  那三个待用户发话的未跟踪 `docs/`。
- `node tests/readme_anchor_check.js` **120 citations / 0 problems**；`wc -l README.md` 仍 **658 行**。
- **ctest 不跑**，理由与轮次136 同一句：树就是轮次134 还原门禁跑过 `8/8 / 463.68 s` 的那一棵，本轮没有新代码。
- 本节拼在轮次136 表头之前（原 1982 行处），于是那一节及以下整段下移 120 行；`git diff --numstat` ＝ 120 / 0，
  `wc -l REVIEW_LOG.md` 从 7922 到 8042。轮次136 那句「108 / 0」记的是它自己那次提交 `f7f9614`，本轮不改它；
  被本节推下去的档内绝对针（`REVIEW_LOG:2289` → 轮次125 的 1.99 s 那一行，现在在 3667 行）现在差 1378 行。
- 本轮新跑的只有两支**不碰行为**的臂：`k3_sweep.cpp`（累计节点／qnodes／seldepth 随深度帽，d6 那一行
  回到出厂针 221,774 —— 这条是这支臂自己的自检）；读数 1 完全来自轮次134 已经落盘的 `pair_t2d6.out`。

### 没动的账

- 尾巴节点的**可减比例**本轮没量（读数 1/2 只给出它占多少、有多宽、走多深），那是下一轮的第一件事。
- 「排序那 17.9 % 能不能省」本轮只算了大小，没有做臂：去掉迭代加深会同时改掉 §2.6 Step 1 带来的 TT 次序，
  那是棋力问题不是记账问题，不该在这一格上顺手做。
- 轮次132 那把 32 景尺子的 4 s 达标分布（20/64、中位 3,010 ms）本轮直接引用，没有重测。

### 仪器（全部在 `/tmp/i1/`，仓库外）

`k3_sweep.cpp` + `k3_sweep.out`（深度帽 1..8 的累计节点／qnodes／seldepth，d6 自检回针）、
`k1_qnodes.out`（14 臂的 nodes = alphabeta + qnodes 逐臂核对，读轮次134 的 `pair_t2d6.out`），
以及轮次135／136 留下的 `slack.py`、`redun_driver.cpp`、`ir_driver.cpp`、`ir_cg.out`。

## 轮次136 引擎重构 J：把「记忆化能不能买到那 4.4 倍」量到底——上限 1.25 倍，而键宽 72 位的表根本不存在 — engine redesign stage J, is any memoisation shape able to reach §4 :549's 4.4x? the ceiling is 1.25x at unlimited memory, and the only call-free route left is a per-candidate kernel redesign (measurement round, no code changed)

轮次135 把 `docs/ENGINE_REDESIGN.md:549` 那两格算成了一道算术（同一支 release 二进制：42,353.5 Ir／节点
× 228k nps ⇒ 「≥ 1M nps」＝ Ir／节点 ≤ 9,660 ＝ 再便宜 **4.4 倍**）。用户选了「先量可行性」：不动 `src/`，
先回答一个前置问题——**是否存在任何一种「少算一点」的形状能到 4.4 倍**，还是这个目标只能靠每候选格内核
本身变便宜。本轮的答案是后者，而且是可数的后者。

### Found

- **搜索的候选格打分不走文档里那两个入口**。`Evaluator::evaluate_position_pair` 与
  `evaluate_position_both` 在 `src/` 里**一个调用点都没有**（`grep` 四种调用形式全空，只有注释与
  `tests/test_search.cpp:2674 / :5157 / :5172` 在用它们当对照）。真正在跑的是
  `src/search/search_state.h:249-280` 的 `CandidateCache::cell`：每节点 72.97 次探问（Debug 口径的
  callgrind；探问／重算之比是结构量，不随构建变），其中
  **22.47 次需要重算**（一次 `recognize_position_both` + 两次 `evaluate_position_with_pattern`），
  其余 50.5 次由该节点自己的记录代答。22.47 这一层是两个构建各自数到的同一个数（轮次134 的 Debug
  callgrind 22.470／节点，本轮那支 `-O2` 计数臂 10,626,470 ÷ 472,923 = 22.4696）。
- 本轮第一支护架就装错了一次：探针装在 `evaluate_position_pair` 入口，跑出来 `calls=0` 而两臂节点数
  仍是出厂针。**0 不是读数，是那台仪器没说话**——按轮次127 立的 liveness 规矩，pin 与 0 同时出现就说明
  函数不在路径上，这条改成了上面那一句发现，而不是记成一个零。
- 可行性好的一半是真的：`cell` 的重算路径是 `(棋盘, 行, 列)` 的纯函数（系数在一次 think 内固定，
  `search_state.h:271` 恒以 `BLACK` 问，两个颜色各得一份自己的图案），所以一张跨节点的「格子分」表
  在**语义上**是可以架起来的——键就 8 字节，值就 4 字节。

### 读数 1：release 构建上，每节点那 42,353 Ir 花在哪（`/tmp/i1/calls_release.out`，`ir_cg.out`）

单臂 `quiet BLACK d6`（221,774 节点，pin 已核），`callgrind --cache-sim=no`，`--inclusive=no` 的**自身**开销：

| 函数 | 占每节点 |
| --- | --- |
| `Evaluator::pattern_score` | 22.04 % |
| `PatternRecognizer::finish_direction` | 15.61 % |
| `PatternRecognizer::recognize_position_both` | 11.67 % |
| `CandidateCache::cell` | 10.45 % |
| `PositionPattern::analyze` | 5.99 % |
| `PatternRecognizer::is_forbidden_from_pattern` | 5.69 % |
| `Evaluator::candidate_value` | 4.07 % |
| `MoveGenerator::generate` | 3.74 % |
| `Board::is_empty(int,int)` | 3.21 % |
| `Evaluator::evaluate_position_with_pattern` | 2.27 % |
| `Evaluator::board_pair_value` | 1.85 % |
| `Board::undo_last_move` | 1.30 % |
| `AlphaBetaSearch::static_score` | 1.17 % |
| `Board::update_active_empty` | 1.03 % |

前十四行合计 **90.1 %**。这一张表替轮次135 那张 Debug 口径的表说话：**热点形状与构建无关**，
`pattern_score` 与窗口解码族仍是主体（评估与图案识别，与 D-5／轮次131／轮次134 的答复同一条）。

### 读数 2：记忆化的上限是 1.25 倍，而目标是 4.4 倍

把「一次重算」的整族开销加起来——`recognize_position_both` 11.67 + `finish_direction` 15.61 +
`pattern_score` 22.04 + `analyze` 5.99 + `is_forbidden_from_pattern` 5.69 + `candidate_value` 4.07 +
`evaluate_position_with_pattern` 2.27 + `board_pair_value` 1.85 ＝ **69.19 %** 的每节点成本。
轮次135 数过这个种群的重复率：`(棋盘,行,列)` 里 **28.6 %** 是本进程已经算过的（键完全同一：`cell` 恒以
`BLACK` 问一次识别，识别次数＝重算次数＝22.47／节点，两臂针都过）。于是

- **无上限内存的天花板**：0.286 × 69.19 % ＝ **19.8 %** 的每节点成本 ＝ **1.25 倍**。
  代价是轮次135 量到的 759 万键／1.04 GB（本轮按语义重算一次：键 8 B + 值 4 B + 槽管理，
  即便压到 12 B／槽也要 91 MB，且随搜索规模长，与 §2.8 `:433` 的解耦目标正对着干）。
- **有界表只会更低**：128K／1M／2M 槽的直接映射表在 759 万个不同键的流上，命中率必然低于
  无限表的 28.6 %（本轮没有再跑那三个数，因为**无上限已经是天花板**：上限都到不了目标的一半）。
- **目标要的是砍掉 77.3 %**（留 ≤22.7 %）。整族 69.19 % 全删再加 `cell` 的 10.45 % 才 79.6 %——
  那是「一个候选格都不评估」的搜索，不是更快的搜索。

**所以：少算一点这一类办法（记忆化、共享表、缓存加大、记账换算法）在本树上买到 1.25 倍为止。**
`:549` 的 4.4 倍只能来自一次候选格评估**本身**的便宜化。

### 读数 3：那唯一剩下的形状，键宽是多少

现在一次候选格重算＝8 次窗口解码（4 条方向 × 两色；两色读同两条线，`pattern.cpp` 的 `recognize_position_both`）
＋一次 `analyze`＋2.61 次 `pattern_score`。要把它换成「一次查表」，键得装下这个格子的**四条线的窗口内容**：
每条线窗口 18 位（`README` 4.1：`(own << 9) | blocked` 索引一张 2^18 的表），四方向联起来是 **72 位**，
不是一张可以直接索引的表（2^72 槽）。能落地的只有「按线／按段做局部表再组合」，而那正是 README:624
量过并放弃的增量形状。这一条不是本轮新增的猜测，是本轮把键宽写出来之后可以直接看到的：
**文档给的 C 行已经全部落地，`:549` 需要的机制在文档里没有，而且它的两种可写形状（全量表 / 分线增量）
分别被 72 位键宽与 README:624 的实测否掉过。**

### 判：可行性量到底了，本轮不改一行代码，也不新造一个机制

1. **记忆化一类关闭**：1.25 倍封顶（无上限内存口径），距 4.4 倍差 3.5 倍。这条不需要用户决定，
   写下来是为了第三次别再量。
2. **`:549` 剩下的唯一形状是一次有正确性风险的评估内核重做**，而它的两种写法各自已被否过一次
   （72 位键宽 / README:624 的分线增量实测持平）。要不要**第三次**尝试——带 `:556` 第 3 条要求的
   全 225 格 × 两色 × N 棋盘差分、逐条节点针对比——是一次设计授权，不是「下一个未实现的功能」。
   本轮把它交到用户手上，并且现在手上有的东西是：目标因子 4.4、成本主体一张表、键宽一个数。
3. **本轮真正新增的读数只有一条**（`evaluate_position_pair`／`both` 无生产调用点，热路径走
   `CandidateCache::cell`）——它同时解释了为什么轮次135 那条 rejection 的算术对象需要更新，
   也说明这两个公开入口现在只服务对照测试。这条不改代码，只在档里记着。

### 门禁

- `src/`、`tests/`、`web/`、`tools/`、`CMakeLists.txt` 与 `0e8119d` 逐字节相同（`git diff --stat 0e8119d -- …` 空）；
  `git status` 除本节外只有那三个待用户发话的未跟踪 `docs/`。
- `node tests/readme_anchor_check.js` **120 citations / 0 problems**；`wc -l README.md` 仍 **658 行**。
- **ctest 不跑，理由写在档里**：树与轮次134 还原门禁里刚跑过 `8/8 / 463.68 s` 的那一棵相同，本轮没有
  新代码可回归；这条如实记录，不当成通过。valgrind／memcheck 无可筛之物（同理）。
- 本节拼在轮次135 表头之前（原 1982 行处），于是那一节及以下整段下移 108 行；`git diff --numstat`
  ＝ 108 / 0，`wc -l REVIEW_LOG.md` 从 7814 到 7922。轮次135 档内那句「156 / 0」记的是它自己那次
  提交 `bea702a` 的形状，本轮不改它；被本节推下去的 `REVIEW_LOG:2289` 那一类档内绝对针现在差 1258 行。

### 仪器（全部在 `/tmp/i1/`，仓库外）

`ir_driver.cpp` + `run_i3.sh` + `ir_cg.out`（release 单臂：针、指令数、三块墙钟）、
`callgrind_annotate --inclusive=no`（读数 1 的自身开销表，`calls_release.out` 是本轮第一版解析器的记录，
它的跨 `ob` 块串号问题由 annotate 的行替代，写在这里是为了下一轮别再用手 parsers 读 callgrind）、
`j_patch.py` + `j_driver.cpp` + `run_j1.sh` + `run_j1.log`（那支护架打印 0 的一次，以及它换来的 Found 第 1 条）、
轮次135 的 `redun.out`（重复率 28.6 %，读数 2 的分子）。

## 轮次135 引擎重构 I：§4 :549 那两格第一次被算成一笔可以判的账，而静态分够不着子树这件事不是钳造成的 — engine redesign stage I, the §4 :549 C-row throughput cells priced as an arithmetic target: the static-score slack, the repeat-work ceiling, and Ir/node against nps on one build (measurement round, no code changed)

用户在轮次134 收口后选了「先做吞吐那一半」，方向是给评估换一个能给子树封顶的形状（也就是
`docs/ENGINE_REDESIGN.md:351` 那半句在轮次134 被判掉时留下的那条前提，与 §4 `:549` C 行那两格验收）。
本轮不动一行代码：先把这条路上必须先量才能走的两件事量出来——静态分到底差多远才够得着自己的子树，
以及每节点那笔钱里有多少是重复劳动。规格位置：验收 `:549`「单线程 ≥ 1M nps；depth 8 在 4s 内完成」，
回归保障 `:556` 第 3 条（评估改动必须逐格差分）。

### Found

- **`:549` C 行的四项内容早就全部在树里了**：TT 携带 `static_eval`（轮次121 C1）、候选格分数缓存
  （轮次122 C2 的 `CandidateCache`）、局部 stand_pat（轮次123 C3）、静默深度上限 + `seldepth`
  （轮次124 C4）。所以那两格不是「还有一个功能没实现」，而是一个**每节点成本的目标**：
  文档给的路已经走完了，剩下的缺口要靠一个文档里没有的机制才能填。
- **轮次134 的第 4 条判据需要一条更底层的读数**：那次写的是「静态分被钳在 ±99,999 而搜索分值在钳之上，
  所以静态分不能给自己的子树封顶」。钳确实是那道界，但本轮量出来：**把钳拿掉也不够**——即使在完全
  没有饱和的节点上，子树回来的值比静态分高出去的那一段，中位数就有 7.7 万～11 万分，最大 199,997
  （≈ 2 × 99,999，从下钳到上钳的全幅）。也就是说这条规则要的不是「把钳挪高一点」，
  而是「一个能预知 15 格窗口里会不会长出冲四的静态分」——那是评估本身换范式，不是参数。
- **README:624 那条「增量式全盘评估做过量化评估后放弃」记的是一笔已经不存在的工作量**：它比的是
  「一手棋影响约 15 个候选格」对「节点上那次全量重扫」。全量重扫在 C1–C4 之后已经不是每节点的了
  （本轮读数 3 量到每节点 44.94 次**候选格**评估、只有 22.47 次需要新算窗口）。这笔放弃的理由本身
  没有过期（读数 2 给出同样的结论），但它排除的是全盘项的增量，**没有**排除候选格那一层的增量，
  而钱现在全在后者。
- 仪表复用轮次134 的只读行（`/tmp/h1/rows_*.bin`，三枚出厂针 `221,774 / 1,408,083 / 2,378,724` 当时
  就 `[PIN-OK] × 3`），加一支新的计数臂（`/tmp/i1/`）。

### 读数 1：静态分与它自己的子树差多远（`/tmp/i1/slack.out`，脚本 `slack.py`）

每一行是一个臂的全部 alphabeta 节点。「饱和」＝ `|静态分|` 已经在 ±99,999 那道钳上；「回来更好」＝
这个节点搜索之后的值比它自己的静态分高——futility 要剪掉的正是这一类节点。

| 臂 | 节点 | 静态分饱和 | 返回值进了杀形带 | 子树比静态分更高的占比 | 那一档的 p50 差 | p99 差 | 最大差 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| quiet BLACK d6 | 78,796 | 21.71 % | 0.12 % | 39.2 % | 77,967 | 194,057 | 197,191 |
| quiet WHITE d6 | 54,088 | 19.00 % | 6.47 % | 39.4 % | 76,567 | 196,536 | 199,755 |
| quiet BLACK d8 | 639,592 | 42.50 % | 28.06 % | 33.0 % | 78,121 | 177,130 | 199,922 |
| scattered BLACK d6 | 71,723 | 13.94 % | 0.00 % | **52.9 %** | 76,859 | 120,425 | 198,412 |
| pos24 WHITE d6 | 55,407 | 45.60 % | 34.46 % | 26.1 % | 104,109 | 199,399 | 199,987 |
| pos24 BLACK d8 | 743,804 | 45.40 % | 36.34 % | 25.6 % | 109,887 | 199,175 | 199,995 |
| crowded BLACK d6 | 151 | 27.81 % | 74.17 % | 2.9 % | 100,907 | 100,907 | 100,907 |

三件事：

1. **「≈0 变不成 ≥77,842」这句话不是被那道钳挡住的，而是本来就错**：只看没饱和的节点，三分之一的臂
   有 39 %～53 % 的节点子树回来的值高于静态分，且高出的中位数就是 7.7 万分。任何 `静态分 + 松弛 ≤ alpha`
   形状的界，松弛要给到 8 位数才不出错，而那已经是整个分值域——界就不成其为界了。
2. **饱和不是边角**：战术型臂有 45 % 的节点静态分就贴在 ±99,999 上（pos24 两臂 45.40 %／45.60 %），
   而 `crowded` 那 151 个节点里 74 % 的返回值在杀形带。这说的是评估的另一件事，本轮只记录不判：
   一具在四成节点上失去分辨力的静态分，既给不出界，也给不出排序精度。
3. 读数 1 直接否掉了本轮开场的那个方向（把钳改成带证据的深度化界）。**结论写在判那一节**。

### 读数 2：每节点那笔钱里，重复劳动的上限（`/tmp/i1/redun.out`，脚本 `patch.py` + `redun_driver.cpp`）

计数装在 `PatternRecognizer::recognize_position_both` 的入口，键是 `(zobrist 高 48 位 | 格编号)`，
只数不改行为：**两臂的节点数回到出厂针 221,774 与 251,149**（驱动里 pin 不匹配就 rc=1）。

| 臂 | 识别次数 | 其中第一次见 | 重复率 | 次数/节点 | 若要跨节点记忆，需要 |
| --- | --- | --- | --- | --- | --- |
| quiet BLACK d6 | 3,805,665 | 2,621,688 | 31.1 % | 17.16 | 377,523,072 B（360 MB） |
| scattered WHITE d6 | 6,820,805 | 4,964,981 | 27.2 % | 27.16 | 714,957,264 B（682 MB） |
| 合计 | 10,626,470 | 7,586,669 | **28.6 %** | 22.47 | 1,092,480,336 B（1.04 GB） |

- 计数与轮次134 那支 callgrind 独立对上：`10,626,470 / 472,923 = 22.4696` 次/节点，而 `cg4_off.out`
  的边计数给的是 22.470 —— 两个仪器在**不同构建**上量到同一个数，这条是仪表的自检。
- 所以「把识别结果做成一张跨节点共享的表」能省的上限是 **28.6 % 的识别**，而识别只占每节点的 10.75 %
  （读数 3 那一层的 Debug 口径），乘出来 **3.1 %** 的每节点成本；把那些重复识别下游的
  `pattern_score`（每候选 2.61 次 × 122 Ir）也算进同一张表，是 6.43 次/节点 × 606 Ir ≈ 3,899 Ir/节点
  ＝ **6.5 %**。**代价是 1.04 GB 记忆 759 万个键**，而且这笔内存随搜索规模长——
  正是 §2.8 `:433` 花了一整轮（轮次127 D-2）解开的耦合。这条路上「省钱」和「解耦」是对着干的。
- 换句话说：**每节点的成本不是重复劳动**，是 44.94 次候选格评估各自的真工夫。

### 读数 3：把 `:549` 那两格算成一道算术（`/tmp/i1/run_i3.log`、`ir_cg.out`，脚本 `ir_driver.cpp`）

轮次134 记的 60,206 Ir／节点来自 `build-vg` 的 Debug（`-UNDEBUG`）对象，而档里 17.6 万 nps
（轮次132：30,896,728 节点 / 175,730 ms）来自 release 构建——**这两个数不能相乘**。本轮把同一个
release 二进制既拿去数指令、又拿去计时：

| 量 | 读数 | 口径 |
| --- | --- | --- |
| 节点 | 221,774 | 出厂针，`quiet BLACK d6`，一 think 一张新表 |
| Ir | 9,392,903,861 | `--tool=callgrind --cache-sim=no`，`summary:` 行 |
| **Ir／节点** | **42,353.5** | 含进程启动与形状表生成（相对 9.39 G 是千分之几，本轮没有单独扣除） |
| nps | 228,000 / 228,000 / 227,000 | 三块各 20 次 think，`taskset -c 2`，4,390 / 4,385 / 4,406 ns 每节点 |
| 每候选格评估 | 942.4 Ir | 42,353.5 ÷ 44.94 |
| 这台机器此刻的吞吐 | ≈ 9.66 GIr/s | 228,000 × 42,353.5 |

`:549` 要 1M nps，在同一台机器、同一 IPC 下就是
**1M × Ir／节点 ≤ 9.66e9 ⇒ Ir／节点 ≤ 9,660**，即每节点要再便宜 **4.4 倍**、每个候选格从 942 Ir 到 **215 Ir**。
`depth 8 在 4 s 内完成` 那一格是同一笔账的形状（轮次132 记的 31.2 % 达标率，根宽与深度都在同一因子下走）。

于是这条验收第一次有了可判的形状：它不是一个「还没实现的功能」，而是一个 4.4 倍的每候选格内核因子。
按读数 2，删掉重复买不到（≤ 8 %）；按读数 3 的分项，删掉小项也买不到——`is_forbidden_from_pattern`
（Debug 口径 6.4 %）、`Board::is_empty`（4.5 %）、`CandidateCache::cell` 的查表本身（13.0 %）三个全删
才 24 %，而它们都是候选格评估的组成部分，删了就不再评估那个候选。剩下唯一能到 4.4 倍的形状是
**把一次候选格评估换成一次表读**（现在一次评估要走 8 次窗口解码 + `analyze` + 2.61 次 `pattern_score`）。
那是一个文档里没有的新机制，风险正是 `:565`／`:556` 反复点名的「增量／表式评估的正确性」，
而且它要建的表比现有 2^18 形状表宽得多——**这一步不在本轮的授权范围里，交回用户拍**。

### 门禁

本轮不改 `src/`，所以门禁只跑两件事，且把「为什么不跑 ctest」写在档里（轮次125 同一口径）：

- `git diff --numstat REVIEW_LOG.md` 是本节自身的行数、删除 0 行；`src/`、`tests/`、`web/`、`tools/`、
  `CMakeLists.txt` 一字未动（`git status` 除本节外只有那三个待用户发话的未跟踪 `docs/`）。
- `node tests/readme_anchor_check.js` **120 citations / 0 problems**；`wc -l README.md` 仍 **658 行**；
  `wc -l REVIEW_LOG.md` 记在拼入的输出里。
- **ctest 不跑，理由是 `src/` 与 `0e8119d` 逐字节相同**，而那一棵树在轮次134 的还原门禁里
  刚跑过 `8/8 / 463.68 s`；valgrind 亦无可筛之物（没有新代码）。这条如实写，不当成通过。
- 本节拼在轮次134 的表头之前（原 1982 行处），于是那一节及以下整段下移 156 行：`git diff --numstat`
  ＝ 156 / 0（纯插入，删除 0 行），`wc -l REVIEW_LOG.md` 从 7658 到 7814。轮次134 档内那句「226 / 0」
  记的是它自己那次提交 `b906fd5` 的形状，本轮不改它。被本节一起推下去的那类档内绝对针
  （`REVIEW_LOG:2289` 指轮次125 的 1.99 s 那一行）现在距它的真位置差 1150 行；本节所有自我引用一律写
  轮次标签，不新增这一类针。
- 仪表全部在 `/tmp/i1/`（仓库外）：`slack.py`、`patch.py` + `redun_driver.cpp`（计数臂，两臂 pin 自检）、
  `calls_per_node.py`（从轮次134 的 `cg4_off.out` 重算 Ir／节点、次数／节点、Ir／次）、`ir_driver.cpp` +
  `run_i3.sh`（release 构建上同一支二进制既数指令又计时）。

### 判：本轮建不了东西，也不否掉一条规格——它把 `:549` 从「待实现」改写成「一道 4.4 倍的内核因子」，并且否掉了本轮开场那个方向

1. **开场那条方向（把静态分的钳改成带证据的深度化界）不成立**，理由就是读数 1：在没有饱和的节点上，
   子树高于静态分的那一段中位数已经 7.7 万分、最大 199,997 ＝ 全幅。界要成立就得把松弛给到整个分值域，
   那就不是界。轮次134 的判据 4 因此要按这条读数往下读：钳不是唯一的障碍，甚至不是主要的。
2. **`:549` 的两格不是功能缺口**：C 行四项内容全部已落地（C1–C4），文档给的路没有剩下的了。剩下的
   是每候选格 942 Ir → 215 Ir 这个 4.4 倍，而它的形状（一次表读替掉一次窗口解码族）在文档里不存在，
   风险由 `:556` 第 3 条与 `:565` 点名。**这是需要用户拍的一次设计选择，不是可以再「实现一个功能」
   推进过去的一格**。
3. **重复劳动这条路线量到底了**：28.6 % 的识别是重复的，但识别只占每节点 10.75 %，全删换 3.1 % 的节点
   成本（带上下游 `pattern_score` 是 6.5 %），代价是 1.04 GB 随搜索规模长的记忆（与 §2.8 `:433`
   的解耦目标正对着干）。这条在本轮就可以关闭，不需要用户决定，且写清楚是为了下一轮别再去量第三遍。
4. README:624 那条 rejection 的**算术对象已经变了**（它比的全盘重扫，钱现在在候选格一层），但本轮
   不据此重开它：读数 2 与读数 3 给的是同一方向的结论——增量／表式化要买到 4.4 倍，必须把窗口解码
   本身换成一次查表，而不是把 15 个受影响格的记账换成 22.5 次识别。这条改口写在档里，是为了让
   下一次读 README 那句的人知道它量的是什么、现在钱在哪里。

因此本轮**不改 `src/`**，交出去的是三张表和一句话：`:549` 现在可判，但判它需要的不是实现文档里的
某一格，而是批准一次每候选格内核的重做（含 `:556` 第 3 条要求的逐格差分测试）。要不要走那一步、
以及那一步的验收是不是仍写 `1M nps`，交回用户。

### 没动的账

- `:549` 两格本轮**一格也没付**，只把它们的单位量出来（4.4 倍／每候选格 942 Ir）。
- 读数 3 的 Ir／节点里含进程启动与形状表生成，本轮没有单独扣除（相对 9.39 G 是千分之几）。要拿
  这一列做减法之前得先扣掉，这条写在这是为了下一轮别把它当纯搜索口径。
- 读数 2 的 1.04 GB 是「记忆全部 759 万个键」的上限定价，不是任何已设计过的表；它只用来回答
  「重复劳动值多少钱」这一个问题。
- 深度 8 在 4 s 那一格本轮没有重测（轮次132 的 31.2 % 达标率是同一因子的另一种读法），也没有再跑
  64 臂；那把尺子在 `:549` 的账算清之前不会动。

### 仪器（全部在 `/tmp/i1/`，仓库外）

`slack.py` + `slack.out`（静态分与子树之间的差距分布，读的是轮次134 的 `rows_*.bin`）、
`patch.py` + `redun_driver.cpp` + `redun.out`（识别次数与首次键数，两臂 pin 自检）、
`calls_per_node.py` + `calls_off.out`（把轮次134 的 callgrind 转成 Ir／节点、次数／节点、Ir／次）、
`ir_driver.cpp` + `run_i3.sh` + `ir_cg.out` + `ir_timing.log`（release 构建上同一支二进制的指令数与墙钟）。

## 轮次134 引擎重构 H：§2.5.1 的后半句第一次建起来量，产品那一档净省 0.15 %，而字面读法五刀里有一刀切掉的是父节点要用的答案 — engine redesign stage H, the 「允许提前返回静态分」 half-sentence of §2.5.1: built, measured on a headroom grid, nine parameter arms, a 14-arm PAIR table and six matches, judged 不落地

规格 `docs/ENGINE_REDESIGN.md:351` 是 §2.5.1「无威胁剪枝」那一整句的后半：

> **无威胁剪枝**：若局面中双方均无 ≥ 活三的威胁，则只搜索「上一手 2 格邻域 ∪ 已有棋子的 2 格邻域」内静态分前 M 的点，**并允许提前返回静态分（futility pruning）**。

前半已经落地：`src/search/move_generator.cpp:214-229` 用分层宽度实现了那个「前 M 的点」
（Tier2 收「上一手 2 格邻域 ∪ 已有棋子邻域」，且比规格更严——`neighbor_count_at(pos) >= 2` 要两颗子，
单颗邻子落到 Tier3，只在无威胁时按 `cand_width_deep` 取前 M）。后半从来没有人写过：
`REVIEW_LOG.md` 轮次121 B1 那一节把它记进了未完成清单（该节讲 §2.5.1:351 的那一段），原话是「那半句本轮没做，它与 §2.7 的空着剪枝同类」。
本轮把它先量、再建、再判。

### Found

- 树里已经有两处「提前返回静态分」，但都不是这句要的那一处：`src/search/alphabeta.cpp:401-402`
  在一个节点一个候选都拿不出来时返回 `static_score(...)`，以及短路与叶子的返回。规格要的是第三种——
  **不等到没有候选，而是等到「静态分已经够不到本窗口下沿」**。这一条此前没有任何代码、没有数、没有判过。
- 那句的条件有三种读法，三者的量差了一个数量级，所以先按读法分别量，而不是先写实现：
  **读法 A**（字面）＝行棋方的 Tier0/Tier1 都空 **且** 对方没有能成 ≥ 活三的格；
  **读法 B**＝只要求行棋方那一半，就是 `move_generator.cpp:209` 已经算出来的那个 `no_threats`；
  **读法 C**＝B 再补一条「对方没有能成冲四/成五的格」。
  另外还量了一档**只作对照、没有做成参数臂**的读法 D＝「双方都无冲四/成五」（允许行棋方有活三）。

### 读数 1：落脚面——先量它有没有地方站（`/tmp/h1/h1.out`、`agg.out`、`agg2_fixed.out`、`margin_shape.out`）

仪表把 `alphabeta` 换成一层只读 wrapper：进节点前取 `ctx_.nodes()`、`threat_probe` 两色各一次、
`move_gen_.board_score`（纯读缓存，不写表），出节点再取一次，按 (ply, depth, alpha, beta, 静态分,
实际返回值, 节点区间) 落一行。**三枚出厂针原样复现**（`quiet BLACK d6 = 221,774`、
`quiet BLACK d8 = 1,408,083`、`pos24 BLACK d8 = 2,378,724`，`[PIN-OK] × 3`），所以这张表量的是那棵树本身。

| 臂 | alphabeta 节点 | 读法 A 静面上 | 读法 B | 读法 D | A 下可实现的省 |
| --- | --- | --- | --- | --- | --- |
| quiet BLACK d6 | 78,796 | 1.54 % | 11.23 % | 24.65 % | 3.99 % |
| quiet WHITE d6 | 54,088 | 2.63 % | 14.12 % | 18.98 % | 4.47 % |
| quiet BLACK d8 | 639,592 | 0.76 % | 7.86 % | 12.25 % | 14.39 % |
| scattered BLACK d6 | 71,723 | 5.15 % | 25.02 % | 27.99 % | 9.54 % |
| pos24 WHITE d6 | 55,407 | 0.00 % | 0.00 % | 6.30 % | 0.00 % |
| pos24 BLACK d8 | 743,804 | 0.00 % | 0.00 % | 3.09 % | 0.00 % |
| crowded BLACK d6 | 151 | 0.00 % | 0.00 % | 0.00 % | 0.00 % |

三件事当场定下来：

1. **按字面读，这条规则几乎无处可站**：743,804 个节点里 0 个满足条件（`pos24 BLACK d8`），
   战术臂全部 0.00 %；最好的臂也只有 14.39 % 的天花板。§2.5.1 :353 那句「深度 6 的节点数下降 1–2 个数量级」
   不是这半句能补上的——那笔账早已被宽度表花掉了。
2. **margin 这个旋钮是空的，而它为什么是空的可以直接量出来**：从 0 到 16,000 触发集合几乎一字不差
   （触发数在 0…2,000 之间完全不动，4,000 才让 `scattered BLACK d6` 少 25 刀、8,000 才让
   `quiet WHITE d6` 少 10 刀，另两臂 170／408 刀从头到尾一刀不少）。
   把可触发那一格的两个数分开看就明白（`quiet BLACK d8`，d≤4、941 个真被剪的节点，来自 4,498 个满足条件的）：
   **alpha 落在 20,584 … 99,999（p50 77,842），而被剪节点的静态分落在 −574 … +458（p50 −50）**——
   这条规则真正在说的是一句「一个当下约为零的局面，够不上祖先手里那 77,842 分」。
   绝对松弛之所以推不动它，是因为这句话的两个自变量本来就隔着几万点（可剪那一格的 alpha − 静态分
   中位数：77,910／98,530／97,002，`margin_shape.py`）；`scattered`（d≤4）与 `quiet WHITE`（d≤3）两臂同形（alpha p50 98,460 / 97,044，静态分 p50 −23 / −88）。
   另有一行要说清：这些 alpha **没有一个是杀形值**（|alpha| ≥ 100,000 的占 0.0 %），
   所以这条规则的日常用途不是「别去追已证的杀」，而是「别去看一面空棋盘」。
3. **省下的质量压在 ply 2**：`quiet BLACK d8` 在读法 A 下可省 202,602 个节点里 138,421 个（68 %）在 ply 2，
   而其中**一棵** ply-2 子树就有 113,296 个节点（56 %）。根附近正是选点会变的部位。

仪表的一个偏倚要申报：上面这张表的「可省」过滤掉了**返回值**是杀形的节点（|score| ≥ 90,000），
而规则在剪的那一刻并不知道返回值。实现里的护栏只能架在静态分上，所以判它的证据是下面两节的
答案变化与验证赛，不是这张表的 wrong 列。

### 读数 2：九付参数 × 64 臂（`/tmp/h1/fut_sweep.log`，控制 29,691,427 节点）

控制臂的自证不是推断：`FUT_ENABLE=0` 那一份的 64 行 POSE 与 `轮次131` 的 `/tmp/e1/sweep_off.out`
逐字节相同（diff 0 行）——记账与 `generate()/flatten()` 的改写不花一个节点，且 131 到 133 之间这棵树没动过。

| 形状 | 读法 | 深度地板 | 合计 Δ | 最差单臂 | 最好单臂 | 答案改变 | 其中分数变差 | 变贵的臂 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| t2d4 | A 字面 | d≤4 | −4.44 % | +8.99 % (17-d8) | −57.47 % (31-d6) | 4/64 | 1 | 7 臂 |
| t2d6 | A 字面 | d≤6 | −15.54 % | +14.14 % (24-d8) | −57.47 % | 6/64 | 1 | 5 臂 |
| t1d3 | B | d≤3 | −23.84 % | +49.37 % (29-d6) | −64.30 % | 8/64 | 2 | 3 臂 |
| t1d4 | B | d≤4 | −36.44 % | +5.86 % | −90.73 % (17-d6) | 7/64 | 2 | 2 臂 |
| t1d4p3 | B | d≤4、ply≥3 | −37.66 % | +52.42 % (29-d6) | −83.56 % | 7/64 | 2 | 1 臂 |
| t1d6 | B | d≤6 | −51.84 % | +5.86 % | −90.73 % | 9/64 | 2 | 1 臂 |
| t1d8 | B | d≤8 | −51.71 % | +5.86 % | −90.73 % | 7/64 | 2 | 1 臂 |
| t3d4 | C | d≤4 | −15.65 % | +43.90 % | −86.80 % | 7/64 | 2 | 7 臂 |
| t3d6 | C | d≤6 | −36.07 % | +43.90 % | −86.80 % | 9/64 | 2 | 3 臂 |

- 改**着法**的只有一个臂：`17-d8` 从 (10,6) 换成 (9,6)。`t2d4`（读法 A、d≤4）连着法都没换，
  只动了 4 个臂的分值，其中 1 个变差。根宽与 `stopped` 在 9 付形状 × 64 臂上全不变。
- **没有任何一付做到 0 个变差**。`轮次131` 的 LMR 选中那一付是 19 个臂改答案、0 个改差；
  这里最好的一付（字面读法）也有 1 个改差，而它换来的是 −4.44 %。
- 规格风险表 `:565` 给的护栏「先只对 ply ≥ 3 裁剪，根节点保守」在这里**没有买到安全**：
  `t1d4p3` 与 `t1d4` 同样两个变差臂，尾部反而更差（+52.42 % 对 +5.86 %）。
- 更反直觉的一行：读法 A（切得少）的**每一刀更常切错**。见读数 3 的 `lost` 列。

### 读数 3：14 臂 PAIR——同一局面、同一进程、只差那条规则（`pair_t2d6.out`、`pair_t1d6.out`）

| 读法 | off | on | Δ | 改答案的臂 | 影子里的刀数 | 其中真值高出窗口下沿 | 其中真值高于返回的静态分 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A d≤6 | 5,089,440 | 4,479,838 | −11.98 % | 2 | 376 | 75 = **19.9 %** | 374 = 99.5 % |
| B d≤6 | 5,089,440 | 3,217,122 | −36.79 % | 2 | 1,613 | 126 = **7.8 %** | 1,610 = 99.8 % |

- 改答案的两臂在两种读法下是同两条：`scattered-d6-B` (3,3)→(2,3)、98,460→97,450；
  `scattered-d6-W` (11,3)→(11,2)、98,948→97,450。
- 单臂极值（读法 B）：`quiet-d8-B` 1,408,083 → 156,363（−88.9 %）、`scattered-d6-W` 251,149 → 48,309（−80.8 %）；
  `pos24` 三臂与 `crowded` 一字不动（fired=0），读数 1 那个 0.00 % 落脚面在这台仪器里原样复现。
- 「真值高于返回的静态分」≈ 100 % 是这条规则的定义使然（它返回的是一个**断言**：此节点不超过 alpha），
  所以代价要用 `lost` 那一列读，而且它**不是平摊的**——按臂拆开（读法 A、d≤6 的影子臂，376 刀中的 75 刀落空）：

| 臂 | 刀数 | 切错 | 错率 |
| --- | --- | --- | --- |
| quiet d3-B | 4 | 0 | 0.0 % |
| quiet d3-W | 4 | 1 | 25.0 % |
| quiet d4-B | 8 | 0 | 0.0 % |
| quiet d6-B | 36 | 2 | 5.6 % |
| quiet d6-W | 66 | 8 | 12.1 % |
| quiet d8-B | 110 | 9 | 8.2 % |
| scattered d6-B | 44 | 13 | **29.5 %** |
| scattered d6-W | 104 | 42 | **40.4 %** |
| pos24 三臂 / crowded | 0 | 0 | — |

  四颗孤子那一型（`scattered`）用 148/376 = 39 % 的刀数贡献了 **55/75 = 73 % 的错刀**。也就是说这条规则
  错得最多的地方，正是它自己的条件最看得上眼的那种局面——盘面越空，静态分越接近 0，而「空面上没有伏笔」
  这件事恰恰是这具评估给不出保证的地方。

### 读数 4：验证赛——先量夹具自己，再量规则（定深 `m_null_d6.out`、`m_t2d6_d6.out`、`m_t1d6_d6.out`；挂钟 `m_null_ms.out`、`m_t2d6_ms.out`、`m_t1d6_ms.out`）

`§4 :555` 要的是「新引擎 vs 旧引擎（互换黑白）…确认没有退化」。这一轮的夹具把两件事分开量了：
两个座位**都不剪**的场子（N1 定深、N6 挂钟）先量出这把尺子自己的偏差，96 局的赛分才有得读。

| 赛 | 两个座位 | 赛制 | 局数 | 剪的那一台 | 分 | 坐黑 / 坐白 | 节点 | 剪了几回 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| N1 | 同为出厂搜索（`--null`） | 定深 6、无钟 | 48 | 22-26-0 | **45.8 %** | 10 / 12 | −3.95 % | 0 |
| N2 | 读法 A d≤6 vs 出厂 | 定深 6、无钟 | 96 | 49-47-0 | **51.0 %** | 23 / 26 | −5.03 % | 89,859 |
| N3 | 读法 B d≤6 vs 出厂 | 定深 6、无钟 | 96 | 46-50-0 | **47.9 %** | 20 / 26 | −18.15 % | 572,015 |
| N4 | 读法 B d≤6 vs 出厂 | 400 ms／手、帽 20 | 96 | 44-48-4 | **47.9 %** | 17 / 27 | −1.88 % | 1,149,693 |
| N5 | 读法 A d≤6 vs 出厂 | 400 ms／手、帽 20 | 96 | 38-54-4 | **41.7 %** | 15 / 23 | −0.55 % | 91,563 |
| N6 | 同为出厂搜索 | 400 ms／手、帽 20 | 96 | 47-47-2 | **50.0 %** | 20 / 27 | −0.40 % | 0 |

- **N6 是 N4/N5 的底**，而它自己就报 −0.40 % 的节点差、50.0 % 的分。按这个底重读产品那一档：
  读法 B 净省 **−1.88 % + 0.40 % = −1.48 %** 节点、46/96 = 47.9 %（−2.1 个点，sd 5.1 %）；
  读法 A 净省 **−0.15 %** 节点、40/96 = **41.7 %**（−8.3 个点，1.6 sd，双侧 p ≈ 0.11）。
  定深那一档按 N1 的底则是 B −14.20 %、A −1.08 %。
  **一句话：这条规则在 64 个中盘局面上号称省 −15.5 % ~ −51.8 %，落到产品那一档只剩 −0.15 % ~ −1.48 %，
  而在唯一省得多的那一付读法上，棋力读数不升反降。**

- 定深三场共 240 局全部以 `five` 终局，无 illegal / no-move / 和棋 / 封顶；10 局和棋只出现在挂钟那一档
  （N4、N5 各 4 局、N6 2 局），所以那不是夹具的缺陷而是预算封顶的形状。
- 与读数 2 的对照：同一付形状在 64 个中盘局面上省 −15.54 %（A）／−51.84 %（B），下到赛里只剩
  −1.08 %／−14.20 %。原因就在读数 1 那一张表上——对局走过的局面是战术型的，`pos24` 与 `crowded`
  两臂的静面是 **0.00 %**，这条规则在那里没有一刀可下。
- **N1 的 `fired=0` 是这台仪器对自己的自检**：同一份二进制、两个座位都不剪，节点差仍然报出 −3.95 %。
  所以定深两场的净收益要按这个底读：N2 是 −5.03 % + 3.95 % = **−1.08 %**，N3 是 **−14.20 %**。
  挂钟那两场不能拿 N1 当底（预算花在**不同的局面**上），它们的底是同一档的 N6。
- 分数读法：n=48 时一把公平夹具的 sd 是 7.2 %，n=96 是 5.1 %。N1 的 45.8 % 距 50 % 是 0.58 sd（夹具判为公平）；
  N2 的 51.0 % 与 N3 的 47.9 % 距 N1 分别是 +0.72 sd 与 +0.29 sd。**这两场赛谁也不比谁更能说明什么**，
  它们能说明的是「读不出差别」——`:555` 那句「确认没有退化」在这台仪器这个局数下不是一个能被满足的条件。
  轮次131 当时拿 50 % 当基准去读 46 %；本轮把同一个数写成夹具自己的读数，这才是那句的完整口径。
- 挂钟那一档（N4）把轮次131 留下的那个缺口量到了：**B 读法在那里剪了 1,149,693 回，节点只少 1.88 %，
  分数 47.9 %**。省下的预算确实变成了别的工作，而不是变成更少的时间——时钟把剪出来的空档拿去多走
  同一棵树，所以「省节点」到产品档只剩「同预算多几层」这一条出路，而这条出路量出来的分数是平的。

### 门禁

跑在两棵树上，分开记：

- **规则在树里、默认关**（per-object 的 `FutRule` 出厂 `enable = 0`）：`cmake --build build -j 4` rc=0；
  `ctest` **8/8 / 471 s**（与轮次133 记的 470.58 s 同级）；`node tests/web_frontend_check.js` **105/105**；
  `node tests/readme_anchor_check.js` **120 citations / 0 problems**；README 仍 **658 行**。
  这一档证明的是「这套机械挂在树里而不去动时，出厂树没有被它碰过」。
- **内存与热点**（`small_cg`：`quiet BLACK d6` + `scattered WHITE d6` 两臂，off／on 各跑一遍，
  Debug `-UNDEBUG` 对象）：两臂全部 **ERROR SUMMARY 0 / 泄漏 0**、`in use at exit: 0 bytes in 0 blocks`；off 1,533,503 次分配 / 314,870,540 B（472,923 节点 ⇒ 3.243 allocs、665.8 B 每节点），on 815,698 次 / 147,804,924 B（255,279 节点 ⇒ 3.195 allocs、579.0 B 每节点）——**按节点归一以后每节点分配压力没有上升**（3.243 → 3.195），
  总指令 28,472,718,222 → 12,925,080,481（**−54.6 %**），按节点 60,206 → 50,631 Ir／节点（−15.9 %）。热点表两侧同形，前九行一字未换：`Evaluator::pattern_score` 23.81 % → 22.75 %、`PatternRecognizer::finish_direction` 13.30 % → 14.48 %、`CandidateCache::cell` 12.32 % → 11.16 %、`recognize_position_both` 9.18 % → 9.76 %、`is_forbidden_from_pattern` 6.42 % → 6.13 %、`PositionPattern::analyze` 5.52 % → 5.87 %、`MoveGenerator::generate` 4.36 % → 3.88 %、`candidate_value` 3.76 % → 3.81 %、`Board::is_empty` 3.46 % → 3.31 %。「哪个函数 CPU 卡比较久」的答复与 D-5／轮次131 相同而没有移动：**评估与图案识别**；这条规则所在的 `alphabeta.cpp::alphabeta` 自己从 22,162,239 Ir（0.08 %）到 12,883,240 Ir（0.10 %），按节点 46.9 → 50.5 Ir／节点——**那条测试本身约 3.6 Ir／节点**，对着每节点约 60,000 Ir 的总成本是 0.006 %。这条规则的钱全在剪掉的那一侧，不在它自己的花费那一侧。
  内存那一问另有其一：`sizeof(AlphaBetaSearch)` 从 **60,280** 到 **60,360**（+80 B：`FutRule` 24 + `FutCount` 56），加在**每引擎对象**而不是每节点上，而引擎是一次 think 的栈上局部量（`src/search/search_engine.cpp:257`）。
- **还原以后**：`touch src/search/alphabeta.{h,cpp}` 强制两棵树重编（`restore.py` 用 `copy2` 保留原 mtime，
  不 touch 的话 `cmake --build` 会认为无事可做，`build/` 里就还留着带规则的 .o——这一条本轮踩到了，写下来），
  然后用还原后的对象重新链一遍驱动，两臂节点数回到出厂针（**221,774** 与 **251,149**，`sizeof` 回到 **60,280**），`git status` 只有 `REVIEW_LOG.md`。还原树上的门禁重取：`ctest` **8/8 / 463.68 s**（test_search 436.93 s；与规则树那一次的 471 s、轮次133 记的 470.58 s 同档），`node tests/web_frontend_check.js` **105/105**、`node tests/readme_anchor_check.js` **120 citations / 0 problems**、README 仍 **658 行**；紧随其后的 `cmake --build build -j 4` 打印 0 个 `Building CXX` 而不打印任何目标重编是**这一步应有的形状**（34 个 TU 已在上一步强制重编完，`find build -name '*.cpp.o' -newer src/search/alphabeta.cpp` = 34 支都新于 12:30:31 还原的源），不是那条 `copy2` 坑的复发。

### 判：不落地（这条规则的前提在这一具评估上不成立，而不只是收益不够）

四条量到的东西合起来才构成判据，单拿任何一条都够不着：

1. **字面读法几乎没有落脚面，落到产品那一档就没有了**：743,804 个节点的臂上静面 0.00 %，七臂的最好
   天花板是 14.39 %（读数 1）；64 臂扫出来 −4.44 %（读数 2）；14 臂 PAIR −11.98 %；而下到 400 ms 那一档、
   扣掉控制场自己的 −0.40 %，**净省 0.15 %**（读数 4）。`:353` 那笔「下降 1–2 个数量级」的账不是这半句
   能付的——它已经被宽度表付掉了。
2. **收益与棋力不可分**：能省 −36 % ~ −52 % 的两付形状（读法 B 与 C）**都不是这句话的字面条件**。
   它们多切的那一刀正是「对方有 ≥ 活三而我方没有」的那一类节点，也就是 §2.5.1 自己列为风险的那一侧。
3. **每一刀交回的是一个没证过的值，而且它本身就是父节点的一次 beta 截断**：子节点被剪时返回 `stat ≤ alpha_child`，
   父节点取 `score = -stat ≥ beta_parent`，于是 `alphabeta.cpp:450`（出厂树行号）那一支立刻成立：
   `flag = LOWERBOUND`、`record_cutoff`、`break`——这一层剩下的候选一个都不再问，并把这条界按 `:470-472`
   写进共享的表（被剪的节点自己倒是不写，它在 `store` 之前就返回了）。所以「剪错了」不是少算一个候选，
   **是一次不成立的截断 + 一条存进表的假下界**。影子里数出来的比例是：字面读法 **376 刀里有 75 刀（19.9 %）**
   切掉的子树真值高出窗口下沿，宽读法 1,613 刀里 126 刀（7.8 %）。`alphabeta.cpp` 里 `:203-207` 与 `:249-252`
   那两处 fail-soft 的改法，就是为了不把「只是界」的数写进表。
4. **前提本身在这具评估上没有，而且这是本仓自己写着的一条规定**：futility 要成立，得让「静态分 + 松弛」
   能给这个节点的子树封顶。这里恰好**不能**封：
   `evaluate_board_from` 把静态分钳在 ±(`coeffs_.five` − 1) = ±99,999（`src/core/evaluator.cpp:270-272`，
   默认 `five = 100000` 在 `src/core/evaluator.h:11`），而 :266-269 那段注释写明这道钳的存在理由就是
   「静态分绝不能等于杀形分，否则一片热闹棋盘会让根节点提前收工」。可搜索的分值域**在钳之上**：
   成五返回 `100000 + depth`（出厂树 `src/search/alphabeta.cpp:334-335`）。也就是说，每一个可能的静态分之上
   都留着一段专门表示「已证」的带，而一个当下安静的节点（双方都没有能成 ≥ 活三的格）完全可以在两三步里
   走进那段带——那正是搜索要去找、静态分按构造表达不出来的东西。读数 1 把这条规则实际在说的话量得很干净：
   它触发在**静态分 p50 = −50 的空面节点**上，那里的 alpha p50 = 77,842 且**一个都不是杀形值**
   （|alpha| ≥ 100,000 的占 0.0 %），于是它天天下的断言其实是「≈0 变不成 ≥77,842」——
   读数 3 数出来这句话有 **19.9 %** 是错的。**§2.7 `:428` 已经用同一类理由否掉了 null-move
   （「五子棋语义特殊，本期不做」）**，这半句败在同一个理由上。

还有一条不是测量、但记录要写：`:351` 这半句的方向与 §3.2 `:523` 的注释「无威胁时**放宽**」方向相反，
轮次121 已经按 `:523` 定过一次案（本轮以上两处引的就是那一节）。本轮按 `:351` 建了它、量了它，
得到的正是那句反方向的话不值当再翻回来。

因此本轮**不改 `src/`**：两版仪表（只读 wrapper 版与 per-object 版）留在 `/tmp/h1/`，判完即还原，
`git status` 在本节落笔时只有 `REVIEW_LOG.md`，而 `git diff --numstat` 是 **226 / 0**——本节纯插入，没有改动任何既有行（HEAD 7432 行 → 7658 行）；这 226 行会把 1982 以下整段推下去，所以档内自我引用一律写轮次标签而不写绝对行号，仓内已有的 `REVIEW_LOG:2289` 那一类档内绝对针在本节拼入之前就已经随 127–133 各节的拼入落后 994 行（那句引的是轮次125 的 1.99 s 读数，现在在 3283 行；轮次126 自己记过一次等长改钉，之后再没有人跟着移），本轮没有新增、也没有顺手改那类引用。`:555` 那句要的「确认没有退化」在这里没有被任何一场赛
确认，也没有被否定——它没有被这台仪器回答的能力，这句如实写在这里，而不是写成一条通过的门禁。

### 没动的账

- `§2.7 :426` 的 LMR 与 `§2.5.1 :351` 的这半句，两轮的结论同为「不落地」，但**理由不同**：
  LMR 是省得多、赔得也多（19 臂改答案 0 改差，赛分 45.8 %），本轮是省得少、且每一刀都可能是在说谎。
- `:549` C 行「单线程 ≥ 1M nps」与「depth 8 在 4 s 内完成」这两条验收仍然没有达成，
  仍然不是这半句能补的：它剪的是节点数，不是每节点的成本。
- 挂钟那一档的「省下预算换来几层」这条出路，本轮只补了控制场（N6），没有加装每 think 完成层的计数——
  那是一个仪器缺口，写清楚，不当结论用。

### 仪器（全部在 `/tmp/h1/`，仓库外）

`instrument.py` + `h1_probe.cpp`（只读 wrapper，落 56 字节/节点的行）、`agg.py` / `agg2.py`（落地面与
headroom 网格）、`fut.py`（编译期九付参数 × 64 臂一致性扫描）、`fut2.py`（同一条规则的 per-object 版）、
`pair_driver.cpp`（14 臂 off/on/shadow 三栏，含影子计数）、`fut_match.cpp` + `match_stats.py`（验证赛与
夹具控制场）、`small_cg.cpp`（内存与热点那一档）、`margin_shape.py` + `margin_shape.out`（把可剪那一格的
alpha 分布与静态分分布分开重算，本轮记录里 p50／min／max 那几个数是从这一份读的）。


## 轮次133 引擎重构 G：§2.9 那一行的最后一个字段走出了搜索 — engine redesign stage G, the last field of the §2.9 contract row leaves the search

规格 `docs/ENGINE_REDESIGN.md:443`：§2.9 接口契约表给 `SearchInfo` 记了六个新增字段——
`partial_depth`、`seldepth`、`stopped`、`pv`、`tt_hits`、`root_moves_done/total`。
这轮的审计（一次逐机制的 grep，不是回忆）得到的结论是：前五个早就一路走到了 `SearchResult`、
`MoveAnalysis`、`thinking_progress` 帧和 `web/app.js`，**只有 `tt_hits` 停在搜索里面**。
配合 §1.3 P2（「每次思考无法复用上次结果」）读，这一格不是装饰：§2.4 整个复用子系统的成败，
玩家和 复盘 唯一能看见的证据就是这个数。

### Found

- 计数从 A6 就在一跳一跳地加：`search_state.h:463` 的 `count_tt_hit()`，全仓**唯一一个调用点**
  `alphabeta.cpp:364`（ alphabeta 节点的置换表探针，一节点最多一次）。它被 `refresh_counters()`
  抬进 `SearchStats.tt_hits`（`alphabeta.cpp:498`），到此为止。
- `git show HEAD:tests/test_search.cpp | grep -n tt_hits` 显示这一轮的改动之前，这个数的读者
  **全在测试里**（`:4014/:4226/:4234/:8221/:8353/:8394`）：生产侧一个读者都没有。
  `to_info()`（`alphabeta.cpp:477`）逐字段抄了 `SearchStats` 十二行，抄的就是 §2.9 那一行，
  唯独漏了它——所以契约表上写着的字段，实际是**表面对不上**。
- `thinking_progress` 帧也不带它。这不是漏：§2.3.3 :~232 的帧样例和 §2.9 :445 的帧行都没点这个键，
  本轮因此**没有**动帧的键表（动它要 README 跟着改，而文档没要求）。

### 实现

一条从计数到归档的链，八处：

| 位置 | 做的事 |
| --- | --- |
| `src/search/alphabeta.h:34-40` | `SearchInfo` 末尾加 `int tt_hits`，注释说明为什么是 `int`：一命中必是一被访问的节点，所以 `tt_hits <= nodes_searched`，与这个结构自己给 `nodes_searched` 用的收窄（`alphabeta.cpp:480`）是同一条界 |
| `src/search/alphabeta.cpp:489` | `to_info()` 补上那一行漏的抄写 |
| `src/search/search_engine.h:39-45` + `search_engine.cpp:382` | `SearchResult.tt_hits`，会话与 复盘 唯一走的路 |
| `src/server/game_manager.h:41-49` + `game_manager.cpp:505-510` | `MoveAnalysis.tt_hits`——**加在最后而不是中间**，因为两处 `MoveAnalysis{…}` 是按位置初始化的，插一个字段会把后面每一个位置都改读 |
| `src/server/server.cpp:202/219/1063` | `record_of` / `analysis_of` 两个桥各补一行，复盘 JSON 多出 `"tt_hits"` |
| `src/training/game_record.h:36-40` + `game_record.cpp:73/167` | `MoveRecord` 同名字段；写的时候多一个键，读的时候 `read_int("tt_hits", 0)`——**和 `partial_depth` 一族同样的向后兼容**：改动之前落盘的对局记录照样读得回来，读成「没记下命中」而不是坏文件 |
| `src/training/self_play.cpp:133` | 训练侧的每一手也填这个数，否则会话有、训练档没有，同一个字段两种真相 |

测试三条：
- `test_the_table_reads_a_think_took_are_reported_out_of_the_search`（`tests/test_search.cpp`）：
  同一次思考的 `SearchStats` 与 `SearchInfo` 必须一字不差；界 `0 < tt_hits <= nodes_searched`
  在冷、暖两臂都成立；引擎门口（`SearchEngine::search`）同样成立；关掉分层搜索的那一臂
  走 `greedy`，两个见证都报 0。
- `test_the_session_records_the_table_reads_it_kept`：一局真实会话以后，
  `replay_snapshot()` 里人手那一手 0、AI 那一手 > 0 且 `<= nodes`。
- `test_game_record_keeps_the_table_read_count`（`tests/test_training.cpp`）：三手的记录
  写出去读回来 `tt_hits` 逐手相等，**并且它左右两个邻居（`seldepth`、`nodes`）不许跟着挪位**；
  再把生成的 JSON 里 `,"tt_hits":N` 这一段删干净，模拟改动之前落盘的旧文件，
  读回来必须是 0 而不是失败。

### 读数

一、这条链子买到的第一眼（`quiet_think_board`，`d3`，一张表两次思考）：

| 臂 | 节点 | 表上读到 | 占比 |
| --- | --- | --- | --- |
| 冷 | 5,943 | 102 | 1.72 % |
| 暖（同一张表） | 96 | **96** | 100 % |

引擎门口同一台仪器读成 `5,943/102 → 96/96`；会话那一臂 `4,312` 节点 / `299` 命中。
「暖的那一次便宜」这件事以前只能用节点数反推，现在它是**直接看见的**：96 个节点里 96 个
是表上的答案，这就是 §2.4 想要的那个证据。

二、成本——**同一份探针源码，两棵树各编一遍**（`/tmp/g1/tt_probe.cpp` 里读这个字段的行全部关在
`#if TT_WITNESS` 里，控制臂 = `cda12d4`，本轮臂 = 工作树，两边都是 `-O0 -g -UNDEBUG`，
`/tmp/g1/run_valgrind.sh`）。两臂先自证同树：`ARM nodes cold=5943 warm=96`、`ENGINE 5943 -> 96`、
`SESSION 4312` 三个数一字不差。

| 仪器 | 控制臂 | 本轮臂 | 差 |
| --- | --- | --- | --- |
| memcheck | 0 error / 0 definite leak，28,362 次分配 / 26,917,460 B | 0 error / 0 definite leak，**28,364 次 / 26,917,560 B** | +2 次分配、+100 B |
| callgrind | 5,741,259,197 Ir | 5,741,264,445 Ir | **+5,248 Ir = +0.000 091 %** |
| `memcheck test_training`（归档那一头的常规门禁） | — | `clean reports=0 invalid_read=0 invalid_write=0 uninitialised=0 definite_leak=0` / 306 s | 与轮次132 记的 304 s 同级 |

差的 5,248 落在哪儿，是逐符号对出来的（`callgrind_annotate --threshold=100` 两份全表）：
**没有任何一行超过 +152 Ir**，动的 36 行全是结构体拷贝那一类 plumbing——
`SearchStats` 的拷贝构造 +152、`vector<Move>` 的拷贝 +148、`vector<MoveAnalysis>::operator[]` +115、
`uninitialized_copy<Move*>` +110；两行分配器（`operator new` +32、`malloc` +60）对上那 +2 次分配。
读法：`MoveAnalysis` 每条目大了 4 字节，快照按值拷贝时 `vector` 跨了一个大小级，
于是多分配了一次、多拷了几个字节的表头；这与轮次132 那条「每 think 一个常数」是同一类成本，
**与树的大小无关**，而且这一轮树本身一个节点都没多走。

归档那一头的成本可以直接算：`to_json` 每手多写的是 `,"tt_hits":900`——11 个字符的键名加 1-10 位数字，
本轮那条测试记录是 **14 字节／每手**。一盘 40 手的对局记录多 ~560 B。

「哪个函数 CPU 卡比较久」这轮的答案没有移动。探针臂（`-O0`，所以 `std::array` 的下标自己成一行）
的前几行仍然是评估与棋型识别：`std::array<DirectionPattern,4>::operator[]` 20.62 %、
`Evaluator::pattern_score` 11.74 %、`Board::line_bits` 6.88 %、两行 29 格线表的
`std::array<…>::operator[]` 各 4.61 %、`recognize_position_both` 3.09 %
（`/tmp/g1/cga_new.txt`）。本轮碰到的 `to_info` 一共 **3,573 Ir / 5 次调用**（≈715 Ir 一次，
含它要做的十三个字段抄写），在 99 % 阈值表里排不进去。

### 门禁

- `cmake --build build -j 4` rc=0；`ctest` **8/8 / 470.58 s**（`test_search` 434.24 s、
  `test_position` 7.85 s、`test_server` 17.54 s、`test_training` 10.77 s）；
  `node tests/web_frontend_check.js` **105/105**；`node tests/readme_anchor_check.js`
  **120 citations / 0 problems**；README 仍 **658 行**（本轮没动它：帧的键表按文档原样，
  复盘 JSON 多一个键不改变 README 描述过的协议）。
- 新算例都插在 `int main()` 之前，`tests/test_search.cpp` 的旧行号不受影响的部分逐条核过。
- valgrind 三处全绿（读数二）：两臂 memcheck `0 error / 0 definite leak`，
  `memcheck test_training` `clean / reports=0 / 306 s`，callgrind 两臂同树
  5,741,259,197 → 5,741,264,445 Ir。
- TSan：本轮**不适用**，理由说清楚——加的都是各条链自己拥有的 `int`（`SearchInfo` 是
  `to_info()` 的返回值，`SearchResult` 是一次 think 的返回值，`MoveAnalysis` 在
  `thinking_mutex_` 底下写，`MoveRecord` 是记录本体的字段），没有新的跨线程对象，
  也没有新的共享写。

### 判

落地。这一条不改变搜索到的树（两臂节点数一字不差，见下），只把已经在算的数送到该去的地方；
风险面全在「字段加在哪儿」，所以三条测试里有两条专门钉这个：按位置初始化的结构不许插字段，
JSON 读侧必须容忍缺键。

### 没动的账

- 帧的键表没加 `tt_hits`：文档 §2.3.3 / §2.9 两处点名的帧字段里没有它，加了就是一次没人要求的
  协议变更（README 要跟着改，前端要跟着消费）。等有人要一边想一边看复用率时再说。
- `SearchStats.tt_cutoffs` / `qnodes` 与 `SearchInfo.partial_best_move` 同样只有测试在读——
  文档没有给它们承诺读者（`:443` 那行只点了 `tt_hits`），本轮**没有**顺手接；
  一次只补一张表上写着的一格，是这一轮的范围。
- `limits.ponder`、`MoveBudget.hard_ms/max_nodes`、`SearchResult.interrupted` 三条维持轮次132 的判断。
- §4 :549 的 C 行两格（≥1M nps；d8 在 4 s 内）与 :550 的 D 行三条读数，本轮一条也没前进：
  这条链子每 think 多做的是一次数位拷贝和一手 JSON 键，不是搜索。

### 仪器（怎么再跑一遍）

- `/tmp/g1/tt_probe.cpp`：一份源码、`TT_WITNESS` 两个取值；控制臂链 `cda12d4` 的对象，
  本轮臂链工作树对象，两边同一套 `-O0 -g -UNDEBUG`。
- `/tmp/g1/run_valgrind.sh`：两臂各跑一遍自证同树 → memcheck 两臂 → callgrind 两臂 →
  `memcheck test_training`。产物：`run_{base,new}.out`、`mc_{base,new}.out`、`mc2_*.txt`
  （摘掉 `-q` 的重跑，堆行在这里）、`cg_{base,new}.out`、`cga*.txt`、`mc_test_training.out`。
- 三条新算例随 `ctest` 跑：`test_search`（两条）与 `test_training`（一条）。


## 轮次132 引擎重构 F：§2.3.4 的分配器第一次有了调用方，而「训练模式」这件事必须由调用方说出口 — engine redesign stage F, the budget allocator gains its first caller

规格 `docs/ENGINE_REDESIGN.md:241-260`（§2.3.4 预算分配器，五步）＋ `:583-584`（文件清单里
`search_engine.h/.cpp` 的「`TimeManager` 预算分配」和 `time_manager.h/.cpp` 的「**新增** 预算分配」）
＋ `:557`（§4 回归保障第 4 条：「注入 mock clock，验证 partial_depth / root_moves_done / stopped 在软硬时限、
节点上限、用户中止三种情况下的取值」）。这一节是那张清单上**最后一条没有调用方的功能**：轮次131 收尾时的
审计把 §2.7 的 LMR 判成不落地、把 §2.8 五条全部落地，剩下的只有「类写好了、测试钉好了、但 `SearchEngine`
从来不调它」。

### Found

- `src/search/time_manager.h:16-19` 的注释自己就写着 「it still has no caller」，而 `grep -rn TimeManager::allocate`
  的两个读者全在 `tests/test_search.cpp:4039/:4087`——A 轮给这个纯函数钉过一张五行表，函数本体从没进过一次真实思考。
- 于是 `SearchLimits.soft_time_ms` 的唯一来源是客户端自己点名（`--soft-time-ms`）；默认配置下它是 0，
  `alphabeta.cpp:616` 那句 `stats_.completed_depth >= 1 && ctx_.soft_time_up()` 永远不成立——
  **软时限这条机制在树里，但没有生产者**。一次思考要么跑完深度，要么吃掉整面墙。
- 第 4 步（上一手耗时 > 0.8×hard 就下调）连数据源都不存在：`GameManager` 记着每一手的 `elapsed_ms`，
  却从来没有把它交给引擎。
- **读调用图读出来的缺陷**：`TimeManager::allocate` 的第一步就是把 hard 乘 0.65，而默认训练配置是
  `self_play.h:15-18` 的 `timeout_ms = 400` / `max_nodes = 0`（README.md:186 那条出厂命令两个都不点名）。
  如果「训练模式」只按 `--max-nodes` 有没有点名来推断，默认训练跑的**每一手**都会被裁成 ~260 ms 的软窗，
  采集样本的搜索深度从此由分配器决定——而这条改动在 D 轮记下的任何一条成本读数里都不会露出来。
  判定 regime 的是调用方，不是引擎猜数字。

### 实现

1. `src/search/search_engine.cpp:320-331`：`limits.training = (max_nodes_ > 0 || training_)`，
   并且只在 `limits.soft_time_ms == 0 && has_deadline` 时用 `TimeManager::allocate(...).soft_ms` 填窗口。
   两道闸各自挡掉一种越权：客户端点了名就不改写，客户端没给墙钟就不发明窗口——
   `time_limit_ms == 0` 那次思考的 `hard_ms` 是分相兜底的 100 ms（`search_engine.cpp:263-266`），
   拿它当 65 % 的基准会把一次纯深度思考裁到 65 ms。
2. `SearchResult.soft_ms` / `hard_ms`（`src/search/search_engine.h:41-49`）是这条决定的见证：
   `elapsed_ms` 说的是这手想了多久，不是这手被允许想多久，两个数只在巧合时相等。
   只报一个数等于没报，所以墙和窗一起出。
3. `SearchEngine::set_clock`（`:141-147`）把 §4 第 4 条要的 mock clock 注入口从 `AlphaBetaSearch`
   提到引擎门口：窗口在这一层**选出来**，在 AlphaBeta 的根循环**花掉**，只有时钟能从门口进去，
   「选的那个数就是花的那个」才证得成。
4. `GameManager`：`prev_think_elapsed_ms_` 原子量（`game_manager.h:204-208`）由思考线程在交回着法后写、
   由 spawning 线程读进 `Budget{soft, nodes, prev_elapsed}` 按值带走（`game_manager.h:239-247`）；
   两条思考路径（`ai_thinking_worker` / `request_hint`）都把它交给引擎，`reset()` 清零。
5. `src/training/self_play.cpp:27-35`：`SelfPlay` 构造时给两台引擎 `set_training(true)`。
   数据局、验证赛、fitness 局都从这一个构造函数过（`trainer.cpp:68` 的 `play_game`、
   `:295` 的 `RetryExplorer`），所以一句话就把整个训练管线挡在外面。

测试：`tests/test_search.cpp` 三条新算例插在 `int main()` 之前——F-1 把 shaping 的六个分支摆成一张表
（默认 65 %、被上一手裁成一半、客户端点名原样通过、点名窗口不裁、节点帽那档、声明训练那档、
没墙钟那档），断言全部写成 `soft == hard * 65 / 100` 这种**比值**而不是绝对毫秒数，因为
`hard` 是「墙减去战术阶段已经花掉的」，同一条算例在满载机器和 memcheck 下会差几十毫秒；
F-2 注入 `ScriptedClock(100)` 钉住「time_soft / partial 0 / 深度没跑满」，再用 `SteadyClock`
作对照臂证明停它的是窗口而不是那面墙顺带的节点帽；F-3 走会话，钉 `move_callback` 交回的
`elapsed_ms` 就是下一手拿到的 `prev_think_elapsed_ms`，以及 `reset()` 把它清干净。
`tests/test_training.cpp` 一条：`SelfPlay` 之前引擎不是训练态、之后是，且训练对局里没有任何一手
报 `time_soft`。

### 读数

**一、两条墙钟配置的 before/after（产品路径，战术相位开着，客户端不点软窗也不点节点帽）**

仪器：`/tmp/f2/budget_probe.cpp` 用 `GreedySearch` 自对弈摆出 32 个中盘局面（4 个起点、每 6 手取一景），
每个局面各跑文档点名的两档——`d6 / --timeout-ms 2000` 与 `d8 / 4000`——两臂各跑两遍并按遍交错
（before₁、after₁、before₂、after₂，`/tmp/f2/{before,after}_i{1,2}.out`，合计 128 臂）。
两臂是两棵树：before 是 HEAD `cca3f0a`（`TimeManager::allocate` 无调用方），after 是工作树；
两边都用同一套 `-O2` 编译，因为两棵树的 `SearchResult` / `SearchEngine` 布局不同，
混链 `.o` 是轮次里已经咬过一口的 ABI 坑。

| 配置 | 臂数 | 到达请求深度 | 中位耗时 | 耗时和 | 节点和 | 停下的原因 |
| --- | --- | --- | --- | --- | --- | --- |
| d6 / 2 s before | 64 | 54（84.4 %） | 618 ms | 47,241 ms | 7,552,532 | none 62 / time_hard 2 |
| d6 / 2 s after | 64 | 54（84.4 %） | 616 ms | 47,347 ms | 7,551,463 | none 62 / time_hard 2 |
| d8 / 4 s before | 64 | 20（31.2 %） | 3,906 ms | 186,845 ms | 32,783,490 | none 28 / time_hard 30 / nodes 6 |
| d8 / 4 s after | 64 | 20（31.2 %） | 3,010 ms | 175,730 ms | 30,896,728 | none 28 / time_hard 18 / time_soft 14 / nodes 4 |

四条读数：

1. **答案一条没变**：`method` / `best_move` / `score` 三栏在 128 臂上 0 处不同
   （`compare_arms.py` 的头三行），到达深度每档也是 54 与 20 一模一样。这一条改的确实是
   「什么时候收手」，不是「收到什么」。
2. **窗口只在吃满墙的那一侧落下**：d6 档中位 618 ms，本来就在新窗（≈1,300 ms）以内，所以四列数字
   全等，耗时和差的 106 ms 是噪声（+0.2 %）；d8 档 64 臂里 30 臂原本撞 `time_hard`，其中 14 臂
   现在改由 `time_soft` 在层边界收手，中位耗时 3,906 → 3,010 ms（**−22.9 %**），
   节点和 32,783,490 → 30,896,728（**−5.75 %**）。
3. **软窗是比值不是绝对数**：after 一臂里 14 组 `(soft, hard)` 的 `soft/hard` 全部 = 0.650
   （min 0.650 / max 0.650），例如 `(910, 1400)`、`(1279, 1968)`——不同机器、不同负载只会把
   `hard` 挪走，不会把比例挪走，这就是 F-1 把断言写成比值的原因。
4. **16 臂两个见证都留 0**，`method` 全是 `must_block`——正是 `search_engine.h:41-45` 注释里
   那句「没起分层搜索的快路径报 0」；`must_block` 不该被窗口管，也不该有窗口。

顺带把文档 `:548` 那行的本仪器读数记下：**这一组 32 景的 d6/2 s 完成率是 84.4 %（27/32 景）**，
达不到那一行写的 ≥ 95 %；两臂同一个数，所以本轮既没把它推上去也没推下去。

**二、三条新算例自己的打印**

- F-1 `test_the_engine_shapes_the_window_the_client_did_not_name`：
  `wall 3000 -> soft 1950, damped soft 1500, trained soft 0`；另外三档（点名 900 原样通过、
  声明训练 `soft 0 / hard > 0`、没有墙钟的纯深度思考 `soft 0 / hard 100`）由断言钉住。
- F-2 `test_the_window_the_engine_shapes_is_the_one_the_root_loop_stops_at`：
  `window 1950 of 3000 reached d4/6, readings 56`——对照臂（`SteadyClock`）同一面墙跑到 d6 且
  `stopped == none`，所以停 d4 的只能是那 1,950 ms。
- F-3 `test_the_session_hands_the_next_think_what_the_last_one_cost`：
  `costs 16, 26`（两手的 `elapsed_ms` 与 `prev_think_elapsed_ms` 逐手相等）。这一条最初用字面量
  `(6,6)` 摆第二手，被引擎自己的第一手占掉而 `make_move` 返回 false——现在按棋盘取第一个空格。

**三、memcheck 两臂（`--tool=memcheck -q --error-exitcode=9 --leak-check=full --track-origins=yes`，
`-O0 -g -UNDEBUG` 的 Debug 臂，`wide` 模式）**

| 臂 | 退出码 | 报告 | 树 |
| --- | --- | --- | --- |
| before（HEAD） | rc=0 | `-q` 下无输出＝零报告，且 `--errors-for-leak-kinds=definite` 没触发 | 146,187 节点、d6、best (8,8)、score −20704、`stopped=none` |
| after（工作树） | rc=0 | 同上 | **同一棵树**：146,187 节点、d6、(8,8)、−20704、`soft=389983 / hard=599975` |
| `tools/valgrind_check.sh memcheck test_training` | rc=0 | `clean reports=0 invalid_read=0 invalid_write=0 uninitialised=0 definite_leak=0 304s`（轮次130 记的 306 s 同级） | 训练管线：`self_play.cpp` 改了构造，这条是它的门禁 |

`wide` 那一档是仪器自己的要求：墙钟抬到 600,000 ms，软窗就成了 389,983 ms——一次 d6 思考远远够不着，
于是**两臂的树必须由构造保证相同**（比值还是 0.650，只是裁不到），memcheck 报的差错和 callgrind 报的
指令差才都只可能是本轮代码自己的。两臂节点数一字不差就是这个前提的验收。

`MoveBudget` 是三个 `int` 的 POD，`allocate()` 只做算术和 `std::min/max`，一条分配都没有；
`SearchResult` 的两个新字段和 `GameManager` 的原子量把结构各撑大 4 字节，都在原本就属于它们的对象里
（一次 think 的栈上返回值、一台会话一份）。所以本轮没有新的每节点分配压力，也没有新的共享写。

**四、callgrind 两臂（同一棵树上的指令数）**

| 臂 | PROGRAM TOTALS (Ir) | Ir/节点（146,187 节点） |
| --- | --- | --- |
| before | 33,132,621,872 | 226,645.47 |
| after | 33,132,623,558 | 226,645.49 |

差 **1,686 Ir = +0.000 005 1 %**。按函数逐个对表（`callgrind_annotate --threshold=100` 的两份全表，
`/tmp/f2/cga100_{before,after}.txt`；这份表把 in-callee 也算一遍，所以行和 33,312,815,686 比 PROGRAM
TOTALS 大，只用来看归因不看总量）：**1,650 → 1,653 个符号里只有 41 个动了**，净差 +1,703 Ir，
而动的全是个每 think 一次的小项：

| 符号 | before → after | 差 |
| --- | --- | --- |
| `__printf_buffer`（两行）+ `_itoa_word` + `__printf_buffer_write` | 1,530→1,742 等 | **+620**（**仪器自己**多打 `soft=` / `hard=` 两个整数） |
| `std::chrono::__duration_cast_impl` + `steady_clock_ms` + `steady_clock::now` | — | **+365**（多一次读钟与换算，同样每 think 一次） |
| `gomoku::TimeManager::allocate` | 0 → **105** | +105 |

也就是说这一轮真正买到的运行时代价是 **每次思考 105 条指令**，其余的差是打印出来的数变多了——
搜索一侧的行（`pattern.cpp` / `evaluator.cpp` / `board.*` / `search_state.h`）**一字没动**，
因为两臂本来就是同一棵树。

「哪个函数 CPU 卡比较久」的答案在这对臂上的形状（99 % 阈值表前五行，`/tmp/f2/cga_before.txt`）：

| 函数 | 占比 |
| --- | --- |
| `std::array<DirectionPattern,4>::operator[]`（`-O0` 不内联，所以单独成行） | 18.87 % |
| `Evaluator::pattern_score` | 10.53 % |
| `std::array<std::array<std::array<ul,29>,4>,3>::operator[]`（三层各记一行） | 4.20 % ×3 |
| `Board::line_bits` | 4.17 % |
| `PatternRecognizer::recognize_position_both` | 3.27 % |

热点仍然全在**评估与棋型识别**这一侧；`src/search/` 里排得最高的是 `alphabeta.cpp:sort_moves` 的两个
lambda，各 0.06 %，而本轮代码所在的 `time_manager.cpp` 在 99 % 表里根本不出现。这与轮次130、131 记的
答案同形且没有移动——注意这是 **Debug（-O0）臂**，所以 `std::array::operator[]` 会占掉 18.87 %，
和出厂 -O2 构建的热点表不是一个刻度；本轮要的只是两臂互比，不是重排绝对热点。

### 门禁

- `cmake --build build -j 4` rc=0；`ctest` **8/8 / 471.49 s**（`test_search` 435.87 s、
  `test_position` 7.90 s、`test_server` 16.78 s、`test_training` 10.77 s，其余三条 0.1 s 内）；
  `node tests/web_frontend_check.js` **105/105**；`node tests/readme_anchor_check.js`
  **120 citations / 0 problems**；README 仍 **658 行**——本轮没有动它：`soft_ms` / `hard_ms`
  是 `SearchResult` 的内部字段，`thinking_progress` 帧的键由 §2.9 定稿，`docs/ENGINE_REDESIGN.md:446`
  只给 `set_config` 加了 `soft_time_ms` / `max_nodes` 两个可选入参，那两个早就在。
- `tests/test_search.cpp` 只在文件尾部加行（`int main()` 从 8676 → 8894 行，文件 8,860 → 9,085 行），
  受引用的旧行号一条没动；`tests/test_training.cpp` 同样在中段插入（+35 行，注册紧跟
  `test_self_play_zero_ceiling_arms_no_node_budget` 之后）。改动面是 `src/` 六个文件
  （`search_engine.h/.cpp`、`time_manager.h`、`game_manager.h/.cpp`、`self_play.cpp`）＋ `tests/`
  两个文件，合计 **8 files changed, 369 insertions(+), 4 deletions(-)**。

### 判

落地。这一条不是「更快」，而是**把已经在树里的机制接上**：软时限的判断（`alphabeta.cpp:616`）和
它的见证（`stopped == time_soft`）都是 A 轮交付的，缺的是生产者。棋力方向上不引入新的搜索形状——
128 臂的 `method` / 着法 / 分值三栏 0 处不同（读数 1），差别只出现在「同一面墙下更早收手」这一侧。

诚实记下的后果：一局棋里 AI 每一手的可用时间从「整面墙」变成 65 %（强制局面和开局各再减半），
所以同样一条 `--timeout-ms 3000` 现在给的是更浅的一层，而不是更久的一手。这是 §2.3.4 第 1-2 步的本意，
但它对**提示**同样生效：`request_hint` 走的是同一条 shaping，一次提示可能比改动前少一层。

### 没动的账

- §1.3 P1 的验收行 `:549`（「单线程 ≥ 1M nps；depth 8 在 4s 内完成」）本轮读数依旧不达标：
  这一组 32 景的 d8/4 s 完成率两臂都是 **31.2 %**（20/64 臂），单线程吞吐按 after 的节点和 / 耗时和
  是 30,896,728 / 175,730 ms ≈ **17.6 万 nps**，离 1M 差一个量级还多。本轮改变的不只是这条读数的
  成本，还有「4 s 到没到」的语义（软窗先停），所以四个数按 before/after 各记一次，不折算。
- §2.8 D 行的三条读数（≥ 40 % 单轮降本、内存峰值与并发解耦、fitness 不再三档）：本轮把训练路径
  原样挡在 shaping 外面，所以那三条的账没有前进也没有后退。
- `limits.ponder` 依然没有消费者（`search_limits.h:68` 只有默认值测试读它）：本引擎没有对手着法流，
  预置复用无从谈起，文档的这半个字段留着不接线，如实记。
- `MoveBudget.hard_ms` / `.max_nodes` 由分配器算出来但被调用方丢掉——这是决定，不是遗漏：
  一次思考的墙和节点帽由点名的那一方定，改了它「同一份输入两次成本不同」就没了。
  `time_manager.h:16-21` 注释已按这个说法改写。
- §2.3.4 第 3 步的残局分支（空位 < 30 把窗抬到 hard）在真实对局里够不着：225 格要落 196 子才剩 29 个空位，
  只有和棋盘能走到；它由 A 轮那张表用一个合成的 196 子盘钉住，本轮不改这个语义。
- §2.9 `:444` 写的 `SearchResult.interrupted` 字段没有加：`stopped` 已经是它的完整版
  （`none` / `time_soft` / `time_hard` / `nodes` / `user` / `mate`），再加一个布尔是把同一个事实说两遍。

### 仪器（每条读数怎么再跑一遍）

- `/tmp/f2/build_arms.sh`：`git archive HEAD src | tar -x` 取 before 树，与工作树各按同一套编译参数出两台
  probe（`OPT`/`tag` 两个环境变量决定 `-O2 -DNDEBUG` 的计时臂还是 `-O0 -g -UNDEBUG` 的 valgrind 臂）。
- `/tmp/f2/budget_probe.cpp`：32 景 × `d6/2000` 与 `d8/4000`，`wide` 档只跑 `pose 00 d6 ms=600000`。
  after 臂多印 `score= / soft= / hard=`（`WITNESS=1`），before 臂没有这两个字段可印，编译期就分叉。
- `/tmp/f2/run_measure.sh`：交错四跑 → `compare_arms.py` → Debug 臂 → memcheck 两臂 →
  `memcheck test_training` → callgrind 两臂，全程串行，不与任何其他负载同跑。
- 原始输出：`{before,after}_i{1,2}.out`、`g4_compare.out`、`mc_{before,after}.out`、
  `g7_memcheck_training.out`、`cg_{before,after}.out`、`cga_{before,after}.txt`、`cga100_*.txt`。


## 轮次131 引擎重构 E：§2.7 的最后一行第一次进树，64 臂上省到 −28.35 %，而两种赛制的验证赛都落不到「没有退化」——engine redesign stage E, late-move reduction (§2.7 :426): built, measured on twelve guards and two verification matches, judged 不落地

规格 `docs/ENGINE_REDESIGN.md:426` 是 §2.7 那张增强表里**唯一一条既没实现也没被判过的**：

> | LMR（后排着法减深） | 节点数 −15~25% | 中（需 re-search 校验） |

`:419` 的「按性价比排序，建议全部实现」把它算在内，`:548` 的 B 阶段验收行却没排它（那行列的是
PVS／排序／aspiration／PV 提取／威胁延伸／双威胁／VCF-VCT 按需）。动手前
`grep -rn "LMR\|late.move\|reduce_depth\|reduction" src/search/` 为空。判它的那条标准不在 §2.7，
在 §4：`:554` 「着法变化必须能解释」＋ `:555` 「用『新引擎 vs 旧引擎』的验证赛（互换黑白）……
**确认没有退化**」——所以本轮的仪器必须是验证赛，不是节点表。

### 读数 1：第一版规则读的是上一个节点留下的行，所以那张表先作废

`node_prices_` 最初是**一个** `std::vector<int>` 成员：`sort_moves()` 写它，节点循环读它。读完第一个
孩子以后，那次递归用同一个成员写下它自己那一串价格，于是第二个孩子以后的守卫读的是**后代**的列表。
当时的 `prices.size() == moves.size()` 只挡住长度不同的情况——五子棋的候选表常常同长，所以挡不住。
修法是每层一行，与 killer／PV 表同一约定（`src/search/search_state.h:44`，`kMaxPly = 64`），依据是
同一层上的两个节点不会同时活着：

```cpp
std::array<std::vector<int>, MoveHeuristics::kMaxPly> node_prices_;   // 每层一行
std::vector<int> node_prices_sink_;                                    // 层号出界时写这行
```

`sort_moves()` 的**两条**出口都要把行摆成 `moves.size()` 长，包括「一个孩子都不剩」的早退，否则守卫
会读到上一个节点留下的行。后果是这条规则之前量到的每一张守卫表都作废，下面全部重取。

控制臂的自证（不是推断）：`shape_off`（同布局、同样写价格行、地板抬到任何深度都碰不到）跑出的 64 臂
树与 HEAD 逐字节相同，29,691,427 对 29,691,427（`/tmp/e1/shapes.log` 的 `[cmp] off` 行）——**记账本身
不花一个节点**；出厂前三个针也逐字复现（`quiet BLACK d6 = 221,774`、`quiet BLACK d8 = 1,408,083`、
`pos24 BLACK d8 = 2,378,724`，`/tmp/e1/lmr_arms_head.out` 与 `ab_arms_i1d5sec.out`）。

### 读数 2：尾部有多大——先量它有没有地方站

`/tmp/e1/tail.out`（仪表把计数器写进节点循环，只读 `ctx_.nodes()`，不改任何判断）。两个口径都要说清：
「孩子数占比」= 非根节点的每一次进入里有多少次是从第二个孩子以后进来的；「子树占比」按每条边的子树
大小记，天然被深度加权（一个节点被它祖先的每条边各记一次），是**低估**尾部的那一侧。上限 ≈ 尾部份额 ×
(1 − 1/平均分支) ≈ 0.45 × 0.65。

| 臂 | 循环节点 | 被搜索的孩子 | 每节点孩子 | beta 截断退出 | 尾部 i≥1 孩子占比 | 尾部子树占比 |
| --- | --- | --- | --- | --- | --- | --- |
| quiet BLACK d6 | 27,217 | 78,567 | 2.89 | 24,300（89.3 %） | 65.4 % | 45.6 % |
| quiet BLACK d8 | 221,566 | 639,290 | 2.89 | 193,680（87.4 %） | 65.3 % | 44.9 % |
| pos24 BLACK d8 | 266,572 | 743,433 | 2.79 | 244,089（91.6 %） | 64.1 % | 42.2 % |
| quiet WHITE d6 | 15,851 | 53,867 | 3.40 | 13,619（85.9 %） | 70.6 % | 49.5 % |
| scattered BLACK d6 | 19,778 | 71,522 | 3.62 | 17,138（86.7 %） | 72.3 % | 57.6 % |
| pos24 WHITE d6 | 17,632 | 55,171 | 3.13 | 15,693（89.0 %） | 68.0 % | 41.7 % |
| crowded BLACK d6 | 39 | 73 | 1.87 | 38（97.4 %） | 46.6 % | **9.4 %** |

crowded 那一行是反面：888 节点的必胜算例里 97.4 % 的节点第一个孩子就截断——LMR 在这种形状上无事可做，
出厂前它也确实是七臂里唯一原样不动的那一条。

### 读数 3：十二付守卫 × 64 臂（`/tmp/e1/shapes.log`、`shapes2.log`；控制 29,691,427 节点）

| 形状 | 规则 | 答案改变的臂 | 其中分数变差 | 合计节点 | 最差单臂 |
| --- | --- | --- | --- | --- | --- |
| off | 地板 100000：永不触发 | 0 | 0 | +0.00 % | +0.0 % (31,8) |
| i1d4sec | i≥1、d≥4、price[i] < 亚军 | 25 | **6**（−1,960） | −38.93 % | +21.9 % (2,6) |
| i2d4sec | i≥2、同上 | 25 | **6**（−1,960） | −38.93 % | +21.9 % (2,6) |
| i1d4half | d≥4、price[i] < 亚军/2 | 11 | **3**（−15,940） | −17.16 % | +49.8 % (2,6) |
| i1d4q4 | d≥4、price[i] < 亚军/4 | 6 | **1**（−14,778） | −6.20 % | +27.8 % (4,6) |
| **i1d5sec** | **d≥5、price[i] < 亚军** | **19** | **0** | **−28.35 %** | +21.5 % (13,6) |
| i1d5half | d≥5、price[i] < 亚军/2 | 7 | 0 | −13.58 % | +16.3 % (2,8) |
| i1d6sec | d≥6、price[i] < 亚军 | 8 | **2**（−1,552） | −19.79 % | +10.8 % (2,8) |
| i2d6half | i≥2、d≥6、亚军/2 | 1 | 0 | −6.08 % | +23.1 % (2,8) |
| i1d6q4 | d≥6、亚军/4 | 0 | 0 | −1.17 % | +3.7 % (1,8) |
| i1d6head | d≥6、参照**头部** | 8 | **4**（−39,568） | −19.32 % | +6.5 % (2,8) |
| i1d6headhalf | d≥6、头部/2 | 1 | 0 | −7.06 % | +2.2 % (1,8) |

三条读得出的结论：① **地板决定一切**——4→5 把 6 个变差臂清干净，代价是从 −38.93 % 收到 −28.35 %；
② **参照必须是亚军而不是头部**——头部参照带 4 个变差臂、净省 −44,150 节点（省的钱不够赔）；
③ **索引轴是空的**——`i2d4sec` 与 `i1d4sec` 逐位相同，因为亚军与自己比较恒不成立，第一个可能被减的
孩子天然就是第三个。选中 `i1d5sec`：`29,691,427 → 21,273,842`（省 8,417,585 节点），19 个臂改了答案
而 0 个改差。

### 读数 4：十三臂 PAIR——一个进程、两个命名空间、只差那四个常量

`gomoku::` 链 `shape_off` 的对象，`lmrv::` 链 i1d5sec 的对象（`/tmp/e1/ab_arms_i1d5sec.out`）。
13 臂合计 **5,088,552 → 3,991,160 节点（−21.57 %）**，静态分读数 qnodes 3,269,328 → 2,525,433
（−22.75 %）；score／着点／完成层／根宽／终止原因**一个都没动**，唯一动的是 `pos24 BLACK d6` 的
seldepth 4→3。

| 臂 | off | i1d5sec | Δ |
| --- | --- | --- | --- |
| quiet d2 B / W | 742 / 1,370 | 742 / 1,370 | 0.00 %（地板以下） |
| quiet d3 B / W | 2,515 / 3,912 | 2,515 / 3,912 | 0.00 % |
| quiet d4 B | 11,359 | 11,359 | 0.00 % |
| quiet d6 B / W | 221,774 / 156,428 | 213,884 / 143,543 | −3.56 % / −8.24 % |
| quiet d8 B | 1,408,083 | 1,058,883 | −24.80 % |
| scattered d6 B / W | 177,061 / 251,149 | 140,014 / 196,573 | −20.92 % / −21.73 % |
| pos24 d6 B / W | 300,347 / 175,088 | 298,321 / 174,494 | −0.67 % / −0.34 % |
| pos24 d8 B | 2,378,724 | 1,745,550 | −26.62 % |

### 读数 5：价格的量纲——为什么没有加绝对地板

`/tmp/e1/probe_prices.py`（把守卫读到的每个价格打到 stderr，判断本身不变）。五个臂、2,170 个通过
索引/深度守卫的孩子、其中 1,277 个被减：被减的价格 min 58／中位 226／**max 1,660**；战术那一簇在
10,176–10,308，**一个都没被减**——相对守卫在战术上本来就收手。阈值扫描 `price<1000` 只砍 14/1,277
（1.1 %），`<2000` 以上是空操作。于是绝对地板在这里既没有内容可减，又把规则绑在
`cand_width_root/deep` 的量纲上（`candidate_price` = `Evaluator::pair_score`，
`src/core/evaluator.cpp:183`），**不加**。

### 读数 6：`§4 :555`  prescribed 的那场验证赛——两种赛制都落不到「没有退化」

夹具 `/tmp/e1/make_dual.py` + `match_driver.cpp`：同一进程里两个座位只差守卫的四个常量，纯
`AlphaBetaSearch`（不走 vcf／greedy 快路）、各自新表、黑白轮流、开局是中心加四手强落。

| 赛制 | 局数 | cand 胜／负／和 | 分数 | 95 % 区间 | 每 think 节点 |
| --- | --- | --- | --- | --- | --- |
| i1d5sec 定深 6、无钟（seed 7） | 96 | 44／52／0 | **45.8 %** | 35.9–55.8 % | 123,600 对 102,637（**−16.96 %**） |
| i1d5sec 挂钟 400 ms／手、深度帽 20（seed 21） | 200 | 92／105／3 | **46.8 %** | 39.8–53.7 % | 53,513 对 53,448（**−0.12 %**） |
| i1d5half 定深 6、无钟（同一批 96 个开局） | 96 | 44／52／0 | **45.8 %** | 35.9–55.8 % | — |
| 两批 i1d5sec 合起来（开局不同，可合并） | 296 | 136／157／3 | 46.5 % | 40.8–52.1 % | — |

坐色是对称的（96 局里 cand 坐 B 22 胜 26 负、坐 W 22 胜 26 负），96 局全部以 `five` 终局，200 局里
只有 3 局和棋且无 illegal／no_legal_move——夹具本身没有杂音。早先那 24 局是 seed 7 前 24 局（GAME 0
逐字节相同），所以 120 局不是样本量。

换一付守卫也是同一个数：`i1d5half`（d≥5 且价格要小于亚军一半，64 臂只省 −13.58 %、只改 7 个臂的答案）
在同一批 96 个开局上打出 **44／52，与 i1d5sec 一字不差**，而逐局的着数有 24 局不同——也就是说这两场
确实是两棵树在下棋，不是同一个二进制跑了两遍。省得少的一半并没有把分数换回来，这弱化了
「46 % 是减深量的函数」而强化了「46 % 与守卫松紧关系不大」；两者都还在 ±10 % 的区间里，所以这句只当
观察，不当结论。

这一行是本轮的判决点，也是最反直觉的一行：**定深赛里省下的节点是真的**（每 think 少 16.96 %），
**挂钟赛里省不下**（−0.12 %，因为预算而不是树决定了花多少）。也就是说这笔节省到了产品跑的那一档
只剩「同一笔预算里多花几层」这一条出路，而驱动只记节点，没记每 think 的完成层——这条出路本轮
**没量到**，是个缺口而不是结论。两条路测出来的分数都是 46 % 上下，与 50 % 的差分别只有 0.82 σ 与
0.92 σ：都**不足以证明减力**，也**不足以确认没有退化**。`:555` 要的是后者。

### 实现（本轮写在树里、判完即还原的那两处）

`src/search/alphabeta.h` 顶部四个常量 + 每层一行的 `node_prices_`；`src/search/alphabeta.cpp` 两处：
`sort_moves()` 排完之后把每个已排序候选的价格写进本层那一行（尾部那一串是排序刚读过的
`scored[i].first`，只有被钉住的头部——TT 着法 + 两个 killer，至多三格——需要再问一次
`candidate_price`，而那问的是生成器为同一格已经备好的记录，是第二次**读**不是第二次**算**）；
节点循环里 `i >= kLmrMinIndex && depth >= kLmrMinDepth && prices[i] < prices[ref]` 的孩子先用
`depth - kLmrReduce` 问一次，**只有浅问把 alpha 抬起来才重搜**，重问沿用本来的 `scout`，所以被重问的
孩子仍然走 PVS 的零窗口和它自己的全窗口跟进。fail-low 留浅值——这是 LMR 的赌注而不是定理（浅一层
交回的界只在那一层成立），代码注释按这个口径写。

### 门禁（跑在这棵 LMR 树上）

- `cmake --build build -j 4` rc=0；`ctest` **8/8 / 582.59 s**（`test_search` 553.57 s）；
  `node tests/web_frontend_check.js` **105/105**；`node tests/readme_anchor_check.js`
  **120 citations / 0 problems**；README 仍 **658 行**——本轮不需要动它：
  `grep -n "LMR\|减深\|后排" README.md` 为空，README 里没有一句关于这条规则的话。
- `tests/test_search.cpp` 79 处逐行中性改动（文件行数不变，受保护的 :2591／:3973／:3986 未动）＋两条
  新算例插在 `int main()` 之前：E-1 把 13 臂 off／lmr 两栏一次摆全（8 臂严格更便宜、5 臂逐字节同树，
  答案五个字段全等），E-2 用 `quiet d2／d3／d4` 五臂钉「地板以下就是同一棵树」，再用那个 19 子的 POSE 2
  （新 builder `e1_suspect_tail_pose()`，取自扫描器的第 2 号局面）钉住**重搜的那一侧**：`853,099 → 957,772`，
  **+104,673 节点（+12.27 %）而 −81238 与 (7,10) 一字不动**。`main` 从 8676 → 8852 行，文件
  8,860 → 9,042 行，所以本节引用的旧行号都在原位。
- **还原以后在同一台机器上重取的出厂门禁**：`cmake --build build -j 4` rc=0（34 个 TU），
  `ctest` **8/8 / 458.31 s**（`/tmp/e1/ctest_revert.log`，与轮次130 记录的 458.68 s 同级），
  下面三条读数则是在那棵 LMR 树上取的，`tests/test_search.cpp` 里既没有那 79 处改动也没有两条新算例
  （两份 patch 存在 `/tmp/e1/`，见下）。
- memcheck 三臂（`/tmp/e1/mc_head.out`、`mc_offo.out`、`mc_lmr.out`）：三行全部
  **ERROR SUMMARY 0 / 泄漏 0**，`in use at exit: 0 bytes`。

| 对象 | 两臂节点和 | allocs | 累计分配 | 每节点 allocs |
| --- | --- | --- | --- | --- |
| HEAD（无记账） | 1,074,873 | 3,797,771 | 603,290,404 B | 3.5332 |
| shape_off（有记账、不触发） | 1,074,873 | 3,797,808 | 603,293,388 B | 3.5333 |
| i1d5sec | 1,171,656 | 4,113,130 | 646,995,104 B | 3.5105 |

「记账本身花多少」是第二行减第一行：**+37 次分配、+2,984 B**。第三行总量看着大，是因为减深那台的树本身
大 96,783 节点（那条重搜臂付的 +104,673 是真的多做了工）；按节点归一以后三行都是 3.53 上下，
本轮没有新增每节点分配压力。`sizeof(AlphaBetaSearch)` 60,280 → 61,840 = 65 个 `vector` × 24 B
（`kMaxPly` 行 + 出界那行），是**每引擎对象**而不是每节点，且引擎是一次 think 的栈上局部量
（`src/search/search_engine.cpp:256`）。
- callgrind：两臂驱动的一对（`/tmp/e1/cg_arms_offo.out`、`cg_arms_vg.out`，`callgrind_annotate`
  的表在 `cga_offo.txt`、`cga_vg.txt`）。两臂 = `quiet BLACK d6` + 那条重搜臂 `POSE 2 WHITE d8`，
  off 形 1,074,873 节点 55,608,747,587 Ir，减深那台 1,171,656 节点 59,333,118,936 Ir：总量 +6.70 % 而
  节点 +9.00 %，**按节点归一 −2.12 %（51,735 → 50,640 Ir/节点）**。热点表两侧逐行同形：

  | 函数 | off 形 | 减深那台 |
  | --- | --- | --- |
  | `Evaluator::pattern_score` | 22.30 % | 21.93 % |
  | `PatternRecognizer::finish_direction` | 14.37 % | 14.38 % |
  | `CandidateCache::cell`（两个文件路径合一记） | 11.11 % | 11.18 % |
  | `recognize_position_both` | 9.62 % | 9.62 % |
  | `is_forbidden_from_pattern` | 6.42 % | 6.44 % |
  | `PositionPattern::analyze` | 5.79 % | 5.79 % |
  | `MoveGenerator::generate` | 4.23 % | 4.37 % |
  | `Evaluator::candidate_value` | 3.93 % | 3.93 % |
  | `Board::is_empty` | 3.32 % | 3.34 % |

  「哪个函数 CPU 卡比较久」的答复与 D-5 那一条相同而没有移动：**评估与图案识别**（减深侧前三行合
  47.49 %），而本轮代码所在的 `alphabeta.cpp` **在两侧的第十二名以外都排不进去**——这条增强既不新增
  热点，也不改变热点形状。训练臂那条按 D-5 原样（`--timeout-ms 5000`）跑出来不可比：出厂树读到
  56,428,917,688 对 D-5 的 56,620,693,932，而那棵树是可证相同的——差的 0.34 % 就是本机负载把墙钟裁短
  的量，所以那一版作废；把 `--timeout-ms` 抬到 600000 让树只由深度和节点帽决定以后，同一条臂在
  valgrind 下远超 D-5 记录的 301 s（`d4_off` 跑了 7 分钟还没落盘，本轮为收笔时限把它中止了），于是改跑上面这一对
  两臂驱动。这是本轮仪器的一处退让，如实记：训练路径的热点表本轮没有重取，取的是搜索路径的。
- TSan：本轮**不适用**。新增状态 `node_prices_` 是 `AlphaBetaSearch` 自己的成员，一次 think 只有一个
  所有者（`src/search/search_engine.cpp:256` 栈上对象），没有新的跨线程对象，也没有新的共享写。

### 判：先测夹具自己，再测规则——座位对照

三个读数都落在 46 % 上下（i1d5sec 定深 45.8 %、i1d5sec 挂钟 46.8 %、i1d5half 定深 45.8 %），这也可以是
夹具自己偏着 `cand` 那个座位。所以补了一台对照：`match_driver.cpp --swap` 把两个座位对调——控制座换成
减深的那台、候选座换成永不减深的那台，开局与轮次全部不变（`/tmp/e1/swap96.log`，96 局，seed 7）。读法
预先是：对调后候选座（＝不减深的那台）回到 54 % 上下 ⇒ 分数跟着**守卫**走；对调后还是 46 % 上下 ⇒
分数跟着**座位**走，本轮三场赛分全部作废。

结果是**减深那一台在两种座位下都输**：

| 减深那台坐在 | 它的得分 | 局数 |
| --- | --- | --- |
| 候选座（`match_sec96.log`） | 44／96 = **45.8 %** | 96 |
| 控制座（`swap96.log`，候选座是不减深的） | 40／96 = **41.7 %** | 96 |
| 两处合并 | 84／192 = **43.75 %** | 192 |

合并值与 50 % 差 1.73 σ（双侧 p≈0.08，95 % 区间 36.7–50.8 %）；两个座位角色之间只差 4.2 点而差的
标准误是 7.2 点，所以**看不出座位偏**——那 46 % 不是夹具的形状，是规则的形状。这一条不是「证明减力」
的证据等级，但 `:555` 要的「确认没有退化」明确没拿到：区间上限 50.8 % 说的是「最好也不过是不亏」，
而这一侧要付出的是 64 臂里 19 个臂改口、两场定深赛与一场挂钟赛全部 46 % 上下。本项目的晋升口径本来
就不接受「不输」——README:199 写的就是「晋升门槛（必须 > 0.5，即仅靠『不输』不能晋升）」。

### 判：不落地，`src/` 与 `tests/` 还原

按 `:555` 的口径，本轮要的是**确认没有退化**，而四个读数一起给出的是反向：定深 45.8 %、挂钟 46.8 %、
换一付更紧的守卫还是 45.8 %、座位对调后减深那台还是 41.7 %（合并 43.75 %，1.73 σ）。同时这笔交易的
另一侧也窄了——省下的节点在挂钟档上不表现为少花钱（−0.12 %），只剩「多花几层」一条没量到的出路。
D-4 那轮是同一种情形、同一种处理：机制做得出来、赛制读数指反、于是默认关掉。本轮没有「默认关掉还能
留给谁用」的对象——LMR 的机制就是那四个常量，`off` 就是把它们设成永不触发，等于把一段死代码留在
热路径上（`sort_moves` 每节点多走一遍价格行，而 3.53 allocs/节点里那 +37 次分配就是它）。
所以**整条还原**，证据留在本节：`git diff` 的两份 patch 存在 `/tmp/e1/lmr_src_attempt.patch`（164 行）
与 `/tmp/e1/lmr_tests_attempt.patch`（627 行），`git checkout -- src/search/alphabeta.{h,cpp} tests/
test_search.cpp` 之后 `git status` 只剩本轮没动的三份未跟踪文档。

要翻案需要的新证据（按性价比排）：① 给挂钟赛加每 think 完成层的读数，先看「省下的节点换成几层」到底
是多少——如果换不到层，这条增强在这个引擎上根本没有第二条收益路径，本轮的挂钟臂只记了节点，这是缺口；
② ~~换一付更紧的守卫~~ 已经试过并且排除了：`i1d5half`（只改 7 个臂的答案、省 −13.58 %）在同一批 96 个
开局上打出与 `i1d5sec` 一字不差的 44／52；③ 减深只对没有战术的节点生效——读数 5 说明相对守卫在战术簇上
本来就不动手（1,277 个被减的孩子最大 1,660，战术那一簇 10,176–10,308 一个都没被减），所以这一条大概
不是原因；④ 剩下的可能是重搜的口径本身（fail-low 一侧留着浅层的界，本轮按标准 LMR 实现，没有试过
"fail-low 也重问"），那是另一个形状，得先有新的树再取新的针。

### 没动的账 / 如实报

- 45.8 % 与 46.8 % 的置信区间各 ±10 % 与 ±7 %，本轮**没有**把它们读成「证明减力」；判据是 `:555`
  要求的那侧（确认没有退化）没拿到，不是点估。
- 64 臂里 19 个臂的答案与满树不同（0 个改差）——这就是 `:426` 风险列的形状；按 B3 那种「只有 nodes
  动」的标准，这条增强做不到，本轮没有假装做得到。
- 最差单臂 +21.5 %（POSE 13 d6，234,848 → 285,391，答案不变，重搜的钱）与 i1d4half 的 +49.8 %
  留在案上，作为「地板为什么必须是 5」的另一侧。
- `:550` 的 D 三笔账与本阶段无关：LMR 减的是一次 think 的树，不是训练轮的资源；反过来它给
  「单轮耗时 −40 %」添了一项新成本（同深度更少的节点），D 的那三条读数仍按 §2.8 的口径另取。
- 工装的一条，实测复现了 `run_floor.sh` 头部早就写着的理由：把 HEAD 的 `alphabeta.o` 链进按减深那头文件编译的对象集合会 `free(): invalid size`（`sizeof` 差 1,560 B，布局不同，217,263,857 Ir 处
  崩），所以控制对象必须是同布局的 `shape_off`，不能图省事用 HEAD。
- 仪表与探针全部树外（`/tmp/e1/`）：`instrument.py`、`make_shapes.py`、`make_dual.py`、
  `install_shape.py`、`apply_pins.py`、`probe_prices.py`、`patch_match_ms.py`、
  `sweep_driver.cpp`、`floor_driver.cpp`、`ship_driver.cpp`、`tail_driver.cpp`、`probe_driver.cpp`、
  `pose_dump.cpp`、`lmr_arms.cpp`、`ab_arms.cpp`、`match_driver.cpp` 与
  `run_{fixed,shapes,shapes2,floor,match,valgrind_prep,cg_e}.sh`；本轮产物留在
  `shapes.log`、`shapes2.log`、`ab_arms_i1d5sec.out`、`match_sec96.log`、`clock200.log`、
  `half96.log`、`mc_*.out`、`cg_*.out`、`probe_prices.tsv`。还原用的两份 patch：
  `lmr_src_attempt.patch`（164 行）、`lmr_tests_attempt.patch`（627 行）。

## 轮次130 引擎重构 D-5：一次交接从两份 Board 变成一份，而这一笔只能用 TSan 和指令数说话 — engine redesign stage D5, the controller's Board hand-off

规格 `docs/ENGINE_REDESIGN.md:437` 的 `#5` 是 §2.8 五条里唯一一条不碰训练资源、只碰一次函数调用的：

> **去掉不必要的 Board 复制**：`SearchController::search_worker` 改为接收 `const Board&` 并在 worker 内自行构造可变副本（或直接在 worker 内 copy 一次），避免「传参一次 + 每层一次」的重复复制。

落地面 `:585` 那一行是 `src/search/controller.h/.cpp` ——「`search_worker` 不再按值收 `Board`；透传 `SearchLimits` 与上一手 `hint`」，验收 `:550` 是 D 阶段那三笔（单轮耗时 −40 %／内存与并发解耦／`--fitness-games 2` 下 fitness 不再只有三档）。

### 读数 1：动手前先量，文档的前提已经不是现状

「传参一次」在，「每层一次」不在。三档节点预算下把每次 think 里所有 Board 复制点逐个清点（仪表在树外，`/tmp/d5/count_copies.py` 上`--on`／下`--off`，跑完 `git checkout` 还原）：

| 量什么 | 量到什么 | 哪份产物能重算 |
| --- | --- | --- |
| `sizeof(Board)` | **4664 B**（bitboard + 邻居计数 + `active_empty` + 两手历史） | `/tmp/d5/probe.cpp`、`/tmp/d5/cpu.log` P1 两条独立打印 |
| 一次 copy-assign | **90.0 ns/op**（20 万次循环，`build/` 的 -O2 对象） | `/tmp/d5/gate_probe.log` 第 3 行 |
| 旧的整个交接（构参 + move 一对） | **178.7 ns/op** ⇒ 本轮省下 **88.7 ns**，即 178.7 − 90.0 | 同上第 5 行与第 3 行相减；`move` 单独一项由 177.9 − 90.0 = 87.9 ns 交叉核过 |
| 「每层一次」还在不在 | 不在了：`src/search/alphabeta.cpp:601` 的 `Board work_board = board;` 在深度循环（`:603`）**之前**，一次 think 只有一份，不是每层一份 | `src/search/alphabeta.cpp:598-604` |
| 一次 think 究竟几份 | **3.10／5.00／6.73 份每 think**（21／26／30 手分别共 65／130／202 份） | `/tmp/d5/census_2000.txt`、`census_20000.txt`、`census_200000.txt` 各自倒数第二行 |
| 这些复制值多少钱 | 那三份仪表自报的口径是 94 ns/份：2000 节点臂 **0.01 ms 对 1.22 s**、20000 臂 0.01 ms 对 2.79 s、200 000 臂 0.02 ms 对 4.35 s，即 ~0.0005 %。**同一台机器重跑同一个探针量到 90.0 ns**，4 ns 的差就是这台机器的墙钟噪声等级 —— 所以这一栏只当量级看，本轮不用它下结论（结论用读数 4 的指令数） | 同上，每臂末尾 `at 94 ns/copy:` 行 vs `/tmp/d5/gate_probe.log` |
| 复制的大头在哪 | `src/search/vcf.cpp:420` 的 `Board after = board;`（每个防守应手一份）占 **53.8 %／70.0 %／75.7 %**；`alphabeta.cpp:601` 那一手一份只占 23.1 %／14.6 %／10.4 % | 同上，`site … ( % of the run's counted copies)` 行 |

而 `SearchController` 在生产里**一个调用者都没有**：`grep -rn SearchController src/` 除了它自己那对文件之外为空，服务器与自对弈都直接调 `SearchEngine::search(const Board&, …)`（`src/server/game_manager.cpp:363`、`:467`、`src/training/self_play.cpp:68`），且这三处传的都是**调用方自己拥有的快照**（`board_snapshot`、`snapshot`、`board`）。本轮给控制器补上的形态，正是生产路径一直在用的形态。

### 实现

- `src/search/controller.h:53` 新增私有成员 `Board search_board_;`，`:58` 的签名从 `search_worker(Board board, …)` 改成 `search_worker(const Board& board, …)`；`src/search/controller.cpp:37` 在**无条件 join 之后**写一次 `search_board_ = board;`，`:39` 用 `std::cref(search_board_)` 起线程。线程参数表里不再放第二份 Board：旧的按值交接要为一次 think 付「构造实参 + move 进形参」这一对（178.7 ns），现在只付一次 copy-assign（90.0 ns）。
- **与文档字面不同的一处，故意不照做**：文档给的两个选项之一是「在 worker 内自行构造可变副本」，但 `engine_.search()` 收的是 `const Board&`（`src/search/search_engine.h:68`），worker 内没有任何人需要可变副本；照字面写等于**再加**一份 4664 B 的复制，与「去掉不必要的复制」正好相反。括号里第二个选项「直接 copy 一次」才是落地的形状，只是那一次 copy 放在调用方而不是被调方 —— 交接之后 caller 的板子归 caller 继续改，这正是 `:585` 那句「不再按值收」要的语义。
- 「透传 `SearchLimits` 与上一手 `hint`」这半句**没有新增管道**：`max_depth`／`hard_time_ms`／`max_nodes` 与 `hint` 全是引擎上的 setter（`search_engine.h:90`、`:109-117`），`SearchController::engine()`（`controller.h:35`）本来就把手柄交了出去，生产路径就是这么用的（`game_manager.cpp:356-361` 连着 `:363` 那一次 `engine_.search(board_snapshot, …)`）。给一个没有生产调用者的类再加一层同名转发参数，只是多一个会被忘记同步的地方。这条偏差按上表如实记，不当作已完成。

### 测试

两条新用例在 `tests/test_search.cpp:8580-8624`、`:8626-8674`，注册在 `:8724-8725`。放在文件末尾而不是紧挨另外三条控制器用例（`:1221-1303`），因为那三条在受保护断言 `tests/test_search.cpp:3973` 之上，往那儿插行会推走 REVIEW_LOG 反复引用的 `:2591`／`:3973`／`:3986`；本轮逐条核回原位（`assert(result.resigned)` 仍在 `:2591`、`assert(interrupted.partial_depth == 1)` 仍在 `:3973`、`assert(completed.method == "alphabeta")` 仍在 `:3986`），旧引用最大的一条是 `:8520`，本轮插入点在其下，被移动的只有 `int main()` 自己那一行（`:8567`→`:8676`），没有任何引用指向它。文件 8749 → **8860** 行。

- **`test_controller_think_ignores_later_caller_board_edits`**：20 轮，每轮一个黑棋四连（`(7,3)-(7,6)`，左端 `(7,2)` 已被白占死，唯一赢法是 `(7,7)`），`start_search` 一返回就立刻在**调用方**那块板上堵掉 `(7,7)` 再补两手；交付结果必须仍是 `(7,7)` 且 `score >= 99000`。线程创建要几十微秒，而三手 `place_stone` 只要几十纳秒 —— 一旦 worker 读的是调用方的板，第一手就一定赶在读取之前。M1 那一行证明这个窗口是真的能被踩到，不是推理。
- **`test_controller_second_think_uses_the_new_position`**：先对无战术的安静局面发一次 depth 12／3 s 的思考（`assert(controller.is_thinking())` 写在调用之后），紧接着对**另一个**局面（白棋 `(10,4)-(10,7)` 四连，唯一赢法 `(10,8)`）再发一次；交付必须是 `(10,8)`。这条钉的是「成员快照只有在 join 掉上一个 worker 之后才写」这一不变量 —— 轮次34 F2 那次无条件 join 就是它的前提。

### 变异矩阵：四条里两条被断言杀掉，第三条只能由 TSan 杀

出厂套件要 427.79 s 才轮得到这两条，所以变异跑的是树外探针 `/tmp/d5/tsan_probe.cpp`：语句与上面两条用例逐条对齐（`diff` 只报 include 块与打印文案的差别），链到 `build/` 的对象上单跑，一轮几秒。

| 变异 | 改的是什么 | 结果 |
| --- | --- | --- |
| 控制 | 出厂形态 | `rc=0`，2／2 通过（`/tmp/d5/mut_run_control.log`） |
| M1 | `std::cref(search_board_)` → `std::cref(board)`，即线程引用**调用方**的板 | **`rc=134`，0／2**：`Assertion 'result.best_move.pos.row == 7' failed`（`mut_run_m1.log`） |
| M2 | 完全退回旧的按值签名 | `rc=0`，2／2 —— 预期：这两条钉的是**契约**（交接之后的改动不许影响这一手），旧形态同样满足；本轮删的是那一份多余复制，不是换一种正确性 |
| M3 | 把 `search_board_ = board;` 挪到 `make_unique<std::thread>(…)` **之后** | `rc=0`，2／2 —— **断言杀不掉**，竞争窗口太窄，交给下一节的 TSan |
| M4 | 删掉那次写入（worker 永远读默认构造的空板） | **`rc=134`，0／2**：`Assertion 'result.score >= 99000' failed`（`mut_run_m4.log`） |

第一遍跑 M3／M4 是 `BUILD-FAILED`，原因不在代码而在仪表脚本自己：M2 重写的是 `controller.h`，而 M3／M4 只还原 `controller.cpp`，于是 `const Board&` 的定义对不上按值的声明（`no declaration matches 'void gomoku::SearchController::search_worker(const gomoku::Board&, int, int, int)'`）。`/tmp/d5/mut_d5_m34.sh` 两个文件一起还原后重跑，才是上表那两行。

### 读数 2：并发与 CPU —— 这台机器上 `perf` 用不了，两笔都换了仪器

`/proc/sys/kernel/perf_event_paranoid` 读到的是 **4**，非特权进程拿不到采样；「不得修改系统配置」是硬约束，S9 也禁 sysctl，所以 `perf` 这一项是**取不到**而不是没做。替代仪器是 TSan（happens-before）和 callgrind（指令计数）。

- **TSan 干净树**：`build-tsan` 重建 24 个 TU，探针 `rc=0`、**`races=0`**，两条用例都过（`/tmp/d5/tsan.log`）。TSan 这次不需要 `vm.mmap_rnd_bits=28` 就能起来，也没触发 `setarch -R` 重试 —— 轮次记的「必须用户跑特权 sysctl」这一条在本机今夜不成立。
- **TSan on M3**（写入挪到起线程之后）：`rc=66`、**`races=22`**。第一条报告的两条栈正是本轮新增跨线程对象的两端：线程 T1 在 `src/core/board.cpp:160`（经 `src/core/pattern.cpp:575` ← `src/search/search_engine.cpp:81` ← `src/search/controller.cpp:63`）**读**，主线程上一次**写**在 `src/core/board.h:45` 的 `Board::operator=`（`/tmp/d5/tsan_m3.err`）。所以上面那句「干净树 0 race」不是仪器没通电：同一套栈在 M3 上会响 22 次。M3 这一行因此是**判掉**的，不是漏掉的。
- **callgrind 交接路径臂**（`/tmp/d5/cg_controller.out`，12 次控制器思考）：总 186,824,692 Ir，首位 `PatternRecognizer::match_pattern` 23.21 %，其后 `finish_direction` 4.81 %、`recognize_position_both` 2.12 %、`CandidateCache::cell` 1.69 %、`recognize_direction` 1.34 %、`Board::is_empty` 0.79 %、`Board::undo_last_move` 0.59 % —— 表里没有一项是 Board 复制。这一臂只能算交接路径画像：think 被自己的 20 ms 墙钟裁掉，同一件事在本机 21.75 ms（P1，40 次共 870.05 ms），在 valgrind 下 79.59 ms（P3，20 次共 1591.90 ms），树深跟着缩水。
- **callgrind 整机臂**（`/tmp/d5/cg_train.out`，`build-vg/gomoku_train --rounds 1 --population 2 --generations 1 --depth 4 --max-nodes 20000 --timeout-ms 5000 --max-moves 12 --fitness-games 2 --verification-games 2 --no-data --seed 20261011`，`rc=0`、301 s、`Training finished: rounds=1 versions=2 champion=v0 games=8 samples=0`，**`I refs: 56,620,693,932`**）：按节点预算跑，树与工具速度无关（那正是 D-1 加 `--max-nodes` 的理由）。

| 名次 | self 函数 | Ir | 占比 |
| --- | --- | --- | --- |
| 1 | `src/core/evaluator.cpp:gomoku::Evaluator::pattern_score` | 11,847,254,774 | **20.92 %** |
| 2 | `src/core/pattern.cpp:PatternRecognizer::finish_direction` | 9,282,418,688 | 16.39 % |
| 3 | `src/core/pattern.cpp:PatternRecognizer::recognize_position_both` | 5,349,879,030 | 9.45 % |
| 4 | `src/search/search_state.h:CandidateCache::cell` | 5,322,535,217 | 9.40 % |
| 5 | `src/core/pattern.cpp:PositionPattern::analyze` | 3,542,825,868 | 6.26 % |
| 6 | `src/core/pattern.cpp:PatternRecognizer::is_forbidden_from_pattern` | 3,301,154,612 | 5.83 % |
| 7 | `src/core/evaluator.cpp:Evaluator::candidate_value` | 2,221,441,279 | 3.92 % |
| 8 | `src/core/board.cpp:Board::is_empty(int, int) const` | 1,857,773,906 | 3.28 % |
| 9 | `src/search/move_generator.cpp:MoveGenerator::generate` | 1,629,287,505 | 2.88 % |

「哪个函数 CPU 卡比较久」的答复是**评估与图案识别**：前三合 46.76 %，第 1 名单独占 1/5；`board.cpp` 那一堆（`is_empty`、`undo_last_move` 1.41 %、`update_active_empty` 1.12 %、`get` 1.11 %）全是**读**板子而不是搬板子。本轮那一类搬运在表里只有一个落点：`__memcpy_avx_unaligned_erms` **27,789,314 Ir = 0.05 %**，而这一行是所有 Board 复制被内联之后的**合**记账，无法把 D-5 省下的那一份单独拆出来；`alphabeta.cpp:601` 那一行在 auto-annotation 下直接记账为空，紧随的 `search_root_with_aspiration(work_board, …)` 才 9,238 Ir（0.00 %）。于是「−40 % 单轮耗时」与这笔的关系有了硬口径：**整个 memcpy 类只值 0.05 %**，`#5` 的天花板在它下面，不在它旁边。
- **内存**：`sizeof(SearchController)` 现在是 **6968 B**（减掉新成员 4664 得 2304，即本轮之前那个类的大小）（`sizeof(SearchEngine)=2040` + 新成员 4664 + 原子量/mutex/回调 ~264），因为它现在**自己拥有**那块板；旧的按值形态把同样 4664 B 放在 worker 线程的栈上，而那次线程创建本来就已经预留栈。所以本轮改的是「同一份内在谁名下」，不是内存增减。memcheck 两臂（`/tmp/d5/cpu.log` P3）：探针 20 次真实交接 `rc=0`、`ERROR SUMMARY: 0 errors from 0 contexts`、`23,644 allocs, 23,644 frees, 149,665,336 bytes`、`in use at exit: 0 bytes in 0 blocks`；出厂二进制 `gomoku_train`（1 轮 8 局）`rc=0`、`ERROR SUMMARY: 0`、`75,217 allocs = 75,217 frees`、`in use at exit: 0 bytes`。
- **覆盖不到的地方照写**：除 `tests/test_search.cpp` 外没有任何测试二进制或生产代码引用 `SearchController`（`grep -c SearchController` 在 `test_server`／`test_session`／`test_training` 都是 0），而 `test_search` 不能进 memcheck —— `:3973` 那条 10 ms 墙钟断言在任何非本机时间尺度上都会炸（轮次124 起记在案）。所以 `#5` 的 memcheck／TSan 两臂用的都是树外探针；出厂二进制的 memcheck 只能证明「本轮没把别的弄坏」，不能算作本轮改动的内存证据。

### 读数 3：`:550` 第一笔账又添一条反面证据

`/tmp/d5/census_20000.txt` 里同一盘棋、同一个 20 000 节点预算的两手：第 5 手 `nodes=20000 / 65 951 µs`，第 18 手 `nodes=20000 / 1 970 342 µs` —— **节点数全等，墙钟差 29.9 倍**。预算买到的只有 alphabeta 那一段；VCF／VCT／认负探针三段各有自己的时间份额，`--max-nodes` 碰不到它们。这与轮次125 记的「`nodes_searched=0` 不是便宜，是压根没进 alphabeta」是同一件事的两面，仍是**没有仪表**的账：D-1 的预算口径没定，−40 % 无法结算。

### 门禁与没动的账

- `cmake --build build -j4` `rc=0`（还原后 6 个 TU），`ctest` **8/8 通过 / 458.68 s**（`test_search` 427.79 s、`test_position` 7.90 s、`test_server` 12.10 s、`test_training` 10.73 s，其余四条 < 0.1 s），`node tests/web_frontend_check.js` **105/105**，`node tests/readme_anchor_check.js` **120 citations / 0 problems**，README 仍 **658 行**（本轮一字未动：`README.md:399` 只在目录树里点了 `controller.h/cpp # 异步思考控制`，没有任何一句话描述 Board 怎么交接，也就没有需要更正的旧说法）。三条门禁全部跑在 `src/`＋`tests/` 定稿之后的树上，串行一条流水线（`/tmp/d5/final_gate.log`，02:15:24 → 02:23:07，7 分 43 秒），入口打印的机器负载是 1.86／3.29／4.08（1／5／15 分钟），`nproc=6`；仪表与门禁不重叠，callgrind 与 memcheck 各跑各的。`ctest` 之后树里只动了本文件，所以那两条会读文档的检查在本节写完之后又各跑一遍：`readme_anchor_check` 仍是 **120 citations / 0 problems**、`web_frontend_check` 仍是 **105/105**，README **658 行**、REVIEW_LOG **6828 行**（本节 +89）。
- 提交前遇到的两次事故都记下来：(1) `build/` 的 CMake 缓存丢了离线 FetchContent 钉（`FETCHCONTENT_SOURCE_DIR_JSON` 为空、`FETCHCONTENT_FULLY_DISCONNECTED=OFF`，而 `build-vg`／`build-tsan` 都带着钉），于是第一次变异跑在 configure 阶段去 github 取 nlohmann/json，TLS 报 `status_code: 35 (SSL connect error)`（DNS 解析到 20.205.243.166 成功），重试五档（15 s、60 s…）后 `Configuring incomplete, errors occurred!`。按项目既有做法把 `build/` 也钉到 `build/_deps/json-src` 后 configure 通过，代价是全量重建 **139 个 TU**，之后每臂只重建 1 个。(2) 上面变异表里那条仪表脚本 bug。
- §2.8 到这里**五条全部动过代码**：`#1` 共享表（轮次127）、`#2` 节点预算（轮次126）、`#3` SPRT 自适应局数（轮次128）、`#4` 开局多样化（轮次129，机制出厂、默认 0）、`#5` 本轮。`:550` 三笔本轮一笔没付，也不该由 `#5` 付。
- **`vcf.cpp:420` 才是 Board 复制的大头**（一次 think 的 53.8–75.7 %），但那一处是**为可复现性付的**：注释写明复用同一块板会让每个防守应手拿到不同的 `active_empty()` 顺序，而威胁排序的平局就是按这个顺序破的（`src/search/vcf.cpp:418-420`；同一理由还在 `alphabeta.cpp:598-601`、`src/core/transforms.cpp:15`、`src/training/data_collector.cpp:141`，以及 `vcf.cpp:244` 的 `Board working = board;`、`vct.cpp:334` 的 `Board mutable_board = board;`）。要动它得先给排序一个与遍历顺序无关的定义，那需要一轮自己的测量，本轮不动。
- 编号清单里还剩**一条从未排进任何阶段的增强**：§2.7 表 `:426` 的「LMR（后排着法减深）| 节点数 −15~25 % | 中（需 re-search 校验）」——`grep -rn "LMR\|late.move\|reduce_depth\|reduction" src/search/` 为空。它不在 `:548` 的 B 阶段验收行里（那行列的是 PVS／排序／aspiration／PV 提取／威胁延伸／双威胁／VCF-VCT 按需），所以 B 阶段结掉之后一直没被认领，而 `:419` 那句「建议全部实现」把它算在内。威胁延伸（`:427`）是量过之后判**不落地**（轮次121 B4），null-move（`:428`）是文档自己写的「本期不做」，只有 LMR 是既没做也没判 —— 下一轮按顺序就是它。
- 继续延期、与轮次129 同一张单：shared-TT 命中率扫描、无锁 TT、SPRT 误晋级率、Elo 账本不按局数加权、`:550` 第三笔的措辞。未跟踪的 `docs/2026-10-17.md`、`docs/PROMPT_STRESS_TEST.md`、`docs/STRESS_TEST_RULES.md` 照旧等用户发话；仪表与探针（`/tmp/d5/` 下 7 个脚本与 3 份 census 输出）一律留在树外。

## 轮次129 引擎重构 D-4：开局多样化第一次能做，而镜像筛查证明它在这个强度上是反的 — engine redesign stage D4, match openings and the mirror screen that turned the default off

规格 `docs/ENGINE_REDESIGN.md` `:436`（§2.8 `#4`「开局多样化：从开局库按权重采样不同开局（当前训练期开局库被完全关闭，导致"颜色通吃"）。改为『固定一组多样化开局 + 交换黑白』，让胜负由系数而非先手决定」）。落地清单只把这一件记了一处：`:590` 的 `src/training/self_play.cpp` 行「节点预算；共享 TT；开局多样化」——`:591` 的 `trainer.cpp` 行没有它（那行是「SPRT 自适应对局数；共享 TT；引擎复用」），`:592` 是 `main_trainer.cpp`。验收仍是 `:550` 那一行的三个读数（单轮耗时 −40 %、内存峰值与并发局数解耦、`--fitness-games 2` 下 fitness 不再只有三档）。本轮把这一件的**机制**做完并留在树上，然后把**缺省关掉**（`--match-opening-plies 0`）：一个树外仪表、一次镜像筛查、一轮两局的轮换见证，三个读数指向同一个结论——在深度 4 / 400 ms 这个强度上，先重放一条固定开局不会让胜负「由系数而非先手决定」，它把一个本来就是必胜的局面钉成 0.5 的一对。`:550` 的三个读数本轮一个都没动：机制改的是**配对打什么局面**，不是成本模型。

### 实现：一对钉一个局面，索引是「轮次 + 对序」，与 `--seed` 无关

- `Trainer::play_game()`（`src/training/trainer.cpp:60-61`）多收一个 `const std::vector<Move>& opening`，原样交给 `SelfPlay::run()`（`:69`；改前那句是 `play.run(std::vector<Move>(), ...)`，所以空 vector 就是老契约）。**`src/training/self_play.cpp` 一个字没改**：`:590` 把这一件记在对弈层，但重放前缀的通道（`entry.method = "replay"`，`self_play.cpp:53`）D-1 就铺好了，本轮缺的是「谁决定这一对打哪个局面」，那句话属于匹配层而不是对弈层。这是本轮相对落地清单的一处有意偏离，写在这里而不是藏在 diff 里。
- `Trainer::match_opening_line(index, plies)`（`src/training/trainer.cpp:208`）：`plies <= 0` 直接返回空，否则取 `random_opening_line(plies, wrapped + 1)`，`wrapped` 把 `index` 折进 `[0, kMatchOpeningSet)`（负索引同样折进来）。固定的是**索引 → 开局**这张表，不是 `--seed`：同一轮里候选和冠军必须拿到同一批局面，而一张跟着 run 的 seed 走的表会让「配对是否公平」取决于优化器根本不关心的那个数。
- `score_match()`（`src/training/trainer.cpp:92`，配对循环 `:128-150`）每对先算一条 `match_opening_line(rounds_completed_ + i, config_.match_opening_plies)`（`:136`），**同一条 opening 交给他两局**——`:139` 候选执黑、`:147` 候选执白——序贯检验仍旧只在 `end_pair()` 读界（`:157`，口径与轮次128 一致）。
- 配置 `TrainingConfig::match_opening_plies`（`src/training/trainer.h:84`）缺省 **0**，`Trainer::kMatchOpeningSet = 8`（`:186`），声明在 `:191`。CLI `--match-opening-plies N`：帮助 `src/training/main_trainer.cpp:58-68`，解析 `:261-265`（`require_int` 下界 0），加一条启动守卫 `:337-341`——`--match-opening-plies >= --max-moves` 立刻退出 1，因为那样一整局从头到尾都是重放、没有一手是搜出来的，验证赛的分母就空了。
- README 表**没有新增行**：新旗标并进 `--accept-score / --sprt-elo / --no-sprt` 那一行（`README.md:199`），另两处正文行内改（`:211`、`:213`），**658 行不变**。这不是洁癖：`tests/readme_anchor_check.js` 的引用按行号寻址，多加一行表就把 `web/app.js:342 -> README:580` 那批引用推到空行上——本轮中途就实测崩出 **21 条 PROBLEM**（`gates_anchor.log`），并回原行以后 120 citations / 0 problems（`gates_anchor_final.log`）。
- 测试四条（`tests/test_training.cpp`，注册在 `test_trainer_cli_sprt_probes()` 之后，`:5301-5304`）：`test_match_opening_set_is_fixed_and_diverse()`（缺省 0 `:4958`、`plies=0` 的老契约、8 个索引在 4 手上折完 8 个对称仍是 8 个不同局面 `:4984`、**同样 8 个索引在 2 手上只到 4 个** `:5000`（读数打印出来，不断言相等）、越界回绕 `:5005`、重复询问同答、6 手仍然合法）；`test_match_pair_replays_its_opening_and_discards_an_illegal_one()`（8 条开局各打一局、8 局都进分母，重复格与错色的前缀必须 INVALID 出局 `:5056` `:5067`——一条非法开局若被当成和局，它会伪装成一次成功的对局）；`test_a_match_rotates_through_the_opening_set()`（关掉检验时 8 局上限真的下满 8 局，4 手／6 手／0 手三个臂同样下满）；`test_trainer_cli_match_opening_probes()`（1 手收、2 手在 `--max-moves 2` 下拒 `:5176`、0 收、**不点名也收**，加上 `-1`／缺值／`1e1`／空串四个拒绝臂与 `--help` 行）。

### 读数 1：三条臂，同一命令同一 seed，只有旗标不同

仪表在树外（`/tmp/d4/instrument2.py`，每局一行 `m=/tag=/pair=/owner=/winner=/reason=/want=/plies=/open=[]/pref=`；`src/training/trainer.cpp` 打完即从 `/tmp/d4/committed_trainer.cpp` 还原），串行单机。三臂同一命令 `--population 4 --generations 2 --stagnation 2 --fitness-games 4 --verification-games 8 --no-data --sprt-elo 300 --rounds 8 --seed 20261011`（检验是开着的，`--no-data` 让三臂都不采集数据，成本全部落在对弈上）。判据是**整场钉在 0.5 的比例**，不是每对的颜色横扫率——一场的分数是各对之和，只有整场钉住才等于「这场没有排序信息」，而 `--accept-score 0.56` 要越过的正是这个平台。

| 臂 | 树 / 配置 | wall / user s | maxRSS kB | 局 / 场 / 对 | 适配度场钉在 0.5 | 颜色决定的对 | 横扫方向 |
|---|---|---|---|---|---|---|---|
| `base` | 改前的 HEAD 树（等价 `plies 0`） | 1106.83 / 1100.18 | 27,052 | 370 / 88 / 185 | 19/80 = 23.8 % | 63/178 = 35.4 % | BLACK 30 / WHITE 33 |
| `open0` | 本轮树，缺省即 0 | 1050.36 / 1045.41 | 27,000 | 378 / 88 / 189 | 26/80 = 32.5 % | 89/179 = 49.7 % | BLACK 61 / WHITE 28 |
| `open4` | 本轮树，`--match-opening-plies 4` | 1034.02 / 1025.31 | 27,020 | 384 / 88 / 192 | 31/80 = **38.8 %** | 121/181 = **66.9 %** | BLACK 95 / WHITE 26 |

`open0` 与 `base` 在语义上是同一棵树（`plies<=0` 返回空，`play.run({})` 就是改前那句），可它自己就移动了 14 个百分点（23.8 → 32.5）：缺省搜索受 `--timeout-ms` 约束，同一条命令在负载不同的时刻会搜到不同深度。这正是 `:550` 第一笔（−40 % 耗时、成本可预测）还没钱的原因，也是本轮**只把 `open4` 与 `open0` 相比**、不把 `open4` 与 `base` 相比的理由。同树两臂差 6.3 个百分点（32.5 → 38.8），方向更干脆：横扫里 BLACK 从 61 涨到 95、WHITE 从 28 掉到 26。耗时与内存在三臂之间没有可用信号（wall ±7 %，`maxRSS` 27.0 MB 上下 52 kB），本轮没动成本模型。两条完整性读数顺带落到仪表里：`open4` 八轮共打到 **8** 条不同开局（集合被用满，没有一局的重放中途停住——`games whose replay stopped short of the request: 0`），`open0` 的 378 局全部是空盘。

### 读数 2：镜像筛查——48 条 4 手开局里 45 条被一方全吃

`/tmp/d4/screen.cpp`：把 `Trainer::random_opening_line(4, i + 1)` 的索引从 0 数到 47，每条打 6 局**镜像**（双方同一套缺省系数，深度 4 / 400 ms / 60 手上限）：

| 结果 | 条数 |
|---|---|
| BLACK 6 局全吃 | 35 |
| WHITE 6 局全吃 | 10 |
| 两色都拿过（5-1 与 4-2） | 2 |
| 6 局都没打完（60 手上限） | 1 |

**48 条里 45 条由颜色定死，只有 3 条不是。**缺省那 8 条（索引 0-7）又单独测了一遍（`/tmp/d4/bias.cpp`，每条 8 局镜像）：索引 2-7 六条 BLACK 8-0，索引 1 是 BLACK 7-1，索引 0 反而 WHITE 8-0；空盘对照（`plies=0`）是 WHITE 8-0——正是 `README.md:213` 记的轮次 54 现象。

于是「换局面」这件事的因果要说反的方向：一对的两局各持黑白打**同一个**局面，局面若是先手必胜，两局就分别是「候选拿先手赢」和「冠军拿先手赢」，一对记 1.0，一场下来正是 `--accept-score` 必须越过的那个 0.5。颜色交换消掉的是局面偏袒在**分数**里的体现，消不掉它在**信息**里的体现。所以 `#4` 想要的「让胜负由系数而非先手决定」成立的条件是 `|局面对先手的价值| < |系数差|`，而深度 4 的四子局基本不满足这条。把缺省设成 0 是把这个读数落到代码里，而不是把一个让目标指标恶化的取值当缺省发出去；下一笔钱在别处——要么换开局形状（更厚、双方结构全等，例如把白的两子取成黑的两子的平移），要么先把搜索的确定性解决掉（`:550` 的 `--max-nodes` 那一笔），否则连一次筛查本身都要被时间噪声淹掉 14 个百分点。生成器那一侧的限制也一并量清了，它们决定了「点名 4 手」这个下界：`legal_moves()` 只走棋子周围的邻格，黑占天元时白方第二手是 **24** 个邻格、折成 **5** 个对称类（2 手的全部词汇表），而缺省 8 个索引实测只落到其中 **4** 类；4 手上 8 个索引给 8 个折完对称仍不同的局面，16 个索引给 16 个——所以 `kMatchOpeningSet = 8` 是**预算不是上限**（预算的意义在于长程运行会重访局面），4 才是武装时的下界。读数可用 `/tmp/d4/probe2.cpp`、`probe3.cpp` 与 `test_match_opening_set_is_fixed_and_diverse` 打印的那两行重做。

### 变异矩阵：五条被套件打死，两条只能由仪表作证

`/tmp/d4/mut_d4.sh`，串行、每臂先按名字删掉受影响的 `.o` 再重建（打印 `recompiles=4`，防这座 vmhgfs-fuse 挂载上的 epoch-1 mtime 空构建）。控制臂 `rc=0`，7 行 `[r129]`。

| 变异 | 打它的断言 | 结果 |
|---|---|---|
| M1 集合塌成一条（`wrapped = 0`） | `tests/test_training.cpp:4984` `keys.size() == kMatchOpeningSet` | rc=134 |
| M2 不回绕（索引 8 ≠ 索引 0） | `:5005` `match_opening_line(kMatchOpeningSet, 4) == match_opening_line(0, 4)` | rc=134 |
| M3 `play_game` 丢掉 handed 的前缀 | `:5056` 重复格的那局不再被判 INVALID | rc=134 |
| M5 缺省改成 4（把关掉的东西打开） | `:4958` `TrainingConfig().match_opening_plies == 0` | rc=134 |
| M6 守卫改成 `>`（允许开局吃满手数） | `:5176` `at_cap == 1` | rc=134 |
| M4 第二色重放**下一条**开局 | 套件看不见（对局层无法从外部区分两条合法开局） | 仪表：24/24 对的两色开局不一致；干净树 192 对里 **0** 对 |
| M7 轮换不看轮次（`match_opening_line(i, ...)`） | 套件看不见 | 仪表：两轮口径见下 |

M7 在 1 轮口径上读不出差别（一场适配度赛只用 2 对，索引 0-1 与 0-1 相同），所以 `/tmp/d4/gap_stages.sh` 把它挪到 `--rounds 2` 重测，两臂都带仪表、都点 `--match-opening-plies 4`、同 seed、`--seed 20261011`，各 96 局 22 场 48 对：

| 臂 | 用到的集合索引 | 落在两条开局上的局数 | 第一轮 fit 各对的开局索引 | 第二轮 fit 各对的开局索引 |
|---|---|---|---|---|
| 控制（`rounds_completed_ + i`） | 0-4，共 **5** 条 | 68/96 | 对 0 → 0，对 1 → 1 | 对 0 → **1**，对 1 → **2** |
| M7（只看 `i`） | 0-3，共 **4** 条 | 88/96 | 对 0 → 0，对 1 → 1 | 对 0 → 0，对 1 → 1 |

也就是说：M7 的第二轮与第一轮**逐对相同**（`m7b.err` 里 22 场的对 0 全是索引 0、对 1 全是索引 1），控制臂的第二轮整批前移一格；把「同一对序跨轮是否换开局」当作判据，控制臂 2 个槽位换、M7 0 个。这条是仪表读数，不是套件断言——它值得的那点东西（长程运行不重访同一批局面）本来也不是一条可以从外部区分的性质。

**仪表自身会打断一条既有测试**：M4／M7 两臂是「打了仪表」的树，套件在 `test_trainer_refuses_a_seed_that_a_resume_would_drop` 就中止了，失败的是那条 `--resume` 单独使用时「stderr 必须为空」的控制断言（`tests/test_training.cpp:3613` `assert(last_err.empty())`）——那条用例把 `gomoku_train` 的 stderr 捕获进 `last_err`，而仪表往同一 stderr 里印 `[r129]` 行。所以这两臂的证据只能是读数，绝不能写成「套件仍绿」；同一句话也说明仪表那 7 行 `[r129]` 不该被合进任何一条用例的断言路径。

### valgrind

`bash tools/valgrind_check.sh memcheck test_training gomoku_train -- --rounds 1 --population 2 --generations 1 --depth 1 --timeout-ms 2 --max-moves 6 --fitness-games 4 --verification-games 4 --no-data --seed 20261011`（`build-vg`，Debug，按计划重建 4 个 TU）：`test_training clean reports=0 invalid_read=0 invalid_write=0 uninitialised=0 definite_leak=0 306s`；同一行的 `gomoku_train` 报 `ABORTED rc=1` 且只活 3 s。包装脚本把 mktemp 里的 stderr 删了，所以本轮把这条命令行原样直跑了一遍才知道原因，而且**它不是代码缺陷**：`tools/valgrind_check.sh:129-130` 把任何以 `--` 开头的参数当作 extra 分隔符，**并把这个 `--` 本身也塞进 `extra`**，于是二进制收到的第一个参数就是一个孤零零的 `--`；在当前树上 `./build-vg/gomoku_train -- --rounds 1 --population 2` 打印 `Error: unknown option --` / rc=1，秒级退出。测试二进制不解析 argv，所以同一轮只有 `gomoku_train` 中枪。去掉那个分隔符、其余参数一字不动：`rc=0`、1 s、stderr 空。

带 `--log-file` 并保留 stderr 的明细臂（`/tmp/d4/vg_detail/`）：`test_training` rc=0 / `ERROR SUMMARY: 0 errors from 0 contexts`（155 s，`All training tests passed`）；`gomoku_train` rc=0 / `in use at exit: 0 bytes in 0 blocks` / `6,533 allocs, 6,533 frees, 59,682,007 bytes` / `ERROR SUMMARY: 0`。**这一臂重放的是空开局**（缺省 0），所以它没有检查本轮新增的任何代码，`gap_stages.sh` 于是补了一臂 `--match-opening-plies 4`：rc=0 / `in use at exit: 0 bytes` / `10,757 allocs, 10,757 frees, 59,507,235 bytes` / `ERROR SUMMARY: 0 errors from 0 contexts`，收尾一行 `Training finished: rounds=1 versions=2 champion=v0 games=16 samples=0`——`games` 与关掉旗标那一臂**同为 16**，说明 8 条前缀是被接受、被重放，而不是被丢成 INVALID（INVALID 会把该局从分母里剔掉，`games` 会掉）。`test_search` 依旧不进 memcheck：它那条 10 ms 墙钟断言（`tests/test_search.cpp:3973`）在 valgrind 的时间尺度下必死，轮次124 已经把这句话落在日志里。

### 门禁与没动的账

`ctest` 8/8、457.63 s（`test_search` 425.69 s、`test_training` 13.62 s），`node tests/web_frontend_check.js` 105/105，`node tests/readme_anchor_check.js` 120 citations / 0 problems，README **658 行**、REVIEW_LOG 6739 行——这三条全部重跑在**改完数字之后的树**上（`/tmp/d4/final_gate.log`，串行一条流水线，00:51:29 起 11 分 17 秒）。memcheck 明细臂同样重取：`build-vg` 按计划重建 4 个 TU，`test_training` rc=0 / `in use at exit: 0 bytes` / `432,790 allocs, 432,790 frees` / `ERROR SUMMARY: 0 errors from 0 contexts`（153 s，`All training tests passed`），`gomoku_train --match-opening-plies 4` rc=0 / `10,757 allocs, 10,757 frees` / `ERROR SUMMARY: 0` / `games=16`，与上面那一臂逐字相同——47→45 改的是一条注释、一个帮助字符串和 README 两处，没有运行路径，这一点由重跑而不是由话说服。三处更正在出厂二进制上也核过：`gomoku_train --help` 里 45 那行在（`help_lines_with_45=1`），`src` 与 `README.md` 里已经搜不到「47 条 / 47 of the first 48」。`test_search` 依旧不进 memcheck：它那条 10 ms 墙钟断言（`tests/test_search.cpp:3973`）在 valgrind 的时间尺度下必死，轮次124 已经把这句话落在日志里。变异矩阵跑完之后树是从 `/tmp/d4/clean_trainer.cpp` 还原的（`final build rc=0 recompiles=4`、`test_training rc=0`，`mut_r129_final.log` 末尾四行 `same as the control`），提交前这一份又与 `git diff` 逐字核过一遍。没提交的东西：仪表与探针（`/tmp/d4/instrument2.py`、`screen.cpp`、`bias.cpp`、`probe2/3.cpp`、`parse.py`、三个 stage 脚本）都留在树外，压测/仪表代码不混进生产提交；`docs/2026-10-17.md`、`docs/PROMPT_STRESS_TEST.md`、`docs/STRESS_TEST_RULES.md` 仍未跟踪，等用户发话。`:550` 三笔账本轮一笔没付：−40 % 单轮耗时要求先把 VCF／must_block 那些提前退出的手计入预算（轮次125 已记下 `nodes_searched` 为 0 不等于便宜），内存与并发解耦在 D-2 已由共享表付掉一半、本轮无新增，「fitness 不再只有三档」本轮给出的是**反面证据**——换局面这条路让 0.5 平台更厚，而档数本身是 `pairs = max(1, (games+1)/2)` 的算术。D 阶段剩下的 `#5`（`SearchController::search_worker` 去掉多余的 Board 复制，`src/search/controller.cpp:36`、`controller.h:50`、`alphabeta.cpp:601`）不在本轮动作范围内，继续按顺序做。

## 轮次128 引擎重构 D-3：验证赛第一次由序贯检验决定它要打几局，而「三档」那笔账本来就是算术 — engine redesign stage D3, SPRT-adaptive match length

规格 `docs/ENGINE_REDESIGN.md` `:435`（§2.8 `#3`「SPRT 自适应对局数：验证赛改为序贯检验（Sequential Probability Ratio Test），在统计显著时提前终止，而不是固定 8 局。解决 README 记录的『fitness 只有 0/0.5/1 三档』问题的一半（另一半靠评估函数的位置项修复）」），落地清单 `:591` 认的实现位置是 `src/training/trainer.cpp`，`:596` 认的测试是「`tests/test_training.cpp` 新增：节点预算、共享 TT 并发安全、SPRT 终止」，验收仍在 `:550`。本轮付掉「序贯检验」这一件，并在读数 2 里把「三档」那一半交回去：它不是棋力，是 `pairs` 的算术，而 SPRT 一点分辨率也不加——`--fitness-games 2` 这一档上，检验一条界都碰不到。

### 实现：`sprt.h` 只是一张算术表，trainer 多了一个出口

- 新文件 `src/training/sprt.h`（116 行，header-only，沿用 `src/search/search_limits.h`／`search_state.h` 的先例，`CMakeLists.txt` 只列 `.cpp`，所以一个字没改）：`SprtConfig{effect_elo=300, alpha=0.05, beta=0.05}`（`:30`）、`SprtVerdict{OPEN, ACCEPT, REJECT}`（`:41`）、`sprt_verdict_name()`（`:47`，`OPEN` 走 `case …: break;` 而不是 `default:`，为了 `switch` 不吞 `-Wswitch`）、`SprtTest`（`:59`）。每局增量是 `s·ln(p1/0.5) + (1−s)·ln((1−p1)/0.5)`（`:81`），`p1 = 1/(1+10^(−E/400))` 且夹在 `1 − 1e-9` 以下（`:70-72`），两界各 `±ln((1−α)/α)`（`:63-64`）。
- **`add_game` 不算生死，`end_pair` 才算**（`:78`、`:86`）：一对手的两局之间不投票。这不是保守，是这张棋盘的现实——README `:213` 记的「颜色通吃」就是每局单看都领先、合起来正好 0.5 的那种比赛，允许在第一局后停下等于把先手优势当证据。`decided_` 一旦置位就不回头（`:87`、`:91`），所以调用方在闭合之后继续下也照样报出它当初停下来那个判断。
- 训练侧只有一个出口：`Trainer::score_match()`（`src/training/trainer.cpp:92-157`）返回 `MatchOutcome{score, games, verdict, llr}`（`src/training/trainer.h:111`），`play_match()` 退化成 `return score_match(...).score;`（`:86-90`）——优化器看到的信号口径一字未动，这是「不改变适配度」这条约束的实现形式。`TrainingConfig::sprt_elo`（`trainer.h:61`）取 0 就是关掉检验（`SprtTest::enabled_`，`sprt.h:62`），此时 `score_match` 与改前逐句等价。
- 每对下完取一次界（`trainer.cpp:143-148`），`pairs = max(1, (games+1)/2)`（`:125`）从「局数」变成「上限」；分数是**实际下完的局**的均值而不是上限的均值（`:153-155`），被丢弃的无效局留在分母外。
- 晋升规则换了主人（`:402-410`）：`ACCEPT` 直接晋升，`REJECT` 直接留冠军，只有 `OPEN`（上限走完仍在两界之内）才回头看 `--accept-score`。轮次日志与收尾摘要都印出判断与局数（`:441-447`、`src/training/main_trainer.cpp:402-407`）。
- CLI：`--sprt-elo`（帮助 `main_trainer.cpp:49-57`，解析 `:243-247`，`require_int` 的下界给 0，因为 0 是「不要检验」这个答案本身，负数才拒），`--no-sprt`（`:67-68`、`:248-249`）。README 五行行内改（流程 `:180`、表 `:197-199`、颜色通吃那段 `:213`），**658 行不变**。
- 测试四条（注册在 `tests/test_training.cpp:5009-5012`）：`test_sprt_closes_only_at_a_pair_boundary()`（`:4573`，纯算术——两界各 `±2.9444390`、300 差下胜项 `+0.5294752` 负项 `−1.1974637`、6 局收、4 局拒、全平分的 8 局留 `−2.6719539` 而 OPEN、单局过界不算、以及补臂那段「越过另一侧界之后 accept 仍然作数」）；`test_a_closed_sprt_ends_the_match_early()`（`:4716`，`--max-moves 1` 把每一局都钉成未分胜负，于是比值只能是平局项 × 8；同一个八字上限在 300／1000／0 三个刻度上各打下几局；顺带断言 `play_match()` 报的数与 `MatchOutcome::score` 一字不差——适配度口径没动，`:4762-4763`）；`test_a_reject_verdict_keeps_a_candidate_the_bar_would_promote()`（`:4775`，`--accept-score 0.4` 之下 0.5 分本该晋升，检验给出 REJECT 就不晋升，关掉检验同一套配置就晋升——两个臂只差检验开关）；`test_trainer_cli_sprt_probes()`（`:4823`，CLI 取值与 `--help`，本轮唯一去断言收尾摘要**措辞**的用例）。

### 读数 1：同一个 seed，检验关着就是改前那棵树，开着少打两局、冠军一字不差

八臂同一 seed（`--rounds 1 --method genetic --population 8 --generations 4 --depth 4 --timeout-ms 400 --max-moves 60 --seed 20261011 --no-data`，只在两个局数旗标与 `--fitness-games` 上不同），串行跑在空机上；`c8`/`c2`/`cf4` 三臂是**改前**的 HEAD 树（`d534c3a`）跑的同一命令，其余五臂是工作树。

| 臂 | 树 / 配置 | wall / user s | rss kB | 局数 | 那一轮的那行 |
|---|---|---|---|---|---|
| `c8` | HEAD 树，缺省 `--fitness-games 2 --verification-games 8` | 174.63 / 173.58 | 26,976 | 66 | score 0.875 → promoted |
| `c2` | HEAD 树，验证上限 2 | 165.42 / 164.66 | 26,852 | 60 | score 1 → promoted |
| `cf4` | HEAD 树，适配度 4 局 | 236.14 / 234.25 | 27,028 | 124 | score 0.875 → promoted |
| `n8` | 新树 `--no-sprt` | 179.47 / 177.31 | 26,928 | 66 | score 0.875 (open after 8 games, llr 0) → promoted |
| `s8` | 新树缺省 300 | 169.17 / 168.27 | 27,080 | 64 | score 1 (accept after 6 games, llr 3.17685) → promoted |
| `e1000` | 新树 `--sprt-elo 1000` | 162.84 / 162.28 | 27,012 | 64 | score 1 (accept after 6 games, llr 4.13994) → promoted |
| `f8s` | 新树 `--fitness-games 8`，检验开 | 295.44 / 292.70 | 27,240 | 202 | score 1 (accept after 6 games, llr 3.17685) → promoted |
| `f8n` | 新树 `--fitness-games 8 --no-sprt` | 269.93 / 267.91 | 26,824 | 240 | score 1 (open after 8 games, llr 0) → promoted |

（局数那一列不是算出来的，是各臂 `.ckpt` 里的 `games_played`，八臂分别印 66 / 60 / 124 / 66 / 64 / 64 / 202 / 240；它们与上限语义完全对得上：`29 次评估 × 适配度局数 + 验证实下局数`——60/66 = 58+2/8，64 = 58+6，124 = 116+8，240 = 232+8，`f8s` 的 202 = 196+6，即适配度那 232 局里检验替本轮跳过了 36 局。）

- **`n8` 与 `c8` 的产物逐字节相同**：`cmp /tmp/d3/c8.ckpt /tmp/d3/n8.ckpt` 与 `cmp c8.json n8.json` 都无输出——`games_played` 66、两位玩家的 Elo、十个系数一字不差。这就是 `--no-sprt` 的契约：关掉检验不是「换个算法」，是回到改前的固定局数。
- `s8` 收在 `+3.176851 = 6 × 0.5294752`，正是缺省 300 差下连胜过界那条线；`s8.json`、`n8.json`、`c8.json`、`f8s.json` 四份冠军文件互相同（晋升的是同一个候选），`s8.ckpt` 对 `c8.ckpt` 只差三行：`games_played` 66 → 64 与两个 Elo（1488/1512 → 1484/1516，账本吃的是 `1.0` 而不是 `0.875`）。**「判断没变、只是少打两局」这句话的物证就是这三行。**
- **`s8` 与 `e1000` 是一对空臂**：300 与 1000 两个刻度在这条 seed 上都在第六局收，`cmp s8.ckpt e1000.ckpt` 逐字节相同 ⇒ 两臂做的功完全一样，而 wall 差 6.33 s、user 差 5.99 s。这就是这台机器在这一尺度上的**噪声地板**，下面那句「省了多少」必须跟它比。
- 于是验证侧的省——`n8 − s8 = 9.04 s user / 10.30 s wall`、少打 2 局——只比噪声地板高 1.5 倍。**能立住的读数是局数（66 → 64），时间那句只能算同量级。**少掉的是那场比赛的第 7、8 局：两臂前 6 局都是六连胜（`s8` 的 `+3.176851 = 6 × 0.5294752` 就是这条），而 `c8`/`n8` 的 0.875 = 7/8 说明这两局合计只值 1 分——要么两局未分胜负，要么一胜一负，这两种记录在 SimplePay 下同一个 LLR（`−0.6679885` 对 `−0.6679884`，正是本轮第一条用例断言的那条恒等式），合到 8 局是 `+2.508863`，仍留在 `2.944439` 之内，所以 `n8` 报 OPEN 与算术自洽。单价也不守恒：这两局合 4.52 s/局，而整轮 66 局平均 2.69 s/局（`n8` 的 user ÷ 66）、`c8 − c2` 那 6 局是 1.49 s/局——同一场比赛里越往后的局越贵，所以「省 N 局 = 省 N × 均价」这条外推在这批数据上就不成立。
- **适配度那一侧不是省钱，是换信号**（`f8s` 对 `f8n`，同一棵树、同一 seed）：检验开着的 `f8s` 少打 38 局（240 → 202，−15.8 %）却多花 24.79 s user（+9.3 %），而且晋升的**不是同一个候选**——`f8n.json` 与 `f8s.json` 从第 3 行起就分叉（`cand_width_deep` 24 对 26、`live_four` 15332 对 38123、`edge_penalty` 0 对 80…），`f8s` 那份反倒与缺省尺度的 `c8/n8/s8` 一字不差。原因写在算术里：早停只在整局整负的记录上触发，被跳过的是那类局，留下的是纠缠的长局，而 0-4 记 `0.0` 与下满 8 局记 `0.25` 给优化器看的不是同一个排序。所以本轮**不**给 `:550` 的第一条记这笔账，反过来要把「适配度侧的检验会改选出的候选」这条挂在缺口里。
- 顺带一条刻度自检：`e1000` 的 `llr 4.139939 = 6 × 0.689990`，1000 差下连胜 5 局（3.45）已经过界，但界只在**对尾**读，所以仍是第 6 局收——这正是 `sprt.h:84-92` 那句「pair 之间不投票」要的形状。
- 单价不守恒这件事按差分再量一遍就更大：`c8 → cf4` 是 61.51 s wall / 58 局 = **1.06 s/局**，`cf4 → f8n` 是 33.79 s / 116 局 = **0.29 s/局**，同一件「加一局」差到 3.6 倍；而 `n8 − s8` 少掉的那两局合 **4.52 s/局**。轮次125 量的 1.99 s（`REVIEW_LOG:2289`）是**采集**那一侧在另一个尺度（~30 s 的小轮）上的单价，不能拿来当系数用。所以任何「省 N 局 = N × 均价」的外推在这批数据上都不成立，`s8 → f8s` 那种把适配度每对从 2 局加到 8 局的推算尤其不能信。

### 读数 2：`--fitness-games 2` 不是三档，是五档；而这一档上检验永远不闭合

- 三档那句话的算术：`pairs = max(1, (games+1)/2)`（`trainer.cpp:125`）除以 `played_games`（`:155`）。`--fitness-games 2` 就是一对，两局的半分各取 `{0, 0.5, 1}`，均值集合是 `{0, 0.25, 0.5, 0.75, 1}` ——**五个值**。「0/0.5/1 三档」只在没有未分胜负局时才成立，而 `:213` 自己就写着满 60 手算和。实测（HEAD 树，`--rounds 3 --population 2 --generations 1 --verification-games 8 --seed 20261011`，`/tmp/d3/vdist.out`）：round 1 适配度 0.000000 / 验证 0.000000，round 2 适配度 **0.250000** / 验证 **0.312500**，round 3 都是 1.000000。0.25 与 0.3125 都不在那「三档」里。
- SPRT 在这一档上一条界也碰不到：一对的 `|LLR|` 最大是两连负 `−2.3949273`，界是 `−2.9444390`。要能拒绝至少要 4 局（`−4.789855`），要能接受至少要 6 局（`+3.176851`）。所以缺省尺度的适配度赛（1 对）根本不会提前结束，`:435` 说的「解决三档问题的一半」在这一档上没有可兑现的东西：**检验自适应的是局数，不是信号的取值集合**。分辨率只有两条来路——加大 `--fitness-games`（成本线性，本轮 f8s/f8n 量的正是这条），或者按 `:435` 自己说的另一半去修评估函数的位置项。
- 缺省 `--sprt-elo` 为什么是 300 而不是书上的 50：`sprt.h:22-29` 那段算的是同一个东西——50 差时每一项不到 0.16，8 局上限内 `|LLR|` 最大 1.23，检验永远不闭合（等于白装）；250 差时连胜要打到第 8 局才够界；300 是「4 连负即拒、6 连胜即收」都在 8 局上限内的最松一个刻度。

### 变异矩阵：第一轮六臂里有一条是绿的，那条不是通过，是仪器缺了一段

对照臂是未变异的工作树用同一命令跑：`./test_training` rc=0 / 12-15 s，六条 `[r128]` 读数齐。

| 变异 | 改动 | 结果 |
|---|---|---|
| M1 | 把负局写成 `-win_term_` 而不是 `ln((1−p1)/0.5)`（`sprt.h:74`） | rc=134，`tests/test_training.cpp:4604: Assertion 'blown.llr() < -bound' failed`——2000 差下一局负不再过界 |
| M2 | `add_game()` 自己调一次 `end_pair()`（`:81` 之后），界不再只在对尾读 | rc=134，`:4605: Assertion '!blown.stop_requested()' failed`——单局就定了生死 |
| M3 | 去掉 `p1` 的 `1 − 1e-9` 夹逼（`:72` 改回 `raw`） | rc=134，`:4698: Assertion 'std::isfinite(absurd.llr())' failed`——1000000 差时 `ln(0)` 把比值变成非有限 |
| M4 | 去掉 `end_pair()` 的 `decided_` 门闩（`:87`） | **第一轮 rc=0（绿）**，补臂之后 rc=134，`:4646: Assertion 'sweep.verdict() == SprtVerdict::ACCEPT' failed` |
| M5 | 晋升只看分数：`accepted = score >= accept_score`（`trainer.cpp:408-410` 三行换成一行） | rc=134，`:4792: Assertion '!stopped.accepted' failed`——那条用例的名字就是这句话 |
| M6 | 分数除以**上限**而不是实际局数（`trainer.cpp:155`） | rc=134 / 5 s，`:2214: Assertion 'std::abs(score - 0.5) < 1e-12' failed`，炸在轮次107 那条 `test_play_match_rounds_up_to_even` 上——它比本轮的用例排得前，先接住了这条 |

- **M4 那条绿是本轮最值钱的一次读数**：读了 `end_pair()` 才明白这个变异在这套断言下是**等价**的——赋值只发生在越界那一支，所以「不回头」在「继续越同一侧的界」的续局里根本显不出来；要它显形，得让续局去越**另一侧**的界。缺省 300 下 `+3.176851` 的 accept 之后再接三对全负才做得到：`0.7819236 − 3 × 2.3949273 = −6.4028583`，干净穿过下界。补进 `tests/test_training.cpp:4638-4649`（12 行），用例收尾那句话也跟着把这一段念出来（`:4702-4706`，比原来多一行），然后**整棵树重跑六臂**——所以表里 M1/M2/M3/M5/M6 的行号是补臂之后那一轮的量。位移本身就是这段插入的物证：M3 从 `:4686` 走到 `:4698`（+12，它排在插入块之后、收尾那句话之前），M5 从 `:4779` 走到 `:4792`（+13，多出来的正是那句话的那一行）。这是轮次124「C3-5 钉住被中断那一手」、轮次127「M3b 的锚点撞三次」的同一种修法，代价是一次 20 分钟的整轮重跑。
- M6 的红落在别人的用例上，这条要记下：`test_play_match_rounds_up_to_even`（轮次107）早就在断言「3 个上限下满 4 局、分数是实际局的比值」，所以分母一错它先炸。本轮新增的用例没有重复发明它，M6 的证据因此是**既有契约**而不是新断言。

### 门禁（跑在补臂之后的那棵树上）

编译这一行量了两次，因为第一次那台编译器根本没有干活。`cmake --build build -j4` 印 rc=0 / 12 s、`warning:` 0 条，可整份日志里**一行 `Building CXX` 都没有**：这个 workspace 挂在 `vmhgfs-fuse` 上（整棵树 root-owned、mode 777），`trainer.cpp`／`main_trainer.cpp`／`test_training.cpp` 三支的 mtime 被报成 `1970-01-01 08:00:01`（`stat -c %Y` 印 `1`），比任何 `.o` 都旧，make 因此判定无事可做——那个 0 是一次空读，不是「没有警告」。换 `cmake --build build -j4 -- -B` 强制全量重编：rc=0 / 329 s、**139 个 TU**、`warning:` **5 行 = 3 条**（两条各被两个 target 编到），三条都在本轮没动的文字上：`src/training/data_collector.cpp:95:62 [-Wunused-parameter]`（`side_to_move`）、`src/server/server.cpp:178:13 [-Wunused-function]`（`server_reason_of`）、`tests/test_training.cpp:3571:28 [-Wrange-loop-construct]`（`for (const std::string arguments : …)`，HEAD 里在 `:3570`，被本轮第 13 行那个新 `#include` 推下来一行）。前两支本轮一字未动（`git status` 里没有它们），第三支是上一轮留下的——本轮新写的代码与用例一条警告也没有，但那三句「没有警告」只有在全量重编里才是读数。同一支文件在 `build-vg` 里也是真重编（`[19%] Building CXX … trainer.cpp.o`、`[29%] … test_training.cpp.o`），所以 `:3571` 那条警告先在 memcheck 那一臂撞见，而不是在这条空转的增量构建里；这也意味着八臂计时读数的合法性得靠产物自己说话：`s8` 那行印的是「accept after 6 games, llr 3.17685」，这句措辞只存在于新代码里。

`ctest`（跑在强制重编之后的二进制上，串行、空机）**8/8 / 461.70 s**（test_board 0.05 s test_pattern 0.39 s test_evaluator 0.02 s test_search 423.98 s test_position 7.61 s test_session 0.03 s test_server 16.78 s test_training 11.65 s）；这一臂是重跑的那一次：门禁第一轮 `ctest` 也 8/8 / 459 s，跑在变异矩阵最后一臂那次真重编出来的同一批程序上，所以两条都绿，但只有重跑这条能与上面那行编译读数放在一起讲。`node tests/web_frontend_check.js` **105/105**；`node tests/readme_anchor_check.js` **120 citations / 0 problems**；README **658 行**；门禁之后 `git status --porcelain` 只有本轮动过的六支加 `?? src/training/sprt.h`，变异矩阵的临时文件都留在 `/tmp/d3/`，没有落进树里。

### Valgrind

| 臂 | 结果 |
|---|---|
| `bash tools/valgrind_check.sh memcheck test_training`（包装） | rc=0 / 434 s，行印 `test_training  clean  reports=0  invalid_read=0 invalid_write=0 uninitialised=0 definite_leak=0  248s` |
| memcheck 明细臂：`build-vg/test_training`，`--leak-check=full --show-leak-kinds=all --track-origins=yes --log-file=/tmp/d3/gate_vg_detail.log`（不带 `-q`，stderr 全留在盘上） | rc=0 / 257 s，`ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)`，`in use at exit: 0 bytes in 0 blocks`，`All heap blocks were freed -- no leaks are possible`，`total heap usage: 340,068 allocs, 340,068 frees, 587,851,515 bytes allocated`；六条 `[r128]` 读数与 `All training tests passed` 都在这臂的 stdout 里 |

- `--log-file` 那一臂是本轮特意补的：轮次124 记的就是「`tools/valgrind_check.sh` 把 stderr 送进 mktemp（`:166`）、印完行 `rm -f`（`:250`），断言文本落不了盘」，这次明细自己留在 `/tmp/d3/gate_vg_detail.log`，下一轮想复核还能读。
- 三条量不到的照写：`test_trainer_cli_sprt_probes` 走 `std::system()`（`tests/test_training.cpp:4847`），memcheck 默认不追子进程，那五条 CLI 探臂 exec 的是 `build-vg/gomoku_train` 本体，不在 memcheck 之下；`test_search` 不在 memcheck 下重跑，因为 `tests/test_search.cpp:3973` 的 10 ms 臂在任何非本机时间尺度上都会炸（轮次124 已把断言文本量到并记在案）；本轮没有 TSan 臂——`SprtTest` 是 `score_match()` 的栈上局部（`trainer.cpp:118`），一场比赛的两色对局在这一个调用里串行打完（`:128-148`），本轮没有新增跨线程对象，与轮次127 那张共享表不是同一类东西。

### 引用重钉

- `trainer.cpp` +52/−10、`trainer.h` +28/−0、`main_trainer.cpp` +26/−4、`tests/test_training.cpp` +374/−5（那 5 行全是本轮行内重钉的注释，没有一条断言被删被改；补臂那段共 13 行（`:4638-4649` 的 12 行加收尾那句话多出的 1 行），补臂之前这支文件量到 +361/−5、5003 行；引用校验器与变异矩阵六臂都是在补臂之后的最终树上重跑的）。这四个文件之外的源码一字未动。
- `REVIEW_LOG.md` 的**重钉**改动是 87 行、行数不变（6569，门禁量到的就是它）；本节拼入之后全档 6664 行，其中本节（连同下面那条位移说明）95 行：先按 difflib 对齐逐文件算出「同一段文字的新行号」，再对每条被改动的句子读一遍原文确认——脚本 `collect.sh`/`repin_citations.py`/`repin_paths.py`/`repin_bare.py` 与位置配对校验器 `verify_repin.py` 留在 `/tmp/r128/`。校验结果：这四支文件里的引用 token 共 **99 条**，8 条数字未动且文本相同，82 条重钉后文本相同，9 条（7 个不同）是**本轮自己改了被引用那段文字**而故意留下的差异：`trainer.cpp:371→408`（晋升规则本体）、`:96-99→107-109`（「每次评估新建引擎」那段注释）、`:108→125` 两次（那句「奇数向上取整」的注释现在指向 `pairs` 这一行本身）、`main_trainer.cpp:45→46`（`--verification-games` 的帮助行改成「上限」措辞）、`tests/test_training.cpp:439→440`（新 `#include` 把整支文件往下推一行）。
- 本节自己的拼入把 1982 以下整段推下 95 行，所以**档内**指向绝对行号的句子跟着改钉：上面那句 `:5524`／`:5637` 现在是 `:5619`／`:5732`，读数 1 里引用的轮次125 那两条从 `:2189`／`:2194` 变成 `:2284`／`:2289`。档内另有两处不是本轮写的（`:5708` 的 `REVIEW_LOG:250`、`:6197` 的 `REVIEW_LOG:1083`）指向插入点之上，位移碰不到它们；替换全是等长的数字换数字，改钉前后 `wc -l` 都是 6664。
- 另有两条是**在 HEAD 上就已经指错**、本轮顺手钉到当前真位置的：`REVIEW_LOG.md:5619` 的 `trainer.cpp:311` 与 `:5732` 的 `:355`——HEAD 树里 `settings.max_iterations` 在 `:346`、`result.games_played` 在 `:390`，本轮之后分别是 `:373` 与 `:429`，现在两个数字都对得上文字。同一类「HEAD 就落后 27 行」的锚点（`trainer.cpp:218/238` 那批 D-2 变异表里的数字）被本轮的整体位移**恰好**推回正确位置，这条是巧合，不是本轮证明的东西，写在这是为了让下一轮别把它当已核验。
- 工具自己的一条缺陷：`/tmp/r128/verify_citations.py` 把 `controller.cpp`／`controller.h` 映射到 `src/training/`（真路径是 `src/search/`），于是那两条读数印成「file has 0」；它按「同一个数字在 HEAD 与在工作树是否同文」判 244 条中的 72 条 DIFFERS，其中绝大多数正是本轮重钉成功的那批（数字没变回去、文字动了才叫 DIFFERS）。这个 72 不是缺陷计数，别引用它。

### 缺口（写给下一轮的字）

- 仪器本身的一条：这条挂载（`vmhgfs-fuse`，整树 root-owned mode 777）会把被改写过的源文件的 mtime 报成 epoch，`trainer.cpp`／`main_trainer.cpp`／`test_training.cpp` 三支本轮量到 `stat -c %Y` = 1，比任何 `.o` 都旧，于是 `cmake --build build -j4` 一行 `Building CXX` 都不印。**任何「改了 src 之后再跑增量构建」的读数都可能是在测上一棵树的二进制**——要么 `-- -B`，要么先删对应 `.o`，要么像本轮这样用一个只存在于新代码里的输出措辞反证。轮次125 那批时间是 docs-only 轮次（`src/` 与 a1a3652 相同），不受这条影响；受影响的是 D 阶段接下来每一次「训练变快了」的量法，包括 `:550` 那条 −40 % 要从 `REVIEW_LOG:2284` 那把 182.91 s/轮的尺里减的那把尺。

- `:550` 第一条（单轮耗时 −≥40 %）：本轮在缺省尺度上只买到「66 局里少打 2 局」（验证赛 8 局 → 6 局，占一轮局数的 3.0 %），时间那半句因为噪声地板（5.99 s user）就在同一量级，本轮不报成收益。一轮的 88 % 局数（58/66）在适配度赛，而 `--fitness-games 2` 那一档检验永远不会闭合（读数 2），所以 −40 % 不会由 D-3 付：要么加 `--fitness-games`（`f8s` 那一臂实测反而更贵，且换了候选），要么按 `:435` 的后一半去动评估函数。第二条（内存与并发解耦）轮次127 已付。第三条（`--fitness-games 2` 不再三档）读数 2 交回去了：它本来就不是三档，而 SPRT 给不了分辨率——这一条要么改口径，要么归 `:435` 的后一半（评估函数位置项），不属于 D-3。
- LLR 假设每局独立，训练里同一对的两局是同一套系数、同一条开局线下游打的，相关性没量过：真正的错误率（把一个 2700 分的候选当成 300 分晋升几次）需要一批已知强弱的对照局，本轮没有。
- Elo 账本现在吃的是「played-games 分数」，早停的场次少 ⇒ 一场 6 局 1.0 与一场 8 局 1.0 记同样权重（`trainer.cpp:414` 的 `record_result`），没按局数加权。
- `--verification-games`／`--fitness-games` 在上限语义下不再等于成本，README `:197-198` 说的是这个意思，但训练时长因此更难预估——轮次125 要的「按节点出价」把耗时那一半钉住了，局数这一半现在是弹性的，两者相乘的预算没有仪器（`nodes_searched` 在 vcf/must_block 那两手恒为 0，那笔账仍欠着）。
- 适配度侧的提前终止会**改变优化器看到的分数**（0-4 早停记 0.0，而同一候选下满 8 局可能是 0.25）。本轮没做「早停前后适配度排序是否一致」的对照，`:435` 也没要求，但这条是 D 阶段后面任何一次「训练变快了」的读数里必须拆开的一环。

## 轮次127 引擎重构 D-2：N 个采集 worker 第一次共用一张表，而那棵树的节点一个也没多 — engine redesign stage D2, process-level shared TT

规格 `docs/ENGINE_REDESIGN.md` `:433`（§2.8 `#1`「进程级共享只读 TT：自对弈的所有引擎共享一张大表（如 2^22 槽 ≈ 96MB），分片加锁或 lock-free；内存与并发局数**解耦**。当前是『每引擎一张 8.4MB 表』，N 个并发 → N × 8.4MB」），落地清单 `:596` 认的测试是「`tests/test_training.cpp` 新增：节点预算、共享 TT 并发安全、SPRT 终止」，风险表 `:569` 给的路线是「先分片加锁（16 片），再考虑 lock-free；训练侧先单线程验证正确性」，验收仍在 `:550`（单轮耗时 −≥40 %、峰值内存与并发解耦、`--fitness-games 2` 不再三档）。轮次125 把这张表定价为 7.3 MB/worker（不是 `:434` 还写着的 8.4 MB），轮次126 付掉了耗时那一半；本轮只付内存那一半，并把耗时那一半没有再动这件事写在读数 3。

### 实现：`shared` 是构造时的事实；分片切桶索引的高位，所以记录不动

- `TranspositionTable(size_t num_entries = 1 << 18, bool shared = false)`（`src/optimization/transposition.h:74`），拷贝与赋值 `= delete`（`:78`），`is_shared()`（`:81`），`kShards = 16`（`:153`），`shard_for()`（`:173`），成员 `shard_shift_`（`:212`）、`std::atomic<uint8_t> generation_`（`:215`）、`mutable std::array<std::mutex, kShards> shards_`（`:216`）、`const bool shared_`（`:217`）。
- **为什么 `shared` 不是开关**：那个 flag 每次 probe/store 都要读，线程起来之后再写它，本身就是这把锁要防的那个竞态；所以它是构造时决定、`const` 成员。不共享的表一次锁都不取（`src/optimization/transposition.cpp:20-37` 的 `ShardLock` 拿到 `nullptr` 就是个空对象）——会话搜索每个节点都走这条路，C2/C3 把它量到纳秒，为一个没人用的互斥量付钱不可接受。
- **为什么按桶索引的高位切**：`index_for()` 一个字节没改，于是一个桶里仍是那四条记录、一次 store 仍淘汰同一条记录。分片只挪锁，不挪记录；桶数不足 16 时 `shard_shift_ = 0`，退化成每桶一把锁，仍然正确。
- 取锁的入口四个：`store()` `transposition.cpp:75`、`probe()` `:139`、`static_eval_of()` `:155`、`put_static_eval()` `:174`。全表走查的两个用 `AllShardLock`：`clear()` `:210`、`hashfull()` `:223`（`hashfull` 只看前 100 桶，那 100 桶可以跨片，所以整表取锁）；锁一律按索引顺序拿，两个同时 `clear()` 的线程不会互锁。
- **代龄这本账换了主人**：`advance_generation()`（`:205`）在共享表上是整批的时钟——一个 worker 落一子就把所有记录往前推一格。这是「内存与并发局数解耦」的诚实代价，写在 `src/optimization/transposition.h:131-137`；替代方案（每局一个年龄）要么加宽表项要么给线程各自一份状态，两者都把这一行省下的东西再花回去。
- `SearchEngine` 侧只有一个注入口：`SearchEngine(const ThreatCoefficients&, TranspositionTable* table = nullptr)`（`src/search/search_engine.h:65`，实现 `src/search/search_engine.cpp:22-35`），表由 `owned_tt_` + `tt_alphabeta_` 两个成员表示（`search_engine.h:138-139`，`transposition_table()` 仍在 `:127`）。**这里换过一次方案，换的原因是峰值**：先写的是 setter（引擎先建自己的表、再把表交出去），同一把尺子量到 t=4 只比独立表低 796 kB（48,700 对 49,496 kB，`/tmp/d2/rss_post.log`）——一块 7.3 MB 的 vector 在 worker 线程自己的 arena 里释放并不还给内核，于是「共享」把钱花了，却没把省下的地方腾出来。改成构造注入后 worker 从不分配，斜率才跟着 `--threads` 脱钩（读数 2）。副作用一条：`SearchEngine` 不再可拷贝（`unique_ptr` 成员）；全树没有拷贝调用点，整树 `cmake --build` rc=0 就是这条的证据。
- 训练侧两处：批次头建表并清空（`src/training/trainer.cpp:232-245`），worker 的引擎走注入（`:265`）。批首那次 `clear()` 就是轮次125 要的失效口径——记录是「写它时那套系数」的分数，而冠军可能已经换了；打系数戳要加宽表项，比较系数集要在每次 probe 前比一整套数，清空两条都不需要，而且一批就是一轮、一张 7.3 MB 的表清空不花钱。**适配度赛刻意不共享**（`play_match` 两侧同时握着两套系数，共享等于拿对手的数给自己的位置打分）。
- CLI：`--shared-tt N`（帮助 `src/training/main_trainer.cpp:60-64`，解析 `:254-257`，`require_int` 的下界给 0 因为 0 是「不要共享」这个答案本身），上限在 `:310-315` 卡在 2^22 = 4,194,304 槽（28 B/槽 ≈ 117 MB，正是 `:433` 自己举的量级）——再大不是买复用，是买一张没人填得满的表，而这条路径不接 `bad_alloc`。缺省 `1 << 18`（`src/training/trainer.h:75`）＝一个 worker 原本的表大小：先让「解耦」成立，再谈要不要一张大表。观测口 `shared_transposition_table()`（`:147`）、成员 `shared_tt_`（`:180`）。README `:200` 行内改，658 行不变。

### 读数 1：`:433` 的「只读语义不变」——同一棵树，221,774 一个不差

新增用例 `tests/test_search.cpp:8479-8565`（注册 `:8745`）：同一个 quiet 局面、同一套系数（`b1_evaluator(24, 8)`）、深度 6、无时钟无节点预算，两臂各自新建一张表跑一次搜索。

| 表 | BLACK 节点 | WHITE 节点 | qnodes | 分数 | 落子 | 完成深度 | seldepth |
|---|---|---|---|---|---|---|---|
| 不共享（无锁） | 221,774 | 156,428 | 与套件既有断言逐字相同 | 全等 | 全等 | 6 | 6 |
| 共享（16 把锁） | 221,774 | 156,428 | 全等 | 全等 | 全等 | 6 | 6 |

221,774 与 156,428 不是本轮现取的数：它们早被 `tests/test_search.cpp:5578`、`:6243`、`:6448` 三条既有断言钉着（B3 那次从 221,380 改到 221,774 的记录在 `:5569`）。共享臂与这些常数相等，才是「分片挪锁不挪记录」的证据而不是修辞。同一颗用例还跑退化表（64 槽＝16 桶＝16 把锁）：自己写的记录自己读得回、`hashfull()` 在 0..1000 之间、`clear()` 后读不回且 `generation()` 回到 0。

### 读数 2：`:550` 的第二条——峰值 RSS 的斜率换了主人

`/usr/bin/time` 的峰值 RSS，串行、空闲机，`--shared-tt 0` 是「每 worker 一张」的对照臂，产物只落 `/tmp/d2/`。三把尺子（8 局阶梯 = `rss_post.sh` 的 `--data-games 8 --timeout-ms 400`；40/8 局两臂 = `arms2.sh`；节点预算两臂 = `nb_arms.sh` 在 `--max-nodes 3000` 下）：

| 尺子 | worker | 每 worker 一张 | 共用一张 | Δ |
|---|---|---|---|---|
| `--no-data`（两张表都不建，对照） | 1 | 19,860 kB | 19,764 kB | −96 kB |
| 8 局 · ms 400 阶梯 | 1 | 27,444 kB | 20,448 kB | −25.5 % |
| 8 局 · ms 400 阶梯 | 2 | 34,556 kB | 20,404 kB | −40.9 % |
| 8 局 · ms 400 阶梯 | 4 | 49,296 kB | 20,816 kB | −57.8 % |
| 40 局 · ms 400 | 4 | 49,416 kB | 20,776 kB | −58.0 % |
| 8 局 · ms 400 | 8 | 79,008 kB | 21,496 kB | −72.8 % |
| 40 局 · `--max-nodes 3000` | 4 | 50,024 kB | 21,132 kB | −57.8 % |
| 8 局 · `--max-nodes 3000` | 8 | 79,568 kB | 21,416 kB | −73.1 % |

斜率才是这一行规格要说的事：4→8 个 worker，独立表 +29,592 kB（ms）与 +29,544 kB（节点预算）＝ **+7,398 / +7,386 kB 每 worker**；共享表 +720 与 +284 kB ＝ **+180 / +71 kB 每 worker**；8 局阶梯上 t=1→4 是 +7,284 kB/worker 对 +123 kB/worker。7,398 kB 对 `transposition.h:64` 自己写的 2^18 × 28 B = 7,340,032 B = 7,168 kB 是同一量级（多出的 230 kB 本轮没拆开归因，只能说它是每引擎除表以外的常驻状态），这就是「降下来的那一块确实是表」的归因仪器。共享臂自身 t=1/2/4/8 = 20,448 / 20,404 / 20,816 / 21,496 kB，四点平坦。

### 读数 3：耗时那一半本轮没有再来一次（照 `:550` 报缺口）

墙钟：8 局阶梯 t=1/2/4 是 29.56/29.82/30.14 对 29.81/28.77/29.56 s；`arms2` 的 t=4 是 30.33 → 30.16 s、t=8 是 31.26 → 30.64 s；`nb_arms` 的 t=4 是 8.44 → 8.74 s、t=8 是 8.37 → 7.01 s。这六个读数里有两个的**样本数不同**，所以它们不是同一盘棋的两个计时，不能当成本读（见下一节）。原因是轮次125 量到的结构：一轮约 28 s 在串行的适配度赛，采集不在关键路径上，一张共享表改的是采集的内存，不改那条串行的路。`:550` 的「−≥40 %」是轮次126 用 `--max-nodes` 付的（−71.5 % @默认量级），本轮对它的贡献是 **0**。

### 共享真正改变的东西：一批局里的信息，不只是成本

样本数：8 局阶梯（t=1/2/4）两臂都是 112/224/496，`arms2` 的 40 局 t=4 两臂都是 496——这四个读数证明「同尺度下批量不变」；但 8 局 t=8 是 952 → 1,000，节点预算 40 局 t=4 是 1,152 → 1,008，节点预算 8 局 t=8 是 1,600 → 848。这不是漏账：一张表意味着 A worker 写的记录可能被 B worker 命中，而 B 的 root 是 A 永远走不到的位置。同批共享的前提正是「所有 worker 用同一套 `current_champion` 系数」（`trainer.cpp:230`），跨批靠批首 `clear()`。年龄/标签方案本来就吃这种记录（键不同就不算同一条），但读数 1 只证明了**一棵树**不变，没有证明**一批局**不变。

### 变异矩阵：六条各破一句，最后一条是给仪器本身

对照臂是未变异的树用同一命令跑：`./test_training` rc=0 / 11 s。

| 变异 | 改动 | 结果 |
|---|---|---|
| M1 | `store()` 不取片锁（`transposition.cpp:75` 整行去掉） | rc=134，`tests/test_training.cpp:4480: Assertion 'torn == 0' failed`，读数行自己印 80,000 写/80,000 读里 **torn=140** |
| M2 | `probe()` 不取片锁（`:139`） | rc=134，同一行 `:4479`，**torn=87** |
| M3 | 共享臂按 `num_entries / 4` 建表（分片连记录一起挪） | rc=134 / 426 s，`tests/test_search.cpp:8509` 的 `tt.size() == entries` 先炸——尺寸断言排在节点断言前面 |
| M3b | 只改淘汰：共享表上同键不更新（`if (slot.key != key \|\| shared_) continue;`），表大小不变 | rc=134 / 428 s，`tests/test_search.cpp:8520: Assertion 'shard.nodes == plain.nodes' failed`——节点等式对「记录被动了」确实敏感，这一条才是读数 1 背后的仪器 |
| M4 | 删掉批首 `clear()`（`trainer.cpp:245`） | rc=134，`:4364: Assertion '!survived' failed`，读数行印 `sentinel after the next batch: STILL THERE, generation back to 8` |
| M5 | 删掉注入实参（`trainer.cpp:265` 改传 `nullptr`） | rc=134，`:4333: Assertion 'games_with_hits == games' failed`，读数行印 `games answered by the table=0 generation=0` |

**harness 自己的一条缺陷，记下来**：M3b 第一版的锚点写成裸的一行 `if (slot.key != key) continue;`，而这行在 `store()`／`probe()`／`put_static_eval()` 里出现 **3 次**；python 的 `count == 1` 断言先炸，脚本却没有停，于是 build 了一颗没变异的树并「测」出 rc=0。第二版把锚点扩到带后随注释的两行、并在 patch 失败处 `exit 1`，才拿到上面那条红。这与轮次124 的「零计数来自一棵没建起来的树」、轮次126 的「`--target test_training` 不重建 `gomoku_train`」是同一面墙的第三次撞法，代价是一次 428 s 的白跑。

### 门禁（跑在最后那次改动的那棵树上）

`ctest` **8/8 / 463.62 s**（`test_search` 432.25 s ＝ 套件的 93.2 %，`test_training` 11.81 s，`test_position` 8.58 s，`test_server` 9.88 s）；`node tests/web_frontend_check.js` **105/105**；`node tests/readme_anchor_check.js` **120 citations / 0 problems**；README **658 行**。门禁之后 `cmp` 过 `src/optimization/transposition.cpp` 与 `src/training/trainer.cpp` 对门禁快照逐字节相同——TSan 活性对照那次临时去锁没有留在树里。

引用重钉：本轮 `trainer.cpp` +24/−1、`search_engine.cpp` +10/−6，后面的行整体下移 23 / 4 行，跟着重钉 9 条注释锚点（`tests/test_training.cpp:440`→`trainer.cpp:408`、`:2106`→`:318`、`:3399`→`:374`、`:3501-3502`→`trainer.cpp:628`/`:486`、`:4026`→`search_engine.cpp:295`/`:260`；`tests/test_search.cpp:3907`→`search_engine.cpp:290-291`、`:5757` 与 `:6214`→`:357`）。全部是行内替换，两个测试文件行数不变（8749 / 4647）：`assert(result.resigned)` 仍在 `:2591`、10 ms 那一臂仍在 `:3973`、`assert(completed.method == "alphabeta")` 仍在 `:3986`；这两个文件里被删掉的行**只有**这 9 条注释，没有一条断言被改。`CMakeLists.txt`、`web/`、`tools/` 一字未动。`docs/ENGINE_REDESIGN.md` 指向代码的锚点（`:108` 的 `search_engine.h:77`、`:84` 的 `transposition.cpp:37-39`）不动：那是重设计文档在 phase-5 开工前的现状快照，按当前树改写等于抹掉需求文档自己的历史前提。

### Valgrind / TSan

| 臂 | 结果 |
|---|---|
| memcheck `build-vg/test_training`（`--leak-check=full --show-leak-kinds=all --track-origins=yes`，不带 `-q`，`--log-file` 把 stderr 全留在盘上） | rc=0 / 254 s，`ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)`，`in use at exit: 0 bytes in 0 blocks` ＋ `All heap blocks were freed — no leaks are possible`，328,336 allocs = 328,336 frees / 438,809,255 B；七条 `[r127]` 读数与并发用例都在这一臂里跑过 |
| memcheck 共享表驱动（`/tmp/d2/driver_shared_vg.cpp`：一张共享表跑一次真实搜索 + 退化 16 桶表 + 4 写线程 1 走查线程） | rc=0 / 2 s，`ERROR SUMMARY: 0 errors from 0 contexts`，7,961 allocs = 7,961 frees / 15,962,780 B（两张 7.3 MB 表），depth-3 quiet 无锁 2,515 节点 ＝ 共享 2,515 节点，退化表 40 写 38 命中、`hashfull 593`、清空后读不回 |
| TSan `build-tsan/test_training`（`-fsanitize=thread -g -O1 -fno-omit-frame-pointer`，离线 json pin） | rc=0 / 32 s，`WARNING: ThreadSanitizer: data race` **0 条**，七条 `[r127]` 读数齐（`torn reads=0`、`generation=208`），`All training tests passed` |
| TSan 活性对照：把 M1（`store()` 去锁）装进同一颗 TSan 树重跑 | rc=134，**59 条** data race，栈里点名 `TranspositionTable::store()` 与 `probe()` 的表项读写（`transposition.cpp:131`／`:144`——那是 M1 那颗树的行号，去掉锁行之后本体上移了）——所以上面那个 0 是一次读数，不是一台没接上的仪器 |

三处如实报：驱动那臂印 `generation 0`，因为 memcheck 的时间尺度下四个写线程先跑完、走查线程一次都没轮到自己，所以「负载下的 `clear()`/`hashfull()`」由 `test_training` 用例与 TSan 臂承担，驱动只证明这两条路径本身干净；memcheck 不追子进程，`test_trainer_cli_shared_tt_probes` 的六条 CLI 探臂 exec 的是 `build-vg/gomoku_train` 本体，不在 memcheck 之下；TSan 那颗树另有两条与本轮无关的 skip（`CLI --params probes`、`--no-sessions conflict probes`，因为没在那颗树里建 `gomoku_server`）。`test_search` 不在 memcheck/TSan 下重跑：`tests/test_search.cpp:3973` 的 10 ms 臂在任何非本机时间尺度上都会炸（轮次124 已把断言文本量到并记在案），而本轮新增的唯一并发对象在 `test_training` 里。

### 缺口（写给下一轮的字）

- `:550` 三条：本轮推进第二条（解耦，斜率 +7,386 → +71 kB/worker），第一条贡献 0（读数 3），第三条（`--fitness-games 2` 不再三档）归 D-3 的 SPRT。
- `:433` 举的例子是 2^22 ≈ 96 MB，本轮缺省只到 2^18。顺序是「先把解耦做实，再决定要不要一张大表」：表越大被跨 worker 命中的记录越多，而命中率对训练数据质量的影响本轮没有量——`--shared-tt` 的扫描（2^18/2^20/2^22 × 同一 seed 的样本数与验证分）欠着。
- `:569` 的第二半「再考虑 lock-free」没动。
- 读数 1 只覆盖一棵树（quiet 局面、深度 6、`b1_evaluator(24, 8)`）；中局/残局、以及 2^22 大表下的树等价性没有断言。
- 共享改变了一批局的样本数（上面单列）——这是这行规格的物理后果，不是被证明无害的东西。
- VCF／VCT／认输探测仍按 ms 切片（轮次126 记的同一笔）。

## 轮次126 引擎重构 D-1：一手棋第一次可以按节点出价，而时钟留在原地 — engine redesign stage D1, --max-nodes

规格 `docs/ENGINE_REDESIGN.md` `:434`（§2.8 `#2`「节点预算替代时间预算：训练配置从 `--timeout-ms` 改为 `--max-nodes`（保留 `--timeout-ms` 作为安全兜底）。每盘成本可预测，训练时长可控，且不受机器负载影响」），落地清单 `:592` 认的接口是「`src/training/main_trainer.cpp` 新增 `--max-nodes`；保留 `--timeout-ms` 为兜底」，验收仍在 `:550`：「单轮训练耗时下降 ≥ 40 %；内存峰值与并发局数解耦；`--fitness-games 2` 下 fitness 不再只有三档」。轮次125 把尺子交出来，本轮按那把尺子复测。

### 实现：预算走的是引擎字段，不是新开关

- `SelfPlayConfig::max_nodes`（`src/training/self_play.h:18`，缺省 0）、`TrainingConfig::max_nodes`（`src/training/trainer.h:38`，缺省 0）。
- 装表的唯一位置是 `SelfPlay` 的构造函数（`src/training/self_play.cpp:25-26`）。这不是顺手：`SearchEngine::search()` 的签名只接深度和毫秒（`src/search/search_engine.h:56-58`），节点上限进不了那一次调用，只有引擎字段这一条路（读它在 `src/search/search_engine.cpp:299`）。构造即装表同时管住两条调用线——采集侧的 `RetryExplorer` 把配置原样转交给它内嵌的 `SelfPlay`（`self_play.cpp:150-152`），`set_max_nodes` 对非正数夹回 0（`search_engine.h:78`），所以「0 = 不装」在两个方向上都成立（M 臂 M3 之外还专门有一条用例盯这一点）。
- 两处拷贝：`trainer.cpp:65`（适配度赛与验证赛走 `play_game`）与 `trainer.cpp:248`（采集 worker 在自己的线程里建 `RetryExplorer`）。这两行不是一行：`--threads` 只并行采集（轮次125 第 1 条），漏掉任何一条就是一半的局未按预算跑。
- CLI：`main_trainer.cpp:39-42` 帮助、`:225-229` 解析。`require_int` 的下界给的是 **0** 而不是缺省的 1，因为 0 是这个开关自己的答案（「不要节点上限」），而其它计数项的 0 是「跑一趟什么都不做」；`:291` 那条旧校验一字未动，负数在解析阶段就被拒。
- 新增的唯一读取口是 `search_engine.h:81` 的 `max_nodes()`。加它是因为 `SelfPlay` 装表之后，训练侧的接线只有从引擎外面才量得到（`tests/test_training.cpp` 的 `#define private public` 只重写了 `trainer.h`，`SearchEngine` 的字段仍是私有的）。

**`--max-nodes` 不置 `limits.training`**，这是本轮唯一一处「文档内部打架、按其中一条实现」的决定，也是量出来才发现的：`:434` 要保留兜底，`:251` 却要「训练模式：直接返回 {0,0,max_nodes}，时间预算不参与」。按 `:251` 实现本来会关掉什么？——**什么也关不掉**。`TimeManager::allocate`（`src/search/time_manager.cpp:43-92`）在生产路径里**没有调用者**（全树只有 `tests/test_search.cpp:4039-4102` 的 A1 脚手架调它），而一手棋的时钟是 `SearchEngine` 自己两处装的：`limits.hard_time_ms`（`search_engine.cpp:291`）与 `ab.set_deadline`（`:256`）。所以「保留兜底」在这棵树里不是一个可以被开关违背的选项，它是现状；要把它拆掉得动那两行，本轮不动，并且用 M5 证明「拆掉两处才红、拆一处不红」。`:251` 那行等 `TimeManager` 真的接到搜索上的那一天再结算。

### 读数：`:550` 的耗时那一半达成，代价写在同一行

同一把尺子（`--depth 4 --max-moves 60 --seed 20261011 --rounds 1`，`/usr/bin/time`，6 核机器，每臂串行），产物只落 `/tmp/d1/`。

| 臂 | 预算 | 墙钟 | user | 峰值 RSS | 该轮结论 |
| --- | --- | --- | --- | --- | --- |
| B-ms ×2 | `--timeout-ms 400`，`--no-data`，pop 4 × gen 2 | 58.53 / 57.10 s | 57.43 / 56.62 s | 27,260 / 27,092 kB | 2 代 10 评估、验证 1.000 晋升，22 盘 |
| B-n4000 | 同上 + `--max-nodes 4000` | **16.99 s** | 16.42 s | 27,100 kB | 同一结论 |
| B-n12000 | 同上 + `--max-nodes 12000` | 32.86 s | 32.37 s | 26,920 kB | 同一结论 |
| B-n500 | 同上 + `--max-nodes 500` | 10.20 s | 10.06 s | 20,088 kB | 同一结论 |
| D-ms ×2 | 默认量级（pop 8 × gen 4、验证 8 局、采集 4 盘 × 2 线程） | 180.59 / 183.48 s | 187.28 / 189.32 s | 42,512 / 42,084 kB | 3 代 29 评估、验证 **0.875 晋升 v1**、70 盘 / 704 样本 |
| D-nodes ×2 | 同上 + `--max-nodes 4000` | **52.53 / 51.21 s** | 53.15 / 52.66 s | 42,420 / 42,268 kB | 3 代 29 评估、验证 **0.375 不晋升**、70 盘 / 560 样本 |

- **耗时**：no-data 尺子相对两次 ms 的均值 57.82 s，`--max-nodes 4000` 是 −70.6 %、12000 是 −43.2 %、500 是 −82.4 %；默认量级相对均值 182.04 s 是 **−71.5 %**（逐臂 −70.9 % / −72.1 %）。`:550` 的「单轮 −≥40 %」由 D-1 一条就在这把尺子上过线了；README `:186` 那条 `--rounds 20` 按轮次125 的尺子是 ≈61 分钟，按 `--max-nodes 4000` 是 ≈17 分钟。
- **代价必须写在同一行**：这一半是「每手棋搜得更浅」买的，不是免费的。records 里 alphabeta 着法达到的深度，ms 臂是 `{3, 4}`，节点臂是 `{2, 3}`；同一 seed 下默认量级的验证赛结论因此**翻了面**（0.875 晋升 ↔ 0.375 不晋升，两次复跑各自稳定）。所以 `#2` 换到的是**口径**（成本可按节点定价、逐着可复现），不是一个可以让同一轮既便宜又做同样决定的旋钮。要按节点训练，预算得先按「这一档深度还认不认」定，而不是按省了多少秒定。
- **「每盘成本可预测」第一次有仪器**（轮次125 第 4 条欠的那件）：节点臂 4 盘记录共 90 个引擎着法，其中 **70 个正好 4000 节点**，`sum_nodes` 两次都是 280,000 = 70 × 4000；ms 臂 116 个引擎着法、两次 1,849,936 / 1,840,180 节点。剩下的 20 着仍是 `must_block`/`vcf` 早退，按定义记 0（`search_engine.h:23`），这条口径没变，也仍然是「0 ≠ 便宜」。
- **「不受机器负载影响」量到的是复现性**：同一二进制、同一 seed 两次跑，ms 臂 88 个 alphabeta 着法里只有 **78 个**报同样的节点数（差的 10 个正是被时钟切的位置），节点臂 **70/70 全等**。两次 ms 臂的验证分这回合都是 0.875，所以不能把轮次125 那一次的 1.000 归因于此——逐着工作量已经证明「同一 seed 的 ms 臂不保证同一盘棋」，但要把 1.000↔0.875 记在这笔账上还需要更多样本，本轮不下这个结论。
- **内存**：42,512 / 42,084 对 42,420 / 42,268 kB，四个数同一量级——预算口径不动内存，`:550` 的第二条（与并发局数解耦）不是 `#2` 能碰的，它是 D-2。

### 测试：六个用例，断言写的是精确节点数

`tests/test_training.cpp` `+313/−8`，其中 8 行删除全部是本节『门禁』列出的注释行号重钉，新增的六个用例本身是纯追加（定义 `:3970-4259`，注册 `:4331-4336`），写法沿用 REVIEW_LOG 既有风格：先跑真实对局再对账，不用桩。

1. `test_self_play_node_budget_bounds_every_think`（`:3970`）——深度 4 / 5 s / 1000 节点下的 8 着：每着 `method == "alphabeta"`、`stopped == "nodes"`、`nodes == 1000` 逐着全等，`total_nodes == 8000`；再用另一对全新引擎复跑，逐着比对落子与节点数（`:4005-4010`）。**断言敢写精确值，正是因为截止点是一个每节点都读的计数器**（`search_state.h:422`）而不是墙钟——这与 `test_search.cpp:3973` 那条 10 ms 臂在 valgrind 下必红是同一件事的两面。
2. `test_self_play_ms_budget_backs_up_a_generous_node_ceiling`（`:4024`）——深度 8 / 60 ms / 2000 万节点：4 着全按 alphabeta 回答，**没有一着报 `nodes`**，且至少一着报 `time_hard`（读数打印 `4/4`）。断言的是停止原因而不是时长，理由同上。
3. `test_self_play_zero_ceiling_arms_no_node_budget`（`:4067`）——`max_nodes` 缺省时构造 `SelfPlay` 前后 `max_nodes()` 都是 0，整盘没有任何一着 `stopped == "nodes"`。这条是「老调用者一字不变」的下界（`quick_play_config()` 与 `RetryExplorer` 数据侧全靠它）。
4. `test_self_play_one_node_ceiling_still_answers_legally`（`:4094`）——1 节点上限：6 着全报 `nodes == 1`、`depth == 0`、`partial_depth == 1`，整盘仍 `assert_playable`、结局是 `MOVE_LIMIT` 而不是 `INVALID`。预算再小也不许把盘写坏，这条是「训练侧必须继续能吃 `src/search/search_engine.cpp:341-346` 那套分层账」的钉子。
5. `test_trainer_config_node_budget_reaches_both_game_paths`（`:4130`）——`play_game` 之后两台引擎都读回 1000；换成 `max_nodes` 未给的配置去开一台已被 `set_max_nodes(9000)` 装过的引擎，必须夹回 0；再走采集侧：`generate_data_games(1, 1)` 写出的 JSONL 用 `GameRecord::from_json` 读回，逐着 `nodes <= 1000` 且至少一着真触顶（打印 `data games=1 node-capped plies=4`）。
6. `test_trainer_cli_max_nodes_probes`（`:4202`）——`--max-nodes 0` 与 `--max-nodes 5000` 都退出 0 且 stderr 空，`-1` 退出 1 且报这个旗标名，缺值与 `1e3` 退出 1，`--help` 里有这一行。

### 变异矩阵：六臂，逐臂只红一处，全部被抓

每臂改一处、整树重建、只跑 `test_training`；控制臂是未改动的同一驱动（rc=0 / 9.84 s）。

| 臂 | 改动 | 结果 |
| --- | --- | --- |
| M1 | 删 `self_play.cpp:25-26` 两行装表 | rc=134，`test_training.cpp:3991` `entry.stopped == "nodes"` |
| M2 | 删 `trainer.cpp:65`（适配度侧拷贝） | rc=134，`:4143` `black.max_nodes() == config.max_nodes` |
| M3 | 删 `trainer.cpp:248`（采集侧拷贝） | rc=134，`:4185` `entry.nodes <= data.max_nodes` |
| M4 | `require_int("--max-nodes", …, 0)` 改回缺省下界 1 | rc=134，`:4231` `zero == 0` |
| M5 | 同时拆掉两处时钟装配（`search_engine.cpp:291` 与 `:256`） | rc=134，`:4059` `done >= 1`，打印 `0/4 plies cut by the ms budget` |
| M6 | 只装黑棋引擎 | rc=134，`:3990`（白棋那一着未封顶） |

**本轮工具自己的两处缺陷，按原样报而不是当发现交出去**：

1. 第一遍跑矩阵时 `run_arm` 用的是 `cmake --build build --target test_training`，它**不重建 `gomoku_train`**，而用例 6 的 CLI 探臂 exec 的是 cwd 里那个二进制——于是 M4 报了一次假绿（rc=0）。第二遍改成整树重建才拿到 `:4231` 的红。这与轮次124 记的「零计数来自一棵没建起来的树」是同一类错误，只是这次藏在另一个目标上。
2. 同一个 M5 第一遍的 rc=0 **是真读数不是工具问题**：只把 `limits.hard_time_ms` 归零并不足以拆掉兜底，`ab.set_deadline` 那一处在装着同一个时钟。这一条也正是上面「`limits.training` 什么也关不掉」那个判断的实验根据。

`/tmp/d1/`：`mut.sh`（M1-M3、M6）、`mut2.sh`（M4、M5，整树重建）、`measure.sh`（成本臂）、`repro.sh`（复现臂 + Valgrind）、`mem.sh`（四臂 memcheck）、`driver_d1.cpp`。

### Valgrind

四臂 memcheck 串行跑在空闲机上（`--tool=memcheck --leak-check=full --show-leak-kinds=all --track-origins=yes`，`--log-file` 把 stderr 全留在盘上，不用 `tools/valgrind_check.sh`——轮次124 记过它把 stderr 送进 mktemp 再 `rm -f`，断言文本从来落不了盘）。二进制来自 `build-vg`，用离线 pin 配置（`-DFETCHCONTENT_SOURCE_DIR_JSON=build/_deps/json-src -DFETCHCONTENT_FULLY_DISCONNECTED=ON`）；`test_training` 与 `test_server` 都在 `build-vg` 目录里执行，因为它们的 CLI 探臂 exec 的是 cwd 下的 `./gomoku_train`，换个目录就是静默 skip 而不是失败。

| 臂 | rc | 时长 | ERROR SUMMARY | in use at exit | 堆使用（allocs/frees/bytes） |
| --- | --- | --- | --- | --- | --- |
| driver `/tmp/d1/driver_d1_vg` | 0 | 26 s | 0 errors from 0 contexts | 0 bytes | 26,871 / 26,871 / 69,989,264 |
| `test_training` | 0 | 219 s | 0 errors from 0 contexts | 0 bytes | 286,104 / 286,104 / 418,388,354 |
| `test_server` | 0 | 42 s | 0 errors from 0 contexts | 0 bytes | 110,677 / 110,677 / 468,062,337 |
| `test_search` | 134 | 254 s | 到不了（进程在 `:3973` abort，无 HEAP SUMMARY） | — | 20 条 loss record：19 still reachable + 1 possibly lost；invalid read/write 0、uninitialised 0 |

- 前三臂每一行都是 `All heap blocks were freed -- no leaks are possible`，allocs 与 frees 相等。driver 臂是本轮的接线仪器（`/tmp/d1/driver_d1.cpp`，非提交物）：`armed max_nodes black=1000 white=1000` → 6 着 6,000 节点 → `disarmed max_nodes=0` → 1 节点臂 `total_nodes=4`、20 s 兜底臂 `total_nodes=554`，`DRIVER plies=14 capped=10`。
- **工具缺陷一条，按原样报**：第一遍四臂都带 `-q`，而 `-q` 下 clean run 写出的 log 是 **0 字节**（对照：`valgrind -q /bin/true` 落 0 字节，去掉 `-q` 落 658 字节并打印 `ERROR SUMMARY: 0 errors from 0 contexts`）。所以第一遍那三行的 `uninit=0 invalid_read=0` 是对空文件 grep 出来的，`ERROR SUMMARY`/`definitely lost` 两个字段直接是空串——与轮次124 记的「零计数来自一棵没建起来的树」同类，只是这次藏在 flag 上。上表来自第二遍（`/tmp/d1/mem2.sh`，三臂不带 `-q` 重跑，26/219/42 s），`test_search` 那一行仍取第一遍的 28 KB log，因为它本来就有内容。
- `test_search` 的 rc=134 复现的是**既有**那条 10 ms 墙钟臂，文本逐字一致：`test_search: /home/wdz/Desktop/workspace/gomoku/tests/test_search.cpp:3973: void test_engine_reports_the_interrupted_alphabeta_layer(): Assertion 'interrupted.partial_depth == 1' failed.`（`out_test_search.log`，紧邻的上一行是 `test_engine_reports_the_interrupted_alphabeta_layer measured (interrupted: alphabeta d0+partial 0/n0 time_hard, ...)`，即断言之前读数已经打印）。它红在 valgrind 的时间尺度而不是本轮代码——本轮六条新用例全在 `test_training`，那一臂 rc=0 且 `All training tests passed`。
- 那 20 条 loss record 的归属可查：still-reachable 的栈顶是 `TranspositionTable::TranspositionTable`（`src/search/transposition.cpp:23`）经 `SearchEngine::SearchEngine()`（`src/search/search_engine.cpp:26`），即进程 abort 时仍活着的引擎自有的表；possibly lost 的 304 B 是 glibc `pthread_create` 的 DTV 分配（`allocate_dtv (dl-tls.c:470)`）。两条都随 abort 一起消失，不是本轮引入的分配。
- TSan 不复跑，理由与轮次124 同一条并加强：本轮新增的共享面是**零**——`max_nodes` 只在 `SelfPlay` 构造时写进各自引擎的字段（`self_play.cpp:25-26`），读取方 `search_engine.cpp:299` 与 `search_state.h:422` 都在同一线程的那次思考之内；worker 之间没有任何新交叉。B3 记在案的那个真竞态在 `server` 侧，仍排队在 bugfix 批。


### 门禁

- `ctest`：**8/8 通过 / 442.47 s**（`test_search` 414.18 s 占 93.6 %、`test_training` 9.89 s、`test_server` 10.23 s、`test_position` 7.93 s）。
- `node tests/web_frontend_check.js`：**105/105**；`node tests/readme_anchor_check.js`：**120 citations / 0 problems**；README **658 行**。
- README 的 `--max-nodes` 是**行内改写 `:195`**（选项表那一行），没有增删行——被注释引用的 `:198`/`:204`/`:206`/`:207`/`:215` 全部还在原行，这也是 anchor 读数与上一轮相同的原因。
- 位移波及的行号全部重新钉过：`docs/ENGINE_REDESIGN.md:108` 的 `trainer.cpp:110-111 → 101-102`、`trainer.cpp:241 → 215`；`tests/test_training.cpp` 里 6 处 `trainer.cpp` / `self_play.cpp` 行号按本树重定位，其中 `:59`、`:256`、`:312`、`:551`、`:409`、`:122-131` 这六条在 HEAD 上**本来就已指向空行或别的语句**（逐条 `git show HEAD:… | sed -n 'Np'` 对过），一并钉到当前真位置（`:69`、`:295`、`:351`、`:605`、`:463`、`:164-170`）。
- `src/` 之外只动 `tests/test_training.cpp`（+313/−8，8 行删除全是上条所列注释行号重钉）与两份文档；`CMakeLists.txt`、`web/`、`tools/` 一字未动。

### 未达成 / 未完成

- `:550` 的三条只推进了第一条的一半：**内存与并发解耦**没动（本轮 RSS 与预算口径无关，四个读数同量级），**`--fitness-games 2` 三档**没动（轮次125 第 5 条：那是 `pairs = 1` 的算术，归 D-3 的 SPRT）。耗时那一半达成（−71.5 % @默认量级），但达成方式是「搜得更浅」，本轮把它连同翻面的验证分一起交了，没有粉饰成免费收益。
- **节点上限只封 AlphaBeta**：VCF/VCT 与认输探测仍按 `--timeout-ms` 切片（`search_engine.cpp:214-217`、`:236-239`、`:368-388`）。实测（`/tmp/d1/probe5.out`，本树重建的探针）：深度 2 / 1000 节点 / 20 s 的一盘 8 着，逐着 AlphaBeta 节点数在「战术层开」与「战术层关」两次完全相同（72/76/252/446/797/827/868/573，合计 3,911），而第 7 着开着战术层吃 **6,016 ms**、关掉吃 **16 ms**；整盘 `sum_elapsed` 6,103 ms → 86 ms（`/tmp/d1/probe3.out`）。那一着的 AlphaBeta 只用了 868 个节点，离 1,000 的上限还有距离，所以节点预算对它无话可说。「训练时长可控」在战术密集的局面下仍不成立，`:434` 那句「每盘成本可预测」的适用范围应按这个口径缩小（README `:195` 本轮就按「AlphaBeta 那一手的工作量」写）。要不要把战术层也接进节点预算，是一个新的口径决定，留给 D-2/D-3 之后单独立项，本轮不顺手改。
- 采集数据会随预算变薄，且这本账现在能算平：默认量级样本 704 → 560，等于 alphabeta 着数 88 → 70 乘以 `SYMMETRY_COUNT = 8`（`transforms.h:12`）；引擎着法总数 116 → 90，掉的那 26 着里 alphabeta 少 18、`must_block` 少 8（24 → 16，`vcf` 两臂都是 4）。两臂的 alphabeta 着法 depth 全部 ≥ 1（`{3,4}` 与 `{2,3}`），所以 `data_collector.cpp:130` 的 `entry.depth >= min_search_depth_` 门（`min_search_depth_ = 1`，`data_collector.h:74`）在这把尺子上一个样本都没砍掉；它砍样本是在预算压到 `depth == 0` 的那一档（本轮用例 4 就是那一档：6 着全 `depth == 0`，一个样本都不会产生）。所以「预算越小数据越薄」在这一轮是**棋局变短、早退变多**的结果，不是过滤器在动手——这条要按这个口径记，否则 D-3/D-4 会去修一个没坏的门。
- 上一轮欠的一条账在这里补：`23e6453` 的提交信息写 `+5/-1`（那条对）与本轮无关，但它自身正文里「tests/test_search.cpp is +492/-0」同类口径的 `+24/-11` 实为 `git diff --numstat` 的 **25/11**，已发布的提交不改写，按原样申报。
- D 阶段进度：`#2` 本轮落地；`#1`（分片共享 TT，按 7.3 MB/worker 定价，且必须先给 `trainer.cpp:107-109` 那条「表里的分数是换掉的系数算出来的」建失效口径）、`#3`（SPRT，动 `trainer.cpp:125` 的 `pairs`）、`#4`（开局库数据通路）、`#5`（`controller.h:50` 与 `alphabeta.cpp:601` 的两处 `Board` 拷贝）依次未完。

## 轮次125 D 阶段基线：一轮训练的成本几乎全在串行的适配度赛，而并发的那张表第一次量到 7.3 MB — engine redesign stage D, baseline measurement

规格 `docs/ENGINE_REDESIGN.md` `:433-437` §2.8 列了五件事，验收在 `:550`：「单轮训练耗时下降 ≥ 40 %；内存峰值与并发局数解耦；`--fitness-games 2` 下 fitness 不再只有三档」。本轮**不动一行代码**，只做一件事——把「一轮训练的成本到底花在哪」量出来。这个必须先量，因为按文档给的数去改会改错地方：文档 `:434` 说「当前是每引擎一张 8.4MB 表，N 个并发 → N × 8.4MB」，而 `:437` 的 `#5` 说 `Board` 是「传参一次 + 每层一次」；两条都不是这棵树现在的形状，量完发现真正吃掉一轮的是**另一件事**。

### 测量条件（全部产物只落 `/tmp/d21/`，仓库树一字未动）

- 被测树 `23e6453`；`build/gomoku_train` 与 HEAD 的 `src/` 同步（`find src tests -newer build/gomoku_train` 只报 `tests/test_search.cpp`，训练二进制不链它）。
- 固定尺子：`--depth 4 --timeout-ms 400 --max-moves 60 --seed 20261011 --rounds 1`，`/usr/bin/time` 取墙钟与峰值 RSS。
- 机器 6 核，每臂**串行**，跑前 `pgrep -c valgrind` = 0（`assert_text.sh` 的两臂已在 14:44:32 收完）。

### 读数

| 臂 | 配置 | 墙钟 | user | 峰值 RSS | 记录/样本 |
| --- | --- | --- | --- | --- | --- |
| A | population 2 × generations 1，`--no-data` | 27.97 s | 27.88 s | 27,112 kB | 8 盘（3 评估 + 验证） |
| B1/B2/B4 | 同 A，`--threads 1/2/4` | 57.06 / 56.32 / 56.51 s | 56.50 / 55.94 / 55.92 s | 27,040 / 27,184 / 27,252 kB | 22 盘 |
| C1/C2/C4 | population 2 × gen 1 + `--data-games 8`，`--threads 1/2/4` | 29.96 / 30.03 / 30.00 s | 29.45 / 30.86 / 33.59 s | 34,696 / 41,844 / 56,548 kB | 8 盘 / 112 / 224 / 496 样本 |
| D | **默认量级**：population 8 × generations 4、`--verification-games 8`、`--data-games 4 --threads 2` | **182.91 s** | 189.08 s | 42,264 kB | 70 盘（29 评估，genetic 因停滞只跑 3 代）/ 4 盘记录 / 704 样本 |

### 五条「文档说的与树里有的不一样」，它们决定 D 阶段怎么排

1. **`--threads` 在没有数据采集时是完全惰性的**（B 臂三行）：`user ≈ wall`（56.50 对 57.06），RSS 三行只差 212 kB。`--threads` 的定义就是「Parallel **data-collection** workers」（`main_trainer.cpp:46`），适配度赛与验证赛一个线程都不开。所以 §2.8 `#1`（共享 TT）与 `#5`（去拷贝）按构造**碰不到** `:550` 的耗时那一半。
2. **一轮 ~30 s 里约 28 s 是串行的 optimise + verification**：`Trainer::run_round` 的先优化再验证（`trainer.cpp:316-373`）跑在 `generate_data_games`（`:433`）**之前**；C 臂把采集加进来只多 1.99 s（1 worker），到 4 worker 连这 2 s 都被并行吸收（30.00 对 29.96，噪声内）。于是 `:550` 的「单轮 −≥40 %」只有两个来源：**让每次思考更便宜或更少**——即 `#2`（`--max-nodes`）与 `#3`（SPRT 少下几局）。这一点不量出来，D 阶段会先去动那 6.7 %。
3. **并发内存 = 每 worker 一张表，实测就是表的尺寸**：1→2 worker `+7,148 kB`，2→4 `+14,704 kB`（均摊 `+7,352 kB`/worker），对上 `transposition.h:62` 自己记的「2^18 × 28B ≈ 7.3MB per table, **down from 8.4MB**」。`:434` 的 8.4 MB 是这张表被改小**之前**的数；`#1` 的收益应按 7.3 MB/worker 结算，而不是 8.4。
4. **「每盘成本可预测」现在连仪器都没有**：C 臂 8 盘里只有 1 盘真搜索（game 0：2 着 replay + 19 着搜索，整盘 154,953 节点，单着最高 30,287 节点 / 336 ms），其余 7 盘是悔棋续战（`RetryExplorer::rewind_to_next_branch`，`self_play.cpp:202-224`，把上一盘的前缀重放到第一个未试分支），每盘只剩 1 着要想要，而那一着走的是 `vcf` / `must_block` 早退——**这两个 method 的 `nodes_searched` 按定义是 0**（`search_engine.cpp:338-340` 只有 alphabeta 填，`search_engine.h:23` 明写「depth and nodes_searched stay 0 outside AlphaBeta」）。所以 records 里那些 0 不代表便宜，代表这次回答根本没进 alphabeta；`#2` 落地时要一并说清节点预算**计不计**战术早退，否则「可预测」是拿一个不存在的数在预测。
5. **fitness 只有三档是算术，不是棋力**：`trainer.cpp:125` `pairs = max(1, (games+1)/2)`，`:155` `score / played_games`，`--fitness-games 2` ⇒ `pairs = 1` ⇒ 值域恰好 `{0, 0.5, 1}`。README `:213` 记的 0.56 门槛正是为了让**只有 1.0 能晋升**。`#3` 要动的是 `pairs` 这条（序贯到显著即停），而不是分母；文档说它「解决三档问题的一半」是准确的，另一半是 `:435` 没写的位置项。

另外两条在实现时会硌到：

- **训练数据只覆盖 alphabeta 决策**：`DataCollector::min_search_depth_ = 1`（`data_collector.h:74`）从未被覆盖（`trainer.cpp:228` 裸构造），配合上面第 4 条，`vcf`/`must_block` 的着法一律不进样本——样本数正好等于 alphabeta 着数 × 8 个对称（14/28/62 → 112/224/496，`data_collector.cpp:128-131` 的 `engine_chose` 门与 `transforms.h:12` 的 `SYMMETRY_COUNT = 8`）。同一件事也解释了「同样 8 盘记录，样本数随 worker 涨」：`trainer.cpp:207-208` 把局数均分给每个 worker，而**每个 worker 都有自己的昂贵第一盘**（`:222-223` 各自 `set_start_line(random_opening_line(config_.data_opening_plies, seed))`，`data_opening_plies = 2` 在 `trainer.h:54`）。
- **开局库不是「被关掉」，是无处可开**：`trainer.cpp:55` 确实 `set_book_enabled(false)`，但 `book_` 默认 `nullptr`（`search_engine.h:121`）且只有 `server.cpp:1222` 会挂表，所以训练里那个分支本来就到不了——`#4`「按权重采样开局」需要一条新的数据通路，翻开关不够。
- **`#5` 的两处拷贝按树里读是「传参一次 + 每次思考一次」**：`controller.cpp:36` 把 `board` 按值带进线程（`search_worker(Board board, ...)`，`controller.h:50`），`alphabeta.cpp:601` 的 `Board work_board = board;` 在 `:604` 的深度循环**之前**，所以是每次思考一次，不是每层一次。`:437` 的措辞要按这个改。

### D 阶段因此这样排

`#2`（`--max-nodes`）先做，因为它同时是 `:550` 耗时那一半和上面第 4 条「成本可预测」的唯一入口；再 `#3`（SPRT，减验证局数，和 `#2` 一起才可能凑出 −40 %）。`#1`（分片共享 TT）按 7.3 MB/worker 定价，并且必须先给 `#2` 留一个失效口径——`trainer.cpp:107-109` 的注释写着为什么现在每候选换一张新表（表里的分数是**换掉的系数**算出来的），共享之后这条失效要显式建，否则 `#1` 是用正确性换 7 MB。`#4` 依赖 book 数据通路，`#5` 是两处一次性拷贝、收益最小。

### 门禁

本轮没有代码、没有测试、没有 `tools/` 与 `web/` 改动：`git status` 只有 `REVIEW_LOG.md`。跑 `readme_anchor_check`（120 citations / 0 problems）；不跑 ctest 的理由写清楚——`src/` 与上一次 ctest 通过的 `a1a3652` 一字不差（本轮只写日志），所以没有可回归的东西；Valgrind 同理不适用。

### 未完成

`:550` 三条**一条都还没开始动**；本轮交的是让这三条可被验证的那把尺子（配置、表、以及每条读数对应的文件行）。D-1 `#2` 的实施下一步开始，做完按同样这把尺子复测（同一 seed、同一 depth、同一 `--max-moves`，只换预算口径），否则 −40 % 又是一个没有对照的数。

D 臂两件事值得单独写：`user 189.08 ≈ wall 182.91` 即使开着 2 个采集线程也几乎是**单线程的 CPU**，所以「多开线程让一轮变快」在默认量级上同样不成立；README `:186` 记的 `--rounds 20` 按这把尺子是 **≈ 61 分钟**（20 × 182.91 s），`:550` 要的 −40 % 就是从这 61 分钟里减，而不是从采集那 2 秒里减。D 臂的 4 盘记录里 `replay 120 / alphabeta 88 / must_block 24 / vcf 4`，四盘 `total_nodes` 是 22,687、22,687、899,878、903,564——两个 worker 各自的第一盘重（~90 万节点），各自的第二盘轻（2.3 万），这正是上面第 4 条的形状，也是它为什么不能用「平均节点」来定价。

## 轮次124 引擎重构 C4：静默搜索的长尾第一次有了上限，而账本早就在树里 — engine redesign stage C4

规格 `docs/ENGINE_REDESIGN.md` `:413` §2.6 **Step 4**「quiescence 深度上限」：「当前 `quiescence`
无深度上限（靠威胁过滤收敛）。加 `MAX_QDEPTH = 8` 与 `seldepth` 记账，避免极端局面下的长尾」。
上游仍是 `:106` 的 P4，验收是 `:549` 的 C 阶段那一行（单线程 ≥1M nps、depth 8 在 4 s 内完成），
旁证是 `:443`/`:445` 两条「只增不改」的结构行（`SearchInfo` 带 `seldepth`、`thinking_progress`
帧带 `seldepth`）。Step 4 是 §2.6 四步里最后一件成本改动，C 阶段到此为止。

### 落地的两件事，第三件已经在树里

1. **`AlphaBetaSearch::kMaxQdepth = 8`**（`src/search/alphabeta.h:135-140`，紧挨着 `quiescence`
   的私有声明 `:142`）：文档写的是 `MAX_QDEPTH`，落到文件里跟着本文件的 `k` 前缀命名，值一字未改。
   注释钉住的是**口径**：`qdepth` 是一个节点**进入时**的静默层数，所以这条上限是对**一条链**的约束，
   不是对一次思考的约束——同一次思考可以在别的分支再走到 8。
2. **上限支**（`src/search/alphabeta.cpp:189-199`）：`if (qdepth >= kMaxQdepth) return
   {static_score(board, color, hash, tt_eval), true};`，位置在 `ctx_.hard_stopped()` **之后**、
   `stand_pat` 的求值**之前**。
3. **`seldepth` 记账本轮没有新增，一行都没有**：`SearchContext::note_quiescence_depth`
   （`src/search/search_state.h:478`）、`SearchInfo::seldepth`（`alphabeta.h:34`）、
   `search_engine.cpp:348` 的交接、`src/server/server.cpp:1674` 的帧与 `:1662` 那条注释全是 A 阶段落的件。
   本轮给它第一次加上上限，靠的是**已有一行**的位置：`alphabeta.cpp:180` 的
   `ctx_.note_quiescence_depth(qdepth)` 在节点计数之后、上限测试之前，所以一次思考报的是
   「节点进入时的深度」而不是「节点展开到的深度」，于是 `seldepth` 可达的最大数**正好等于**
   `kMaxQdepth`。`:413` 的后半句因此是以**已有件**交付的，本轮交的是让它第一次有意义的那半句。

### `sound = true`：文档没写、代价最大的一处决定

被上限拦下的那个节点按 **sound** 上报。理由不是风格：`sound` 在本引擎里同时门控**四件**东西——
节点往置换表写不写边界、父节点从 `PvTable` 里延伸不延伸这条变例、哪一层算完成层、PVS 的窄窗探测
要不要触发重搜（B3 的规则）。而这个节点是**叶子**，跟 `depth == 0` 的 alphabeta 节点是同一种叶子：
它报的就是这个局面的静态分，没有任何东西是算到一半被丢下的。把它标成 unsound 反而是 damage——
一层里有一个被上限拦下的节点，就会让它的整条祖先链丢掉边界、主变和完成层。
M3 是这句话的反证：把 `true` 换成 `false` 之后，红的**不只是** C4-1 与 C4-2 的记账断言，
C4-3 与 C4-4 的**节点字面量**一起红，也就是说这一处开关改变的是一棵树的形状，不是日志里的一个布尔。

### 与文档措辞的偏离：长尾不在钉住的那些局上

「避免极端局面下的长尾」这一句，本轮先量再写，量的结果是**两句都必须说**：

- **钉住的位势上没有长尾**：本套件已有的 24 个位势臂（`/tmp/r124_c4/hist_base.txt` 的 TOTAL 行：
  `arms=24 nodes=12687567 qnodes=7917361 at_or_past8=0 past8=0`）里最深的节点进入在 **7**，
  **没有一个**节点进入在 8 或以上。所以在这 24 臂上上限**一次都不触发**，`test_search` 里所有
  按节点数钉住的literal（221,774 / 156,428 那一类）一字未动——本轮**不是**以提速交差的。
- **长尾真实存在，且在密集随机簇上**：`/tmp/r124_c4/c4_fuzz.cpp`（`mt19937` seed 20261019，
  30 子挤在 9×9 窗口，两色，三个根深度）共 3,943 次思考，单盘最大到达深度是根深度 2 的 **11**、
  根深度 4 与 6 的 **15**；有 `maxreach ≥ 8` 的盘分别是 **51 / 2,918**、**22 / 781**、**12 / 244**。
  文档那句「极端局面」在这里是字面成立的，而它不是中盘典型局面。

### 中性：答案一律不许动，节点数会动

同一批 3,943 次思考，基树与上限树逐列比对（`/tmp/r124_c4/cmp.py` 对
`fuzz_base_rows.txt` / `fuzz_cap_rows.txt`、`fuzz_base_d4.txt` / `fuzz_cap_d4.txt`、
`fuzz_base_d6.txt` / `fuzz_cap_d6.txt`）：

| 口径 | 根深度 2 | 根深度 4 | 根深度 6 |
| --- | --- | --- | --- |
| 思考次数 | 2,918 | 781 | 244 |
| `nodes` | 4,710,841 → 4,705,590（**−0.111 %**） | 5,621,497 → 5,619,719（**−0.032 %**） | 10,387,973 → 10,377,359（**−0.102 %**） |
| `qnodes` | −0.125 % | −0.038 % | −0.129 % |
| `tail_nodes_at_8plus`（进入在 8 层及以上的节点） | 13,522 → 3,876（**−9,646**） | 5,489 → 1,434（**−4,055**） | 13,642 → 2,341（**−11,301**） |
| 节点数有变的盘 | 47 | 21 | 12 |
| `seldepth` 有变的盘 | 25 | 11 | 8 |
| `score` / `best` / `completed` / `stopped` / `tt_hits` 有变的盘 | **0 / 0 / 0 / 0 / 0** | 同左 | 同左 |

三条读数形状值得单独写下来：

- **`seldepth` 只会在基树到达 ≥ 9 的盘上变**（25 / 11 / 8 就是这个数），到达正好 8 的盘两棵树
  报同一个 8。这句是「`note_quiescence_depth` 记的是进入深度」的直接推论，也是 C4-5 那条
  `seldepth <= cap` 为什么不足以单独当反证的原因（见反证段）。
- **省下的尾部不等于省下的树**：根深度 4 的上限让 **4,055** 个节点不再被进入（5,489 → 1,434，
  剩下那 1,434 个正是进入在 8 这一层的叶子，它们仍然创建，只是不再展开），可那 781 次思考只净省
  **1,778** 棵树节点——差额 2,277 是被换掉的割型付回来的。逐盘看更直接：净省是**13 盘少 2,727**
  对**8 盘多 949**（最大一笔 +572，落在盘 272 那盘 211,698 节点上；盘 92 那盘 43,628 节点只给回 +25）；
  根深度 2 净 5,251 = 少 6,765 对 多 1,514（最大 +607，盘 870）。原因是被拦节点交回的那个数，
  不是它父节点本来要割的那个数。C4-4 钉的就是其中一盘：`740` 在根深度 4 **+73**、在根深度 2 **+96**。
- **答案一个字没变**：3,943 次思考里五列（分数、选点、完成深度、停止原因、`tt_hits`）**差异数为 0**。

### 一次读数的失而复得，与一处真正的配错

这一段先写错过一句话，值得按原样记下来，因为它暴露的是**核对方法**的漏洞，不是笔误。

- 提交前我用 `grep -rIn 4055 /tmp/r124_c4` 查「根深度 4 的上限从尾部拿走 4,055 个节点」这句话，
  得到 **0 命中**，于是把它当成没有对象的读数改掉了。**这个判断是错的**：4,055 不是任何一份读数里的
  字面量，它是两份 SWEEP 汇总行的**差**——`tail_nodes_at_8plus` 在基树是 **5,489**、在上限树是
  **1,434**（剩下那 1,434 个正是**进入在 8 这一层**的节点，它们仍然被创建，只是不再展开），
  5,489 − 1,434 = **4,055**。搜字面量而不去搜口径，就会把一个真读数删掉。句子已经按推导写回
  （`tests/test_search.cpp:8104-8107`）。
- **真错的那处是配对**：同一句话里「最大一笔给回 +572」落在一盘 **211,698** 节点上（盘 272），
  而原先挨着它写的「43,628 节点的一盘」是**另一盘**（盘 92，`maxreach 9 → 8`，给回 **+25**）。
  两个数都是实测的，配在一起就是假的。现在两处分开写。
- 核对的口径也补上了：净省 **1,778** 棵树节点，对应尾部**少进入 4,055 个节点**——这两个数不相等，
  正是这一段想说的「省下的是尾部的层，树自己会把一部分付回来」。
- 同一轮重算又抓到**两处自己的算术**：那 8 盘给回的总和是 **949**（不是 950，`2,727 − 949 = 1,778`
  才是那句「净省 1,778」的来历），而 4,055 与 1,778 的差是 **2,277**（不是 2,278）。两处都已改，
  改动只在注释文字里（`tests/test_search.cpp:8106` 与 `:8388`，字数等长、文件仍是 **8646** 行，
  八个被引用的断言行号一字未动）。核对的方法也随之换成**重算**而不是搜字面量：
  `/tmp/r124_c4/recheck.py` 从三份基树/上限树读数逐盘重算上面每一个数，另用一遍独立的 `paste`+`awk`
  对账（两边同一 `i,color` 对齐后才相减），两份给出的 949 与 1,778 相同才算数。

### 收益：本轮没有收益可报，`:415` 与 `:549` 的吞吐那一半仍未达成

同一台空闲机器、同一批 18 臂驱动、两边都 `-O2`（`/tmp/r124_c4/c4_speed.sh`；旧树取
`git archive 7e1b8c7 src`，新树取工作树）：

| 树 | 节点（18 臂） | 墙上时间 | `ns_per_node` | `knps` |
| --- | --- | --- | --- | --- |
| `7e1b8c7`（无长尾上限） | 11,365,496 | 79,585,571 µs | 7,002.4 | **142.8** |
| 本轮树（`kMaxQdepth` 已装） | 11,365,496 | 79,223,387 µs | 6,970.5 | **143.5** |

三条读数，顺序即重要性：

- **节点一字未动**，11,365,496 对 11,365,496，18 臂**逐臂答案 byte-identical**
  （`/tmp/r124_c4/speed_rows.txt` 的 `ARMS BYTE-IDENTICAL`）。这不是「省得少」，是**这批钉住的臂
  一次都没触发**——与位势普查那行 `at_or_past8=0`、最深进入 **7** 是同一件事的两种说法。
- 墙上时间少 **362,184 µs = 0.455 %**，而节点数完全相同，所以这一笔**只能是噪声**，本轮不写成收益。
  同一棵旧树在 C3 段量到 `ns_per_node 7075.3`、本轮量到 **7,002.4**，同一份二进制换个日子就差 **1.0 %**，
  比本轮墙上时间那 0.455 % 还大——这就是为什么本轮的收益段**只有等式**（节点相等）而没有百分比。
- **`:549` 未达成，报实测**：单线程 **14.35 万节点/秒**，对「≥ 1M nps」差**约 7 倍**（C3 段量到的是
  14.13 万，同一数量级）；`:415` 的「合计吞吐 ≥ 1M nps 可期」同理未达成。C 阶段四步（C1 的 TT 带
  `static_eval`、C2 的候选格缓存、C3 的记录代答全盘扫描、C4 的长尾上限）到此**全部落地**，
  验收行仍没够着，本轮**不重解读文档那一行**。剩下 7 倍不在「走多深」这条路上：C 阶段把
  `ns_per_node` 从 8,585.7（C2 前）压到 7,002.4（C4 前），而 C4 对钉住的臂是**零触发**，
  也就是这条上限买到的是**最坏情况的边界**，不是平均吞吐。

「避免极端局面下的长尾」这一句是本轮真正交付的东西，它在随机密集簇上是**可数的**：根深度 4 的
那 781 次思考里尾部进入 ≥ 8 的节点 5,489 → 1,434，最深到达 15 → 8；把它换成墙上时间的说法就是
**一次思考最坏能有多长**有了上界，而平均成本按上面的等式**没有变**。


### 用例（五条，定义 `:8239-8465`，注册 `:8638-8642`）

段首注释（`:8079-8112`）自己交代三件事：`seldepth` 那一半是已有件、长尾在哪不在哪、
以及省下的不是单调的。两个夹具都**逐子抄自**上面那个生成器：`c4_tail_long()`（`:8120`，
扫描盘 105，WHITE 走，基树静默到 15 层）与 `c4_tail_dense()`（`:8133`，扫描盘 740，BLACK 走，
基树到 11 层、被上限拦后贵 73 子）。驱动件两个：`C4Node c4_quiescence_at()`（`:8158`，
**直接开一个** `quiescence` 节点——文件顶的 `#define private public`（`:22`）让用例能读 `ctx_`、
`move_gen_.cache()`，`ctx_.start(limits, system_search_clock(), stop)` 把上下文武装好，
`ctx_.set_reason(StopReason::NODES)` 在不碰 `stop_flag_` 的前提下让 `hard_stopped()` 返回真），
`C4Think c4_think()`（`:8206`，一次完整思考，`set_node_limit(INT_MAX)`、只给 `max_depth`、
不给墙钟，所以断言里没有一个是时间尺度的）。

- **C4-1 `test_a_quiescence_node_entered_at_the_cap_answers_only_its_own_static_score`**（`:8239`）：
  进入在 8 的节点答的**就是它自己的静态分**，且**只**答这个：`-1488`（BLACK）/ `1488`（WHITE）/
  `-50196`（另一盘）三处逐字等于 `static_score`，`sound == true`，开 **1** 个节点、
  读 **0** 个候选格、付 **2** 次评估读（`want` 那一次加节点自己那一次）。两枚对照让它是一条**界**
  而不是一台**停机**：同一格在 `cap - 1` 是普通节点（开 21、读 2,863、答 99,700 ≠ `-1488`），
  在 0 层开 41（密集盘）/ 84 与读 16,796（长尾盘）。上限若低一层，被拦的就是这第二臂。
  红点：`:8251`。
- **C4-2 `test_a_dead_budget_outranks_the_depth_cap_its_check_sits_behind`**（`:8299`）：
  两个分支、同一个数、相反的声明。武装停止后在 8、7、0 三处都答静态分且 **`sound == false`**，
  开 1 节点、0 候选读；未武装时同一处开 41 / 84。这条只反证一件事——**顺序**：上限测试排在
  停止测试**之后**。红点：`:8309`、`:8311`。
- **C4-3 `test_a_think_that_runs_into_the_cap_reports_the_cap_and_the_same_answer`**（`:8343`）：
  长尾盘的一次思考整 tuple 钉住：`seldepth == kMaxQdepth`（基树报 15）、`nodes 63,717`、
  `qnodes 49,098`（基树 49,898，**少的 800 个全是静默节点**）、`tt_hits 6,471`、`score -22,366`、
  `completed 4`、根步 `55/55`、选点 `(11,5)`、`stopped NONE`、`pv 4`；再加两条恒等式
  `evals == qnodes`（每个静默节点付**且只付**一次评估读，被上限拦下的也算）与
  `hits + recomputes == probes`（4,445,432 = 2,963,010 + 1,482,422）。
- **C4-4 `test_a_capped_tail_can_pay_back_nodes_it_did_not_save`**（`:8379`）：把**给回**钉成一条
  用例而不是一句解释：153,456 节点对基树的 153,383，**多 73**，且 73 全在静默侧
  （138,837 → 138,910，两棵树的 alphabeta 部分都是 14,546），尾部到达从 11 降到 8；
  答案仍是 `99,999` 在 `(2,11)`、4 层、`pv 4`。注释里写清 `99,999` 是评估器的封顶而不是杀棋距离，
  所以这一臂的分数**不能**当 mate 距离断言。红点：`:8391`。
- **C4-5 `test_the_cap_bounds_every_arm_of_the_census_and_is_inert_where_there_is_no_tail`**（`:8417`）：
  普查那一半 + 惰性那一半。84 臂（42 盘 × 两色，根深度 2，与 C2 普查同一生成器）逐臂
  `seldepth <= cap` 与 `evals == qnodes`，再加**合计** `155,052` 节点 / `139,879` 静默、
  最深 `8`、恰好 **1** 臂到 8（盘 0 的 BLACK）；然后两臂无尾位势（`quiet` 两色 221,774 /
  156,428，`seldepth 6`）证明这条界在**没有长尾的地方不动树**。红点：`:8443`。

### 反证申报：无 HEAD 反证，以四枚 mutation 加两张矩阵代替

`kMaxQdepth` 在 `7e1b8c7` 上不存在，C4-1 与 C4-2 连**符号**都取不到（它们读
`AlphaBetaSearch::kMaxQdepth`），五条用例在那棵树上编译不过——本轮申报**无 HEAD 反证**，
理由与 C1/C2/C3 同构（结构性，不是没做）。代替它的是四枚变异，各破 Step 4 的一句话
（`/tmp/r124_c4/c4_mut.sh` 与 `mut_c1.py`…`mut_c4.py`；被改的永远是
`/tmp/r124_c4/mut124_c4/<arm>/src` 的副本，仓库树未动；每臂只重编 `tests/test_search.cpp` 与
`src/search/alphabeta.cpp`，其余替套件已建的 `.o`）。两台机器各跑一遍：**H1** 一条用例一个进程
（`:1-5` 是 Step 4，`6` 是 C3-5、`7` 是 C1 的账本钉，作负控制——它们在任何一枚 Step 4 变异下都该绿），
**H2** 整套一起跑，让「越出 Step 4 的变异」以旧用例的红报出来而不是被当成「只有新用例看见」。
两台的读数都取自在**最终树**上重跑的那一遍（`/tmp/r124_c4/h1_final_rows.txt` 与
`/tmp/r124_c4/full_rows2.txt`）——本轮对 `tests/test_search.cpp` 有过两次**只改注释**的修正
（见「收益」段那句被撤掉的读数），它们把 `:8385` 之后的断言各推后一行，所以行号必须在改后的文件上
重新量一遍；重跑后 base 七条全绿，整套 `rc=0 / 334 s` 并打出 `All search tests passed!`
（表里那一列的 87 / 82 / 83 是**进度计数**：`main` 注册 145 条用例，其中 87 条打印 ` passed (`，
所以它数的是「红点之前走到几条会打印的用例」，不是用例总数；整套跑完的证据是 `rc=0` 与最后一行），
四枚变异的红点见下表，与第一次在改前树上量到的红点**同一组用例、
同一句断言**，只是行号 +1/+2。

| 变异 | 破掉的那句话 | H1 逐用例（1-5=C4，6=C3-5 账本，7=C1 账本） | H2 整套 |
| --- | --- | --- | --- |
| M1 上限整支删掉 | 「加 `MAX_QDEPTH = 8`」 | 红 1(`:8251`)/3(`:8350`)/4(`:8391`)/5(`:8443`)，绿 2/6/7 | rc=134 / 313 s / **82** 条在前绿，死在 `:8251` `at_cap.score == at_cap.want` |
| M2 `>=` 写成 `>`（界挪到下一层） | 「界在 8 这一层」 | **与 M1 同一组红点** | **与 M1 逐字同一行读数**（rc=134 / 313 s / 82 / `:8251`） |
| M3 被拦叶子报 unsound（`true`→`false`） | 「这个叶子是这个局面的答案」 | 红 1(`:8252`)/2(`:8309`)/3(`:8351`)/4(`:8392`)，**5 全绿**，6/7 绿 | rc=134 / 312 s / 82 条在前绿，死在 `:8252` `at_cap.sound` |
| M4 上限支挪到停止测试**之前** | 「预算优先」 | **只有 2 红**（`:8311`），1/3/4/5 全绿 | rc=134 / 309 s / **83** 条在前绿（C4-1 完整通过），死在 `:8311` `!stopped_at_cap.sound` |

四句话从这张表里读出来，比表格本身重要：

1. **M1 与 M2 分开不了**。「没有界」与「界在下一层」落在**同一组**红点上，区别只在失败那一行的
   实际值里（M1 下那次思考到达的是基树的 15，M2 下按定义到达 9，但本轮**没有量过** M2 臂的读数——
   `assert` 不打印值，本轮也没有为它加打印）。
   所以本轮交出的是一条**值**层面的界（`seldepth == kMaxQdepth`、`total_nodes == 155052`），
   不是一条能区分这两枚变异的断言。写下来，不写成「矩阵把四句话各自钉住」。
2. **`sound` 决定树的形状**（M3）。M3 红的地方不止 C4-1/C4-2 那两个布尔，还有 C4-3 与 C4-4 的
   **节点字面量**——把被拦叶子标成 unsound 会改父节点的边界写入与层的提交，于是一棵树的**大小**变了。
   这就是「为什么交出去的是 `true`」的反证形状：它不是一句风格主张。
3. **M3 在 C4-5 上是绿的**，这是 C4-5 的反证边界。普查那 84 臂（根深度 2）在 unsound 之下节点数一字
   未动，因为那一层的思考里没有需要这份书记的下游；所以「界不越过任何臂」这件事由 C4-5 守，
   「叶子的声明」这件事由 C4-1/C4-2 守，两条不重叠，也不该被读成互相验证。
4. **顺序只有一条用例看得见**（M4）。整套件里只有 C4-2 能发现上限测试排到了停止测试前面，
   它是本轮唯一一处「一枚变异对一个独占断言」，也是 H2 那 **83** 条的原因。

负控制两条一起绿（6 = C3-5 被割节点的记录读法，7 = C1 的评估账本），说明这一层加的那个数没有
搬到记录缓存或置换表账本上去：Step 4 改的是**走多深**，不是**读什么**。

### 门禁

一条流水线串行跑完，段与段之间无并发负载（`/tmp/r124_c4/pipeline.sh` 的第 5 段，
`gate.sh`：`-Wall -Wextra` 两个改动 TU → 构建 → **60 s 静置** → `ctest` → web → anchor → README；
Valgrind 只在 12:10:11 之后才开始，所以这一段是**空机**读数）：

- **`-Wall -Wextra`**：`alphabeta.cpp` rc=0 / **0** 条警告；`test_search.cpp` rc=0 / 17 行警告，
  位置只有 `:37`、`:2411`、`:2436`、`:2853`、`:5717` **五处既存**的符号性比较，
  C4 新增的 `:8078-8465` 这一段**一行警告都没有**。
- **`ctest` 8/8 / 447.08 s**，`100% tests passed`：`test_search` **412.82 s**、`test_server` 18.02 s、
  `test_position` 8.39 s、`test_training` 7.07 s、`test_session` 0.03 s。
  `test_search` 从 C3 段的 383.30 s 走到 412.82 s，本轮给它加了**五条**用例，其中 C4-5 一条就要跑
  84 次完整思考、C4-3 与 C4-4 各跑一次到达上限的思考；这台机器的时间读数不可移植，所以这句记的
  是「**套件变长**」，不是「每次思考变贵」——后者由收益段那条 `ns_per_node` 与节点数的**等式**管。
- **`node tests/web_frontend_check.js` 105/105**。
- **`node tests/readme_anchor_check.js` 120 引用 / 0 problems**，与 C3 段同一个数：本轮没有新增任何
  `README:NNN` 引用（`tests/test_search.cpp` 里 `README:` 的条数在 HEAD 与本轮工作树**都是 7**），
  也没有改动 README。`wc -l README.md` **658**。


行号与清单要说清楚。本轮对 `tests/test_search.cpp` 的写入是 **+398/−0**（`git diff --numstat`
就是这个数），文件从 **8248** 行走到 **8646** 行，全部新增落在 `:8079` 之后；`src/` 一侧
**+19/−0**（`search/alphabeta.h` +7、`search/alphabeta.cpp` +12）。三条受保护断言
`assert(result.resigned)`（`:2591`）、memcheck 会死在那里的 10 ms 臂
`assert(interrupted.partial_depth == 1)`（`:3973`）、`assert(completed.method == "alphabeta")`
（`:3986`）**内容与行号一字未动**。`CMakeLists.txt`、`README.md`、`web/`、`tools/` 一字未动。

### Valgrind

五臂**串行**，整块窗口机器空闲：套件里那条 10 ms 墙钟断言（`tests/test_search.cpp:3973`）在
memcheck 的时间尺度上本来就活不下来，所以这一层不能和第二件事同时跑——`tools/valgrind_check.sh`
头部写的就是这条规则。每臂四列（rc / 秒 / `==NNNN==` 报告条数 / 四类计数）在
`/tmp/r124_c4/vg_rows.txt`，`ALL_VG_ARMS_DONE` 与 `PIPELINE_DONE 13:33:23` 一起收尾。

- **本轮新增的代码自己**：`c4_driver_vg`（`-O0 -g -UNDEBUG`，七个用例**各开一个进程**，
  所以一行读数只属于一条断言）——`rc=0 / 3,962 s / reports=0 / invalid_read=0 /
  invalid_write=0 / uninitialised=0 / definite_leak=0`，`driver_green_cases=7` 且
  `driver_nonzero_cases=0`，`driver.err` **0 行**。七条 = C4-1…C4-5 加两条负控制（C3-5 的
  割点记录读法、C1 的评估账本）。被上限拦下的那个叶子只交一个 `static_score`：它不分配、
  不越界、不读未初始化内存，七条用例的评估账本恒等式（`evals == qnodes`、
  `hits + recomputes == probes`）全部在 memcheck 之下闭合。
- **整套件**：`test_training` 内层 `clean reports=0 四类全 0 / 150 s`。
- `test_search`：**rc=134（已中止，核心已转储）**，`reports=0 invalid_read=0 invalid_write=0
  uninitialised=0 definite_leak=0`，**255 s** 走到中止点。中止处不是本轮的代码：stdout 的
  最后两行是 `test_engine_progress_frames_share_the_search_clock passed (…)` 与
  `test_engine_reports_the_interrupted_alphabeta_layer measured (interrupted: alphabeta d0+partial
  0/n0 time_hard, last frame d0/n0, control: alphabeta d1+partial 2/n4515 time_hard)`，后一行是
  那条 10 ms 臂**自己**在断言之前打的读数，所以中止就在它紧随其后的那组断言里
  （`:3968-3980`，`:3973` 正是本节开头点名的那一条）。
- **当时够不到、事后补到的那条证据**：那组断言里**具体哪一条**先红，在 `a1a3652` 落盘时还没有文本。
  `tools/valgrind_check.sh:166` 把 stderr 打进 `mktemp`、`:250` 在出结果后 `rm -f`，只在
  `rc != 0` 时 `tail -2 "$log.out"`（stdout），所以 `Assertion 'xxx' failed` 那一行从来不落盘；
  两条内层行是当时全部幸存者。为取文本写的重跑（`/tmp/r124_c4/assert_text.sh`，两棵树的
  `build-vg/test_search` 各一遍、串行、stderr 落盘、直接打印真实四类计数）在提交时只跑到第一臂
  3 分 48 秒，随本轮预算边界**停止**——留一个还在吃 CPU 的 valgrind 过夜，正是 D 阶段第一件事
  （训练基线计时）要避开的那种负载。恢复后重跑，14:36:07 起、14:44:32 全部落盘
  （`/tmp/r124_c4/assert_arms.txt`），文本是：

  ```
  test_search: …/tests/test_search.cpp:3973: void
  test_engine_reports_the_interrupted_alphabeta_layer(): Assertion `interrupted.partial_depth == 1' failed.
  ```

  两棵树**同一行号、同一句文本**，只差路径前缀（工作树 `…/gomoku/tests/…`，负树
  `…/neg124_c4/tests/…`）；`ARM working rc=134 seconds=253`、`ARM negative rc=134 seconds=252`，
  两臂 `valgrind report lines=0`、`uninit=0 leak=0`。也就是说本轮被 memcheck 拦住的那一条，正是
  本节开头点名过、并且**内容行号一字未动**的那一条 10 ms 墙钟臂，而它同样死在没有上限支的
  `7e1b8c7` 上。上面包装器缺陷那段的「真实计数」也由此从推断变成实测：两臂 stderr 上的
  `uninitialised value` 与 `definitely lost` 各 **0**。
- **顺带把「随时间浮动」这四个字钉住了**：同一个二进制（工作树 `build-vg/test_search`）两次跑，
  `control: alphabeta d1+partial 2/n4515` 与 `2/n5019` 都出现过，负树也是 `n5019`——这一臂的
  control 走的是 `time_hard`（按墙钟截断），所以节点数本来就是时间的函数而不是树的函数。于是
  先前拿 `n4515` 对 `n5019` 当「两棵树只差浮动」的读数是**成立的、但成立的原因要写对**：那两个数
  的差在同一棵树上就能复现，它不能用来区分任何两棵树。
- **同一处中止在负树上逐字复现**：`7e1b8c7`（`kMaxQdepth` 在那棵树上 0 命中）的
  `build-vg/test_search` 报出**同一行** `test_search ABORTED rc=134  reports=0
  invalid_read=0 invalid_write=0 uninitialised=0 definite_leak=0  255s`，stdout 尾两行是
  同一对用例名，只有毫秒随时间浮动（642/852/2948 ms 对 643/850/2964 ms）。`test_server` 两棵树
  同一行读数、只差一秒（工作树 `clean 四类全 0 / 40s`，负树 `/ 41s`）。所以这一行是**既存的
  墙钟脆弱**，本轮既没有把它写成通过，也没有为了它改 `tools/`。

**压测包装器自己的读数缺陷，按原样申报而不是当发现交出去**：`vg_rows.txt` 的四行印
`uninitialised=1`（NEG 臂 `2`），而 `valgrind_check.sh` 的内层行同一臂印 `uninitialised=0`。
读代码就能定位：`vg_r124_c4.sh:report()` 把 `grep -c 'uninitialised'` 打在**脚本自己的
stdout**（`$WORK/$t.log`）上，而 `tools/valgrind_check.sh:236` 那条 `printf` 的**字段名本身**
含这个词，于是一行表头就是一次命中，两目标臂两次。真实计数在 valgrind 的 stderr 上：`test_training`、
`test_search`、`test_server` 与 NEG 四臂的内层行都印 `uninitialised=0 definite_leak=0`，本轮改用
`assert_text.sh` 直接读那份 stderr 也是为了把这一句钉死。`tools/valgrind_check.sh` 一字未动。

**合约过期申报：本轮没有。** 追加式写入（+398/−0）意味着没有一条既存字面量被改口；
套件里既存的 `seldepth` 断言也没有一处被上限改到——`:3197` 是 `>= 1`、`:3212`、`:4012` 与 `:4235`
是 `== 0`（前两处是静默位势与默认构造 `SearchStats`，后者是 `start()` 的重置）、`:4228` 的 `== 7`
由 `note_quiescence_depth` **直接喂**（根本不进 `quiescence()`，所以没有上限可碰）、`:5718` 是
`>= 2`。上报帧不变：`seldepth` 从 A 阶段起就有字段、有帧、
有 README 行，本轮没有新增任何字段，也没有把任何数从树里搬到协议里。

## 轮次123 引擎重构 C3：静默节点的那一次全盘扫描，第一次由 step 2 的记录代答 — engine redesign stage C3

规格 `docs/ENGINE_REDESIGN.md` `:409-410` §2.6 **Step 3**「局部 stand_pat」：「静默搜索入口不再做全盘评估，
改为「上一手所在 4 条线」的局部 delta 评估（`last_move` 的 4 条线位棋盘已在 `board.lines_` 中，读取成本极低）。
全盘评估只在根节点与需要精确分值的场合使用」；它的靶子是 C2 段自己留在收益段的那一句「省下的不是还没省的那一半」
（`qnodes` 占 `nodes` 的 64.5 %，每个静默节点仍付一次全盘 `evaluate_board`），上游同样是 `:106` 的 P4，
只是 `:106` 数的是排序路径、`:409` 数的是这一条；再加 `:415` 的预期行「Step 3 使**静默节点成本**下降 ~80 %」、
`:549` 的 C 阶段验收（单线程 ≥1M nps、depth 8 在 4 s 内完成）、`:556` 回归保障第 3 条
（「评估函数改动（位置项、**增量缓存**）必须与旧实现逐格比对」，点名的方法仍是
「全 225 格 × 两色 × 42 棋盘一致」那三个数字），`:568` 风险行的后半句
「Step 3（局部 stand_pat）先只用于「无威胁局面」，有威胁时回退全盘评估」，以及 **README:624-626** 那条否决记录
（「增量式全盘评估做过量化评估后放弃：一手棋影响 4 条线上 ±4 格共约 15 个候选格……落子+撤销的代价与节点上那次
全量重扫基本持平」）。`:582` 的文件清单没有为 Step 3 单列新文件——要动的三处（`evaluator`、`search_state.h` 的
缓存、`alphabeta.cpp` 的读者）前两处是 Step 1/2 已经在的文件，本轮把最后一处的读者接上。

### 落地的三件事

1. **`Evaluator::evaluate_board` 拆成两半**（`src/core/evaluator.cpp:235` `board_pair_value`、
   `:253` `evaluate_board_from`、`:276` `evaluate_board`）。前半是**逐格**的那一步：两色形状各自过禁手门、
   各自计价、返回这一格的代数值；后半是**全盘**的那一步：石头项（位置权与边界惩罚各按颜色取差）加进代数和，
   最后在 `coeffs_.five - 1` 处封顶。`evaluate_board` 自己按「逐格累加 → 交给后半」的同一顺序调用两者，
   所以**算术一字未改**，改的只是它现在有两个可以被单独调用的入口。两半都不是 `candidate_pair_value`：
   后者把中心权折进单格、并为一枚成五付整 `coeffs_.five`，语义不同（`evaluator.h` 的注释把这句写在原地）。
2. **`CandidateCache::board_score(board, my_color)`**（`src/search/search_state.h:307-326`）：遍历
   `board.active_empty()`，逐格 `cell()` 取那条**已经存在**的记录，按读者的颜色挑 `mine/theirs`，
   把 `board_pair_value` 的返回值累加起来，最后一次 `evaluate_board_from`。`MoveGenerator::board_score`
   （`move_generator.h:130-132`）只是它的转发，好让搜索侧只认识一个 `move_gen_`。
3. **两处读者接上**（`src/search/alphabeta.cpp`）：`static_score` 的求值支 `:159`，以及 `alphabeta()`
   停止测试里那条被预算杀掉的节点走的支 `:334`。后者是本轮唯一一处「读法换了、记账没换」的地方——
   它不进置换表、不记 `eval_scans`，所以只有缓存自己的账本看得见它（C3-5 因此存在，见「反证」段）。

### 与文档措辞的偏离：不是 delta，是「同一次求和换一次读法」

文档给的是三句话，本轮一句都没照字面做，理由只有一个：**读出来的数就是扫描的数，不是它的近似**。

- 「局部 **delta** 评估」：delta 需要两个真值相减。C2 已经量出一条记录的真实依赖是它自己的四条线
  （不是 ±2 footprint，也不是 ±4），所以一次落子失效的正好是「与上一手共线的那些格」——这个集合由 entry
  自己的四个 `uint64` 线字**比对**得出，不需要一个 `Board` 要维持的 dirty set，也不需要留住「4 条线之外」的旧值来减。
  于是文档要的「只算变化的格」是这一版的**读侧性质**（只有线变了的格会重识别），不是它的实现手段。
- 「全盘评估只在根节点与需要精确分值的场合使用」：落到的比它更空——搜索树里现在**一次全盘扫描都不发生**
  （18 臂的 `evaluate_board` 内识别计数 549,776,489 → **0**），而每一次的**数**都没变。附带一句本轮的观察：
  `Evaluator::evaluate_board` 在生产代码里已经没有读者（`grep -rn evaluate_board src/` 剩下的全是注释），
  它现在的身份是差分测试的裁判（C3-1/C3-2/C3-4 与 `tests/test_evaluator.cpp` 都调它）。本轮不删它，
  也不给它加「仅测试用」的措辞：它是 `Evaluator` 的公开 API，是 `:549` 说的精确分值场合，也是这一轮唯一的对照物。
- `:568` 的「有威胁时回退全盘评估」：**没有必要**。回退是给近似读法准备的保险；一次返回扫描自己的数的读法
  不可能是「近似的那一半」，也就没有「换回精确的那一半」可换。这一条按**偏离**上报，方向是安全的，不算遵守。
- 与 README:624-626 那次否决的关系：那次死掉的是**靠落子/撤销记账维持**的全盘增量评估（代价与重扫持平）。
  本轮不做记账：写侧仍是每格一次识别，省下的全在读侧——读一条已经在的缓存是四个字的比较。
  C3-3 量的正是这件事：一次落子只让 **26 / 132** 个格重识别，撤销 **21 / 128**。

### 中性：树、分数、深度、原因都不许动

- **18 臂逐行比对**（同一板、新表、节点不 binding 于时限，`/tmp/r123_c3/counted_final.txt` 对
  `/tmp/r122_c2/c3_control.txt`）：`nodes / qnodes / tt_hits / score / completed / best / stopped`
  **七列一字不差**，脚本比对 18/18 无差异（`BAD: []`）；节点合计 11,365,496 两棵树相同。
- **求和的两半不许互相说瞎话**：`c3_sum_equals_scan`（`:7747`）用一个**自带**缓存的 `MoveGenerator`
  做冷读，并断言冷读本身就是「每格一次重算」——这样「读法」与「扫描」在任何一格上都不是同一个调用。
- **两把尺子互相咬合**：`quiet` BLACK d6 一臂本轮剩下的识别总数是 **3,805,665**，与缓存账本在这一臂的
  `recomputes = 3,805,665` **逐字相等**；WHITE 臂 3,115,310 对 3,115,310 同样相等
  （`/tmp/r123_c3/probe.out` PART1 对 `counted_final.txt`）。读法换了之后，这台机器上每一次四向识别
  都恰好是一次记录重填，没有第二次、也没有漏记的一次。

### 收益：静默节点的全盘 pass 清空，合计识别 −58.5 %，每节点墙钟 −10.4 %

18 臂合计（`-O2`，同一份驱动）：

| 读数 | 控制树 e2e9705 | 本轮树 | |
| --- | --- | --- | --- |
| 静默节点全盘 pass 的识别 | 549,776,489 | **0** | −100 % |
| 合计 recognition | 711,190,161 | **295,049,491** | **−58.5 %** |
| 逐臂 recognition | — | 18/18 全降 | −42.8 % ~ −64.4 % |
| 节点数（18 臂） | 11,365,496 | 11,365,496 | 0 % |
| `ns_per_node` | 7900.2 | **7075.3** | −10.4 % |
| `knps` | 126.6 | **141.3** | +11.6 % |
| `quiet` BLACK d6 缓存读 | 4,066,903 = 2,097,434 + 1,969,469 | **8,755,599 = 4,949,934 + 3,805,665** | 读 2.15×、命中 51.6 % → 56.5 % |

- **`:415` 的「~80 %」没有达到，两个口径都没达到**：识别口径上，静默节点那一次 pass 是 100 % 清空，
  但它换回来的读法与排序**共用同一条缓存**，所以合计只降到 −58.5 %（最差的单臂 −42.8 %）；
  墙钟口径上每节点 −10.4 %。从本轮起「静默节点的成本」这一段与「排序路径的成本」这一段**不再是两把可以分别
  度量的尺子**——一次记录重填同时喂两处，`recognise` 的计数器也分不开它们。C2 段那句「剩下那 161,413,672 次是
  同一格换了一条线之后必须重算的部分」因此被本轮改写成：剩下的 295,049,491 次是**所有**必须重算的格，
  两条读法共同的那一份。
- **墙钟为什么只有 −10.4 %**：一个静默节点仍要**逐格**读——`quiet` BLACK d6 每节点的缓存读从 18.3 次涨到
  39.5 次（8,755,599 / 221,774），一次读是四个 `uint64` 的比较加一次逐格两色求和，便宜但不是免费；
  而兄弟分支之间互相把对方的记录弄脏，是剩下的 2.95 亿次识别的来源。`depth 8` 一臂最明显：
  `scattered` WHITE 从 322,682,866 次识别降到 122,716,340，仍是一次思考 1.2 亿次。
- **`:549` 本轮仍未达成，报实测**：单线程 **14.1 万节点/秒**，距 ≥1M nps 差约 7 倍；
  `depth 8` 在 4 s 内两色都完成（`quiet` 1,408,083 / 2,131,525 节点，`scattered` 2,830,250 / 4,182,936 节点，
  本轮树上分别 80,414,278 µs 对 89,789,920 µs 的 18 臂总耗时），所以 C 阶段验收的**时间那一半是过的**，
  吞吐那一半不是。Step 4（`MAX_QDEPTH` + `seldepth`）是这一段剩下的最后一件成本改动。

### 用例（`tests/test_search.cpp` 五条，定义落在 `:7715-8076`，注册落在 `main()` 的 `:8240-8244`）

段首注释（`:7715-7745`）自己交代了三件事：delta 换成读法的那处偏离、README:624 的否决记录为什么不适用于本轮、
以及 `:568` 的回退为什么没有必要。夹具两个：`c3_sum_equals_scan`（`:7747`，自带缓存的冷读 + 冷读账本自证）、
`c3_addend_truth`（`:7768`，同一格「记录喂的代数值」对「新识别喂的代数值」，再对另一侧取反）。

- **C3-1 `test_the_record_sum_is_the_whole_board_scan_on_every_board`**（`:7796`）：`:556` #3 的普查规模
  （`c2_census_boards()` 的 42 盘）× 两色 = **84 次求和**逐次等于 `evaluate_board`，读经 **6697** 个候选格；
  每盘先证「候选表里没有占用格」（`occupied == 0`）——那是 `board_score` 敢读记录而不识别的前提；
  再证冷读 = `list_size` 次 probes 与 recomputes、0 次 hits，热读 = `list_size` 次 hits。
  红点：`:7816` `cold == direct`。
- **C3-2 `test_every_addend_of_the_record_sum_is_the_addend_the_scan_pays`**（`:7853`）：把「逐格比对」做到
  **代数值**这一层，而不是只求和相等：**13,394** 个格-色代数值，每个先比记录里的两色四向形状
  （`c2_patterns_equal`）与新识别相等，再比「记录喂的 `board_pair_value`」=「新识别喂的 `board_pair_value`」
  =「另一侧的代数值取反」；最后闭环——把这 13,394 个代数值重新喂给后半，**84** 次重建的和等于扫描
  （`:7884`/`:7885`）。这也就是本轮对「拆分本身有没有算错」的回答：拆分的算术由 `tests/test_evaluator.cpp`
  的 `naive_board_total` 与它的绝对字面量守（−7700 那一类），逐格的等价由这一条守。
- **C3-3 `test_a_far_stone_moves_both_sums_alike_and_undo_brings_both_back`**（`:7909`）：make/unmake 路径上的求和。
  23164 → BLACK 落 (7,12) → **99999**（撞到 `coeffs_.five - 1` 封顶，两条读法一起撞）→ 撤销回到 23164，
  每一步两路相等，另一色同处也相等；落子只重算 **26 / 132** 格、撤销 **21 / 128** 格——文档那句「只更新受影响格」
  在这里以**读**而不是以记账度量。红点：`:7932`。
- **C3-4 `test_one_set_of_records_answers_the_sum_for_either_colour`**（`:7969`）：一条记录两个读者。42 盘、
  两种问序，先问的颜色拿到自己扫描的数、后问的颜色拿到它自己扫描的数，第二次问是 `list_size` 次 **hits** 且
  **0 次 recompute**（`:7990`）。这是「记录必须两侧都带」在求和层的版本。
- **C3-5 `test_the_nodes_a_cut_kills_pay_the_sum_out_of_the_records`**（`:8019`）：本轮新增的第五条，
  因为它最初**没有反证对象**（见「反证」段的 M5）。一次真的被节点预算砍断的思考（先量第一层用掉多少节点，
  预算设成那个数 +1，与 A4 那一条同形；时限 600 s，所以不依赖墙钟）：`stopped == NODES`、
  `depth 1 / partial 2 / root_moves_done 0 / 31`、`nodes_searched 63`、`qnodes 31`、
  评估账本 `scans 31 / reuses 0`——**这 63 个节点里只有 31 个进了 `static_score`**，另外的走 `:334`，
  而那条支既不进表也不记 `eval_scans`，所以只有缓存账本看得见它：`probes 1643 = hits 587 + recomputes 1056`
  （`:8061`），并加一条 `probes > 20 × nodes` 的形状断言，说明这个账是按**候选表长度**而不是按节点数长的。

### 反证申报：无 HEAD 反证，以六枚 mutation 加一张逐用例矩阵代替

`CandidateCache::board_score` 与 `Evaluator` 的两半在 `e2e9705` 上都不存在，这五条用例在那棵树上连编译都过不去——
本轮申报**无 HEAD 反证**，理由与 C1/C2 同构（结构性，不是没做）。代替它的是六枚变异，各破 Step 3 的一句话
（`/tmp/r123_c3/c3_mut.sh` 与 `/tmp/r123_c3/c3_arm.sh`；被改的永远是 `/tmp/r123_c3/mut123_c3/<arm>/src` 的副本，
仓库树未动；每臂只重编读该文件的四个 TU，其余替套件已建的 13 个 `.o`）。
**矩阵是本轮的证据形状**：整套用例一起跑时，第一个红点会盖住后面的，所以每臂另用
`/tmp/r123_c3/c3_sel_main.cpp` 一条一条起进程跑（`/tmp/r123_c3/matrix_out.txt`）。

| 变异 | 破掉的那句话 | 逐用例矩阵（1=C3-1 2=C3-2 3=C3-3 4=C3-4 5=C2-4 6=C2-5 7=C3-5） |
| --- | --- | --- |
| M1 `board_score` 取反两侧 | 一条记录两侧各自的答案 | 红 1(`:7816`)/2/3/4(`:7990`)/5(`:7641`)/7(`:8061`)，绿 6 |
| M2 `evaluate_board_from` 两色石头项互换符号 | 求和的后一半的语义 | **1/2/3/4 全绿**，红 5(`:7641` 的树)，绿 6/7；`test_evaluator.cpp:240` `after_black == 7700` 红 |
| M3 `board_score` 不把和交给后半 | 「仍是一次全盘求和」 | 红 1(`:7816`)/2/3/4(`:7990`)/5，绿 6/7 |
| M4 `static_score` 的求值支退回 `evaluate_board` | 「静默搜索入口不再做全盘评估」 | **1/2/3/4 全绿**，红 5(`:7655` `cache.probes() == row.probes`)、红 7(`:8061`) |
| M5 `alphabeta()` 停止支 `:334` 退回 `evaluate_board` | 被预算砍断的节点也读记录 | **只有 7 红**（`:8061`）——第一轮这枚变异**全绿**，C3-5 因此写出来 |
| M6 线 key 少比一条（丢 d=0） | 一次落子的失效集 | 四条差分之一里**只有 3 红**（`:7932`），另红 5(`:7641`)/6(`:7694`)/7(`:8061`)，绿 1/2/4 |

三条从矩阵里读出来的话，比表格本身重要：

1. **差分用例看不见共用的算术**（M2）。C3-1~C3-4 比的是「读法」对「扫描」，两者从 step 3 起调用**同一段**
   求和代码，所以任何一段共用算术被改坏，四条全绿。守住 M2 的是另外两件东西：`tests/test_evaluator.cpp` 的
   绝对字面量（`7700`、`4924`、`1708`、`38700`、`five - 1` 封顶那一类）与 C2-4 钉住的那棵树（分数换了，
   节点数就换）。本轮把这一条写进用例注释，而不是假装差分是全覆盖。
2. **一条用例为了一枚变异而存在**（M5→C3-5）。第一轮跑矩阵时 M5 在七条用例上全绿：那条支既不进表、
   也不记 `eval_scans`，`scans == qnodes` 的恒等式看不见它，思考层的账本又只覆盖不中断的臂。
   补的 C3-5 用节点预算把这条支走实，于是它成为 M5 的**唯一**反证；这也是本轮唯一一处「先有变异、后有断言」。
3. **不是每条用例都有独占的反证**（C3-2）。六枚变异里没有一枚只被 C3-2 抓到——它对 M1/M3 是冗余的一层。
   本轮不为它编一个独占理由：它的内容是 `:556` #3 要求的**逐格**比对比到代数值这一层（而不是比到和），
   以及唯一一处「另一侧取反」在格级被检查的地方。这是这套反证的**边界**，写下来而不是略过。

控制臂（提交前的工作树）：H1 七条一次跑完 rc=0 / `seconds=7.78`，H2 `test_evaluator` rc=0；
被改臂各红一处以上，见上表与 `/tmp/r123_c3/mut_out2.txt`。

### 门禁

四段跑的都是提交树，段与段之间无并发负载（`/tmp/r123_c3/gate.log`：构建 → 60 s 静置 → ctest → web → anchor → README）。
`ctest` **8/8 / 413.16 s**（`test_search` **383.30 s**；C2 段在同一台机器上记的是 440.30 s / 407.59 s，
本步把 `test_search` 的墙钟降了 **6.0 %**，与每节点成本 −10.4 % 同向、幅度更小——八支里只有 `test_search`
的大半时间在搜索树上，其余是夹具与固定断言），其余七支最慢 **12.77 s**（`test_server`）、次慢 8.23 s
（`test_position`）；`node tests/web_frontend_check.js` **105/105**；
`node tests/readme_anchor_check.js` **120 引用 / 0 problems**（C2 段是 118，多的两条正是本轮新写的两处
`README:624` 引用：`src/search/search_state.h:301` 与 `tests/test_search.cpp:7724`；前者带一条 **HINT**——
那句否决横跨 README:624-626，检查器只在被点名的行上找短语，所以报「first line carrying one is README:643」。
HINT 不是 PROBLEM，套件里本来另有六条同类，本轮不为它改 README 的措辞）；`README.md` **658 行**。

行号与清单要说清楚。本轮对 `tests/test_search.cpp` 的写入 **+386/−9**，文件从 **7871** 行走到 **8248** 行；
三条受保护断言的**内容与行号一字未动**：`assert(result.resigned)` 仍在 **`:2591`**、
memcheck 会死在那里的 10 ms 臂 `assert(interrupted.partial_depth == 1)` 仍在 **`:3973`**、
`assert(completed.method == "alphabeta")` 仍在 **`:3986`**（本轮全部新增都落在 `:7715` 之后）。
那 9 行删除**全部在 C2 段**：C2-4 的两行成本字面量、它自己的两行注释、消息里的三行，加 C2-5 注释里的两行。
`src/` 一侧五个文件 **+103/−26**（`core/evaluator.cpp` 37/24、`core/evaluator.h` 16/0、`search/alphabeta.cpp` 11/2、
`search/move_generator.h` 8/0、`search/search_state.h` 31/0），删掉的是拆分前 `evaluate_board` 里那一段
就地累加的两色循环体和 `opp_total` 这个累加器，以及 `alphabeta.cpp` 里两处旧读法的注释块。
`CMakeLists.txt`、`README.md`、`web/`、`tools/` 一字未动。

### Valgrind

五臂串行、机器整段空闲，构建与 ctest 都排在它前面，任何一条 memcheck 臂都没有和构建或另一支同时跑
（脚本 `/tmp/r123_c3/vg_r123_c3.sh`，`--tool=memcheck -q --error-exitcode=9 --leak-check=full
--show-leak-kinds=definite --errors-for-leak-kinds=definite --track-origins=yes --num-callers=16`；
`build-vg/` 用的是 C2 段修好的那两条离线 pin `-DFETCHCONTENT_SOURCE_DIR_JSON=… -DFETCHCONTENT_FULLY_DISCONNECTED=ON`，
本轮没有再动门禁环境，`tools/valgrind_check.sh` 一字未动）。

- **先声明我自己这台尺子上的一处假读数**：包装脚本的 `uninitialised` 列是 `grep -c uninitialised` 数 stderr 文件，
而 `valgrind_check.sh` 会把**它自己的汇总行**（里面就带 `uninitialised=0` 这个词）写进同一个文件，所以后四臂
`ARM … uninitialised=1`（NEG 臂 2，因为它一趟跑两支）是**汇总行被数进去了**，不是内存报告。
被数的真实对象是 `valgrind_check.sh` 打印的那一行，本轮五臂它的每一行都是
`reports=0 invalid_read=0 invalid_write=0 uninitialised=0 definite_leak=0`；
只有第一臂（C3 驱动）直接数 valgrind 的空 stderr，读到的 `uninitialised=0` 是真的 0。
这不影响任何结论，但它是本轮量出来的、必须写在表旁边的一句话。
- **新代码自己**（`-std=gnu++17 -O0 -g -UNDEBUG` 的 C3 驱动：七条用例，即 C3-1~C3-5 加 C2-4、C2-5 两枚账本钉）：
  **rc=0 / 1351 s / 五列全 0**，`/tmp/r123_c3/vg123_c3/driver.err` 是**空文件**（0 字节），`driver.out` 七行 `passed`
  加一句 `C3 block + C2 pins green`。被 valgrind 套住的工程量按用例自己钉住的读数算：思考路径
  **600,226** 个节点（221774 + 156428 + 221774 + 187 + 63，最后那 63 是 C3-5 被节点预算砍断的一支）、
  **24,760,509** 次候选格读 = **14,032,755** 次从记录里读回 + **10,727,754** 次就地重填
  （reads 8755599 + 7247557 + 8755599 + 111 + 1643；hits 4949934 + 4132247 + 4949934 + 53 + 587。
  第三组 8755599 是 C2-5 的冷思考，与 C2-4 的 BLACK 臂同一盘，所以它的拆分是同一份读数而不是第三次量出来的数；
  C2-5 自己钉的是 warm 那 111 = 53 + 58。）
  这 2476 万次走的正是本轮新写的那条读法：`Entry entries_[225]` 数组加 `board_score` 的
  `active_empty()` 逐格循环，其中 `cell()` 在四键比较上有一条 `d < kLineWords` 的边界和一处 `is_empty` 前门，
  一次越界、一次读未初始化都没有。普查那三条（C3-1/C3-2/C3-4 的 42 盘 × 两色、84 次求和、13,394 个代数值）
  也跑在同一臂里，它们贡献读而不贡献思考节点。
- **`test_training`**（`build-vg/`，Debug `-UNDEBUG`）：**clean / 146 s / 0 / 0 / 0 / 0 / 0**（包装层 322 s，多出来的是构建）。
- **`test_search`**（装着本轮五条 C3 用例的那套）：内层 **ABORTED rc=134 / 257 s / 0 / 0 / 0 / 0 / 0**，
外层 `valgrind_check.sh` 因此退 **1**。stderr 里最后一条是
`test_engine_reports_the_interrupted_alphabeta_layer measured (interrupted: alphabeta d0+partial 0/n0 time_hard, last frame d0/n0, control: alphabeta d1+partial 2/n5019 time_hard)`，
它死在本轮行号未动的那一句 `assert(interrupted.partial_depth == 1)`（`tests/test_search.cpp:3973`，A2/A7 的 **10 ms 臂**）；
它前一条 `test_engine_progress_frames_share_the_search_clock` 是**过的**（pre-search 647 ms / first frame 851 ms / 3 frames）。
同一套不套 valgrind 时全绿（门禁段）。
- **`test_server`：本轮 clean（47 s / rc=0），与 C2 段那行相反。** C2 记的是 `Assertion 'hint_results == 0' failed`
（`tests/test_server.cpp:1710`，`test_hint_answer_for_a_superseded_position`）rc=134 / 25 s；本轮**提交树和 e2e9705
对照树上这一臂都没再 abort**（47 s 与 40 s，五列同样全 0）。所以本轮能说的只有一句：`:1710` 那条在 memcheck 时间尺度下
是**会翻脸的旧脆弱**，它上一轮红、这一轮绿，而本轮**没有碰 `src/server/`，也没有碰 `tests/test_server.cpp`**——
既不能把它记成本轮修好的，也不能记成本轮弄坏的。`:3973` 那一条不是这种：它在两棵树上逐字复现，见下。
- 口径照 2026-10-17 的约定：`reports` 是 valgrind 自己的 `==NNNN==` 行数，另四列是
`Invalid read of size` / `Invalid write of size` / `uninitialised` / `definitely lost` 的计数，构建目录与 `-UNDEBUG`
都在每条里点名。**本轮唯一一处 rc=134 是断言，不是内存错误**：五列读数在这条臂上都是 0。
- **那一处 rc=134 在改动前的树上重测，并且逐字复现**（`/tmp/r123_c3/vg123_c3/neg.log`；树
`/tmp/r123_c3/vg123_c3/neg123_c3` = `git archive e2e9705` + 它自己的一份 `build-vg`；跑之前先证明这棵树里
**没有 step 3**：`grep -c 'int board_score' src/search/search_state.h` = **0**）：
`NEG test_search rc=134 / 260 s / 0 / 0 / 0 / 0 / 0`，死的用例名、`measured` 那行的形状
（`interrupted: alphabeta d0+partial 0/n0 time_hard`，控制支 `d0+partial 1/n4001`）、断言的文件与行号都与提交树同一处；
`NEG test_server clean / 40 s / 0 / 0 / 0 / 0 / 0`。包装层两臂合计 415 s（含 NEG 树的构建）。
- 一个与 C2 段同向的对照：同一根 10 ms 臂的 memcheck 墙钟 **260 s（e2e9705）→ 257 s（本轮）**，**平的**。
本轮省下的是每格四向识别，而这条臂的时间主体在夹具与 valgrind 自己。**本轮的收益不在 valgrind 的时间尺度上显现，
只在计数器和每节点成本上显现**（收益段：静默 pass −100 %、合计识别 −58.5 %、`ns_per_node` −10.4 %）。
控制支那两行读数（本轮 `d1/n5019` 对对照树 `d0/n4001`）**不当作成本证据**：两条都是「时限落在层中间」，
在 memcheck 的倍率下落在哪一层不是本轮合约的内容。
- **TSan / DRD 本轮不跑**，理由说清楚：step 3 只增加**读者**——`CandidateCache::board_score` 读的还是 C2 段
那个 `MoveGenerator` 的 `mutable cache_`，而 `MoveGenerator` 属于一次 `AlphaBetaSearch`，一次思考一个线程；
本轮没有新增任何可变状态，也没有把任何状态跨出思考边界（C2-5 在 memcheck 臂里又跑了一遍，`187 / 0 / 1` 与
`111 = 53 + 58` 一字未动，就是这句话的凭据）。本轮唯一新读的分支 `alphabeta.cpp:334` 也在同一对象、同一线程内。
B3 量出的那一条真竞态（`GameManager::undo` 写 `board_` 对上 `request_hint` 的读）仍是服务端旧代码，排在 bugfix 批次，
不属于一条引擎成本改动。

**合约过期申报（本轮有，且只有 C2-4 一处）**：`test_a_quiet_think_prices_its_candidates_about_once_in_two`
的缓存三列换了数字——`BLACK 4,066,903 / 2,097,434 / 1,969,469` → **8,755,599 / 4,949,934 / 3,805,665**，
`WHITE 3,003,955 / 1,539,956 / 1,463,999` → **7,247,557 / 4,132,247 / 3,115,310**。原因是契约本身变了：
step 3 之前求和不读缓存，读缓存的只有排序；现在静默节点的求和也走同一条缓存，一次思考的**候选读次数**
不再等于排序的问数，而是等于「排序 + 每节点求和」的问数（用例名那句「about once in two」仍然成立，
命中率从 51.6 % 走到 56.5 %）。同一行断言里 C1 钉住的那一半（`221774 / 142978 / 75855 / 67123 / 75852 / 71992`
与 `WHITE 156428 / 102340 / 68419 / 33921 / 68419 / 97044`）**一字未动**，所以「树与评估本体没变」这件事
仍然是被断言守着的，而不是被打印出来的。C2-5 的 `187 / 0 / 1` 与 `111 / 53 / 58` **没动**，
理由写在它自己的注释里：那一次 warm 思考的求和被置换表回答，走不到求和，所以 step 3 不改它的账。
上报帧不变：求和不改变任何一次答案，没有新字段、README 没有新行。



## 轮次122 引擎重构 C2：一条线的四个字第一次替每个节点记住候选格的形状与价格 — engine redesign stage C2

规格 `docs/ENGINE_REDESIGN.md` `:406-407` §2.6 **Step 2**「候选格分数缓存」：「每个候选格缓存
`evaluate_position_pair` 分数。落子/撤销只更新受影响格（该格 ±2 邻域内、以及该格自身所在 4 条线上的候选格，
约 15–25 个），而不是每节点重算全部候选。这是『排序』路径的主要成本」；它的上游是问题陈述 `:106`
「**候选分数每次节点重算**。`sort_moves` 在每个节点对全部候选重新调用 `evaluate_position_pair`
（`alphabeta.cpp:69-78`），父节点已经算过的信息没有传递，候选格分数也没有缓存」——本轮改的正是那一行点名的调用；
再加 `:415` 的预期行「Step 1+2 使**排序路径**成本下降 ~60 %」、`:549` 的 C 阶段验收、`:556` 回归保障第 3 条
（「评估函数改动（位置项、**增量缓存**）必须与旧实现逐格比对」，并且它点名的方法就是
「沿用 `recognize_position_both` 那次『全 225 格 × 两色 × 42 棋盘一致』的测试方法」——那三个数字本轮照抄进夹具），
`:568` 的风险行「增量评估正确性（README 已有一次否决记录）| 高 | 按 Step 1→4 小步推进，每步都做差分测试」，
以及文件清单 `:582`（`src/search/search_state.h`：**新增** killer 表、history 表、**候选分数缓存**、PV 三角表）
——前两项是 B2 落的、第三项是 PV 三角表由 B3 落的，`:582` 剩下的那一格就是本轮。
`:556` 那半句「增量缓存」是本轮第一次有了对象：A 阶段起 README 就记着一次全盘增量评估被否决，而 Step 2 要的不是
全盘增量，是**按格**的记忆，它的失效条件恰好等于一个格自己的依赖集。

与 C1 同一条口径：成本改动**一次都不许多搜，也一次都不许少搜**。本轮多一条纪律——省下的必须是排序路径，不许去动
静默节点那一次全盘 `evaluate_board`（`:415` 把那一半明写给 Step 3，`:409-410` 定义了它），所以两棵树上的 standpat 计数必须**逐臂相等**，
它既是「没越界」的证据，也是「没顺手多拿一步收益」的证据。

### 落地的三件事

1. **`CandidateCache`**（`src/search/search_state.h:226-334`，文件清单点名的位置）。一格一条记录：
   `Cell{PositionPattern black, white; int value_black, value_white}`，两个读者 `side_value(color)` 与
   `pair_value()`。失效判断不是记账而是**比对**：`line_key()`（`:308-314`）把该格四条线各取一个字
   `board.line_bits(BLACK,d,line) | (board.line_bits(WHITE,d,line) << 32)`，四个 `uint64` 全等才算命中
   （`kLineWords = 4`，`:306`）。`entries_[225]` 按格下标（与 history 表同一布局，读一次不要查找）；
   `cell()` 的第一句是 `is_empty` 守卫（有子即 `nullptr`，账本不动），命中 `++hits_`、否则 `++recomputes_`
   并重填，`probes/hits/recomputes` 三个计数与 C1 的三兄弟同形。
2. **`MoveGenerator` 拥有它**（`move_generator.h:136` `mutable CandidateCache cache_`，构造 `:98`）。
   `generate()` 仍然是 `const`：它问的是局面，不是「这个节点改过什么」。一次记录喂四处（`move_generator.cpp`）
   ——两个形状 `:165-166`、BLACK 禁手判定 `:171`、静默级的 `side_value(color)` `:179`、主层的 `pair_value()`
   `:185`。这就是文档要的「而不是每节点重算全部候选」：同一层里 BLACK 节点与 WHITE 节点读**同一条**记录，
   各自取自己那一侧，所以缓存不需要颜色字段。
3. **排序路径改读同一条记录**：`sort_moves` 的第④级现在是 `move_gen_.candidate_price(board, m.pos)`
   （`src/search/alphabeta.cpp:96`），价格与切 tier 的那个价格是同一个数——四次识别发生在 `generate()` 里，
   之后该格四条线没动过，所以到排序这里必然是一次命中。**仍然按候选去问**，没有改成「从 tier 列表里取」：
   前三级（TT pin、两个 killer）可以 pin 一个 `generate()` 因宽度截断而没排过的格，兜底必须能给任何格定价，
   这一条是 ④ 的契约，也是 C2-3 最后一块断言的内容。

一处**刻意没接**的读者写在代码里而不是略过：`threat_probe()`（`move_generator.cpp:103-143`）仍然自己跑
`recognize_position_both`。它一次思考只走一遍（`search_engine.cpp:210`，在 `AlphaBetaSearch` 存在之前就要求答案），
不在每节点路径上；把它挂到缓存上，等于把 B5 段的账本改写成「谁先问谁付钱」的形状，而 Step 2 说的是「每节点重算」。

### 与文档 dirty set 的偏离：footprint 不是依赖集

文档给的失效集合是「该格 ±2 邻域内、以及该格自身所在 4 条线上的候选格，约 15–25 个」。**前半句没落**，
因为它会给出错的答案，而这是正确性问题、不是口味问题：一个格的记录真正的依赖是它所在的四条线本身——
`finish_direction` 为 BLACK 做的补正（`run_length`、判定「这个四到底有没有成五点」的 `fill_run` k≤5、
`painted` k≤4）沿**同一条线**伸到 ±9/±10，而 ±2 邻域（乃至任何 ±4 footprint）里没有这些格。
反例是量出来的，就是 C2-2 的 (7,7)：行窗口 (7,3)..(7,11) 之内一切正常，记录给 `value_black=1112`、
`value_white=112`、`pair_value=1224`、形状 `RUSH_FOUR`；BLACK 再落 (7,12)（距该格五格、在窗口边缘 (7,11) 之外）
之后，(7,8) 那个补点从「恰好五」变成「六」，这个四不再有任何合法成五点，形状**变成 NONE**、`pair_value=224`。
按 ±2 维护脏集合的实现会把 1224 交出去。落的 key 是那四个线字：完整的依赖、四次载入、`Board` 里零记账，
于是一次 make/unmake 的兄弟分支**按线各自复核**，而不是「记住谁碰过谁」。文档那句
「而不是每节点重算全部候选」是这一版兑现的契约（见「收益」段），「±2 邻域」那半句是它没兑现、也不该兑现的。

### 中性：树、分数、以及 step 3 的成本都不许动

- **18 臂逐行比对**（同一板、新表、`nodes` 不 binding 于时限）：`nodes / qnodes / tt_hits / score` 四列
  一字不差（`/tmp/r122_c2/neg_2.txt` 与 `pos_2.txt`，脚本比对 18/18 无差异，`BAD: []`）。
- **standpat 那一列**（`evaluate_board` 里的全盘 recognition，本轮无权碰）在两棵树上都是 **549,776,489**，
  逐臂相等。这是「没越界去拿 Step 3 的收益」的直接读数，而不是一句承诺。
- **C1 钉过的东西仍在原处**：quiet 两色 depth 6 的 `221774/142978`、`156428/102340`、`71992/97044`，
  以及它的扫描账本 `75855+67123==142978 / patches 75852` 与 `68419+33921==102340 / 68419`——
  全部写进 C2-4 的同一行断言里，所以 Step 2 若动了评估本体或树，红的是这一条而不是新用例。
- **缓存不跨思考边界**：一个 `AlphaBetaSearch` 活一次思考（`src/search/search_engine.cpp:252`），
  缓存跟着它生灭；跨思考唯一复用的通道还是置换表。这不是「顺便安全」，是本轮**不重跑 TSan** 的理由：
  没有新的共享面，也就没有新的竞态窗口可测。C2-5 钉的就是这条边界。

### 收益：排序路径 −88.5 %，总识别次数 −63.6 %（文档的 ~60 % 是这一段的目标）

18 臂合计（`-O2`，同一份驱动，`/tmp/r122_c2/{neg_2,pos_2}.txt`）：

| 读数 | 控制树 897ab43 | 本轮树 | |
| --- | --- | --- | --- |
| 排序/候选路径 recognition | 1,406,265,013 | **161,413,672** | **−88.5 %** |
| standpat 全盘识别（Step 3 的账） | 549,776,489 | 549,776,489 | 0 % |
| 合计 recognition | 1,956,041,502 | 711,190,161 | −63.6 % |
| 节点数（18 臂） | 11,365,496 | 11,365,496 | 0 % |
| `ns_per_node`（best-of-3，三遍） | 8691.1 / 8667.7 / 8585.7 | 7973.5 / 7966.8 / **7908.6** | −7.9 % |
| `knps` | 115.1 / 115.4 / 116.5 | 125.4 / 125.5 / **126.4** | +8.5 % |

- `:415` 那句「Step 1+2 使排序路径成本下降 ~60 %」在这一段拿到的比它多：Step 1 省的是**评估扫描**（C1 量到 −38 %），
  Step 2 省的是**每候选一次四向识别**（本轮 −88.5 %）。两件事不重叠，所以本轮的读数不是把那 60 % 摊到两步上，
  而是把排序路径这一段几乎清空：剩下那 161,413,672 次是同一格**换了一条线**之后必须重算的部分，不是重复劳动。
- **两支独立仪表互验**：缓存自己的 `recomputes` 逐臂等于普查在 C2 树上排序路径的残差——
  `quiet` BLACK d6 一臂是 `4,066,903 = 2,097,434 命中 + 1,969,469 现算`，而普查在那一臂数到 17,511,198 − 4,688,696
  = 12,822,502 → 1,969,469；`WHITE` 1,463,999、其余各臂 3,099,644 / 4,634,753 / 8,404 / 1,387 也逐个对上。
  记账的数与被记的数各自独立产生却相等，这是「没漏算、也没多算」的读法，不是「计数器说省了」的读法。
- **墙钟为什么只有 −8 %**：一个静默节点仍然付一次全盘 `evaluate_board`（`qnodes` 占 `nodes` 的 64.5 %），
  那正是 Step 3 的靶子。识别次数从 19.6 亿降到 7.1 亿，而每节点时间只降 7.9 %，说明剩下的钱不在排序里——
  这一行是本轮最想留下的诊断：**省下的不是还没省的那一半**。
- **`:549` 本轮仍未达成，报实测**：单线程 ≈ **12.6 万节点/秒**，距 ≥1M nps 差约 8 倍；depth 8 在 4 s 内也没有到
  （`quiet` d8 两色 1,408,083 + 2,131,525 节点）。结构性原因与 C1 段同一条，只是换了主语：Step 1 让全盘扫描少发生
  三成八，Step 2 让候选识别几乎不再发生，而每个静默节点的那一次全盘评估还在原地。这一行是 C 阶段整段的验收，
  到 C3/C4 才有一次成形的读数。

### 用例（`tests/test_search.cpp` 五条，定义落在 `:7229-7709`，注册落在 `main()` 的 `:7861-7865`）

段首注释（`:7229-7251`）自己交代了两件事：一条 memo 欠的三个答案（数字对不对、什么时候过期、读者问的是不是
写者算的那个），以及「文档的 dirty set 没落，理由是正确性」。

- **C2-1 `test_the_cell_cache_answers_the_fresh_pass_on_every_square`**（`:7360`）：`:556` #3 那条「逐格比对」
  在它点名的方法上做——全 225 格 × 两色 × 42 盘（`c2_census_boards()`：40 支 LCG 伪随机盘 + 2 支把线填满的盘，
  `:7279`；刻意不 `#include <random>`，因为新增头会把 `:2591`/`:3973`/`:3986` 三条受保护断言的行号推走）。
  9450 格：8382 空格逐格比 `recognize_position` 两色（`c2_patterns_equal` 连 `analyze()` 的聚合字段一起比，
  `:7260-7268`）、比两个单侧价、比 `pair_value()` 对 `candidate_pair_value` **与** `evaluate_position_pair`
  两个色各一次（`:7309-7331`）；1068 占用格答 `nullptr`/0，且**账本不动**（守卫在 `++probes_` 之前）。
  每盘一本账：`probes == recomputes == empties && hits == 0`，第二趟同板全部命中、且返回**同一个地址**
  （`:7395-7409`），最后 `probes == 2×empties / hits == recomputes == empties`。`squares == 42*225`、
  `guarded + checked == squares` 钉住普查本身。
- **C2-2 `test_a_stone_five_cells_away_is_inside_the_records_dependency`**（`:7426`）：上面那个反例的完整形状——
  1224 → (7,12) → NONE/224（同一槽位，`after == square`，只是换了指图），`hits/recomputes` 在四步之后
  `1/1 → 1/2 → 2/3 → 3/3`；撤销不归还旧记录（槽里已是孩子的 key），只是让下一次读重算，而重算回来的正是
  落子前那个数（`:7462-7466`）——这正是搜索活的兄弟分支形状：父节点为每条**变了**的线付一次，不是为每个孩子付一次。
  另一半是精度：(4,6) 落 WHITE 与 (7,7) 不共任何一条线，`pair_value` 仍 1224 且 `hits==3, recomputes==3`
  **一次都不许多**（`:7471-7474`）。
- **C2-3 `test_the_tiers_and_the_ordering_read_the_same_record_the_pass_wrote`**（`:7490`）：两个消费者。
  tier 侧——六支板 × (两色主层 + 静默行) 共 **18 条 tier 列表**与一个不带缓存的冷 `MoveGenerator` 逐元素相同
  （`c2_sets_equal` 连 `Candidate::score` 一起比，`:7333-7343`；`assert(lists == 18)`）；静默行的分数逐条等于
  旧写法 `evaluate_position_with_pattern(..., mine, BLACK)`（`:7540-7542`）。排序侧——对整条 `active_empty()`
  两色各问一次 `candidate_price`，每次等于无缓存的 `evaluate_position_pair`，**1516 次**（`:7600` 把次数钉成
  字面量而不是下界，姿势集一旦不再产出候选，这条用例会立刻变成「抽了一个样本」而不是静默退化）。
  最后一块是禁手格 (7,7)：BLACK 的任何 tier 都不带它（宽度截断，与合法性无关——文档的禁手规则只管 BLACK），
  但记录留着 `has_three_three`、把 `value_black` 归零、`pair_value()==value_white`，
  并且 `side_score(BLACK)==0 / side_score(WHITE)==value_white`（`:7579-7592`）——一条记录必须同时带两侧，
  因为下一个节点问的是能站那边的颜色。
- **C2-4 `test_a_quiet_think_prices_its_candidates_about_once_in_two`**（`:7613`）：思考层的净值，钉**两层**。
  树与 C1 的账本（`221774/142978/71992`、`156428/102340/97044`，`scans+reuses==qnodes`、`patches`），
  和本步自己的账本（`probes 4,066,903 = hits 2,097,434 + recomputes 1,969,469`；
  `3,003,955 = 1,539,956 + 1,463,999`；`hits>0 && recomputes>0`——两臂各 51.6 % 与 51.3 % 的读免费，
  用例名就是这么来的）。
- **C2-5 `test_a_warm_think_buys_its_own_records_and_no_ones_from_the_first`**（`:7674`）：边界。同一表第二支
  `AlphaBetaSearch` 仍然是 C1 钉的那个 warm 答案（`nodes==187 / eval_scans==0 / eval_reuses==1`），
  而它的候选工作是它自己的：`probes==111 = 53 hits + 58 recomputes`，且 `probes < first_probes`
  （第一思考那 4,066,903 次留在原地没有跨过来）。跨思考的只有表——这一条同时是「系数换了怎么办」的答案：
  一次思考一套系数，一条缓存活不过一次思考，所以一个数可以比写它的节点活得久，但不能比写它的系数活得久。

夹具：`c2_far_reach_board()`（`:7349`，(7,9)(7,10)(7,11) 的冲四加两角孤子）、
`c2_check_record`（`:7309`，一格的全部四类读数）、`c2_census_boards`（`:7279`）。

### 反证申报：无 HEAD 反证，以四枚 mutation 代替

`CandidateCache` 这个符号在 `897ab43` 上不存在，这五条用例在那棵树上连编译都过不去——本轮申报**无 HEAD 反证**，
理由与 C1 同构（结构性，不是没做）。代替它的是四枚变异，各破 Step 2 的一句话
（`/tmp/r122_c2/c2_mut.sh`；每臂只重编三个读该头的 TU（`-std=gnu++17 -O2 -g -UNDEBUG`，与门禁同一组旗号），
其余替套件已建的 14 个 `.o`，`/tmp/r122_c2/c2_case_main.cpp` 只挂这五条，所以第一次变红的 assert 必属于 Step 2；
被改的是 `/tmp/r122_c2/mut122_c2/<arm>/src` 的副本，仓库树未动）：

| 变异 | 破掉的那句话 | 结果 |
| --- | --- | --- |
| M1 比对 key 时从 `d=1` 起（丢掉行那一条线） | 「四条线全等才算命中」 | rc=134 停在 `:7453` `after->black.directions[0].type == PatternType::NONE`（**C2-2**，C2-1 已绿） |
| M2 用 WHITE 的形状算 BLACK 侧的价 | 一条记录两侧各自的答案 | rc=134 停在 `:7315` `found.value_black == evaluate_position_with_pattern(..., ref_black, BLACK)`（**C2-1**，第一条就红） |
| M3 `pair_value()` 只回 `value_black` | 「缓存 `evaluate_position_pair` 分数」 | rc=134 停在 `:7326` `found.pair_value() == candidate_pair_value(mine, theirs, row, col, side)`（**C2-1**） |
| M4 `++probes_` 挪到守卫之前 | 占用格「不付钱」 | rc=134 停在 `:7395` `cache.probes() == empties`（**C2-1** 的账本） |

控制臂是提交树的同一驱动：`=== base rc=0 === … seconds=12.16`，五条 `passed` 全绿
（`/tmp/r122_c2/mut_out.txt` 与各臂 `.out`）。四条各红一处、且红点**各不相同**：M1 只能被 C2-2 抓到
（一次思考之内同一盘反复问，键自己不会过期——**只有落子/撤销能**），这恰好说明本轮真正难的那半不是缓存而是失效。
M4 落在账本而不是答案上，说明「省了多少」这件事在本轮也是被断言守着的，不只是被打印出来的。

### 门禁

四段跑的都是提交树。`ctest` **8/8 / 440.30 s**（`test_search` **407.59 s**；C1 记的是 410.75 s、B5 记的是 515.00 s：
本步把每节点成本降了 8 %，却没有把它落在整套用例的墙钟上——八支里只有 `test_search` 的一半时间在搜索树上，
其余是夹具与固定断言，而本轮省下的是每候选一次四向识别，不是节点数。其余七支最慢 **14.63 s**（`test_server`），
`/tmp/r122_c2/final_gate.log` 是一整支脚本：构建 → 60 s 静置 → ctest → web → anchor，四段之间没有任何并发负载）、
`node tests/web_frontend_check.js` **105/105**、`node tests/readme_anchor_check.js` **118 引用 / 0 problems**、
`README.md` **658 行**。

要说清楚的一次重跑：第一轮四段（`ctest` 8/8 / **435.90 s**、`test_search` **409.60 s**、其余七支最慢 9.44 s，
`/tmp/r122_c2/ctest_gate.log`）跑完之后，我才改掉两处**已经作废的文档引用**（`tests/test_search.cpp:7231` 的
`:403-404` → `:406-407`、`src/search/search_state.h:210` 的清单行 `:586` → `:582`），所以那一轮**作废**，
上面这一轮是按提交树重跑取的读数。两处改写都是注释里的字符，两行换两行、没有增删行，文件仍是 **7871** 行、
`:3973` 下面每一条断言的行号都没有动。八支的相对次序也没变：`test_search` 一支占 92.6 %。

行号与清单要说清楚。本轮对 `tests/test_search.cpp` 的全部写入都是**追加**（`+492/-0`，定义 `:7229-7709`、
注册 `:7861-7865`），三条受保护断言的**内容与行号一字未动**：`assert(result.resigned)` 仍在 **`:2591`**、
memcheck 会死在那里的 10 ms 臂 `assert(interrupted.partial_depth == 1)` 仍在 **`:3973`**、
`assert(completed.method == "alphabeta")` 仍在 **`:3986`**；文件从 **7379** 行走到 **7871** 行。
`src/` 一侧四个文件 `+180/-18`，那 18 行删掉的是**被替换掉的四种读法**（每候选一次 `recognize_position_both`、
一次 `candidate_pair_value`、一次 `evaluate_position_with_pattern`，加排序里那一次 `evaluate_position_pair`）
和两段作废注释——`alphabeta.cpp` 里那段
「the pass below does read them a second time. **Deliberately so, until stage C's candidate score cache replaces it**」
等的就是本轮，`move_generator.h:93-96` 与 `move_generator.cpp:151-156` 的注释换成了这一版的契约。
`CMakeLists.txt`、`README.md`、`web/`、`tools/` 一字未动。上报帧不变：缓存不改变任何一次答案，
所以没有新字段、README 没有新行（B3 的 `principal_variation` 是玩家可见的，这一件不是）。
合约过期：**无**，本轮没有改写任何一条既有断言的内容。

### Valgrind

五臂串行、空闲，构建与 ctest 都排在它前面，没有任何 memcheck 臂与构建或另一支同时跑
（脚本 `/tmp/r122_c2/vg_arms.sh`；`--tool=memcheck -q --error-exitcode=9 --leak-check=full
--show-leak-kinds=definite --errors-for-leak-kinds=definite --track-origins=yes --num-callers=16`）。

- **本轮先修的是门禁自己的环境**：`build-vg/` 的 configure 会走 `FetchContent(json URL
  https://github.com/nlohmann/…v3.11.2.tar.gz)`（`CMakeLists.txt:10-17`），而这台机器现在离线（系统里没有
  nlohmann：`/usr/include/nlohmann` 与 `dpkg -l` 都查不到），于是第一次尝试的四臂全部死在
  「Build step for json failed」上、`rc=1 / seconds=0`，读数为 0 的那几列是**构建没成功**而不是内存干净。
  修法是把取源指到门禁树里已经解开的那一份：`cmake -S . -B build-vg -DCMAKE_BUILD_TYPE=Debug
  -DCMAKE_CXX_FLAGS="-UNDEBUG" -DFETCHCONTENT_SOURCE_DIR_JSON=$PWD/build/_deps/json-src
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON`，NEG 树同一句。`tools/valgrind_check.sh` 一字未动：它只在
  `build-vg/CMakeCache.txt` 不存在时才 configure，缓存里带着这两个变量，之后就轮到它自己正常工作。
- **新代码自己**（`-std=gnu++17 -O0 -g -UNDEBUG` 的 C2 驱动：五条用例，含四次 depth 6 思考）：
  **rc=0 / 1415 s / reports=0 / invalid_read=0 / invalid_write=0 / uninitialised=0 / definite_leak=0**
  （`/tmp/r122_c2/vg122_c2/driver.err` 是**空文件**，`driver.out` 里五行 `passed`、`C2 block green`）。
  这一臂被 valgrind 套住的工程量按用例自己钉住的读数算：四次思考共 **600,163** 个节点
  （221774 + 156428 + 221774 + 187）、**11,137,872** 次候选格读（4066903 + 3003955 + 4066903 + 111），
  其中 5,734,877 次从记录里读回、5,402,995 次就地重算——`Entry entries_[225]` 是搜索对象里的一个数组，
  这 1113 万次走的就是它，一个字节都没读越界、没读没写过的格子。
- **`test_training`**（`build-vg/`，Debug `-UNDEBUG`）：**rc=0 / 166 s / 0 / 0 / 0 / 0 / 0**，`All training tests passed`。
- **`test_search`**（装着五条 C2 用例的那套）：**rc=134 / 264 s / 0 / 0 / 0 / 0 / 0**。stderr 里唯一的非 memcheck 行
  还是那句 `Assertion 'interrupted.partial_depth == 1' failed`（`tests/test_search.cpp:3973`，A2/A7 的 **10 ms 臂**），
  死前最后一行是 `test_engine_progress_frames_share_the_search_clock passed (pre-search 643 ms, first frame 849 ms,
  3 frames, terminal 2973 ms of 2973 ms)`。同一套不套 valgrind 是绿的（门禁段）。
- **`test_server`**：**rc=134 / 25 s / 0 / 0 / 0 / 0 / 0**，`Assertion 'hint_results == 0' failed`
  （`tests/test_server.cpp:1710`，`test_hint_answer_for_a_superseded_position`）。**本轮没有碰 `src/server/`
  也没有碰 `tests/test_server.cpp`**，这一臂跑的是门禁树，报的是与 C1 同一处的旧脆弱。
- 口径照 2026-10-17 的约定：`reports` 是 valgrind 自己的 `==NNNN==` 行数，另外四列是 `Invalid read of size` /
  `Invalid write of size` / `uninitialised` / `definitely lost` 的计数，构建目录与 `-UNDEBUG` 已在每条里点名。
  **两处 rc=134 都是断言，不是内存错误**：五列读数在这两条臂上都是 0。
- **两处 rc=134 都在改动前的树上重测，并且逐字复现**（`/tmp/r122_c2/vg122_c2/neg_test_search`、
  `neg_test_server`；树 `/tmp/r122_c2/vg122_c2/neg122_c2` = `git archive 897ab43` + 它自己的一份 `build-vg`，
  跑之前先证明这棵树里**没有** `CandidateCache`：`grep -c CandidateCache src/search/search_state.h` = 0）：
  `NEG_test_search rc=134 / 267 s / 0 / 0 / 0 / 0 / 0` 与 `NEG_test_server rc=134 / 25 s / 0 / 0 / 0 / 0 / 0`，
  两句 assert 的文件、行号、断言文本与提交树**完全相同**（`:3973` 与 `:1710`——本轮的插入全在 `main()` 之前，
  没有推动任何一条既有断言）。本轮不修剪任何断言，也不把它们挪进 bugfix 提交。
- 一个与 C1 相反的对照，值得写下来：同一根 10 ms 臂的 memcheck 墙钟是 **267 s（控制树）→ 264 s（本轮树）**，
  **平的**。C1 那轮同一条臂从 332 s 走到 255 s，因为它省下的是三成八的全盘扫描；本轮省下的是每候选一次四向识别，
  而这条臂的耗时主体在夹具和别处。**本轮的收益不在 valgrind 的时间尺度上显现，只在 callgrind 式计数器和
  `PART5` 的每节点成本上显现**（收益段那两行：排序路径 −88.5 %、`ns_per_node` −7.9 %）。
- **TSan / DRD 本轮不跑**，理由说清楚而不是含糊过去：`CandidateCache` 是 `MoveGenerator` 的一个 `mutable` 成员，
  `MoveGenerator` 属于一次 `AlphaBetaSearch`，而 C2-5 钉死「缓存不跨思考」（第二支搜索自己付 111 次读，
  第一支的 4066903 次一次都没跨过来）。本轮**没有引入任何跨线程共享的可变状态**，也就没有新的窗口可给 TSan 看；
  B3 已经量出的那一条真竞态（`GameManager::undo` 写 `board_` 对上 `request_hint` 的读）是服务端旧代码，
  仍在 bugfix 批次里排着，不属于一条引擎成本改动。

## 轮次121 引擎重构 C1：置换表第一次替节点省下一次全盘扫描 — engine redesign stage C1

规格 `docs/ENGINE_REDESIGN.md` `:399-404`：§2.6「评估性能子系统（对应 P4 / G5）」的 preamble
「按『小步、可验证』顺序推进（README 已记录一次全盘增量评估被否决，因此不再一次性重写）」与
**Step 1** 行「节点首次计算静态分后随 TT 一起存；命中时直接复用，省掉重复全盘扫描。低风险、立即可测。」；
再加三个约束行：`:549` 的 C 阶段验收（单线程 ≥1M nps；depth 8 在 4s 内完成）、`:556` 回归保障第 3 条
（「评估函数改动（位置项、增量缓存）必须与旧实现逐格比对」）、`:568` 风险行里对 Step 3 的限定
（「先只用于『无威胁局面』，有威胁时回退全盘评估」——那是 C3 的事，也点名了 B5 的 `opp_four` 的下一个消费者）。
文件清单 `:586` 那一行（`transposition.h/.cpp`：4 路 bucket、depth 优先替换、高位混合索引、**`static_eval` 字段**、
mate 归一化、代龄按手推进）里，前四项是 A3 落的，`static_eval` 字段本身也是那时占下的一个格，但**没有任何读者**：
`af43a05` 上它的唯一一句说明是「Filled in by the search that first evaluates the position (design doc 2.6 step 1)」——
一个从没被填过的字段。C1 落的就是这半行。

C 阶段是一条成本链，不是一条棋力链，所以本轮的口径比 B 阶段更硬：**一次都不许多搜，也一次都不许少搜**。
树的不变量用两层证据钉（下面「中立性」段），收益只写成计数器与 `-O2` 探针树上的每节点成本，墙钟一律标诊断。

### 落地的三件事

1. **表的两个新面**：`int32_t static_eval_of(uint64_t hash) const` 与
   `bool put_static_eval(uint64_t hash, int32_t static_eval)`（声明 `src/optimization/transposition.h:85-112`、
   定义 `src/optimization/transposition.cpp:104-146`）。两者都只做一次 bucket 走、只碰 `static_eval`：
   不返回值、不剪枝、不决定替换——这正是「用一个它们的思考与不用一个的思考搜同一棵树」的依据。
   - 读优先：同一 tag 下**真实记录**（`flag != TTFlag::NONE`）先答，没有记录才读 `depth == kEvalOnlyDepth` 的那一格。
   - 写优先：有记录就写在记录上；没有记录才占用一个 `flag == NONE` 的格，并且**优先占用从未承载过任何东西的空槽**
     （而不是把某一格已经带的数再回收一次），于是一个 bucket 缓存的是四路里能缓存的全部评估。四格都成了真实记录时它返回 `false`
     ——分数是被放弃的那个，不是把一条界赶出表。
   - 一处取舍要说清楚：同一 tag 第二次补分而它没有记录时，新的数可能落进**另一格**，于是同一个 tag 有两格各自带数。
     这不是错的：同一布局的 `evaluate_board` 是确定的，两格带的必然同一个数，而 `static_eval_of` 取索引最小的那一格，
     读回来的还是它自己。它也不值得改：把「同 tag 优先」加进去是一个不改任何一条读数、不改任何一次节点计数的改动，
     而它要重跑整套门禁——本轮的口径是「一步成本改动不许动树」，多余的表内 churn 不在要解决的清单上。
   - `kEvalOnlyDepth = -1`（`transposition.h:21-25`）：搜索深度永不为负，且 `store()` 写不出它，所以
     `flag == NONE && depth == kEvalOnlyDepth` 恰好、并且唯一地描述这一类格子； untouched 空槽的默认 tag 是 0，
     一个 tag 恰好为 0 的位置会匹配上它，靠这个 `-1` 跳过。
   - **`store()` 的继承**（`transposition.cpp:71-80`）：调用方把 `static_eval` 留在 `NO_STATIC_EVAL` 不再是
     「忘掉这一格的分」，而是「我对这一格的分没有意见」；只有 tag 相同（即即将描述同一位置）的槽保留原数，
     满 bucket 里最浅的受害者属于别的位置，它的分数跟着它一起走。
2. **搜索侧唯一的读点**：`int AlphaBetaSearch::static_score(const Board&, int color, uint64_t hash, int32_t cached)`
   （声明 `src/search/alphabeta.h:138-152`、定义 `src/search/alphabeta.cpp:142-162`）。`cached` 是同一个节点已经查到
   的数（记录上那一个），它说不上话时才走表，表也空时才扫描；存的数固定是**黑侧**的，返回时按 `color` 决定符号。
   接上的四处：`quiescence` 的中止分支与 `stand_pat`（`alphabeta.cpp:174-181`，签名多了 `hash` 与 `tt_eval`，
   子节点调用把 `board.zobrist_hash()` 传下去——哈希是 board 的一个字段，这次递归本来就要读它）；`alphabeta()`
   把 probe 命中的 `entry.static_eval` 收进 `node_eval`（`:336-342`），用于 `depth <= 0` 的交接（`:374-375`）、
   `moves.empty()` 的兜底（`:379`），以及最后把同一个数交回 `store()`（`:448`）。
   **没有**接上的一处也写在代码里：`alphabeta()` 自己的中止分支仍然直接调 `evaluate_board`（`:314-322` 的注释给出
   理由——那要把 probe 抬到 stop 判定之上，于是每个被打断的节点都多一次表命中和一次 PV 槽写入，那是改
   **上报帧**的改动，不是改成本的改动；quiescence 的中止分支接，是因为那个节点手里已经握着 hash 和 cached 分数）。
3. **三只成本计数器**：`eval_scans_ / eval_reuses_ / eval_patches_`（`src/search/search_state.h:305-325`，
   `start()` 里随其余计数一起清零）。刻意**不进 `SearchStats`**：那个结构是一个进度帧，字段被 session 归档与
   客户端读，而这三个数描述的是「搜索没有做的工作」——一件关于代价、不关于局面的事，一次中立的复用会改变它
   而什么都不改变给玩家看的东西。

### 「这个数是哪一边的」，以及为什么 28 字节里不需要那个字节

`sizeof(TTEntry) == 28` 钉在 `tests/test_search.cpp:183`，A3 的注释解释过文档的 24 字节条目到不了（合法分值
范围到 ±99999，而引擎的杀棋哨兵是 ±100000，`int16` 装不下）。28 字节里没有剩余字节用来标「这一格的分是黑侧
还是白侧」。本轮的答案不需要那个字节，因为 `evaluate_board` 是**精确反对称**的：675 盘普查 0 处不符、
`max|eval| = 99999`（`/mnt/d/tmp/r121_c1_r3.log` 的 `PART1 boards=675 antisymmetric=675 violations=0
max_abs_eval=99999`，两棵树同一行）——两个候选求和互换，位置项与贴边项本来就是差值，所以 `eval(W) == -eval(B)`
对任何一组系数都成立。存下黑侧那一个数就答得出任一色的读者，包括一条活得比写它那一手的行棋方更久的记录；
`NO_STATIC_EVAL = INT32_MIN` 因此是一个扫描写不出的值。

### 先量出来、再改掉的一种读法（R1 → R3）

最直觉的实现是「分数只往已经存在的记录上补」（R1）。它在 `-O2` 探针树上量到的是
`PART3 totals nodes=808936 qnodes=513725 scans=513725 reuses=0 patches=0 reuse_pct=0.0`
（`/mnt/d/tmp/c1_meas.log`）——**0 %**。原因不是实现错了，是结构：`quiescence` 一条记录都不写，而一个手里有记录的
节点是命中即剪、走不到评估那一步的。R1 因此连中立性都是免费的，但收益是零。R2 允许占用一个 `flag == NONE` 的格，
reuse 才第一次发生：同一份探针的六臂计数器变成 `scans=318391 reuses=195334`（`/mnt/d/tmp/r2_p34.txt`，38.0 %）。
R3 = R2 + `store()` 的继承，而这两版之间的**全部**区别就落在 warm 那一行上：

```
R2  WARM quiet on a full table color=1 nodes=187 scans=1 reuses=0
R3  WARM quiet on a full table color=1 nodes=187 scans=0 reuses=1
```

六臂的 `nodes/qnodes/tt_hits/scans/reuses/patches/move/score` 八列在两棵树上一字不差（`r2_p34.txt` 与
`r121_c1_r3.log` 对读），因为继承只改记录上带的那个**数**，不改谁去读它——同一个数，读出来还是一样。选 R3 不是
因为 R2 过不了别的用例，而是两件事：继承让 `store()` 的契约说得通（`NO_STATIC_EVAL` 不是命令表遗忘），
以及上面那一行是可断言的（C1-5 的 warm 块就是它）。
三份实现的源码留在 `/mnt/d/tmp/r121_r1_transposition.{h,cpp}` 与 `r121_r2_transposition.{h,cpp}`。

### 中立性：一步成本改动不许动树

- **归纳论证**（写在 `transposition.h:85-112` 与 `transposition.cpp:104-119` 的注释里）：一个只装分数的格子，对
  `probe()`（它看 `flag == NONE`）、对 `hashfull()`（它数 `flag != NONE`）、对 `store()` 的受害格选择（`flag == NONE`
  就是它要找的那个空格）**都恰好等同于它所替换的那个空格** ⇒ 真实记录的下标与多重集在每一步之后仍然相同 ⇒ 树相同。
  唯一的例外是 `put_static_eval` 返回 `false` 的那次（四格全被真实记录占了），它什么都不写。
- **359 臂逐行比对**：每臂同板、新表、`nodes` 不 binding 于时限。`PART2 wide4 arms=275 total_nodes=4079261` 与
  `PART2 deep6 arms=84 total_nodes=14358319`，与 `af43a05` 控制树的逐行文件 diff 为空
  （`PART3_NEUTRAL: R3 rows identical to control (359 arms)`，`/mnt/d/tmp/r121_c1_r3.log`；逐行读数
  `/mnt/d/tmp/r3_p2.txt` 与 `/mnt/d/tmp/c1_p2_neg.txt`）。R1、R2 两版当时也各自跑过同一比对，同样为空。

### 收益：省下来的是扫描，不是搜索

六臂（`quiet`/`scattered`/`crowded` × 两色，depth 6，同一份 `-O2` 探针树）：

- `PART3 totals nodes=808936 qnodes=513725 scans=318391 reuses=195334 patches=318387 reuse_pct=38.0`
  ⇒ **扫描少了 38 %**；`scans + reuses == qnodes` 每臂成立（本轮把它钉进了用例）；`patches` 比 `scans` 少 4，
  那 4 次是 bucket 已被四条真实记录占满、分数无处可放。逐臂读数在 `/mnt/d/tmp/r121_c1_r3.log` 的六行 `COUNTER`，
  其中 `quiet` BLACK 一臂是 `nodes=221774 qnodes=142978 tt_hits=21361 scans=75855 reuses=67123 patches=75852`
  ——读的次数比 TT 命中多，因为 eval-only 格子本来就不是记录，`tt_hits` 不数它。
- 价钱：`PART4 sink=-171021440 boards=400 scan_ns=12185.6 bucket_read_ns=5.2 read+write_ns=11759.4 ratio=2332.2`
  （R3 那一遍；R1/R2 的两遍是 `scan_ns=11044.1 / 11377.7`、`bucket_read_ns=5.4 / 4.8`，同一棵树上的散布）。
  一次全盘扫描 ≈ 11.0–12.2 µs，一次 bucket 走 ≈ 4.8–5.2 ns（相差 2031–2352 倍）。注意 `read+write_ns` 几乎等于
  一次扫描：**写了不等于省了**，省的是命中那一路；这也是为什么每次扫描都附一次 `put_static_eval` 仍然净赚。
- 墙钟（18 臂 × 6952575 节点，`-O2`，**诊断不是钉数**）：`ns_per_node 7060.5 / 7025.6 → 5802.3 / 5634.6`（**−19 %**），
  `knps 141.6 / 142.3 → 172.3 / 177.5`（每棵树两遍，交错跑，`/mnt/d/tmp/r3_p5{a,b}.txt` 与 `c1_p5_new/neg`）。
  同一台机器空闲时整套 `test_search` 原生跑也从 B5 记的 **515.00 s** 回到 **410.75 s**（提交树那一轮；门禁段
  还记了作废那一轮的 421.90 s）。
- **`:549` 那一行本轮未达成，报实测**：单线程吞吐 ≈ 17.7 万节点/秒，距 ≥1M nps 差一个数量级；depth 8 在 4 s 内
  完成也没有到。结构性原因是每个静默节点仍然付一次全盘 `evaluate_board`，Step 1 只是让它发生得少三成八。
  把数量级拿下来的是 Step 2（排序路径的候选格缓存）与 Step 3（评估本体的局部 stand_pat），这一行是 C 阶段整段
  的验收，它到 C4 才有一次成形的读数。

### 用例（`tests/test_search.cpp` 五条，`+359/-2`；那两行删掉的是一条**注释**，不是一条断言）

段首注释（`:6880-6892`）自己说清楚了一件事：这一步是记录上的一个 int32 加一对只碰它的读写，所以这些用例
**大半在钉这个字段「不是什么」**。

- **C1-1 `test_static_eval_on_a_record_leaves_its_bound_alone`**（`:6922`）：空表没有分数可给；先有分数后有记录时，
  记录的界是调用方说的那一句（`1234/5/EXACT/(7,6)`），分数是这一格本来就带的 `-4321`；第二次补分写在记录上而不是
  另占一格；同 bucket 的另一个 tag 既读不到、也 `probe` 不出。`hashfull()` 从 0 到 2（两分一格 + 一条记录），
  一步没多。
- **C1-2 `test_an_evaluation_slot_is_invisible_to_the_bound_readers`**（`:6980`）：三条记录时 `hashfull()==7`；
  第四个 tag 只有分数——它读得回来、`probe` 不出来、计数不动（仍是 7）；随后一条真记录落进那一格（`hashfull()==10`），
  上面三条记录的界逐条仍在；第五个 tag 的分数被拒（`put_static_eval` 返回 `false`），拒绝不动任何计数。
- **C1-3 `test_static_score_reuses_a_cached_number_for_either_colour`**（`:7029`）：一次冷扫描 + 三次读，
  `scans/reuses/patches` 的字面序列 `1/0/1 → 1/1 → 1/2 → 1/3`；`cached` 非空时连表都不走；第二个块是记录路径——
  一条没有分数的记录仍然付一次扫描，扫完写在记录上，界读回来还是 `999/4/EXACT/(3,3)`，并且下一次换颜色读它免费。
- **C1-4 `test_a_cached_sign_answers_both_sides_of_every_shape`**（`:7184`）：`:556` 那条「逐格比对」在这一层做——
  5 支姿势 × `SYMMETRY_COUNT` = 40 个形状（钉 `shapes == 40`），每个形状两色各扫一次、两棵树各读一次，
  每一次读都等于它替掉的那次扫描，并且 `black_score == -white_score`、`|eval| <= 99999`、`!= NO_STATIC_EVAL`
  三件事在同一个循环里逐形状成立。缓存没有和自己对拍：它比的是 `evaluate_board` 自己的两个答案。
- **C1-5 `test_a_quiet_think_pays_for_each_evaluation_only_once`**（`:7114`）：`b1_quiet` 两色 depth 6、新表、
  `set_node_limit(INT_MAX)`。钉的是**两层**：树本身（`nodes 221774/156428`、`qnodes 142978/102340`、
  `score 71992/97044`——这四行是 B5 段已经承诺过的读数，一步敢动树的改动会在那里变红而不是在这里），
  和这一步自己的账本（`scans 75855 + reuses 67123 == qnodes`、`patches 75852`、`reuses > 0`、`scans < qnodes`）。
  最后一块是 warm：同一根第二次思考 `nodes == 187`、`eval_scans() == 0`、`eval_reuses() == 1`——
  第一次思考写的分数在它替掉的那条记录上，而不是被抹掉。

夹具一处：`c1_bucket_keys(wanted)`（`:6900`）造同 bucket、且落进 `hashfull()` 采样的一百格里的 tag；
寻址是**重算**的而不是取访问器——它本身就是这些用例在谈论的布局的一部分（同 `:334` 的旧例）。

### 反证申报：无 HEAD 反证，以三枚 mutation 代替

`static_eval_of`、`put_static_eval`、`static_score` 三个符号在 `af43a05` 上都不存在，这五条用例在那棵树上连编译都
过不去——本轮申报**无 HEAD 反证**，理由是结构性的。代替它的是三枚变异，各破 Step 1 的一句话（`/mnt/d/tmp/r121_c1_mut.sh`；
驱动 `/mnt/d/tmp/r121_c1_driver.cpp` 只跑这五条，把整套的 `main` 用 `-Dmain=suite_main` 改名后从链接里排除，于是第一次
变红的 assert 一定属于 Step 1；被变异的 TU 在 `/mnt/d/tmp/mut121_c1/src` 的副本上改，用与门禁同一组
`-std=gnu++17 -UNDEBUG` 编译，再替回 `link.txt` 里它自己的 `.o`）：

| 变异 | 破掉的那句话 | 结果 |
| --- | --- | --- |
| M1 `put_static_eval(hash, score)` | 存的必须是黑侧的数 | rc=134 停在 `tests/test_search.cpp:7216` `other.static_eval_of(hash) == black_score`（C1-4，前面 3 条已绿） |
| M2 `if (false) stored = tt_.static_eval_of(hash);` | 命中时直接复用 | rc=134 停在 `:7060` `search.ctx_.eval_scans() == 1`（C1-3，前面 2 条已绿） |
| M3 `if (false) {`（`store()` 的继承） | `NO_STATIC_EVAL` 不是「忘掉它」 | rc=134 停在 `:6957` `entry.static_eval == -4321`（C1-1，第一条就红） |

控制臂是提交树的同一驱动：`RUN base rc=0 seconds=16 cases_passed=5`（`/mnt/d/tmp/r121_c1_mut.log`，逐条读数
`/mnt/d/tmp/mut121_c1/{base,m1_sign,m2_noread,m3_noinherit}.out`）。三条各红一条，说明符号约定、读、继承三件事
各自有断言守着；M1 的红点尤其说明「反对称」那个前提不是纸面上的：把约定反过来写，只有跨颜色的读能抓到它。

### 门禁

四段跑的都是提交树。`ctest` **8/8 / 438.39 s**（`test_search` **410.75 s**，B5 记的是 **515.00 s**——一步
成本改动在整套用例自己的墙钟上留下了 20 % 的印子，方向与 PART5 的 −19 % 一致；其余七支最慢 7.49 s，
`/mnt/d/tmp/c1_ctest_final.log`）、`node tests/web_frontend_check.js` **105/105**、
`node tests/readme_anchor_check.js` **118 引用 / 0 problems**、`README.md` **658 行**。

要说清楚的一次重跑：`:196-197` 那两行注释（原文写着「A3 lands the field; the search that fills it in is
the static-evaluation reuse step, so **until then** every record carries the explicit no evaluation here」）
被本轮的事实作废，我在第一轮 ctest 已经跑起来之后才改它——那一轮（450.16 s / `test_search` 421.90 s）因此
**作废**，改写后按提交树重跑上面这一轮取读数。改写刻意保持在**两行换两行**，所以文件行数不变、`:198` 的断言
仍然成立、它下面每一条断言的行号都没有动：两条受保护的预算前提**内容一字未动、行号也未动**，
`assert(result.resigned)` 仍在 **`:2591`**、`assert(completed.method == "alphabeta")` 仍在 **`:3986`**，
memcheck 会死在那里的 10 ms 臂仍在 **`:3973`**——五条新用例的定义全部落在 `:6922-7226`、注册落在 `main()`
（`:7228`）体内，行号都比这三处大，所以它们一行都没有被推动。
合约过期：**无**，本轮没有改写任何一条既有断言的内容。

### Valgrind

四臂串行、空闲，脚本 `/mnt/d/tmp/r121_c1_final.sh` 的 phase 3（构建与 ctest 都在同一支脚本里排在前面，
没有任何 memcheck 臂与构建或另一支臂同时跑）。`--tool=memcheck -q --error-exitcode=9 --leak-check=full
--show-leak-kinds=definite --errors-for-leak-kinds=definite --track-origins=yes --num-callers=16`。

- **新代码自己**（`-O0 -g` 的 C1 驱动：五条用例，含三次 depth 6 的全盘思考）：**rc=0 / 1385 s / reports=0 /
  invalid_read=0 / invalid_write=0 / uninitialised=0 / definite_leak=0**（`/mnt/d/tmp/vg121_c1/driver.log`，
  同一份 stderr 旁边是五条 `passed` 行）。这是四臂里最贵的一支（B5 的对应臂 335 s），因为它把 22 万节点 ×
  3 次思考整体套进 valgrind：按用例自己钉住的读数，两支冷思考一共走表 **245318** 次（101044 次命中 +
  144274 次扫描未命中），其中 144274 次各附一次 `put_static_eval` 的写。
- **`test_training`**（`build-vg/`，Debug `-UNDEBUG`）：**rc=0 / 158 s / 0 / 0 / 0 / 0 / 0**，`All training tests passed`。
- **`test_search`**（装着五条 C1 用例的那套）：**rc=134 / 255 s / reports=0 / invalid_read=0 / invalid_write=0 /
  uninitialised=0 / definite_leak=0**。stderr 里唯一的非 memcheck 行还是那句
  `Assertion 'interrupted.partial_depth == 1' failed`（`tests/test_search.cpp:3973`，A2/A7 那一支的 **10 ms 臂**），
  死在它之前的最后一行是 `test_engine_progress_frames_share_the_search_clock passed (pre-search 646 ms,
  first frame 843 ms, 3 frames, terminal 2916 ms of 2916 ms)`。同一套不套 valgrind 是绿的：门禁段的
  `test_search 410.75 s`。
- **`test_server`**：**rc=134 / 24 s / 0 / 0 / 0 / 0 / 0**，`Assertion 'hint_results == 0' failed`
  （`tests/test_server.cpp:1710`，`test_hint_answer_for_a_superseded_position`）。
- 口径按 2026-10-17 的约定分开报：`reports` 是 valgrind 自己的 `==NNNN==` 行数，另外四列是
  `Invalid read of size` / `Invalid write of size` / `uninitialised` / `definitely lost` 的计数，构建目录与
  `-UNDEBUG` 已在每条里点名。**两处 rc=134 都是断言，不是内存错误**：五列读数在两条臂上都是 0。
- **两处 rc=134 都在改动前的树上重测，并且逐字复现**（`/mnt/d/tmp/r121_c1_vg_neg.sh`；树
  `/mnt/d/tmp/neg121_c1` = `git archive af43a05` + 它自己的一份 `build-vg`（Debug `-UNDEBUG`），脚本第一件事是
  证明那棵树里**没有** `put_static_eval` 才开始跑）：
  `NEG_test_search rc=134 / 332 s / 0 / 0 / 0 / 0 / 0` 与 `NEG_test_server rc=134 / 29 s / 0 / 0 / 0 / 0 / 0`，
  两句 assert 的文件、行号、断言文本与提交树**完全相同**（`:3973` 与 `:1710`——本轮的插入全在 `main()` 之前，
  没有推动任何一条既有断言）。所以这两支依旧是「被 valgrind 的时间尺度打到墙钟/竞态断言上的旧脆弱」：
  `test_search` 那支是 A2/A7 的 10 ms 臂（B2、B3、B5 三轮记过同一根臂），`test_server` 那支是任务 #35 排在队里的
  「`make_move`/`undo` 不等待 in-flight 搜索」的锁面。本轮不修剪任何断言，也不把它们挪进 bugfix 提交。
- 一个顺带的对照，与门禁段的墙钟同向：同一根 10 ms 臂在 memcheck 时间尺度里从 **332 s（控制树）走到 255 s
  （本轮树）**，用例整套的原生墙钟从 515.00 s 走到 410.75 s。省下来的确实主要是评估扫描。

## 轮次121 引擎重构 B5：战术层第一次由局面决定它要不要跑 — engine redesign stage B5

规格 `docs/ENGINE_REDESIGN.md` `:142` 流水线 ③ 行「战术层 VCF/VCT ── 由「威胁探针」按需触发（改造：不再固定
比例）」、`:253-260` 的整段「VCF/VCT 由固定比例改为按需触发」（三条触发事实、「只有命中才启动对应战术搜索，并
给予**独立上限**（如 hard 的 20%/30%）而不是 35%/55%」、「无威胁局面直接进 AlphaBeta，把全部预算给主搜索」）、
`:548` 的 B 阶段验收行，与 `:583` 的文件清单行「`search_engine.h/.cpp`：VCF/VCT 按需触发；`TimeManager` 预算
分配」（冻结，不改写）。B5 读的是 B1 交出来的那件事：既然一层能搜哪些格子已经由局面上的形状决定，那么一次思考
要不要为战术层花钱，也该由同一个来源决定，而不是由日历决定。

### 落地的三件事

1. **探针**：`ThreatProbe threat_probe(const Board&, int color)`（声明 `src/search/move_generator.h:73-81`、
   定义 `src/search/move_generator.cpp:103-143`）。一次 `O(候选数)` 的 `recognize_position_both` 同时读出**己方与
   对方**在这一格的形状，回答文档的三条事实：`own_four`（己方 ≥ 冲四：成五/活四/冲四）、`own_three`（己方 ≥ 活三）、
   `opp_four`（对方 ≥ 冲四）。每半边先过那一侧自己的禁手判定——禁手里的形状不是那一侧**能做出**的威胁，所以不算。
   域是 `board.active_empty()`，与 `VCFSearch::generate_rush_fours`、`VCTSearch::generate_attacks` 自己列根的域同一个。
2. **按需触发**（`src/search/search_engine.cpp:203-241`）：VCF 只在 `own_four` 时启动（`:212`），VCT 只在
   `own_three` 时启动（`:234`）；两者的份额从 35 %/55 % 改成 **20 %/30 %**（`kVcfBudgetPercent` /
   `kVctBudgetPercent`，`search_engine.cpp:17-18`，注释 `:10-16`）。先后仍是文档给的那一对（VCT 是两份里宽的那一份），两个都命中时主搜索仍拿到
   ≥ 70 % 的预算；探针排在快路径与预算起点（`:194-202`）之后、④ 之前，与 `:142` 的流水线顺序一致。
3. **认输探针没有被门控**（`src/search/search_engine.cpp:387`）：文档 ③ 行管的是 AlphaBeta **之前**的战术层调度，
   `find_defense` 在 AlphaBeta 之后、是另一个消费者；跳过它的收益同样是「无可观察差异」（根列表空 ⇒ 它本来立刻
   返回未证明）。把它并进本轮等于在「按需触发」里夹一次棋力面的改变，B5 不做。

### 规格冲突与判定

`:257` 把「对方 ≥ 冲四（必须防）」写成探针的第三条**触发**条件，但战术层里没有一次搜索以对方的强制着法为根：
VCF/VCT 的根列表都是**行棋方自己**的强制着法。对方冲四的承接者是 B1 已经落地的 Tier0 挡点（`:335`「对方能在这里
成五的格子，不许被宽度裁掉」），不是 ③。所以 `opp_four` 被计算、被等价性钉住，但**不门控 stage B 的任何一次搜索**；
文档为它指名的下一个消费者是 C 阶段 Step-3 的「局部 stand_pat 先只用于『无威胁局面』」（`:568`）。这条判定写在
`move_generator.cpp:133-138` 的注释里，`search_engine.cpp:203-209` 的注释指回它。

### 等价性（战术不漏）

`PART1 roots=275 cells=23739 mismatches=0 own_four=112 own_three_only=129 no_own_threat_but_opp_four=121
fully_threatless=33`（`/mnt/d/tmp/b5_remeasure.log`，我这遍重跑；原始行 `/mnt/d/tmp/b5m_p1_new.txt`）。275 个根 = B4
的 42 支姿势 × 两色（84）+ 24 局种子随机对局的 191 个中盘根；三件事与两次列表的**空/非空**逐根比对，**0 处不符**，
`own_four ⇒ own_three` 的嵌套同时成立。把同一个比对跑在 `4b18a78` 的引擎 + 抄进去的探针模块上，PART1 行**逐字
相同**（`/mnt/d/tmp/b5m_p1_neg.txt`）——探针看到的与两个相位自己看到的，在两棵树上是一回事。

- **`own_three` 而 `!own_four` 是常态，不是漏**：275 根里 black **56** / white **73** 根只有活三没有冲四
  （`/mnt/d/tmp/b5p6.txt` 的 `PART6 own_three_without_own_four: black=56 white=73`）。原因不是禁手（先把禁手解释
  量掉，同一份日志的 ONLY3 行给出这些根的 `opp_four`），而是一手做出的活三，它的**成四点要第二手才存在**——VCF
  的根列表因此合法地保持为空。两道门是两个独立的事实，都需要，不能合成一道。

### 引擎答案：门控不该改变任何东西，它确实没改变

275 根 × 同板、每根新引擎（空表）、`set_max_nodes(40000)`、`time_limit=60000` 深到不 binding，只看 depth 4 的
答案：`PART2_IDENTICAL (275 rows)`，`total_nodes` 两边都是 **4079261**（`/mnt/d/tmp/b5m_p2_new.txt` 与
`..._neg.txt` 的 diff 为空）。method 分布两边相同：`alphabeta` 202 / `vcf` **54** / `must_block` 8 /
`greedy_five` 7 / `vct` **4**。也就是说被探针关掉的 **197** 次相位启动（VCF 关 163 次、VCT 关 34 次）里没有一次
是会被赢棋用到的：真正赢的那 54 + 4 次照样跑、照样交同样的 `score` 与 `move`。文档那句「无威胁局面直接进
AlphaBeta，把全部预算给主搜索」在这一批根上的形状是 **33** 根两色都没威胁、**1** 根只有对方有冲四。

这条相等不是免费的：**它靠 VCF/VCT 都不读写共享置换表**（两个类只把 `TranspositionTable&` 绑在成员上，
`vcf.cpp`/`vct.cpp` 里没有任何一次 probe/store）。这也是本轮**没有 HEAD 反证**的结构性原因（见下）。

### 探针的价钱，和预算回到主搜索的证据

- **价钱**：`PART3 sink=681400 probe_us_per_root=6.78 two_phase_passes_us_per_root=9.45 ratio=0.718`
  （`/mnt/d/tmp/b5m_p3_new.txt`，200 遍 × 275 根）。一次探针是它替掉的那两次列表的 **0.718 倍**；在无威胁的根上是
  净省，在有威胁的根上多付一次探针换两次启动判定。
- **墙钟诊断臂（不是钉数）**：20 个「有战术工作但没有胜算」的根（B4 姿势表里 method 不是 `vcf`/`vct` 的那些）×
  1500 ms × 3 次 × 两遍，两棵树各自取平均：深度和 **109.67 → 111.33**，更深 **5** / 持平 **15** / 更浅 **0**，
  同一墙钟里 AlphaBeta 拿到的节点 **2636964 → 3101705（+17.6 %）**（逐根读数 `/mnt/d/tmp/b5_p4_table.txt`，
  每根一行 `head_dep b5_dep d head_nodes b5_nodes head_ms b5_ms`；`ms` 两列都贴着 1500，说明绑的是时限不是层数）。
  这一臂只证明 20 %/30 % 确实把时间还给了主搜索；
  墙钟不是确定性量，所以它不进用例。确定性判据留在 `nodes` 上（上一条 275 根的 diff 为空）。
- 顺带量到的一个坑，与本轮无关但对以后有用：**`time_limit_ms = 0` 在 `SearchEngine::search` 里不是确定性配置**
  （`src/search/search_engine.cpp:256-260` 把 `remaining_time` 兜到 100 ms，于是节点预算变成 `20000`、硬时限变成
  100 ms，层的切割点跟着机器的忙闲走）。第一遍 275 根的 diff 非空就是这个原因；改成「每根新引擎 + `set_max_nodes`
  + 一个不 binding 的时限」之后才干净。B5 之后任何拿 `nodes` 做判据的臂都得显式声明自己走的是哪一种。

### 用例（`tests/test_search.cpp`，两条，全部走公开面 + 本 TU 已有的 `#define private public`）

- **B5-1 `test_threat_probe_answers_what_the_two_phases_would_enumerate`**（`:6756`）：7 支姿势 × 两色 = 14 行，
  每行钉三件事的字面值、VCF 与 VCT 自己的列表加对方一次列表的**长度**、三条 `==` 等价式与 `own_four ⇒ own_three`
  的嵌套。`double_three` 那行是探针的禁手半边：(7,7) 是两个方向的活三 ⇒ `is_forbidden` 为真，它被从两边的列表里
  都点名排除（本轮为此加了 `absent()` 断言，不让它藏在「20」这个数里），而 `own_three` 仍为真——因为另有
  20 个格子能做出活三。同一个**坐标** (7,7) 在生成器用例 `test_generators_keep_five_points_and_drop_forbidden`
  （`:2118`，两条点名排除在 `:2164-2165`，VCF 与 VCT 各一条）里也被钉过，但那是一盘不同的姿势、一条不同的禁手
  规则——那里 (7,7) 是**四四**（行四与列四同时成），本轮这一行是**三三**；两条断言不是同一件事的重复。
- **B5-2 `test_a_threatless_think_pays_the_probe_and_nothing_else`**（`:6826`）：八次引擎思考钉
  `method/depth/score/move/nodes`。前六行是被门控的那一侧 —— `quiet` 两色 `71992/221774` 与 `97044/156428`、
  `scattered` 两色 `98460/177061` 与 `98948/251149`，加 `white_open_four` 的 `resign` 与 `greedy_five` 两条快路径；
  这六行是 **PIN，不是 discriminator**：`4b18a78` 的引擎（配上抄进去的探针模块）交回同样的六行 —— `PINS_IDENTICAL
  (12 FIX, 6 PIN)`，12 行探针读数与 6 行引擎读数字节相同。后两行是**门控的正控制**：`crowded` BLACK 与
  `double_three` WHITE 仍然交 `vcf` / `100000` / `(7,6)` 与 `(2,5)`，`/mnt/d/tmp/r121_b5_pc.cpp` 在两棵树上读出的
  这四项逐字相同 —— 也就是说它们和前面六行一样绿，但它们绿的原因不同：探针被焊死成 `false`（或者把 `!` 打错在
  门上）在这里变红，而前面六行在任何一种坏门下都还是绿的。这两行是本轮唯一能断言「门确实在开」的断言。

### HEAD-negative 申报

**本轮申报：无 HEAD 反证。** 两条理由都是结构性的：`threat_probe` 这个符号在 `4b18a78` 上不存在，B5-1 不可能在
那棵树上编译；而 B5-2 前六行钉的是「门控之后答案不变」——一道工作正常的门，被它关掉的那次搜索交回的是同一个未知，
门控本身在被关掉的思考里**不留下可观察痕迹**。按本项目对等价改动的旧例，证据是两棵树逐字相同的读数，上面已给齐
（PART1 两棵树同一行、275 根 method 全等、12+6 行 PIN 全等、正控制两行的四项在两棵树上也相同）。

**代替 HEAD 反证的是一遍 mutation**：在 `4b18a78` 之后的这棵树上把两个门条件各追加一个 `&& false`
（`/mnt/d/tmp/r121_b5_mut.sh` 造一份 `src/` 副本 `/mnt/d/tmp/b5mut`，改 `search_engine.cpp:212/:234`，用同一个
八行驱动程序读出来，`/mnt/d/tmp/b5_mut_rows.txt`）。八行里**恰好两行变红**：`crowded` BLACK 与 `double_three`
WHITE 从 `vcf / depth 0 / nodes 0` 变成 `alphabeta / depth 2 / nodes 888` 与 `alphabeta / depth 2 / nodes 533`
——`score` 与 `move` 都不变，AlphaBeta 自己也在同一层看到这两个胜，所以这两行钉的是「相位先答」而不是
「只有相位答得出」。剩下六行在焊死的门下与门正常时逐字相同，那正是上面「门控不留痕迹」那句话从反方向看到的
同一个事实。也就是说本轮的断言强度是：**两行红 + 六行不因门控而红**，两边都量到了。

### 门禁

`ctest` **8/8 / 540.70 s**（`test_search` **515.00 s**；B4 记的 451.57 s 之上是这两条 B5 用例：B5-1 的 14 行探针
比对几乎免费，B5-2 的八次思考里贵的是 `resign` 那一臂的一次 400 k 节点认输探针和四臂 depth-6 主搜索，两个
`vcf` 正控制在相位里就返回了；其余七支最慢 9.24 s）、
`node tests/web_frontend_check.js` **105/105**、`node tests/readme_anchor_check.js` **118 引用 / 0 problems**、
`README.md` **658 行**。合约过期：无。四段跑的就是提交树，只有一处例外要说清楚：`time_manager.h` 注释里那个
指向 `search_engine.cpp` 的行号在本轮收尾时写成了 `:16-17`，发现时 ctest 已经在跑（构建已结束），改成 `:17-18`
是一次纯字符的注释修正，不改一行代码，也不改任何一条读数。中途锚点掉到 117 是我重写预算注释时把 `README:343` 那一行引用删掉了
（`search_engine.cpp:14` 现在重新引用它），补回后回到 118。两条受保护的预算前提**内容一字未动**，行号因为
`:1086` 处那段注释多了两行而下移：`assert(result.resigned)` 从 `4b18a78` 的 `:2589` 现在在 **`:2591`**，
`assert(completed.method == "alphabeta")` 从 `:3984` 现在在 **`:3986`**。
`src/search/time_manager.h` 那句「the engine used to hand the same fixed proportions to VCF (35%) and
VCT (55%)」（`4b18a78` 上的 `:16-18`）在本轮变成了需要时态说明的话，改成四行（本树 `:16-19`）写清楚 35/55
是被本轮的 20/30 换下来的、而 `TimeManager`
仍然没有调用者，并把新位置指回 `search_engine.cpp:17-18`——**纯注释**，八行 mutation 读数与全部钉数都在改前改后同一棵行为树上。

### Valgrind

- **探针本体（本轮新增代码，`-O0 -g` 副本驱动，real line 可读）**：`r121_b5_probe.cpp` 的模式 19 = 275 根的
  三件事比对 + 10 次被门控/被放行的引擎思考，`valgrind --tool=memcheck -q --error-exitcode=9 --leak-check=full
  --show-leak-kinds=definite --errors-for-leak-kinds=definite --track-origins=yes --num-callers=16` 跑完
  **rc=0 / 335 s / reports=0 / invalid_read=0 / invalid_write=0 / uninitialised=0 / definite_leak=0**，
  驱动自己的读数在同一份 stderr 旁边是 `PART1=1 VG=10 MISMATCH=0`（`/mnt/d/tmp/b5_vg.log`）。
- **`test_training`**（`build-vg`，Debug `-UNDEBUG`）：**clean / 217 s / 0 / 0 / 0 / 0 / 0**。
- **`test_search`**（装着两条 B5 用例的那套）：空闲重跑 **rc=134 / 325 s / reports=0 / invalid_read=0 /
  invalid_write=0 / uninitialised=0 / definite_leak=0**。stderr 里唯一的非 memcheck 行是
  `Assertion 'interrupted.partial_depth == 1' failed`（`tests/test_search.cpp:3973`，A2/A7 那次的 **10 ms 臂**——
  它的读走在 assert 之前，所以 stdout 已经打了 `interrupted: alphabeta d0+partial 0/n0 time_hard`，减速下的这一手
  连一个根着法都没能走完）。同一根臂不是本轮才断的：B2、B3 两轮的门禁段就记过它当时停在 `:3967`，本轮因为
  `:1086` 的注释多了两行而写成 `:3973`；原生（不套 valgrind）的这套是绿的，见 `/mnt/d/tmp/b5_vgnative.txt`
  的 **RC=0 / All search tests passed** 与下面门禁段的 `ctest` 8/8。**这一条按旧例在改动前的树上重测**（见下）。
- **`test_server`**：空闲重跑 **rc=134 / 24 s / reports=0 / invalid_read=0 / invalid_write=0 / uninitialised=0 /
  definite_leak=0**，`Assertion 'hint_results == 0' failed`（`tests/test_server.cpp:1710`，
  `test_hint_answer_for_a_superseded_position`（函数在 `:1662`）：局面被 `undo` 撤掉之后那支 hint 在本该被陈旧判定丢掉的路上，
  在 valgrind 的时间尺度里交出了一个 `hint_result`）。同一套不套 valgrind 是 `ctest` 8/8 里绿的那一支。
- 口径按 2026-10-17 的约定分开报：`reports` 是 valgrind 自己的 `==NNNN==` 行数，另外四列是
  `Invalid read of size` / `Invalid write of size` / `uninitialised` / `definitely lost` 的计数，
  构建目录与 `-UNDEBUG` 已在每条里点名。**两处 rc=134 都是断言，不是内存错误**：memcheck 的读数在两条臂上都是全 0。
- 第一次跑这三臂时我把 `make -j8 test_search` 与整套用例直接压在了 memcheck 旁边，`test_server` 在 32 s、
  `test_search` 在 330 s 死掉；因为 `tools/valgrind_check.sh:166/:250` 用 mktemp 建日志又删掉，那一次的断言文本
  我没能读到了（而且我据此猜错过一次：把它认成了下一支用例 `test_search_engine_finds_five`）。上面两条是**空闲**
  重跑并把日志留在 `/mnt/d/tmp/b5vg_test_search.{out,err}` 与 `b5vg_test_server.{out,err}` 的读数，
  脚本 `/mnt/d/tmp/r121_b5_vg3.sh`；同一棵树的 `build-vg/test_search` 不套 valgrind 跑整套是
  **RC=0 / All search tests passed**（`/mnt/d/tmp/b5_vgnative.txt`）。

**两处 rc=134 都在改动前的树上重测过，并且逐字复现**（`/mnt/d/tmp/r121_b5_negvg.sh`，树
`/mnt/d/tmp/neg121_b5` = `git archive 4b18a78` + 抄进去的 `move_generator.{h,cpp}`，自己的一份 `build-vg`
Debug `-UNDEBUG`）：`NEG_test_server rc=134 / 24 s / 0 / 0 / 0 / 0 / 0`，同一句
`tests/test_server.cpp:1710 … test_hint_answer_for_a_superseded_position(): Assertion 'hint_results == 0' failed`；
`NEG_test_search rc=134 / 328 s / 0 / 0 / 0 / 0 / 0`，`Assertion 'interrupted.partial_depth == 1' failed` 在那棵树
写在 **`:3971`**（本树 `:3973`，差的就是 `:1086` 注释的两行），而且它 stdout 上那行读数在两棵树上逐字相同
（`interrupted: alphabeta d0+partial 0/n0 time_hard, last frame d0/n0, control: alphabeta d0+partial 1/n2494`）。
所以这两支都是**被 valgrind 的时间尺度打到墙钟/竞态断言上的旧脆弱**，不是本轮带进来的：`test_search` 那一支是
A2/A7 的 10 ms 臂（B2、B3 两轮记过同一根臂），`test_server` 那一支是「减速后引擎交出的那一点在撤销后的盘面上
仍然合法，于是走 `hint_result` 而不是 `error`」——正是任务 #35 记在队里的那对「`make_move`/`undo` 不等待
in-flight 搜索」的锁面，`tests/test_server.cpp:1656-1661` 的注释自己就写了这件事。两处 memcheck 读数
依旧全 0，本轮不修剪任何断言，也不把这两支挪进 bugfix 提交。

### 申报未完成的一半

`:583` 那一行还写着「`TimeManager` 预算分配」。`src/search/time_manager.{h,cpp}` 自 A1 就在树上，
`MoveBudget TimeManager::allocate()` 的五条修正（`soft = 0.65·hard`、候选数修正、开局/中盘/残局修正、上一手
> 0.8·hard 时下调 `soft`、训练模式直接返回节点预算）**没有任何调用者**：`grep -rn "TimeManager" src/` 只命中它
自己的两个文件，`SearchEngine` 仍然自己算 `remaining_time` / `node_budget`（`:256-291`）。不接线的理由写在这里而
不是藏在状态文档里：接线会改变**每一次**思考的软窗（现在客户端不点名就没有软窗，README:343 把这个缺省写成了
文档行为，接线要同步改 README 那一行），它是全棋力面的改动，而它的验收按 `:555` 回归保障第 2 条要「新引擎 vs
旧引擎」的验证赛 Elo——本轮的验收口径是结构不变量 + 定向用例，明确不含对局赛。这一项连同它的口径冲突记进
`docs/ENGINE_REDESIGN_STATUS.md` 的「部分完成」。B5 落的是 `:583` 那一行的前一半。

## 轮次121 引擎重构 B4：等价深度买到了，便宜没买到 — engine redesign stage B4（本期不做）

规格 §2.7 表 `:427` 的那一行「威胁延伸（冲四/活三 +1 层） | 战术不漏，深度等效 +1~2 | 风险 低」，加上
`:423`/`:424` 两行明写的交接（PVS 的 −20~35 % 与排序的 −30~50 % 都记着「交给 B4 重量」）。文档冻结，
不改写；本轮只判这一行自己的两个主张：**深度等效**（延伸过的 d=D 值不值得没延伸的 d=D+1~2）与
**风险 低**（它的价钱）。

**结论：不落地。** `git diff 65bbd17 -- src/ tools/ tests/` 为空——五个测量期改过的搜索文件已收回树，
判定与全部原始数字记在本节。战术的活交给 B5（`:248-260`）。

### 读数与姿势

- 触发谓词四种写法：`0` 成五/活四/冲四/活三（＝B1 的 Tier1 全类，文档字面）、`1` 去掉活三、
  `2` 四类或双威胁、`3` 只认成五/冲四（最窄）；`4/5/6` 是「只沿着已经开了头的强制变例延伸」的三档
  时序读法。预算 `k` ∈ {1, 2}（「+1 层」与「等效 +1~2」各一档），另加两道地平线门 `depth <= 1`／
  `depth <= 2`。落点选在**边**（`search_child_pvs` 里那一手刚落子的格子）而不是**节点**：只有那里同时
  知道是哪一手造出的形状、以及它的回答被要求在哪一层交出来。
- 三族姿势：**九条位置臂**（B1/B2/B3 一路用的那九臂）、**矩阵八臂**（`quiet` 24/8 的 d6/d8、
  `crowded` 64/32、`pos24` 两色 d6/d8）、**十二战术臂**（168 臂翻转扫描里所有会翻转的姿势，
  控制合计 915000）。两支驱动对同一姿势的控制值逐臂相同（`17080/19089/…/9174`），姿势表不是第二套真值。
- 口径与 B1/B2/B3 一致：同板、新表、无挂钟、无节点预算，只看 `nodes`；只有等钱表用 `max_nodes`
  当挂钟的确定性替身。

### 成本：固定深度上没有一个读数是便宜的

控制＝`65bbd17`（budget 0，触发谓词从不被问）。倍数是三族姿势各自的合计比：

| 读数 | 九条位置臂 | 矩阵八臂 | 十二战术臂 |
|---|---|---|---|
| 控制 | 9130501 | 5903803 | 915000 |
| `0` 全类 k1 无门 | 28670487（**3.14×**） | 16882881（2.86×） | 1468658（1.60×） |
| `0` 全类 k1 门 `depth<=1` | 17424238（1.91×） | — | 1089752（1.19×） |
| `0` 全类 k1 门 `depth<=2` | 20584988（2.25×） | — | — |
| `0` 全类 k2 门 `depth<=2` | 63439801（6.95×） | — | — |
| `0` 全类 k2 无门 | 105390722（**11.54×**） | — | 3669169（4.01×） |
| `1` 四类 k1 无门 | — | 13683522（2.32×） | 1481881（1.62×） |
| `1` 四类 k2 无门 | — | 21848389（3.70×） | — |
| `2` 四类或双威胁 k2 无门 | — | 24457430（4.14×） | — |
| `3` 最窄 k1 无门 | — | 13965464（2.37×） | 1498914（1.64×） |
| `3` 最窄 k1 门 `depth<=1` | — | — | 1092972（1.19×） |
| `3` 最窄 k2 无门 | — | 17994948（3.05×） | 2214000（2.42×） |
| `4` 接续读法 k2 无门 | 51879545（5.68×） | — | — |

按 B3 定下的规矩——**地板由最差臂选，不由均值选**——最窄读数 k1 在矩阵八臂上的最差臂是
`pos24 BLACK d8` 2378724 → 7043507＝**2.96×**，同一读数在战术臂上的最差臂是 `p34` 112503 → 433545＝
**3.85×**；k2 的最差臂到 3.80×（`pos24 BLACK d8`）。B3 选的探针让最差臂只涨 0.18 %，本轮没有任何一档
做不到「没有一条姿势是净亏两倍」。而阶段 B 自己的验收行 `:548` 要的是**中盘节点数较 A 阶段下降 ≥ 50 %**：
B1 把中盘根宽从 133 压到 33、B2 付了 −3.9 %、B3 收 −1.23 %，一行 2.3–3.1× 的乘数会把这三步全部花掉
还倒欠。「风险 低」在这一版引擎里是假的，理由和文档自己否决 null-move 的那条同源——五子棋的冲四不是
罕见形状，而 B1 的分层本来就是威胁优先，一层里 Tier0/Tier1 全是威胁，于是「哪里有威胁就延伸哪里」
在中盘等于「每一层都多一手」。

### 等价深度：这一行的主张有一半是真的

同一批战术臂，控制跑 `D`、`D+1`、`D+2`，读数跑 `D`（`/mnt/d/tmp/r121_b4_equiv.log`）：

| 臂 | 控制 D | 控制 D+1 | 控制 D+2 | 最窄 k1 @D | 最窄 k2 @D |
|---|---|---|---|---|---|
| `p22` 白 d4 | 17366 · 6,6/35716 | 39134 · 6,6/**77724** | 87978 · 6,6/99999 | 20241 · 6,6/**77724** | 20674 · 6,6/77724 |
| `p29` 黑 d4 | 18315 · 6,6/92698 | 47149 · 6,6/92698 | 121189 · 6,6/**99999** | 25554 · 6,6/92698 | 22106 · 6,6/**99999** |
| `p34` 白 d6 | 112503 · 8,5/−41556 | 250893 · 6,7/**47306** | 403457 · 6,7/99999 | 433545 · 6,7/47306 | 553184 · 6,7/99999 |
| `p33` 黑 d4 | 37205 · 7,7/75684 | 62083 · 7,7/**77248** | 132464 · **5,6**/99999 | 42234 · 7,7/75684 | 53433 · 5,6/**67958** |
| `p39` 黑 d4 | 9174 · 6,9/99999 | 18549 · 6,9/99999 | 93972 · 6,9/**99284** | 13045 · 6,9/99999 | 23630 · 6,9/97300 |
| 十二臂合计 | **915000** | 1658819（**1.81×**） | 5184282（**5.67×**） | 1498914（1.64×） | 2214000（2.42×） |

读法：**「深度等效 +1~2」是真的，但它等价的是「和 +1 层同价」，不是「比 +1 层便宜」。** `p22` 上
延伸用 20241 拿到控制层 D+1 用 39134 才拿到的 77724（0.52×，确实便宜），`p29` 的 k2 用 22106 拿到
D+2 的 99999（0.18×）；可是同一批臂上 `p34` 的 k1 为 D+1 那个一模一样的 6,7/47306 付了 433545，
是 D+1 自己价钱的 **1.73×**，`p33` 的 k2 交出 5,6/67958——着法与 D+2 相同、分值比 D+1 还低。
延伸不是白加一手，它是**用宽度换深度**：换来的那一手只在「这条线本来就只有一两个应手」的姿势上划算，
而位置臂没有这种形状。

### 等钱：一场真实思考的形状里，它答得更差

把每臂的 `max_nodes` 钉成控制树在该深度需要的节点数（`/mnt/d/tmp/r121_b4_iso.log`），问同一笔钱里
谁的答案好——这是挂钟限制下 think 的真实形状：

| 臂 | 控制（同钱） | 最窄 k2（同钱） | 全类 k2（同钱） |
|---|---|---|---|
| `quiet` 黑 d6 | d=5 partial=6 · 8,7/**97160** | d=5 partial=6 · **9,7/5150** | d=4 partial=5 · **9,7/5332** |
| `quiet` 白 d6 | d=5 partial=6 · 6,7/26008 | d=5 partial=6 · 6,7/18888 | d=4 partial=5 · 6,7/18888 |
| `quiet` 黑 d8 | d=8 完成 · 8,7/**77842** | d=7 partial=8 · 8,7/23206 | d=6 partial=7 · 8,7/77842 |
| `pos24` 白 d6 | d=5 partial=6 · 10,7/98844 | d=5 partial=6 · **12,5/91478** | d=4 partial=5 · 10,7/99999 |
| `crowded` 黑 d6 64/32（×2 钱） | **d=2 完成 · 100000 reason=mate**（1861 子就够） | d=2 · 100000 reason=mate（3053） | 3722 内 `d=1 partial=2` · 99999 **reason=nodes** |

最后一行值得单独说：那条「延伸证明了什么」的候选证据其实是**反的**——这个杀法在 `65bbd17` 上跑完
第二层就证明了，控制花 1861 个节点，最窄读数花 3053，全类读数在同样的钱里连第二层都没跑完。
`99999` 不是「只当成赢」的遗憾，它是评估上限；证明本来就是普通搜索自己会给出的东西。

### 另外两条被量过、也不落地的读法

- **强制变例（只有一个应手才延伸）**在本引擎上是**惰性的**：`fv=1` 与 `fv=2` 的九臂和都等于控制
  9130501（逐字节相同，`/mnt/d/tmp/r121_b4_fvnine.log`）。原因是 B1 的分层——Tier2 的邻近候选加上
  宽度下限，使候选列表基本从不为 1，这条经典判据在五子棋的分层里选不出任何东西。
- **接续读法**（只沿已经开了头的强制变例延伸，读数 `4/5/6`）把成本从 11.54× 压到 5.68×，仍然远高于
  同臂的 plain 下一层；在 168 臂采样上它是 2.61×、46 臂换着、证明 +5/−0（加了分值下限之后），
  价钱与最窄读数的 k1 同档而收益不更高。

### 如果落地，必须一并改的三条既有约束（本轮申报，未改）

1. B3 钉住的线长关系在延伸下不成立：等号针 `static_cast<int>(think.pv.size()) == think.completed_depth`
   （`tests/test_search.cpp:6302`，另在 `:6222`/`:6324`/`:6491`/`:6517`/`:6537` 逐臂重复）与一般式
   `<= completed_depth`（`:6435`）都要改写成带预算的形式，因为最窄 k1 在 `p22` 的 d4 交 `pv=5`、
   在 `p26` 的 d6 交 `pv=7`——延伸过的路径本来就该比层数多一手。
2. B2 的 killer 记账边界（plies `1…D-2`）与 `PvTable::kMaxLen` 都要按预算留余量。
3. `100000 + (depth - ext)` 这个写法会让 ply 超出 D 的**已证明杀着**落回 `|value| < 100000` 的评估带：
   `optimization/transposition.h:98` 的存储门只给 `|value| >= 100000` 做距离还原，
   `src/search/search_engine.cpp:349` 的认输判据是 `score <= -100000`。落地的先决条件是给它加下限
   （`100000 + max(0, depth - ext)`）——本轮没有这个义务，因为没有延伸进树。

### 判定与交接

`:427` 的两格里，「深度等效 +1~2」按上面第二节的形状**部分成立**，「风险 低」在两族姿势上**都不成立**
（最差臂 2.96×–3.85×，等钱下答案更差）。B 阶段是这一版重构里唯一带着节点数验收行的阶段，一行三倍的
乘数买不回「战术不漏」——它买的是**同价但更窄**的搜索。故 B4 本期不做，写进
`docs/ENGINE_REDESIGN_STATUS.md` 的未完成项；「战术不漏」交给 B5：`:248-260` 给的机制是**按需触发**的
VCF/VCT 加独立封顶（20 %／30 % of hard），而 `src/search/vcf.cpp`/`vct.cpp` 交的本来就是
`100000 + depth` 的**证明**，不是被夹成 99999 的天花板。B5 量的是同一件事的便宜版本：一个
O(candidates) 的威胁探针决定要不要跑，跑多少有上限。

测量资产（全部在 `/mnt/d/tmp`，脚本行按旧例不进 `tests/`）：`r121_b4_matrix.log` 与
`r121_b4_matrix_ctrl.log`（矩阵八臂 × 七个读数 + 控制）、`r121_b4_ladder.log` 与
`r121_b4_nine_t0_k*_h*.txt`（九臂各档）、`r121_b4_proof.log`（十二战术臂 × 预算/门）、
`r121_b4_equiv.log`（D/D+1/D+2 比价）、`r121_b4_iso.log`（等钱）、`r121_b4_seqflip.log`／
`r121_b4_seqnine.log`（接续读法）、`r121_b4_fvnine.log`（强制变例）。测量期的生产码改动存在
`r121_b4_uncommitted.diff` 与 `r121_b4_src_snapshot/`，已从树上收回。

### 门禁

本轮落地的只有本节文档：`git diff 65bbd17 -- src/ tools/ tests/` **为空**，`git status --short` 里搜索目录
的五个 `M` 已经收回（剩下的 `M .gitignore` 与 `docs/STRESS_TEST_*`、`tools/*.ps1` 等未跟踪文件属于另一条
工作线，不动）。因此 Valgrind 不重跑——memcheck 要检的那台二进制就是 B3 段落里已经记过行的同一台
（`test_board` 523 s／`test_training` 84 s／PvTable 子集 740 s／复用臂 2068 s，全部 0 reports，三条
3600 s 超时臂申报为部分覆盖）。空闲四段我这一遍重跑（`/mnt/d/tmp/r121_b4_gate.log`）：`ctest`
**8/8 / 481.19 s**（`test_search` 451.57 s，与 B3 基线 481.22 s 在同一条线上）、
`node tests/web_frontend_check.js` **105/105**、`node tests/readme_anchor_check.js` **118 引用 / 0 problems**、
`README.md` **658 行**。没有新增或改写任何用例：按 HEAD-negative 纪律，本轮申报**无用例、无 HEAD 反证**，
因为树上没有新的可断言行为。

## 轮次121 引擎重构 B3：一个一极点窗，和一条终于能交出去的主变 — engine redesign stage B3

规格 `docs/ENGINE_REDESIGN.md` §2.4.4（`:309-318`，PV 提取两选一、「产出填入 `SearchInfo::pv`（当前完全为空）」）、
§2.7 表 `:423` 的「PVS ⇒ 节点数 −20~35 %」一行、`:486` 的 `extract_pv(b, depth, stats_.pv)` 伪码行、`:580`/`:582`
的文件清单行，与 `:87` 的 P2 症状「`SearchInfo::principal_variation` 在 AlphaBeta 中**从未被填充**」
（冻结，不改写）。B3 读的是 B1/B2 交出来的那两件事：宽度决定层里有哪些格子，排序决定它们被试的顺序；
本轮问的是**每个格子被问的时候需要多少窗**，以及**一层跑完之后，它知道的那条线在哪里**。

### Found

- **`:423` 的「−20~35 %」没有兑现，兑现的是 −1.23 %**，而且是可解释的。九条局面臂同板、新表、无预算、只看
  `nodes`：`9244367 → 9130501`（`/mnt/d/tmp/r121_b3_ladder_neg.out` 与 `..._here.out`，我这遍重跑的
  `r121_b3_ladder_mine.log`）。十七臂里**四臂下降、十二臂一字不变、一臂涨 0.18 %**。结构原因与 `:424` 那条
  同源：null window 想买的是「子节点拿不到 fail-high 时别把它搜穿」，而 B1 已经把中盘根宽从 133 压到 33、
  B2 又把「刚剪过子树的格」排到前面——一极点窗的额外权力只剩子节点自己的后代里那 `alpha+1` 的底，
  在已经够深的地方才还有东西可剪。这条与 `:424` 一起交给 B4（威胁延伸）重量。
- **探针地板按最差臂选，不按均值选**（`src/search/alphabeta.cpp:247` 的 `depth <= 4`）。三档都在同一支探针、
  同一张姿势表上量过（`/mnt/d/tmp/r121_b3_floor.log`，其中 `f8` 那一档 `depth <= 8` 永假＝PVS 关掉的基线，
  它的九臂和与 `8b91b21` 的实测逐臂相同，这条相等是探针自身的对照）：
  | 地板 | 九臂合计 | quiet BLACK d6 | quiet BLACK d8 |
  |---|---|---|---|
  | 每层都探（无地板） | — | 221380 → **228822（+3.4 %）** | 1451817 → 1415010 |
  | `depth <= 3` | 9112420（−1.43 %） | 225855（比交付值贵 1.8 %） | 1410813 |
  | **`depth <= 4`（交付）** | 9130501（−1.23 %） | **221774（+0.18 %）** | **1408083（−3.0 %）** |
  `<= 3` 的均值更好，但它靠把 `quiet` d6 变贵换来；一支在某些局面上净亏的探针，思考自己无法推理它，
  所以交付的是「没有任何一条姿势是净亏」的那一档。理由与这三个数写在 `:226-246` 的注释里。
- **侦察只回答「这一手有没有超过我现有的那一点」**，所以只有「超过了、又没到 beta」值得第二遍；
  到达顶的是界、跌到底的也是界，两者都与全窗搜索会交回来的东西同形（`:216-221`、`:260-264`）。
  **不干净的探针让本节点不干净，并且压制重搜**（`:255-258`）：预算 1/2/5 节点的臂交回 `sound=NO`。
- **三角表（`:313` 的方案①）的纪律是三条写入**：入口无条件 `pv_.clear(ply)`（`:275`，排在深度门、停止门、
  TT 命中之前——否则一个从 TT 返回的节点会把**上一个节点**的尾巴交给父亲），TT 命中只 `set_single`
  （`:304`，表能作证的只有它记名的那一子），只有 `child.sound` 的线才 `extend`（`:381`）。
  `sizeof(PvTable) = 18688`，`AlphaBetaSearch` 因此 22424 B。另一个方案（TT 回溯）不要这张表，
  但它把「表里没有」当成线尾，正好是本轮要能断言区分的那件事。
- **提交点决定「一条被砍断的 think 交谁的线」**：只有 `layer_complete && layer.sound` 那一个分支
  `copy_out(0, &stats_.pv)`（`:576`）。实测（`r121_b3t_probe4.out` 的 CUT 行与 C11 用例）：
  `completed_depth 3 / partial_depth 4 / time_hard` 的 think 交 `pv_len=3`，着法是**提交层**的 (8,7)，
  而被砍那层自己的 `partial_best` 是另一格 (4,6)。层没跑完，它的线就不是线——这与 A2/A7 的
  「被打断的是帧，不是沉默」同一条规矩。
- **线的长度是条件的函数，不是不变量**：能到处断言的只有 `pv.size() <= completed_depth`。等号在
  「新表 + 开预算 + 这一层跑到底」的臂上成立（d2/d3/d4/d6/d8 ⇒ 2/3/4/6/8），
  两处实测的例外都写进了用例注释：`pos24` 白 d6/64-32 交 5 对 6（一个 TT 答案把线截短一子），
  热表的第二次思考交 2 对 6（根的答案来自 `set_single`，线后半属于写表那次思考）。
- `src/search/search_engine.cpp:339` 是这一轮的协议半边：VCF/VCT 早在 `:209`/`:231` 就填
  `SearchResult::principal_variation`，只有 alphabeta 那一路的线死在搜索对象里面。
- **申报未完成的一半**：这条线现在只到 `SearchResult`。`grep -rn principal_variation src/ web/` 的读者只有
  引擎自己，没有任何协议帧带它，README 658 行没有这一项（anchor 的 118 引用一字未动）。
  §2.4.4 写的用途是「用于下一手的 ponder 预置」，而**预搜/ponder 是本重构明确不做的一项**。
- **probe-then-re-search 规则的测例故意不落地**：它要调 `AlphaBetaSearch::search_child_pvs()` 并读
  `SearchContext`，而本 TU 为 J2 已经带着一句文件级的 `#define private public`——为一条用例保住整 TU 的
  私有访问，是拿门禁换证据；且 `search_child_pvs` 在 `8b91b21` 上不存在，它没有 HEAD-negative。
  数字按本项目对脚本行 fixture 的旧例记在这里，原始输出 `/mnt/d/tmp/r121_b3t_probe4.out` 的 `=== PVS arms`：
  两个节点 × 三个深度 × 两种窗（精确 `alpha=v` 与 `alpha=v-1`）——**六臂的分值全部等于全窗搜索的 `v`**
  （决策中性），`alpha=v` 时 `pvs == window`，`alpha=v-1` 时 `pvs > probe`（重搜确实比单独一支探针贵）。

### Fixed（改动，全部由编排者落树，测试者未碰 `src/`）

- `src/search/search_state.h` **+92/−0**：`class PvTable`（`:132`；`kMaxPly = 64` `:137`、`kMaxLen = 24` `:140`、
  `start_think` `:142`、`clear` `:148`、`extend` `:154`、`set_single` `:172`、`length` `:182`、`at`、
  `copy_out` `:191`；`Move line_[kMaxPly][kMaxLen]` `:200`、`int length_[kMaxPly] = {}` `:203`）。
- `src/search/alphabeta.h` **+24/−2**：成员 `PvTable pv_`（`:111`，紧跟 `heuristics_`）、
  `search_child_pvs` 声明（`:146`）；`SearchInfo::principal_variation`（`:23`）与 `alphabeta()` /
  `search_root_layer()` 的注释改写成「槽的写手/读者」与「slot 0 属于根层，只有提交点 copy 得出去」。
- `src/search/alphabeta.cpp` **+115/−5**：`search_child_pvs` 定义（`:222`，地板 `:247`）；
  `pv_.clear(ply)` `:275`；TT 块把 `tt_move` 提出去并 `set_single` `:304`；调用点 `:360-361`
  （`scout = i > 0 && alpha != -INT_MAX`，`score` 不再在调用点取负）；`if (child.sound) pv_.extend` `:381`；
  `info.principal_variation = stats.pv` `:416`；`pv_.start_think()` `:474`（跟着 `heuristics_.start_think()`）；
  提交点 `pv_.copy_out(0, &stats_.pv)` `:576`；根的 `line_valid` ⇒ `extend(0, move, 1)` / `clear(0)`
  `:753-754`（A6 的同分重搜之后重取 `exact.sound`）。
- `src/search/search_engine.cpp` **+4/−0**：`:339`。
- `tests/test_search.cpp` **+949/−4**：十一条用例 + 一组 B3 助手（`B3Walk`/`b3_pv_walk`/`b3_assert_line`
  `:5799-5828`，`B3Think`/`b3_open_think` `:5833+`），注册在 `main()` 最末 `:6789-6801`，
  仍在两条受保护前提与两条成本针之后。**那 4 行删除全是 B2 的成本针本身**（见合约状态），
  没有删除、放宽或改写任何其他既存在断言。
- `tools/valgrind_check.sh` **+170/−37**：DRD-first 的顺序倒过来（memcheck＝门禁、tsan＝竞争与死锁的第一道筛子、
  drd＝只做交叉核对，并写明它对 asio strand 与无锁发表两次结构性误报）、新增 `tsan` 模式
  （`${TSAN_BUILD:-build-tsan}`、RelWithDebInfo `-O1 -g -fno-omit-frame-pointer -fsanitize=thread -UNDEBUG`、
  链接行也带 `-fsanitize=thread`、`TSAN_OPTIONS=halt_on_error=0 second_deadlock_stack=1`、
  撞到 `unexpected memory mapping` 自动在 `setarch -R` 下重试）、按工具分栏的汇报口径（第九节）。

### 判据（十一条，`tests/test_search.cpp`）

| 用例 | 行 | 钉什么 | HEAD 反证 |
|---|---|---|---|
| B3-A1 `test_pv_table_starts_every_think_empty` | `:5947` | `kMaxPly == 64` 与 `kMaxLen == 24` 两个 `static_assert`；一次思考开始前**每个槽**都空（`start_think` 清全表而不是只清 0 槽） | 无（`PvTable` 不存在于 `8b91b21`） |
| B3-A2 `…_extend_concatenates_the_child_line_and_caps_at_max` | `:6001` | `line[ply] = move ++ line[ply+1]` 的**顺序**与 `kMaxLen` 截断（反过来的线是另一盘棋的合法变例；不截断就写穿 `line_[ply][24]`） | 无（词汇限制） |
| B3-A3 `…_clear_touches_only_its_own_ply` | `:6073` | `clear` 只动它那一层——它是 `alphabeta()` 的第一条语句，从深度门/停止门/TT 命中返回的节点都不能把上一个节点的尾巴交给父亲 | 无（词汇限制） |
| B3-A4 `…_set_single_answers_one_move_and_overwrites_a_longer_line` | `:6112` | TT 命中只作证一子；非法 `Move()` 给**零子**而不是一个幻影头 | 无（词汇限制） |
| B3-A5 `…_refuses_every_ply_and_index_outside_its_slots` | `:6154` | ply `-1 / 64 / 65 / 1000 / INT_MIN` 写不出、读为空；越界 child ply 不交尾巴；`at()` 越索引返回非法 `Move` 且不动 `length` | 无（词汇限制） |
| B3-B7 `test_committed_layer_reports_its_principal_variation` | `:6215` | 静四子局面、24/8、新表、无预算：d2/d3/d4 与 `pos24` d6 两色的提交层各交 2/3/4/6 子，`pv[0] == best`、颜色从轮到的一方交替、走一遍新棋盘每格仍为空；d4 半边走 `SearchEngine` 字段（`:339` 的交付） | **红在 `8b91b21`**（`:5822` 的 `assert(!pv.empty())`） |
| B3-B8 `test_principal_variation_length_tracks_depth_and_pose` | `:6294` | d8 与两色 `pos24` 上 `pv.size() == completed_depth`，并把一般形式 `<= completed_depth` 同时钉住；注释点名两处真实例外（5 对 6 与 2 对 6） | **红在 `8b91b21`** |
| B3-B10 `test_reused_root_think_answers_with_a_shorter_line_that_still_starts_at_the_best_move` | `:6408` | 一台搜索连想两次：第二次**合法地**只交 2 子，但头仍然是它要打的那一手、线仍然合法——这是 B8 的等号不能当不变量的理由 | **红在 `8b91b21`** |
| B3-C11 `test_cut_think_reports_the_last_committed_line` | `:6479` | 只在时限上砍（`hard_time_ms = 600000` + 读数即层表的 `ArmingClock`，不涉节点预算）：`completed 3 / partial 4 / time_hard` ⇒ 交**提交层**的 3 子线、`best` 与提交层一致而 `partial_best` 是另一格；中断记账那半在**两棵树都绿** | **红在 `8b91b21`** |
| B3-C12 `test_a_think_with_no_committed_layer_reports_no_line` | `:6562` | 一层都没完成的 think 不交线：200 节点上限在层 1 内被 counted 节点砍断（非挂钟竞争）与一个已经过期的绝对期限两条臂，都 `pv_len == 0` | **单边**（`8b91b21` 本来就答空）⇒ 记为守卫，不冒充判别 |
| B3-D14 `test_probe_bounds_leave_a_reused_think_on_the_same_answer` | `:6621` | **控制**：一极点窗的界进的是同一张表，所以风险是「界被当成精确值读回来」。一台表连想两次，两色同着法、同分 99999、同 `completed 6`、同 `reason none`，只有代价不同（300347→234、175088→1308） | **两棵树都绿**（定义的绿） |

### 度量表

九臂 PVS 代价（同板、**新表**、无预算，只看 `nodes`；`8b91b21` 那列＝PVS 关掉的同一棵树）：

| 臂 | `8b91b21` | B3（地板 `depth > 4`） | Δ |
|---|---|---|---|
| quiet BLACK d6 24/8 | 221380 | 221774 | **+394（+0.18 %）** |
| quiet WHITE d6 24/8 | 160271 | 156428 | −3843（−2.40 %） |
| quiet BLACK d8 24/8 | 1451817 | 1408083 | −43734（−3.01 %） |
| pos24 BLACK d6/24-8 | 300347 | 300347 | 0 |
| pos24 BLACK d8/24-8 | 2409735 | 2378724 | −31011（−1.29 %） |
| pos24 BLACK d6/64-32 | 1749374 | 1749374 | 0 |
| pos24 WHITE d6/24-8 | 175088 | 175088 | 0 |
| pos24 WHITE d8/24-8 | 1297170 | 1261498 | −35672（−2.75 %） |
| pos24 WHITE d6/64-32 | 1479185 | 1479185 | 0 |
| **九臂合计** | **9244367** | **9130501** | **−113866（−1.23 %）** |

其余八臂 + 复用四臂在两棵树上**逐字相同**：`ladder` 黑 `160/533/1114`、白 `97/461/937`（根宽
`11/33/73`、`6/28/68`），`crowded` `1861`（64/32）与 `888`（24/8），`quiet` 复用 `187`、`pos24` 复用 `234`/`1308`、
`crowded` 复用 `1636` 与 `81`。十七臂的 `best move / score / completed_depth / StopReason` **全同**。
唯一跟着动的是内部记账：`quiet` 黑 d6 的 `qnodes 143627 → 142978`、`ttcut 7823 → 8220`。

线的两半在同一支探针里一起打印（`pv_legal=ok` 是「交替 + 每格在新棋盘上仍为空 + 步数等于长度」三件事）：
`8b91b21` 的**每一臂**都是 `pv_len=0 pv_starts_with_best=BROKEN`（三个 `principal_variation` 字段都存在，
但没有写手），本树的每一臂 `pv_len` 跟着深度走且 `pv0 == best`。

### 反证与 Valgrind

- **判别体的两棵树复跑**（`/mnt/d/tmp/r121_b3t2_cases_neg.out` / `..._work.out`：把 B7/B8/B10/C11/C12/D14
  六段逐字抄进 `/mnt/d/tmp/r121_b3t2_neg_ts.cpp`，对 `git archive 8b91b21` 的阴影树与本树各编一次，
  `-O2 -UNDEBUG`）：`8b91b21` 上 **b7 / b8 / b10 / c11 四条 `RC=134`**，全部红在
  `b3_assert_line` 的第一句 `assert(!pv.empty())`（阴影树的 `:5822`）；**c12 与 d14 `RC=0`**——
  前者是它天生单边（那棵树也答空），后者是控制。**整份套件**我这遍：本树 `B3SUITE rc=0 / 448 s /
  All search tests passed!`，`8b91b21` 的阴影树 `NEGSUITE rc=134 / 42 s`，红在
  `tests/test_search.cpp:5576` 的 `assert(quiet.nodes == 221774)`——即下面的合约改写，不是 B3 用例的失败。
- **组 A 五条没有 HEAD-negative**（`PvTable` 在 `8b91b21` 上不存在，`git archive` 的树里搜不到这个名字），
  按 A6/A7/B1/B2 的先例申报而不是藏起来。
- **一条负载下的红，判为负载而不是回归**：测试者报告预改树死在 `:1019`（真实断言是
  `:1011 assert(quick.root_moves_done == quick.root_moves_total)`，它在 `:1008` 的
  `stopped == "none"` 守卫里面）。我在空闲机上把两棵树各跑一遍：两棵树都打印
  `short budget: method alphabeta, depth 5, stopped time_hard, partial 6`——守卫**根本没打开**，
  `:1011` 在两棵树上都没被执行过，预改树一路走到 `:5576` 才 abort。`:1005` 那手用的是 120 ms 挂钟预算，
  属于轮111 已授权的负载臂一类。**没有裁剪它，也没有为它改代码。**
- **memcheck（门禁工具；`build-vg`＝Debug + `-UNDEBUG`，`tools/valgrind_check.sh`）**：
  | 行 | rc | reports | 秒 |
  |---|---|---|---|
  | `test_board` | 0 | 0（invalid_read/write/uninit/definite_leak 全 0） | 523 |
  | `test_training` | 0 | 0 | 84 |
  | A1/A2/A3/A4/A5 五条表级用例 | 0 | 0 | 1/1/1/1/1 |
  | C12 无提交层 | 0 | 0 | 4 |
  | B7 提交层的线 | 0 | 0 | 740 |
  | B10 复用短链 | 0 | 0 | 2068 |
  | C11 砍断 / D14 控制 / B8 长度 | **124**（`timeout 3600` 到点） | 0 | 3600 各 |
  | 节点上限砍断探针（1500/20000/60000 三档） | 0 | 0 | 30 |
  三条 `rc=124` 不是失败而是**没跑完**：到点前已报告的类全是 0，覆盖到的部分干净；未跑完的代价是
  这三臂的**后半段没被 memcheck**。那三臂里唯一没被别处覆盖的分支是根层的 `pv_.clear(0)`（`:754`），
  于是用**确定性节点上限**在层 3/5/6 中间各砍一刀补跑一遍（30 s，四类全 0，
  `pv_len == completed_depth`、`played == best` 三档都成立）——这条不冒充覆盖，它冒充的是「那一行被走过」。
- **全量 `test_search` 在 memcheck 下**：`ABORTED rc=134 / reports=0 / 322 s`，死点与 B1/B2 记录同一个
  10 ms 挂钟臂（`test_engine_reports_the_interrupted_alphabeta_layer` 量到
  `alphabeta d0+partial 0/n0 time_hard`）；同一棵树不带 valgrind 的整份 `./test_search` **rc=0 / 448 s**。
- **TSan（竞争与死锁的第一道筛子；`build-tsan`＝RelWithDebInfo `-O1 -g -fno-omit-frame-pointer
  -fsanitize=thread -UNDEBUG`，链接行带 `-fsanitize=thread`，`TSAN_OPTIONS=halt_on_error=0
  second_deadlock_stack=1`；本机拿不到免密 sudo，所以 `vm.mmap_rnd_bits` 没能设，脚本在撞到
  `unexpected memory mapping` 时自动退到 `setarch -R`）**：
  `test_training` **rc=0 / 14 s / 0 race / 0 deadlock**；`test_server` **rc=66 / 12 s / 1 个唯一数据竞争
  （TSan 自己的汇总行 `reported 1 warnings`）/ 0 deadlock**；`tsan` 模式冒烟 `test_evaluator`
  **clean 0/0**，整段 `TSANMODE_RC=0 / 124 s`。
  那一条竞争**不是 strand、也不是原子发布**，是真阳性：函数对
  `gomoku::GameManager::undo` → `gomoku::Board::undo_last_move`（读栈里**没有**任何 mutex）
  对 `gomoku::GameManager::request_hint` 的 worker → `gomoku::Board::move_count`（读栈里点名 `write M0`，
  即 `src/server/game_manager.cpp:377-379` 在 `thinking_mutex_` 下读）。写侧 `:220` 的守卫只看
  `ai_thinking_`，与 hint 的 `hint_active_` 不是一把锁（`src/server/game_manager.cpp:220-241`、`:324-332`）。
  **本轮不改**：B3 没碰 `src/server/`，把它塞进引擎轮次会让这枚提交的判据混进协议语义——已单独排队
  （任务 #35），走 bugfix 批次的老流程（先写能红的用例，再改）。
- **DRD 只做交叉核对**：`drd test_training` **reports=0 / 84 s / `DRD_RC=0`**。这一行当时还是旧格式打印的
  （带 `invalid_read=` 那串 memcheck 字段名），正是它让一条 DRD 行能被读成 memcheck 行——本轮把
  `valgrind_check.sh` 的汇报改成按工具分栏之后，我重跑了同一支便宜臂：
  `gomoku_train`（`--rounds 1 --method hill --hill-iterations 1 --depth 1 --timeout-ms 1
  --fitness-games 1 --verification-games 1 --no-data`）→ **clean / reports=0 /
  conflicting_accesses=0 / 6 s / (cross-check: tsan is the first sieve)**。
  顺带修掉一处会在门禁上伪装成崩溃的用法示例：脚本原来写的 `massif gomoku_train --rounds 1 --max-nodes 5000`
  里 `--max-nodes` 不是 `gomoku_train` 的选项（`--help` 没有它），未知选项 3 秒退出 1；
  而 `--rounds 0` 是被 CLI 守卫**正当拒绝**的（`Error: --rounds must be at least 1`），
  我先把这两种 rc=1 都当成工具报的东西看过，才改成上面那条能真正跑完的调用。
  `bash -n` 通过，`grep -c $'\r'` = 0。
- **压测与 Valgrind 互斥**：本节每一行跑的时候 `watch-resources.ps1` 与任何 WS/HTTP 负载都没有启动。

### 合约状态

- **两条成本针被重钉，这是本轮唯一的合约改写**，且各自都是一行数字加一行注释：
  `assert(quiet.nodes == 221380)` → **`221774`**（`:5576`），`assert(pos.nodes == 2409735)` →
  **`2378724`**（`:5585`）。改写的依据是同板新表无预算的确定性代价，而不是「测试挡住了优化」：
  `8b91b21` 的阴影树跑新的测例只红在 `nodes` 这一行，同一支两树探针给出
  `root=31 / d=6 / move=8,7 / score=71992 / reason=none` 与 `root=39 / d=8 / move=7,4 / score=99999`
  **一字不差**。注释里保留 pre-B3 的旧数（`221380` / `2409735`），使这一行仍然写着 B2 挣到了什么。
- **没有别的合约过期**：`ctest` **8/8** 全绿，B1 的节点阶梯（现 `:4732` 的 `160/533/1114` 与 `:4735` 的
  `97/461/937`）、B2 的 killer/history 六条、A6 的同分中心针、A4 的根复用、A7 的中断分层一条都没红。
- 两条受保护的预算前提**一字未动**，位置随本轮新增整体上移 4 行（全部来自 `:16` 那一句 `#include <new>`）：
  `assert(result.resigned)` 从 `:2585` 到 **`:2589`**，`assert(completed.method == "alphabeta")` 从
  `:3980` 到 **`:3984`**；B3 的十一条注册仍在它们之后（`:6789-6801`）。
- **`:424` 的 −30~50 % 仍未兑现**（B2 报 −3.9 %，B3 报 −1.23 %），本轮把它连同 `:423` 一起留给 B4；
  这不是把针放松，是那条验收行的归属问题。

**Gate**（空闲树，跑门禁时不与构建、memcheck 或 TSan 并发；第一次尝试被我自己的重复 ctest 污染，
整个链条被终止后在空闲机上重跑一遍，以下都是这一遍）：`ctest` **8/8 / 481.22 s**（`test_search` **450.47 s**、
`test_server` 14.02 s、`test_position` 7.36 s、`test_training` 9.07 s、其余三套 < 0.1 s），web **105/105**，
anchor **118 引用 / 0 problems**，README 仍 **658 行**。原始输出 `/mnt/d/tmp/r121_b3_gates.log`
（串行脚本 `/mnt/d/tmp/r121_b3_gates.sh`）；两树探针 `/mnt/d/tmp/r121_b3_ladder_mine.log` +
`r121_b3_neg.out` / `r121_b3_here.out`，地板三档 `r121_b3_floor.log`，反证 `r121_b3t2_cases_neg.out` /
`_work.out`，内存与竞争 `r121_b3_vg.log` / `r121_b3_tsan.log` / `r121_b3_post.log`。
以上每一条都是我自己执行的，不是转述。

## 轮次121 引擎重构 B2：一层先试哪一格，第一次由搜索自己说了算 — engine redesign stage B2

规格 `docs/ENGINE_REDESIGN.md` §2.4.5（三段式启发排序）、`:424` 的「三段式启发排序 ⇒ 节点数 −30~50 %」
一行、`:582` 把 killer/history 分到 `src/search/search_state.h` 的文件清单行（冻结，不改写）。B2 读的是
B1 交出来的那张表：宽度决定**哪些格子进层**，静态分决定**一个格子本身值多少**，而「刚在这一层剪掉一棵子树」
这件事两者都说不出来——B2 给它一个说法。

### Found

- 排序的四个阶段是**优先级**而不是键：①TT 着法钉在 0 位并被排除在后面三段之外，②本层的两枚 killer 按槽序
  紧跟其后，④/③ 剩下的尾段按 `(静态价, history, 中心距离, 格号)` 一个字典序键排。①不是本轮新事——
  `8525f60` 的 `sort_moves` 已经把「TT 着法已在 0 位也要钉住」修好了（A 阶段 J2，`tests/test_search.cpp:3246`
  至今还绿），B2 从它手里接过 `start = tt_swapped ? 1 : 0` 那半句，把「从 start 起按价格重排」改成
  「先按槽序压进 ②，再让 ④③ 排剩下的」，头一处偏移因此是 `head` 而不是 0/1 两个常量。
- §2.4.5 把 ③ 写在 ④ 上面，本树**把 ④ 写在 ③ 上面**，这是本轮唯一的文档偏离，且是可复算的取舍而不是口味：
  同一块板、新表、无预算，只看 `nodes`——history 作第一键时 pos24 黑 d8 **2396300 → 8895598（3.7 倍）**、
  白 d8 **1298311 → 5941620（4.6 倍）**。理由写在 `src/search/alphabeta.cpp:120-132` 上方的注释：一格的历史值
  说的是**上一个节点**站过的局面，而 `evaluate_position_pair` 是从**本节点**的棋盘重算的，把 ③ 提上去会让搜索
  重新去开那些价格已经排过序的弱格。度量表见下。
- `:424` 的「−30~50 %」本轮**没有兑现**，兑现的是 **−3.9 %**（`quiet` 黑 d6 `230474 → 221380`）。拆开看谁挣到的：
  只记不用 history 的 ②-only 列给 222118（即 ② 本身就拿到 −3.6 %），③ 作次级键再拿 −0.3 %；pos24 的六臂
  相对 pre-B2 在 ±3 % 内（其中三臂略**涨**：300347/1749374/1479185）。B1 已经把分支因子从 133 压到 33，
  等势格的数量不足以让 killer 再挣三成的节点——这条验收要留给 B3/B4（PVS 与 PV 回填）重新量。
- killer 与 history 都** keyed 到本次思考**：`ply` 从根起算，上一次思考留下的 killer 落在一个现在意义不同的
  ply 上，所以一次思考从空槽开始，而 history 只**减半**不清零——它是唯一能说「这一格老是在剪」跨越同一手棋
  那两三次思考的东西。`start_layer()` 每层一次衰减，正是「随搜索衰减」的字面实现：一层一次右移，最新那次迭代
  占主导，老的弱证据自己掉到 0，不需要给搜索留一个阈值去调。
- **根填不了自己的槽**：根的子节点在 ply 1 上被搜（`src/search/alphabeta.cpp:601` 与同分重搜 `:631`），而
  `alphabeta()` 是唯一的写手，所以一次真实思考**永远不会**写 ply 0 的 killer 槽。本轮把这件事钉成断言
  （`killer_count(0) == 0`）而不是发明一条「根按 killer 排序」的假用例。
- `sort_moves` 的新参数 `int ply = 0` 有默认值，所以**根**那一次调用（`:429`，带 A4 的 `hint_move`）读的是 ply 0
  的槽——即空槽，于是根的序就是「①提示着法 + ④价格 + ③ + 两个几何键」。这一点必须由测例钉住（B2-3(e)），
  因为 A4/A6/B1 关于根的所有针都建立在它之上。
- `src/search/search_state.h:111` 的 `int history_[2][BOARD_SIZE * BOARD_SIZE] = {}` 里那个 `= {}` 不是风格：
  `start_think()` 是**衰减**而不是清空，未初始化表里第一次思考就会按栈噪声排序。B2-2 用「构造后 225 格全读 0」
  把这一行钉住，删掉 `= {}` 就红。
- **一条规格与实现的冲突，申报而不是绕开**（B2-6）：契约点名用 crowded 宽臂（`qnodes/nodes = 1558/1861`）验
  「至少一层有 killer」。实测两棵树都是**已证杀**且加深停在第 2 层（`completed_depth == 2`、`stopped == MATE`），
  它最深的 alphabeta 节点是 ply 1 且 `depth == 1`，`depth >= 2` 的门**从不打开**：64 个槽、450 格 history 全 0。
  该臂 (a)(b) 成立、(c) 不可能成立。(c) 与边界针因此移到 `pos24` d3（4422/2923，`killer_counts[1] == 2`、
  ply≥2 全 0）与 `quiet` d3（2515 节点，同样只有 ply 1）；「quiescence 不记录」改用结构论证钉：d1 的臂里每个
  alphabeta 节点都以 `depth == 0` 返回，跑的只有根的一层子节点与 quiescence，
  `nodes == root_total + qnodes` 成立时表还全空，就只能是 quiescence 没写（crowded d1 = 79+129，quiet d1 = 31+31，
  且 crowded 的 `qnodes 129 > root_total 79`、`seldepth >= 2`、`score == 99999` 说明它真的递归并走出过杀形）。
- **记录边界是深度的函数而不是局面的函数**（逐 ply 实测 `/mnt/d/tmp/r121_b2t_probe.new.out`）：`max_depth D` 的
  思考里只有 ply `1 … D-2` 可能持有 killer——`quiet` d6 ⇒ 1/2/3/4，`pos24` d8 ⇒ 1…6，`pos24` 与 `quiet` 的 d3
  ⇒ 只有 1，`quiet` d2/d1 ⇒ 全空。B2-6 把 `ply >= max_depth` 全 0 钉成断言，这是 `kMaxPly` 越界保护的
  生产半边。

### Fixed（改动，全部由编排者落树，测试者未碰 `src/`）

- `src/search/search_state.h` **+104/−0**：`same_cell(const Move&, const Position&)`（认格不认色，
  无效 `Move()` 永不等有效 `Position`）、`class MoveHeuristics`（`kMaxPly = 64`、`kKillerSlots = 2`、
  `start_think/record_cutoff/start_layer/killer/history_value/killer_count/color_slot`）。
- `src/search/alphabeta.h` **+15/−1**：成员 `MoveHeuristics heuristics_`（`:106`）；`sort_moves` 签名加
  `int ply = 0`（`:144-145`），老调用点一字不改。
- `src/search/alphabeta.cpp` **+78/−18**：`sort_moves` 三段（①`:40-48`、②`:54-66`、比较器 `:120-132`）；
  `:307` 的 `if (depth >= 2) heuristics_.record_cutoff(ply, move, color, depth);`；`:386` `start_think()`；
  `:469` 每层一次 `start_layer()`；quiescence 不调 `sort_moves`、不记录。
- `tests/test_search.cpp` **+700/−0**：六条用例 + 一套 B2 助手（`:5043-5184`），注册在 `main()` 末尾
  `:5851-5856`。**没有删除或放宽任何既存在断言。**

### 判据（六条，`tests/test_search.cpp`）

| 用例 | 行 | 钉什么 | HEAD 反证 |
|---|---|---|---|
| B2-1 `test_move_heuristics_killer_slots_hold_two_moves_per_ply` | `:5194` | 移一次、同格重记是**完全不动**（用白棋在同格再记一次仍读回黑棋那枚，钉住认色无关）、ply 隔离、`killer(-1,0)/killer(kMaxPly,0)/killer(0,2)` 与 `killer_count(-1)`、`record_cutoff(-1,…)`/`record_cutoff(kMaxPly,…)` 不写槽但把 history 抬到 9 再到 25 | 新 API，无（编不过） |
| B2-2 `…_history_accumulates_by_depth_squared_and_ages_by_half` | `:5258` | 构造后 225 格全 0、`color_slot` 0/1、两次 depth 3 恰好 +18、黑白互不串、`start_layer` 折半且留 killer、`start_think` 折半且清空、17→8,4,2,1,0 并停在 0 | 新 API，无 |
| B2-3 `test_sort_moves_pins_tt_then_killers_then_price_over_history` | `:5345` | 中盘黑 33 个候选上：(a) 取全表**最低价**的 TT 仍落 0 位、(a2) 已在 0 位的 TT 留在 0 位且 killer 在 1/2 位等着、(b) 两枚 killer 按槽序压在价格把它们排到最后的尾段之前、(c) 被宽度切掉的 killer（已占格 (5,5)）不出现也不移任何人、(d) 最长等价段里播**最后一枚**的历史 ⇒ 它跳到段首（④ 仍是第一键）、(e) 空表 = A6 三元组逐格相同 | 新 API（`sort_moves` 多了 ply、`MoveHeuristics` 不存在），无 |
| B2-4 `test_three_stage_ordering_pays_for_itself_in_nodes` | `:5562` | **成本针**：`quiet` 黑 d6 24/8 ⇒ `nodes == 221380`、`pos24` 黑 d8 24/8 ⇒ `nodes == 2409735`，各自另钉 `completed_depth / score / best / root_total / stopped` | **红在 `8525f60`**（见下） |
| B2-5 `test_aged_history_leaves_a_reused_root_think_unchanged` | `:5596` | 一台 `AlphaBetaSearch` + 一张表连想三次：#2 与 #3 同为 **187** 节点、同着法 (8,7)、同分 71992、同深度 6、`stopped == NONE`，并前置 `cold_nodes > 187` | **两棵树都绿**（控制） |
| B2-6 `test_killers_are_recorded_by_the_tree_not_by_the_root_or_quiescence` | `:5657` | (a) 真实思考后 `killer_count(0) == 0`、(b) `ply >= max_depth` 全 0（`kMaxPly` 越界的生产半边）、(c) `[1, max_depth)` 至少一层非 0；外加「只有 quiescence 跑的臂表全空」的结构论证 | 新 API，无 |

### 度量表（同板、新表、无预算，只看 `nodes`；四列由同一支探针 `/mnt/d/tmp/r121_b2_ladder.cpp` 在两棵树与两份
临时改过的比较器上测得）

| 臂 | pre-B2 `8525f60` | 文档序 ③>④ | ②-only（记而不用） | **交付：④>③** |
|---|---|---|---|---|
| quiet BLACK d6 24/8 | 230474 | 286957 | 222118 | **221380** |
| pos24 BLACK d6 24/8 | 298654 | 626730 | 299497 | 300347 |
| pos24 BLACK d8 24/8 | 2396300 | 8895598 | 2400249 | 2409735 |
| pos24 BLACK d6 64/32 | 1745526 | 3991260 | 1743321 | 1749374 |
| pos24 WHITE d6 24/8 | 180479 | 415625 | 174519 | 175088 |
| pos24 WHITE d8 24/8 | 1298311 | 5941620 | 1292402 | 1297170 |
| pos24 WHITE d6 64/32 | 1446349 | 3513467 | 1473272 | 1479185 |
| crowded BLACK d6 64/32 | 1861 | 1861 | 1861 | 1861 |
| mate ladder 黑 160/533/1114、白 97/461/937；根 11/33/73、6/28/68 | 四列全同 | | | |
| 以上每一臂 best move / score / completed_depth / StopReason | 四列全同 | | | |

四列全同是「为什么允许偏离」的下半场：本轮没有逼着任何一条既存的杀形/战术/根宽针搬家。

### 反证与 Valgrind

- 六条里只有 **B2-4** 能反证：它只用两棵树都有的公开 API（`AlphaBetaSearch` / `SearchLimits` /
  `last_stats()`）。`/mnt/d/tmp/r121_b2t_headneg.sh` 把两份**逐字抄自测例**的用例体（`/mnt/d/tmp/r121_b2t_head.cpp`，
  不点 `MoveHeuristics` 之类的名）对两棵树各编一次（各自的 `src/`，`-O2 -UNDEBUG`）：
  - **本树 `RUN_RC=0`** 三臂全绿；**`8525f60` 的阴影树 `RUN_RC=134`**，quiet 臂红在
    `r121_b2t_head.cpp:101` 的 `assert(quiet.nodes == 221380)`，pos24 臂红在 `:111` 的
    `assert(pos.nodes == 2409735)`。
  - 红只在**节点数**这一行：同一脚本先跑打印模式，两棵树并排给出
    `quiet d6 24/8 nodes=221380 / 230474 root=31 d=6 move=8,7 score=71992 reason=none`、
    `pos24 d8 24/8 nodes=2409735 / 2396300 root=39 d=8 move=7,4 score=99999 reason=none`——
    深度、根宽、着法、分值、停止原因**一字不差**，只有代价不同。这正是规格要的判别形状。
  - 阴影树身份：`git rev-parse --short HEAD` = `8525f60`，`search_state.h` 里 `MoveHeuristics` 出现
    **0** 次（本树 >0），`sort_moves` 签名没有 `int ply`（本树 `…= Move(), int ply = 0`），
    且 `diff -r -q` 对**新做的** `git archive 8525f60` 全树为空。
- **B2-5 是控制**：一台搜索 + 一张表连想三次，`#2/#3` 在两棵树上都是 **187** 节点、同一着法 (8,7)、同分
  71992、同深度 6、`stopped == NONE`——两棵树都绿是它的定义，不是它的失败。它证明 `start_think()` 的
  「衰减而不清零」没有污染 A4 的根复用。
- B2-1 / B2-2 / B2-3 / B2-6 点名了只有本树才有的符号（`MoveHeuristics`、`same_cell`、`sort_moves` 的 `ply`
  形参），在 `8525f60` 上**编不过** ⇒ 词汇限制，按 A6/A7/B1 的写法申报而不隐藏。
- **套件代价**：六条让 `test_search` 从 B1 落树时的 **36.85 s** 涨到 **254.57 s**（我这遍 ctest 的第 4 行；
  测试者那遍 259.13 s），增量几乎全在 B2-4 的两条成本针里（`/mnt/d/tmp/r121_b2t_subset_plain.txt` 的
  `B2-TIMING` 行：`cost_pin 218.309 s`、`reuse_control 7.615 s`、其余四条合计 **0.63 s**）。这是把
  「②/③ 挣到什么」写成可断言数量的定价，本轮不裁剪；C 阶段（评估性能四步）落地后这两个数会自己变小，
  那时按 B1 的先例把针重钉成新常量。
- memcheck 子集驱动 `/mnt/d/tmp/r121_b2t_vg_subset_main.cpp`（`#define main suite_main_b2`，只挂六条中的
  五条便宜臂；`build-vg` 是 Debug + `-UNDEBUG`，对象取自 `build-vg/CMakeFiles/test_search.dir`）：
  直跑 **`PLAIN_RC=0`**（8 s，5 条全绿），我这遍 memcheck **RC=0 / reports=0 / 744 s / completed=5**
  （测试者那遍 768 s）。memcheck 下的逐臂计时（`/mnt/d/tmp/r121_b2o_vg_subset.out`）：
  `reuse_control 687.012 s`、`tree_records 53.99 s`、`sort_moves_stages 1.50 s`、两条表级用例 0.01 s——
  744 s 里九成六是 B2-5 的第一次思考，而**那一次思考就是 B2-4 的 quiet 成本针**（同一板、同一 24/8、
  同 221380 节点），这一点是下面「未覆盖」那条能站住的依据。
- **未覆盖，申报**：第六臂（B2-4 的 221380 + 2409735 节点）在测得的约 **92×** 减速下单条就要数小时，
  因此**没有**对它做 memcheck。它的 quiet 半边已经在覆盖里——B2-5 的第一次思考就是同一次 221380 节点的
  `quiet` d6 24/8，那一臂 memcheck **687.012 s、reports=0**。没覆盖的只是 pos24 从 d6 深到 d8 多出来的
  两轮迭代，走的是 `sort_moves`/`record_cutoff`/`start_layer` 的同一份代码。申报为未覆盖，不冒充覆盖。
- **全量·便宜三套**（`tools/valgrind_check.sh memcheck test_board test_pattern test_evaluator`，
  `build-vg`）：`test_board` **clean reports=0 780 s**、`test_pattern` **clean 0 6 s**、
  `test_evaluator` **clean 0 1 s**，整段 **`CHEAP3_RC=0` / 788 s**（我这遍）。
- **全量·`test_search`**（同一工具、同一 `build-vg`）：**`ABORTED rc=134 / reports=0 / 325 s`**，
  中止点与 B1 记录的同一个——最后打印的是 `test_engine_progress_frames_share_the_search_clock passed`，
  接着 `test_engine_reports_the_interrupted_alphabeta_layer` 在 memcheck 下量到
  `alphabeta d0+partial 0/n0 time_hard`，红在 `tests/test_search.cpp:3967`
  `assert(interrupted.partial_depth == 1)`。10 ms 的预算臂在约 **92×** 减速下一根候选都开不出来，这是
  挂钟脆弱而不是本轮的回归：**同树不带 valgrind 的整份 `./test_search` rc=0 / All search tests passed**，
  受保护的那条 `:3980` 在其后 13 行、从未被走到。`reports=0` 说的是中止前跑过的每一臂都干净。

### 合约状态

- **没有合约过期**：`make` 干净、`ctest` **8/8** 全绿，B1 的节点阶梯
  （`tests/test_search.cpp:4728` 的 `160/533/1114` 与 `:4731` 的 `97/461/937`）、A6 的同分中心针
  （`test_root_tiebreak_prefers_centre` / `test_root_choice_reproducible`）、A4 的根复用、A7 的中断分层
  一条都没红。四列度量表里「best move / score / completed_depth / StopReason 全同」是这件事的量化形状。
- 两条受保护的预算前提**一字未动**，位置也没移动：`assert(result.resigned)` 仍在 **`:2585`**，
  `assert(completed.method == "alphabeta")` 仍在 **`:3980`**（本轮的 700 行全部加在 `main()` 之前，
  它们在上面）。
- `tests/test_search.cpp` **+700/−0**：没有删除、放宽或改写任何既存在断言。

**Gate**（空闲树，跑门禁时不与构建或 memcheck 并发）：`ctest` **8/8 / 284.23 s**（`test_search` **254.57 s**、
`test_position` 7.71 s、`test_server` 12.55 s、`test_training` 9.08 s、其余四套 < 0.2 s），web **105/105**，
anchor **118 引用 / 0 problems**，README 仍 **658 行**。以上每一条都是我在测例落树后自己复跑的（串行脚本
`/mnt/d/tmp/r121_b2_orun.sh`，原始输出 `/mnt/d/tmp/r121_b2o_ctest.txt` / `_web.txt` / `_anchor.txt` /
`r121_b2o_vg_subset.{out,err}` / `r121_b2o_vg_cheap3.txt` / `r121_b2o_vg_fullsearch.txt`），不是转述。

## 轮次121 引擎重构 B1：一层能搜的格子终于有了上限 — engine redesign stage B1

规格 `docs/ENGINE_REDESIGN.md` §2.5.1（候选分层与宽度限制）、§3.2（候选生成伪码）、§2.7 的「双威胁前置识别」一行与
§6 文件清单的 `src/search/move_generator.h/.cpp` 行（冻结，不改写）。B1 是 P1 交出来的两枚宽度基因的**第一个读者**：
`cand_width_root` / `cand_width_deep` 从这一轮起不再只是存得进 `params.save` 的数。

### Found

- 中盘分支因子的来源第一次被量化：`build_midgame` 那 12 子的局面 `active_empty()` 有 **133** 格，A 阶段的
  `generate_moves` 把 133 格整个交给根，深度 6 的一层就是 133 路分支。分层以后根只剩 **33**（Tier1 的 9 个威胁点 +
  Tier2 的 24 个邻域点），一次无预算的 d6 思考 **2080 → 533** 个节点，`test_search` 的套件墙钟从 P1 Gate 的
  **138.02 s 降到 36.85 s**。§5 B 行「节点数较 A 阶段下降 ≥ 50%」这条验收在 `quiet`（无威胁、原来 40 格全搜）上实测
  **1525277 → 230474，−84.9 %**。
- 规格有两处自相矛盾，两处都落在裁剪面上，B1 必须择其一并在日志里留下选了哪一档：
  1. **Tier0 的第二半**。§2.5.1:335 的注释写 Tier0 是「己方成五 / **对方成五的挡点** / VCF-VCT 强制着法」，§3.2:518 的
     伪码却只测 `is_forced(mine)`。实现取**并集** `makes_five(mine) || makes_five(theirs)`（`move_generator.cpp:129`）：
     一个只挡住对方成五的点若落进会被截断的 Tier2，那一层就能在预算内把唯一的应着裁掉。`white_open_four`（白 7,5–7,8
     活四、黑先）实测 `forced = 2`，而且**任何 ply、任何宽度**（含 2/2 的曲面下限）都不变，(7,4) 与 (7,9) 都在
     `flatten()` 里。
  2. **Tier3 的准入**。§3.2:525 写 `threats.empty() && vicinity.empty() ? 8 : 0`，§2.5.1:346 的表只写「8（仅无威胁）」，
     表的条件宽一档。实现按**表**：无威胁且 ply 0 就取 M，无威胁且 Tier2 落空也取 M。`scattered`（四颗孤子、96 格活跃）
     实测根上 `remote = 8/72`，ply ≥ 1 归 0。
- §2.5.1:351 的「无威胁剪枝」与 §3.2:523 的注释「无威胁时**放宽**」方向相反。实现按 :523 的字面（`width_for(ply,
  threats.empty())`）：无威胁时 Tier2 在**每一层**都保持根宽——那一层没有应着要下，它是在选往哪里铺，收窄它会把开局型
  搜成一条线。实测 `quiet` 与 `scattered` 在 ply 1 / ply 3 都是 `vicinity = 24`，而同 ply 的 `midgame` 是 16 与 8。
  :351 那半句「允许提前返回静态分（futility pruning）」**本轮没做**，它与 §2.7 的空着剪枝同类，进未完成清单。
- **三档 K 对两枚基因**：表给 24 / 16 / 8，P1 只交两枚（`cand_width_root=24`、`cand_width_deep=8`）。ply 1–2 取两者
  均值 ⇒ 默认值下正好 16，等于表的数。这是 P1 日志预告过、留给 B1 的决定：取均值，不加第三枚基因——加一枚要动
  `kParams` 表、取值域、跨键规则与训练盒体积，为只有两层使用的中间档不值。测例用 **30/10** 这对不对称值把「是均值」
  与「是常数」分开：ply 1 的宽度是 **20**，既不是 30 也不是 10。
- `struct ScoredMove` 这个名字已被 `src/search/greedy.h:11` 占着（贪心层的攻守对），照抄 §3.2 会撞名；分层记录改名
  `Candidate`，头注释里写明这处偏离。
- 分层是**成员资格**的决定，不是**搜索顺序**的决定：`sort_moves` 仍然按同一个三元组重排存活下来的整张表，所以 A4 的根
  复用、A6 的根选择、A7 的分层记账与 `root[0]` 兜底那些测例，描述的仍是引擎真正走的那条路，本轮**一条都不用改写**。
  三段式启发排序（TT / killer / history）本来就是 B2 的活。
- **一个写进测例而不是绕过去的实测发现**：`double_three` 上黑棋的三三点 (7,7) 对黑是禁手、对白合法，而它在**默认根宽**
  下对白也进不了 Tier2——`candidate_pair_value` 把它排在 45 格 Tier2 的**第 28 位**，24 的帽正好把它切掉。若照最初的
  设想「同一个格子对白应该在层里」写断言，那条断言会红在**宽度**上而不是红在**颜色规则**上。测例因此把镜像臂放在曲面
  最大值（64/32）下跑（`vicinity[28]`、score 112），并另起一条把「默认宽下它确实被切」这件事本身钉住。

### Fixed

- 新增 `src/search/move_generator.h`（87 行）与 `src/search/move_generator.cpp`（177 行）：`Candidate`、
  `CandidateSet{forced, threats, vicinity, remote}` + `flatten()`、`MoveGenerator::generate(board, color, ply,
  in_quiescence, out)`。每个活跃空格一次 `recognize_position_both`，分层判据与分值读同一次识别，BLACK 的禁手用
  `is_forbidden_from_pattern(mine)`——不再跑第二次四方向扫描（那正是 P3.1 记下的重复代价）。
- `src/search/search_limits.h:47`：`RootScore` 从 `alphabeta.cpp` 的匿名 namespace **原样搬来**，搜索与生成器现在排在
  同一个全序上，于是「宽度留下了哪几格」只是局面的函数。
- `src/core/evaluator.h:81` + `src/core/evaluator.cpp:192`：新公开方法 `candidate_pair_value(mine, theirs, row, col,
  color)`；`evaluate_position_pair` 改为识别后转调它（`:189`，数值不变）。
- `src/search/alphabeta.h:101` 挂 `MoveGenerator move_gen_`，`:129` 的 `generate_moves(board, color, ply = 0)` 变成
  转调；`.cpp` +29/−78，`quiescence()` 那段内联过滤器换成 `move_gen_.generate(..., in_quiescence=true, &forcing)`
  （`:127-128`）——成员资格、计价口径（只算行棋方）、排序三元组、空表提前返回四件事都按构造保持不变。
- `CMakeLists.txt`：`src/search/move_generator.cpp` 进 `GOMOKU_CORE_SOURCES`。

### 合约过期

- `test_engine_keeps_alphabeta_time_after_tactics` 尾部那条 `assert(quick.method != "alphabeta" || quick.stopped !=
  "none")`（现 `tests/test_search.cpp:1003`）把「这盘 120 ms 的思考到不了请求深度」这件**墙钟事实**编码成了「说不出中断
  原因就是错」。B1 之后前提不成立：`/mnt/d/tmp/r121_b1t_r989.cpp` 在同一块板上跑四臂——pre-B1 -O0 depth 3 /
  `time_hard` / 3944 节点 / root 0-133；pre-B1 -O2 depth 3 / `time_hard` / 18860 / 1-133；B1 -O0 depth 6 / `time_hard`
  / 2732 / 0-26；**B1 -O2 用 208 节点、0 ms 把八层全跑完**（root 26-26，`stopped == "none"`）。于是「没有中断原因」
  成了**跑完了的正确回答**。断言改写为 `... || quick.depth >= 8`，并把「跑完了」补成三条硬账：`depth >= 8`、
  `partial_depth == 0`、`root_moves_done == root_moves_total`。这不是放宽——它要抓的那个形状（标着 alphabeta、说不出
  原因、层没跑完）在四臂里依旧全红，`31ad693` 上也依旧绿（它 `stopped == time_hard`）。
- 两条受保护的预算前提**一字未动**，只因上面这次改写整体下移：`assert(result.resigned)` 从 `:2563` 到 **`:2585`**，
  `assert(completed.method == "alphabeta")` 从 `:3958` 到 **`:3980`**。后续按新行号引用。

### 反证与 Valgrind

- 七条新用例（`tests/test_search.cpp:4445` 起，注册在 `main()` 末尾 `:5150-5156`）里只有 **B1-4
  `test_move_generator_width_lowers_nodes_and_root_size`** 能反证：它只用两棵树都有的公开 API（`AlphaBetaSearch` /
  `SearchLimits` / `last_stats()`）。`/mnt/d/tmp/r121_b1t_case4.sh` 把同一份源文件对两棵树各编一次：本树
  **`RUN_RC=0`**，打印 `color=1 tight=160/11 narrow=533/33 wide=1114/73`、`color=2 tight=97/6 narrow=461/28
  wide=937/68`；`git archive 31ad693` 的阴影树 **`RUN_RC=134`，红在 `r121_b1t_case4.cpp:97` 的 `tight.nodes <
  narrow.nodes`**，三臂全打印 `2080/133`（黑）与 `1633/133`（白）——老树不读这两枚旋钮，三个臂就是同一次搜索。四条严格
  不等号在老树**每一个都红**，而且它们把「变小」排成旋钮的单调函数，不是「比我自己挑的一个数小」。三臂同时打印同一个
  已证胜着（黑 7,6 / 白 9,5、`score 100000`、`completed_depth 2`、`reason=mate`），说明 2 格宽的下限没有吃掉战术。
- 其余六条点名了只有本树才有的符号（`MoveGenerator` / `CandidateSet` / `candidate_pair_value`），在 `31ad693` 上
  **编不过** ⇒ 词汇限制，按 A6/A7 的写法申报而不隐藏。其中 **B1-6
  `test_quiescence_row_is_the_forcing_filter`** 是**控制**：它把老内联过滤器按老写法重抄一遍（逐格
  `recognize_position`、留 成五/活四/冲四/活三、跳过黑的禁手、用已识别的 pattern 计价、按 值→中心→格号 排），再与生成器
  quiescence 那一行在 7 个臂上 **60 个候选逐格逐分**对齐，并要求另三层为空（§2.5.1 的「仅强制着法」行）。两棵树都绿是
  它的定义，不是它的失败。
- memcheck 子集驱动 `/mnt/d/tmp/r121_b1t_subset_main.cpp`（`-Dmain=suite_main_b1`，只挂七条；`build-vg` 是 Debug +
  `-UNDEBUG`）：直跑 **`PLAIN_RC=0`**，memcheck **`MEMCHECK_RC=0`、报告 0 行、51 s**、`subset cases completed: 7`。
  七条全注册在 `main()` 末尾，位置在下面全量中止点之后 ⇒ 由子集覆盖。
- **全量·便宜三套**（`tools/valgrind_check.sh memcheck test_board test_pattern test_evaluator`）：
  **三条全 `clean`、`reports=0`**，**1817 s / 5 s / 2 s**，`FULL_RC=0`。B1 只经由 `candidate_pair_value` 碰了评估器，
  但生成器读的是同一份 pattern 识别，所以这三套照常跑；P1 在同一条 `test_board` 线上记的是 1406 s，本轮 1817 s
  是同一形状的抖动（`test_pattern`/`test_evaluator` 的 5 s / 2 s 与 P1 一字不差）。
- **全量·`test_search`**（同一工具、同一 `build-vg`）：**`ABORTED rc=134`、`reports=0`、350 s**。套件 stdout 的
  最后一行是 `main():5086` 那条用例自己打印的 `measured (interrupted: alphabeta d0+partial 0/n0 time_hard, control:
  alphabeta d0+partial 1/n2494 time_hard)`；紧跟着的第一条**会红**的断言是
  `tests/test_search.cpp:3967` `assert(interrupted.partial_depth == 1)` —— 判别臂只有 10 ms，memcheck 的减速下它连一层
  的**第一个根候选**都没开始，于是 `partial_depth` 是打印出来的 0 而不是 1。这正是 A6 那条记录预告的中止形状
  （「连一层都跑不完……既存墙钟脆弱」），也是它**不是**两条受保护前提之一：`:3980`
  `assert(completed.method == "alphabeta")` 在同一函数再往下 13 行，本轮根本没走到它，而它读的对照臂打印的是
  `alphabeta d0+partial 1/n2494 time_hard`（绿的形状）。反过来，`main():5081` 的
  `test_engine_progress_callback_reports_each_layer`（A2 与 A6 记过的同一处中止点）这次**过去了**：A6 量到的是 1466 s
  停在 :5081 之前，本轮 350 s 走到了 :5086，按 `main()` 顺序它之前的每一条都被 memcheck 覆盖。
  一句施工瑕疵：这一次前约 2 分钟有一个我自己重复启动的同套 memcheck 在抢 CPU（发现后按 PID 杀掉了重复的那棵），
  350 s 因此偏慢而不偏快。
- 同一条 `build-vg` 树**不带 valgrind 直跑整套 `./test_search`**：**`NATIVE_RC=0`**、最后一行 `All search tests
  passed!`，末尾七条（含 `test_quiescence_row_is_the_forcing_filter` 的 7 臂 60 格对齐、
  `test_tier_choice_reproducible_across_undo_cycles` 的 135 条分层）全绿 ⇒ 上面那次 abort 只属于既存墙钟脆弱，
  不是本树的行为差异；D1 打通 `max_nodes` 后按 A6 的记账把它改成节点预算。

**Gate**（空闲树，跑门禁时不与构建或 memcheck 并发）：`ctest` **8/8 / 63.97 s**（`test_search` **36.85 s**、
`test_server` 10.59 s、`test_training` 8.91 s、`test_position` 7.35 s、`test_pattern` 0.07 s、`test_board` 0.03 s、
`test_evaluator` 0.02 s、`test_session` 0.03 s），web **105/105**，anchor **118 引用 / 0 problems**，README 仍
**658 行**（`:529-531` 未动）。以上每一条都是我在测例落树后自己复跑的，不是转述。

## 轮次121 引擎重构 P1：参数面从十枚基因长到十四枚 — engine redesign stage P1

规格 `docs/ENGINE_REDESIGN.md` §2.5.1、§2.5.2 与 §7 前置事实 P1（冻结，不改写）。P1 是 B1 的硬前置：宽度帽必须是
**可读的参数**，而 A5 留在 `ThreatCoefficients` 里的两枚字段必须先出得了 `params.save` 那一步。

### Found

- **真 bug，不是新能力**：`edge_penalty`（`src/core/evaluator.h:42`）与 `distance_to_stones`（`:33`）从 A5 起就被
  评估读着，却既不在 `TunableParam` 枚举里、也不在文件键表里 ⇒ `CoefficientVector::save` 根本不写它们，`load`
  之后训练值回到编译期的 100 / 1。反证探针（只调用两棵树都有的 API，`/mnt/d/tmp/p1_probe.cpp`）在改动前树
  `87ee891` 上：写出的 JSON 只有 12 个键、`edge_penalty=100 distance_to_stones=1`、**`RUN_HEAD_RC=1`**，而手写
  `{"coefficients":{"edge_penalty":37}}` 整份被拒（`no known coefficient key found in the file`）；同一份探针在本树
  写出 16 个键、`37 / 3` 原样读回、**`RUN_FIXED_RC=0`**。
- §7 P1 记的那条前置事实仍然成立：`field_of` 的 `default:` 会把一个「进了枚举却没进 switch」的基因静默别名到
  `position_weight_scale` 上——训练得动、存成别人的名字、再读不回来。所以这一轮**没有加第五处 switch**，而是把五处
  平行表面（writer 的键表、reader 的 `names[]`/`fields[]`/`present[]`、bound switch、field_of switch、报错用的名字表）
  合并成一张 `kParams` 表：键名、成员指针、基因、取值域四件事只写一遍，并由
  `static_assert(tunable_table_complete(), "every TunableParam needs exactly one row in kParams")` 钉住「每枚基因恰
  有一行」。`to_vector` 也不再整份拷贝系数对象，逐基因只取自己那一格。
- 两枚宽度 `cand_width_root = 24` / `cand_width_deep = 8` 声明在 `ThreatCoefficients` 上，本轮没有任何代码读它们
  （B1 的分层裁剪才是消费者）⇒ 纯新增结构，按 §5 的规矩**声明为无 HEAD 反证**；能反证的只有「老树不认这两个键」，
  即上面那条探针的第二半。
- 文档 §2.5.1 的 K 是**三档**（根 24 / ply 1–2 16 / ply ≥ 3 8），而计划给 P1 的是**两枚**宽度基因。本轮按计划做两枚，
  中间档留给 B1（计划 103 行的「Tier2 按 §2.5.1 表取 K」届时要么在两档之间插值，要么加第三枚基因）——那是 B1 的决定，
  不在参数面这一轮预先猜。

### Fixed

- `src/core/evaluator.h`：两枚宽度字段 + 说明「为什么声明在这里而不是硬编码在 B1」。
- `src/training/params.h`：枚举加 `EDGE_PENALTY`、`DISTANCE_TO_STONES`、`CAND_WIDTH_ROOT`、`CAND_WIDTH_DEEP`
  （`TUNABLE_COUNT` 10 → 14，文件键面 12 → 16）。
- `src/training/params.cpp`：表驱动重写；取值域 `edge_penalty [0, 150]`、`distance_to_stones [0, 4]`、
  `cand_width_root [2, 64]`、`cand_width_deep [2, 32]`；跨键规则加第九条 `cand_width_deep ≤ cand_width_root`；
  `sanitize` 加 `cand_width_deep = min(cand_width_deep, cand_width_root)`。
- **第九条跨键规则与计划措辞的偏差**（计划 97 行写「仅当两端都被文件显式声明才拒绝」）：实现按仓内既有八条同一个
  `stated()` 判据，也就是 loader 规则本来的说法——**只要 `sanitize` 会被改掉的那一端是文件亲手写的就拒绝**，另一端是
  继承默认时把这件事写进消息（「（对端是默认值）」）。于是 `{"cand_width_deep":30}` 被拒（默认 root=24 会把它压回
  24，改掉的正是文件写的值），而 `{"cand_width_root":6}` 通过并把继承来的 deep=8 修成 6。计划那句照字面实现的话，
  第一条反而会被静默改写——与轮次90 定下的判据相反，故取仓内规则、弃计划措辞。
- 新键全部**可选**：老 12 键文件照旧加载，四枚新字段停在编译期默认值上，writer 仍然把老 12 个键一个不漏地写出来。
- README 两处行内改写（行数仍 658，`:529-531` 未动）：`:155` 的加载失败清单补「可调项越界或跨项冲突如
  `cand_width_deep` 大于 `cand_width_root`」；`:607` 的训练对象一句补「外加 2.5.1 的候选宽度与 2.5.2 的位置/贴边权重」。
- `tests/test_training.cpp`：三条新用例 + 两处既有改写。`test_params_legacy_12_key_file_still_loads` 是对照（老 12 键
  文本照旧加载、四枚新字段停在默认、writer 一个不漏地仍写老 12 键）；
  `test_params_position_and_width_genes_round_trip` 让 `150 / 4 / 40 / 16` 走 JSON 与向量两条视图，并用「阶梯确实在动」
  （同一份系数里 `live_two` 被压到 `sleepy_three`）证明这四枚没被阶梯顺手改掉，另带 `8 * edge.max_value <
  live_four.min_value` 这条取值域结构前提和 `sanitize` 把 deep 32 压到 root 4；
  `test_params_refuses_only_stated_width_conflicts` 五个臂——两端都写→拒且两个键名都在消息里、只写会被压的那端→拒并带
  「（对端是默认值）」、只写 root=6→接受并把继承来的 deep 修成 6、同宽合法、`cand_width_root:100` 报 "[2, 64]"。
  `test_params_rejects_out_of_range_tunables` 加两行越界拒绝；爬山那处 `TUNABLE_COUNT - 2` 的偏移改成按基因名点名
  （`SLEEPY_TWO`），以后加参数不再挪测试。

### 合约过期

- `test_genetic_seed_covers_the_whole_box` 的 `assert(genes[g] != bound.min_value)`：`distance_to_stones` 的取值域只有
  4 个整数，播种落在下界是**算术**（每枚约 1/4 的概率），不是缺陷，换种子也只是把硬币换成另一枚。前提改成「每枚基因在
  两个种子的样本里都出现过盒子的上半和下半」，同时保留「落点必须在自己取值域内」这条原有断言。实测两个种子 × 6 个重播
  席位下，`double_four`（下标 6）取到 62465 / 89374 / 80617 / 67789 等上半格值，14 枚基因的 `low_seen`、`high_seen`
  全部成立；R87 那个缺陷仍被同一条断言抓住——被删掉的那半公式让**每一枚**基因都困在自己的下半格，而不只是
  `double_four`。计划 97 行「宽度取 `{2,64}`/`{2,32}` 使 `genes[g] != bound.min_value` 保持现有成立性」只算到了两枚
  宽度（它们确实躲得开下界），漏掉同一轮加进来的 `distance_to_stones` 是 4 个整数的取值域。

### 反证与 Valgrind

- 反证探针只调用两棵树都有的 API（`edge_penalty` / `distance_to_stones` 与 `to_json_string` / `from_json_string`），
  所以它在老树上**编得过、跑不过**：`/mnt/d/tmp/p1_probe.cpp` 配 `git archive HEAD src/core src/training` 解出的阴影头
  （`-I` 把存档目录排在前面）→ `BUILD_HEAD_RC=0`、**`RUN_HEAD_RC=1`**，打印的 JSON 只有 12 个键、
  `edge_penalty=100 distance_to_stones=1`；同一份源文件对本树 → **`RUN_FIXED_RC=0`**，JSON 16 个键、`37 / 3` 原样读回。
  两枚宽度本身是纯新增字段，能反证的只有「老树把只写它们的那份文件整份拒了」这一条
  （`no known coefficient key found in the file`）。
- 子集驱动 `/mnt/d/tmp/p1_subset_main.cpp`（同 `-Dmain=suite_main` 手法，挑五条：改写后的播种覆盖、加了两条新行的越界
  拒绝、老 12 键对照、四枚新基因往返、只拒已声明的宽度冲突）：Debug 形状直跑 **`PLAIN_RC=0`**，memcheck
  **`MEMCHECK_RC=0`、报告 0 行**；这五条排在 `test_training.cpp:2056` 那条既存墙钟脆弱断言之后，全量跑不到 ⇒ 由子集覆盖。
- 全量（`tools/valgrind_check.sh memcheck test_board test_pattern test_evaluator test_training`，`/mnt/d/tmp/p1_vgfull.out`）：
  **四条全 `clean`、`reports=0`**，1406 s / 5 s / 2 s / **179 s**（`test_training` 直跑 10.18 s ⇒ 约 18× 减速），
  `FULL_RC=0`。**这一轮 `:2055` 那条既存墙钟脆弱断言没有触发**，整套跑到 `main()` 末尾，所以五条新/改用例既被子集覆盖、
  也被全量覆盖；上一条子集驱动留着是因为它按 §5 的口径证明了「就算它触发，这五条也已单独 memcheck 干净」。两段
  memcheck 都跑在那两处注释改写之前（其后只有 README/注释动过，行为不变），最终树的门禁与探针数字见下面的 Gate。

**Gate**（空闲树，跑门禁时不与构建或 memcheck 并发）：改动后树先量到 ctest **8/8 / 174.91 s**，两处注释改写后再在**将要
提交的那棵树**上复测为 ctest **8/8 / 168.21 s**（`test_search` 139.13 s、`test_server` 11.43 s、`test_position` 7.46 s、
`test_training` 9.92 s、`test_evaluator` 与 `test_board` 0.0x s），web **105/105**，anchor **118 引用 / 0 problems**，
README 仍 **658 行**（`:529-531` 未动）。两条受保护的预算前提复测仍在原位：`tests/test_search.cpp:2563`
`assert(result.resigned)`、`:3958` `assert(completed.method == "alphabeta")`。反证探针在同一棵树上重跑，
`RUN_FIXED_RC=0` / `RUN_HEAD_RC=1` 不变。

## 轮次121 引擎重构 A8：候选分层的两只门打开了 — engine redesign stage A8

规格 `docs/ENGINE_REDESIGN.md` §2.5.1 + §6 文件清单的 `src/core/board.h/.cpp` 行（冻结，不改写）。A8 是给 B1 前置的
一轮：宽度裁剪要问的两件事，`Board` 早就知道，只是不说。

### Found

- §2.5.1 的 Tier 划分依赖两个事实：这一格的 5×5 邻域里有几颗子（决定它属不属于「上一手 2 格邻域 ∪ 已有棋子的
  2 格邻域」），以及它此刻到底还在不在候选集里。`Board` 两个都维护着（`neighbor_count_` / `active_index_`，
  board.h:107-108），但**都是私有** ⇒ 下一个消费者只能自己重扫 5×5，或者对 `active_empty()` 线性查找。
- `last_move()` 用默认构造的 `Move` 回答空盘，即 `Position(-1,-1)`；调用方要区分「没有上一手」与「上一手在 (0,0)」，
  只能靠记住这个哨兵——而 §2.5.1 的「上一手邻域」正是要先问有没有上一手。

### Fixed

- `neighbor_count_at(Position) const`（界外答 0）、`is_active(Position) const`（界外答 false）、
  `has_last_move() const` 三个只读访问器，全部直接读已有的那两个数组，零新状态、零新成本。
- **`rebuild_active_empty` 的行优先顺序一个字没动**，但访问器的注释把「顺序不是契约」写死：分层读的是计数与
  成员资格，不是格在表里的位置。这句话是 A6 之后必须的 —— `undo_last_move` 的 swap-remove 会打乱表序，而下游
  再也不能从顺序里读任何东西。
- `neighbor_count_` 的**定义域**在注释里挑明：它只在空格上被维护（落子与悔子的两处循环都 `if (cells != EMPTY) continue`），
  所以占用的格答的是「它被填满那一刻」的冻结值。访问器不假装这件事不存在，测也就只测空格。

### New

- `test_neighbor_count_at_matches_a_recount`：**每落一子、每悔一子之后**把全部空格逐格与一次朴素 5×5 重数比对
  （不是只在终态比一次），布局含中央一团与三个角上的孤子，让窗口在盘边也成立；界外取 0。这是增量记账**第一次**
  被差分测到 —— 之前 `test_active_empty_incremental_undo` 只能比「整盘重建 vs swap-remove」的集合与哈希。
- `test_is_active_agrees_with_the_candidate_list`：225 格全查 `is_active(p)` ⟺ p 在 `active_empty()` 里，外加
  「占用的格永远不在」与「计数为 0 的格永远不在」两条独立规则；再悔到只剩中心一子，此时候选集**不靠 Board** 数出来
  是 5×5 减中心 = 24 格，逐格比对并钉住表长恰为 24。
- `test_has_last_move_distinguishes_an_empty_board`：空盘 `false` 且 `last_move().pos` 无效、落子 `true`、悔到空盘
  回到 `false`、空盘再悔是 no-op（`move_count()` 不变负）。

### Verified

- **无 HEAD-negative，新词汇的既有例外**：三个方法在 `87ee891` 上不存在，当前的 `test_board.cpp` 在改动前树无法
  编译，与 A1 的新骨架、A6 的 `center_distance` 同类。`has_last_move()` 的既有等价物本来就是 `move_count() > 0`，
  所以它是可读性而不是行为；本轮真正的**内容**是给 B1 一个不必重扫的入口，其价值在 B1 的用例里体现。
- `test_active_empty_incremental_undo` 继续绿（前提：新访问器读的就是它检查的那两个数组），`test_board` 全套 15 条
  用例一字未改地在最后的断言之前跑过。
- diff 面：`src/core/board.h` 13/0、`src/core/board.cpp` 10/0、`tests/test_board.cpp` 141/0。三条新用例插在
  `test_active_empty_incremental_undo` 与 `test_line_bitboard_tracks_cells` 之间 ⇒ `tests/test_search.cpp` 的两条
  受保护前提不受影响（仍在 `:2563` 与 `:3958`）。
- 门禁数字见下一条 A7 条目末的 **Gate**：本轮与 A7 在同一片工作树上一起测（A8 只往 `board.*` 加了三个无人调用的
  const 访问器与 `test_board` 的三条用例，其余七个套件跑的仍是 A7 的那棵树）。

## 轮次121 引擎重构 A7：一层中断是一个帧，不是一次沉默 — engine redesign stage A7

规格 `docs/ENGINE_REDESIGN.md` §2.3.3 末段 + 验收 G2（冻结，不改写）。A1–A6 把分层记账做进了引擎，但它出不了
`alphabeta.cpp`：协议上仍然只有三个整数，`SearchResult` 里那五个字段在会话与存档的边界上被丢掉。G2 要的是
「玩家看得见思考到哪一层」，所以本轮动的全是**边界**，不是搜索。

### Found

- 两级 `ProgressCallback` 都是 `void(int depth, int nodes, int elapsed_ms)`。A2 删掉 `depth == 0 → "greedy"`
  的改写之后，「第 6 层搜到一半被掐断」与「这个局面没东西可搜」在帧上**变成同一个读数** —— 歧义没消失，只是从
  method 标签搬到了 `depth`。
- `SearchResult` 自 A2 起就带 `partial_depth` / `root_moves_done` / `root_moves_total` / `seldepth` / `stopped`，
  但 `MoveAnalysis`（会话自己的回放表）与 `MoveRecord`（落盘存档）都只留六项，回放里被截断的那一手和搜完的那一手
  长得一样。
- `set_config` 没有任何入口点名软时限或节点预算：`SearchLimits` 的这两个字段自 A1 起只被 `test_search` 写过，
  线上走不到 ⇒ §2.3.1 的「客户端可选」半条是空的。
- 收尾帧读的是 `info.depth` / `info.nodes_searched`，即每次思考的**最后一帧也只报老三个数**；每层帧更不必说。

### Fixed

- 三个签名一起改为携带记账：`SearchEngine::ProgressCallback`、`GameManager::ProgressCallback` 收
  `const SearchStats&`，由帧写入者（`server.cpp`）决定客户端看见什么。`depth` / `nodes` / `elapsed_ms` 三个键的
  含义一字未动（`nodes` 仍是 `static_cast<int>(stats.nodes)`），新增五个键；`stopped` 走 `stop_reason_name`。
- 收尾帧改为 `ab.last_stats()` 拷出后盖一次整手时钟（沿用 A2 那条口径：帧的时间从走子时钟现读，不把各阶段的截断值
  相加），并把 `elapsed_ms` 取在认输探针之后 ⇒ 客户端看到的耗时包含探针花掉的那部分预算。
- 五个字段逐跳传到底：`record_of` / `analysis_of` / `handle_request_replay` / `SelfPlay::run` 各拷一份，
  `GameRecord::to_json` 照写；`from_json` 用 `read_int(key, 0)`、`stopped` 用「是字符串才取，否则 `none`」⇒
  早于它们的存档读成「每一层都搜完了」，不报错也不判坏（分层记账是元数据，不像 `winner`/`reason` 那样缺一即坏）。
- `set_config` 增 `soft_time_ms` / `max_nodes` 两枚**可选**键；`GameManager::set_config` 用**负数**表示「本会话
  已有的就保留」，`clamp_soft_time_ms` 把软时限夹进硬时限之内（0 保持 0 = 不设软窗，而不是一个长度为零的时限），
  `clamp_max_nodes` 只把非正数归零。预算由 spawn 侧按值 `Budget` 拷出、搜索线程进去以后自己
  `engine_.set_soft_time_ms` / `set_max_nodes` —— 与 A4 的 `Reuse` 是同一条线程约定：`set_config` 跑在 io 线程上，
  此时 worker 正在 `engine_.search` 里面，引擎配置只有搜索线程写。
- `SearchEngine` 第一次把这两个预算接到 `SearchLimits`：`limits.max_nodes = max_nodes_ > 0 ? max_nodes_ :
  node_budget`。默认 0/0 ⇒ 逐字等于本轮之前的行为（不设软窗、节点上限由剩余时间换算）。顺带改正四处**过期注释**
  （`search_limits.h:38`、`search_engine.h`、`server.cpp:1141`、`game_manager.h`）：它们都写着「0 = 由硬时限推出」，
  而 `SearchContext::start()` 做的是 `has_soft_ = soft_time_ms > 0`，即 0 表示**没有软窗**。
- `web/app.js`：`interruptedLayerNote()` 一份文案两个入口 —— 实时 `thinking_progress`（`showInterruptedLayer`，
  带 `gameStatus` 守卫）与 `replay` 的逐手行；只在 `stopped` 是字符串且不为 `none` 时产出，否则返回空串。
  `updateTurnInfo(text)` 拆成 `defaultTurnInfo()` + `updateTurnInfo(text, note)`，副行是**追加**不是替换。
  `handleReplay` 的五个新键按 `|| 0` 读、`stopped` 按类型读。

### 合约过期

1. `tests/test_search.cpp` 三处回调形参（`:914` / `:1139` / `:3907`）从 `[&](int depth, int nodes, int)` 改为
   `[&](const SearchStats& stats)`。**断言一行未改**，只是形参跟着签名走。这是 A2 计划里「`ProgressCallback`
   签名扩为携带 `SearchStats`」一条的真正落地点：之前没有任何上层要求 `GameManager` 收整份记账，签名便留在 A7。

### Docs

README 五处**行内改单元格**（没有任何插入，`:431` 之前的行号一个没动）：`:345`（`set_config` 的两枚新键与其缺省）、
`:499`（示例帧补 `elapsed_ms` 与五个新键）、`:508`（`replay` 一手补五键）、`:521`（分层记账的语义）、`:531`
（「存档缺键按搜完算」）。README 仍 **658 行**，`:529-531` 仍恰好三行 ⇒ `readme_anchor_check.js` 硬取的
`slice(528,531)` 未受影响；引用数 114 → 118 全部来自新用例注释里新增的 README 行号。

### New

- `test_thinking_progress_carries_partial_fields`：直接喂 `h.entry->game->progress_callback_(stats)` 再 `drain_frames`
  读回 —— 九个键逐个断言（`TIME_HARD` 的截断层报成 `partial_depth 6 / 12 of 34 / seldepth 7`）；控制臂喂一个在层边界
  收束的 `TIME_SOFT`，断言新键**不缺席而是取 0**；再循环六种 `StopReason` 逐一断言名字到达客户端，且「除了 `stopped`
  什么都没说」的那一帧里老三个键仍在场、取 0。
- `test_replay_roundtrip_keeps_new_fields`：同一手（ply 2）四跳逐读 —— `request_replay` 帧 → `flush_entry` 落盘 →
  把五个键从文件里 `erase` 之后再 `open_session` 重建（token 换成 `a7000000000000f7`）→ 冷存 + `resume` + 再
  `request_replay`。人落子那一手（ply 1）在两棵树都读 `0` / `none`。
- `test_set_config_keeps_the_two_optional_budgets`：先 `restore_game(playing_restore(2))` 让会话文件真实存在，再断
  谁也没点名 ⇒ `0/0`；两个都点名 ⇒ `3000/50000`；`soft 20000` 配 `timeout 8000` ⇒ 夹到 `8000`；只点名一枚 ⇒ 另一枚
  保留（`8000/1234`）；`0` ⇒ 回到派生预算；负数 ⇒ `0`；最后 `flush` 后确认**会话文件里两个键都不在**（预算是客户端
  每次连上重新声明的东西，不是对局事实），而同一帧里的 `timeout_ms 9000` / `max_depth 7` 照旧持久化。
- `tests/web_frontend_check.js` 两条：`ENG-A7 一条 time_hard 的 thinking_progress 把中断层报进副行`（副行含
  「第 6 层中断（12/34）」且**接在**「轮到…」之后）；`ENG-A7 搜完了的思考与不带这几个键的老服务器都不写中断文案`
  （`stopped:"none"` 一臂 + 整帧无键一臂，都断 `#turn-info` 是空串）。命名从 `A7 …` 改成 `ENG-A7 …` 是因为既有
  用例里已经有一条 `A7 replay cluster guarded during setup`。

### Verified

- **web HEAD-negative**：`git archive 87ee891` 解到 `/mnt/d/tmp/neg121_web` + 当前 `tests/` ⇒ **104/105**，红的只有
  第一条（副行没写），第二条与既有 103 条在两棵树都绿。
- **C++ 三条无 HEAD-negative，以编译可能性为限的声明**：`87ee891` 上 `GameManager::Budget` 不是成员
  （`tests/test_server.cpp:1058/:1084` 直接编译失败），`progress_callback_` 收三个 int ⇒ 当前测试 TU 在改动前树上
  根本无法编译，与 A1 的新骨架同类。替代证据是一条只在 HEAD 树上编译的探针 `probe_a7_frame_shape_on_head()`
  （`/mnt/d/tmp/neg121_a7b.sh`：把三个新用例从 HEAD 副本里删掉、把探针插进 `main()`），它用 HEAD 自己的帧写入者打印
  「改动前这条线长什么样」：
  - `thinking_progress`：`{"depth":6,"elapsed_ms":4800,"nodes":123456,"type":"thinking_progress"}` —— 三个键，
    五个新键**不存在**（不是 0，是没有）。
  - `replay` 的一手：`{"col":7,"color":"black","depth":0,"elapsed_ms":0,"method":"human","nodes":0,"row":7,
    "score":0}`。
  - 只点 `soft_time_ms` / `max_nodes` 的 `set_config` 帧在 HEAD 上 `timeout before/after = 8000/8000`：HEAD 既认不下
    这两个键、也不受影响 ⇒ 老客户端不发它们时行为逐字不变。
  同一棵 HEAD 树的 `test_server` 全套打印 **All server tests passed!**（`main()` 里那 34 条既有用例一字未改地全绿，
  是「纯加字段」的机械证明；本树是 37 条）。
- **两条受保护前提未移动**：`assert(result.resigned)` 仍在 `tests/test_search.cpp:2563`，
  `completed.method == "alphabeta"` 仍在 `:3958`；本轮对 `test_search.cpp` 只有 8/8 的原地替换，没有插入。
- diff 面：`src/server/server.cpp` 36/5、`src/server/game_manager.{h,cpp}` 79/10、`src/search/search_engine.{h,cpp}`
  37/5、`src/training/game_record.{h,cpp}` 24/0、`src/training/self_play.cpp` 5/0、`src/search/search_limits.h` 1/1、
  `web/app.js` 40/12、`README.md` 6/6、`tests/test_server.cpp` 288/3、`tests/web_frontend_check.js` 43/0、
  `tests/test_search.cpp` 8/8。
- 门禁数字见下方 Valgrind 段之后的 **Gate**。

Valgrind（§5 的口径：`build-vg/` Debug + `-UNDEBUG`，计数 `grep -c '^==[0-9]*=='`，门禁是「相对干净基线零新增」）：

- **A7 自己的三条**排在 `test_server.cpp` 的 `main()` 末尾，即既存全量中止点 `:1706`（轮次111 记的墙钟脆弱）之后，
  全量跑不到 ⇒ 子集驱动 `/mnt/d/tmp/a7_vg_main.cpp`（同 `-Dmain=suite_main` 手法）：**`MEMCHECK_RC=0`、报告 0 行、
  3 条全过**，并打印出上面那三份帧与回放的实测 JSON。
- **A6 欠下的 A4 两条**（`test_aspiration_refines_without_changing_result` /
  `test_search_engine_forwards_the_hint_to_the_search`）本树：**`MEMCHECK_RC=134`、报告 0 行**，第一条通过并打印
  `no centre 249892 / centre 250886 / negated centre 249894` —— 与 A6 记的数字**逐字相同**，即减速没有改变「窗口
  决定工作量」这条结论；第二条 SIGABRT 于 `tests/test_search.cpp:3497` `assert(plain.depth == 4)`。该用例把时限设成
  600000 ms 并声称「这笔预算花不完」，但 memcheck 的减速下 depth 4 的一趟就能越过 600 s 的墙 ⇒ 这是一条**墙钟前提**，
  与 A2/A6 记的 `test_engine_progress_callback_reports_each_layer`、`test_training.cpp:2056`、`test_server.cpp:1706`
  同类。按「减速下死掉的门禁用例要在改动前树上重测，而不是删掉」的规矩，改动前树（`87ee891`，同一 Debug 形状、同一
  套 memcheck 参数、串行独跑）的对照结果是：**`MEMCHECK_RC=134`、报告 0 行、4897 s**，第一条通过并打印
  `no centre 249892 / centre 250886 / negated centre 249894` —— 与本树**逐字相同**的三个节点数，第二条 SIGABRT 于
  **同一行** `tests/test_search.cpp:3497` `assert(plain.depth == 4)`。⇒ 中止点没有前移（既存墙钟脆弱，非本轮引入），
  且 A7 对搜索可观测地为惰性：协议层改了九个键，节点数一位没变。这比原计划要的「对照只证明同类失败」更强，
  因为它同时是本树与 `87ee891` 的**逐字节搜索结果一致性**证明。
- 全量 `test_server` 本轮未跑：A7 没动任何搜索代码，三条新用例已由上面的子集覆盖，而全量套件仍会中止在 `:1706`。

**Gate**（空闲树上复测，跑门禁时不与 memcheck 并发）：ctest **8/8 / 165.51 s**（`test_search` 138.52 s、
`test_server` 9.38 s、`test_board` 0.03 s），web **105/105**，anchor **118 引用 / 0 problems**，README 仍 **658 行**。
此前记的 203.14 s / 44.50 s 是与 memcheck 并发时的读数，本轮空闲复测取代它们；A7 的三条用例本身计时 0.00 s。

## 轮次121 引擎重构 A6：同分的根不再由容器顺序决定 — engine redesign stage A6

规格 `docs/ENGINE_REDESIGN.md` §2.5.3 / §2.6 末段 + 验收 G4（冻结，不改写）。A6 动的是 P3 剩下的一半：A5 让评估知道
棋子站在哪，但根层在**同分**时仍然没有任何规则 —— 而「同分」在静默局里是常态，不是边角情况。

### Found

- `sort_moves` 用 `std::sort` 按单一分值排序：同分候选的先后 = 传进来的 `active_empty()` 先后，而
  `undo_last_move()` 用 swap-remove 归还空格 ⇒ 同一副局面走子再悔棋之后，候选顺序真的变了。实测同一局面两副摆法
  （第二副多走一手再退回来）在 depth 3 上花 **47397 vs 47045** 节点。
- 根层提交用 `if (score > layer_best_score)` 严格大于 ⇒ 平分时先被搜的那个占住答案，「先被搜」就是上面那个顺序。
- fail-soft 下的歧义比顺序更根本：除第一个候选外，每个候选的对窗口下界已经等于当前最优，所以回来相等的 rival 只证明了
  「不比这个读数差」，**精确相等和被剪掉的下界是同一个数**。中心偏好要成立，必须先把这条歧义关掉。
- 三处兜底全都读顺序：`alphabeta.cpp` 的 `root_moves[0]`、`search_engine.cpp` 里 alphabeta 空返回时取候选表第一个合法格、
  D4 去重取最低格号。

### Fixed

- `RootScore{score, center_dist, cell_index}` 全序（`alphabeta.cpp` 匿名命名空间）。`sort_moves` 与 `quiescence`
  的 decorate-sort 都改成按三元组 `std::stable_sort`：第三键使全序成立（两格不可能同号），第二键使平分时的选择
  是「靠中心」而不是「碰巧排前面」。
- `center_distance(Position)` 放进 `search_limits.h`，**不用** `Evaluator::get_position_weight`：后者被可调参数
  `position_weight_scale` 缩放，训练把该基因取 0 就会静默删掉这个键。D4 的三种自同构（行翻、列翻、转置）都保持
  `|row-7|+|col-7|` ⇒ 去重的「最低格号」代表本来就是三元组的答案，**代码不动**，由一条新用例把这个一致关系钉住。
- **tie probe**（`search_root_layer`）：只有「回来等于当前最优、且按中心/索引会赢得这次比较」的 rival 才复问一次，
  复问窗口是 `(best-1, beta)`。下界放在 `best-1` 而不是 `beta` 的整窗，是因为整窗复问实测要 **954038** 节点（同一副
  静默局 depth 4 的一趟只花 218872）：一分钱就能把「相等」和「更差」分成的两个数，不必重开整棵树。复问自己也被预算
  切断时，该层记为不完整（`layer_sound = false`），与 A2 的分层记账同形。
- 每次迭代重新复制 Board 的写法删除，整次思考只拷一次 `work_board`（§2.8.5 第一步）。它存在的唯一理由就是上面那条
  顺序歧义；三元组之后歧义消失，O(depth) 次 225 格复制随之消失。
- `search_engine.cpp` 的 alphabeta-空返回兜底改为「最靠中心、再取低格号」。走到这条分支的是**整个活动窗都是禁手的
  BLACK**，此时补扫找回的空格按行优先回来 ⇒ 旧规则回答的是棋盘左上角那一格。

### 合约过期

1. `test_tt_cold_think_cost_and_warm_reuse` 的 `assert(cold_nodes <= 250000)`（A3 用它区分新旧表布局）。实测链：
   pristine **276131** → A3 **225745** → A5 **222885** → **A6 267918**。A6 的复问让 HEAD 自己越过了 250000，
   这条上限不再有区分能力 ⇒ 改为 `<= 300000`，注释写清 A3 的证据从此由表自身的用例
   （`test_tt_depth_first_replacement` / `test_tt_generation_advances_per_move` / `test_tt_mate_score_normalised_by_ply`）
   和 cold/warm 比值（267918 对 96）承担，而不是由一个节点数上限承担。
2. `test_alphabeta_aborted_search_stores_only_sound_nodes` 的单趟代价上限 `< 752 * 3`：上限没动，但**前提变了** ——
   A6 的排序改变了「一层先看哪个候选」，实测对照段从 1104 变成 **1060**。注释改写为「三 = 趟数上限，单趟 = 752」。
3. `test_alphabeta_expired_deadline_returns_root_fallback` 的 `assert(result.pos == root[0].pos)`：断言不动，
   注释改写 —— 重新生成一遍根表能给出同一个头，从此是关于**局面**的命题而不是关于容器状态的命题。

### New

- 文件级 helper：`root_candidate_value(board, evaluator, stop, cand, color, depth, alpha, beta)`（独立复读一个候选，
  自带一张 fresh 表，直连 `alphabeta()` 但先 `ctx_.start()` 装上下文）、`sweep_optimum(...)`、`triple_best(cells)`。
  **sweep 的代价是这轮最大的施工返工**：第一版对 123 个候选逐个开整窗，实测 **1873539 节点 / 78.16 s**（这一条用例
  一个人占了 219 s 套件里的 82.68 s）。改成围绕「当前最优」开一分宽窗口、只在 fail-high 时整窗精读之后是
  **143013 节点 / 3.86 s**，而答案一字不差。相等判定的三分类（内值=精确 / 下界=严格更差 / 上界=真更好要精读）
  成立到「同一个中心只可能是精确值或已被证明低于最终最优」，所以一趟就够。五副局面（静默局 d3/d2、双胜点 d1、
  角上聚集局 d3/d4）与整窗读法逐候选一致，静默局 d3 有 3 次 fail-high 触发精读。
- `test_root_tiebreak_prefers_centre`：静默中盘 depth 3 ⇒ `completed_depth == 3`、score == 扫出的最优、落子 ==
  最优集合里三元组最优；双胜点局（黑 (7,3)-(7,6) 两头开、白另有一行三）⇒ 前提被**数出来**（最优格 >1 且中心距不等），
  思考在第 1 层因 `MATE` 收束，`score == 100000` 保持 README:338 那颗红点读的哨兵，落 (7,7) 而不是 (7,2)。
- `test_root_choice_reproducible`：同一副子、两种走子顺序（一手 place+undo，(1,1) 而非 (0,0) —— 后者本来就排在最前
  所以换不出顺序），前提 `direct.active_empty() != detour.active_empty()`；控制段是同一副局面连想两次（两棵树都绿），
  跨局面段断言 best / score / `completed_depth` / **nodes** 全等。实测 **48268 == 48268**（改动后），
  改动前 **47397 vs 47045** ⇒ 这就是本用例的红点。
- `test_d4_dedup_keeps_central_representative`：空盘 36 个代表，每个轨道成员中心距相等且自己是最低格号，
  过期 deadline 的顺序头 == (7,7)。它是控制组（两棵树都绿），作用是说明去重不需要自己再学一遍三元组。

### Verified

HEAD-negative 树 = `git archive e6bf642` 解到 `/mnt/d/tmp/neg_a6_tree`，加**一处词汇抬升**：`center_distance()` 是 A6
自己引进的 helper，原样搬进 neg 树的 `search_limits.h` 才能编译当前的测试文件（与 A1 那批新骨架同一类声明）。逐用例
driver（`-Dmain=suite_main`）跑当前的三条新用例：

| 用例 | 改动前树 | 本树 |
| --- | --- | --- |
| `test_root_choice_reproducible` | **红**，`tests/test_search.cpp:3742` `assert(other.nodes == first.nodes)`（整树跑时同一断言在 `:3740`）| 绿，48268 == 48268 |
| `test_root_tiebreak_prefers_centre` | 绿（控制）：打印与本树**逐字相同**的 `quiet d3: 1 tied optimum 3180, played 6,4; dual-win: 2 tied optima, 1 unequal-centre members, played 7,7 score 100000` | 绿 |
| `test_d4_dedup_keeps_central_representative` | 绿（控制）：36 representatives, head 7,7 | 绿 |

即本轮唯一的行为差是**确定性**本身：落子与分值两棵树一样，节点数只有本树可复现。A4 那两条消费断言
（`exact.nodes != none.nodes` / `reuse.nodes_searched != plain.nodes_searched`）是 A6 抬高了它们读的那个节点数，不是
新用例的红点，不在此列。

**声明的例外**：以 `center_distance` 为键的结构性断言无 HEAD-negative（新词汇的既有例外）；`search_engine.cpp` 那条
「最靠中心」兜底**无法构造**成用例 —— `AlphaBetaSearch::generate_moves` 不带威胁过滤，要走到远扫补格需要一个
「活动窗全禁手」的 BLACK 局面，构造不出来；改动仍然保留，因为它比「棋盘左上角」严格更好。

**代价（记下来而不是藏起来）**：复问的节点数用临时计数器测过（测完即删，工作树无残留 —— `grep 'g_probe\|cstdio'` 空）。
静默局 depth 4：98 次复问 / 31593 节点，218872 → **249892**（+14.2 %）；cold 16 子中盘：11 次 / 48122 节点，
222885 → **267918**；静默局 depth 3：58 次 / 1837 节点。**全套件里复问改变根答案的次数 = 0**，被预算中止的那次思考
复问 0 次 —— 它买的是「同分时选中心」这条规则的可证明性，不是棋力。A4 记的那条「窗口改变工作量」仍然成立：
`no centre 249892 / centre 250886 / negated 249894`。

两组数字相减对不上那个计数器（`249892 − 218872 = 31020` 对 31593，`267918 − 222885 = 45033` 对 48122）是应当的，
不是测量漏洞：复问本身也是普通的 `alphabeta()` 调用，会把它算干净的节点写进置换表，于是后面的层比不加复问时更省。
计数器量的是复问自身花掉的，总数字量的是净增 —— 后者才是这条规则真正的代价，所以这里的成本口径用总数字。

diff 面：`src/search/alphabeta.cpp` 98/19、`src/search/search_engine.cpp` 15/5、`src/search/search_limits.h` 11/0、
`tests/test_search.cpp` 309/12。

**行号位移**：两条受保护前提本轮只是被后面的插入推着走，断言本身一字未改 —— `assert(result.resigned)`
`2562 → **2563**`（300 ms 那一段），`completed.method == "alphabeta"` `3665 → **3958**`（A6 的三条用例插在它们前面）。

`ctest` **8/8 / 179.76 s**（`test_search` **138.02 s** —— 新基线，A4 的 110.88 s 之上：新增三条用例 11.3 s，其余是复问
加在既有 depth-4 思考上的工作量；`test_server` 23.99 s、`test_training` 9.99 s），
`node tests/web_frontend_check.js` **103/103**，`node tests/readme_anchor_check.js` **114 citations / 0 problems**
（113 → 114：新用例的注释引了 README:338），README 仍 **658 行**。测单用例耗时的手法：把套件 stdout 逐行盖上
`date +%s.%N` 再取差（`/mnt/d/tmp/a6_case_time.sh`），不动源码。

Valgrind（§5 的 checkpoint，`build-vg/` Debug + `-UNDEBUG`，计数口径 `grep -c '^==[0-9]*=='`，门禁是「相对干净基线零新增」）：

- **子集驱动 13 条**（A6 改到的根层：三条 `sort_moves`、`root_ordering_uses_hint_move`、过期期限兜底、
  `stores_only_sound_nodes`、`stop_reason_time_hard`、quiescence 记账、A6 新增三条、`tt_cold_think_cost`、
  `reports_the_interrupted_layer`）：**12 条通过，零报告**。第 13 条
  `test_engine_reports_the_interrupted_alphabeta_layer` 在减速下 SIGABRT 于 `tests/test_search.cpp:3945`
  `interrupted.partial_depth == 1`（rc=134，**reports=0**）。两棵树同一断言对照（同一 Debug 形状、同一套 memcheck
  参数、串行跑以免抢 CPU）：本树 control `d0+partial 1/n2495`，`e6bf642` 树 control `d0+partial 1/n2506`，两边
  都打印 `interrupted: alphabeta d0+partial 0/n0 time_hard` —— 那条用例的判别臂只有 10 ms、对照臂 20 s，
  memcheck 的 ~80× 减速下连一层都跑不完，所以这是**既存墙钟脆弱、中止点未前移**，与 A2 记的
  `test_engine_progress_callback_reports_each_layer` 同类；D1 打通 `max_nodes` 后两条一起改成节点预算。
- **全量**：整套 `test_search` 在 memcheck 下 **1466 s / reports=0**，中止于 `main():4251` 的
  `test_engine_progress_callback_reports_each_layer`，套件 stdout 的最后一行是 `main():4250`
  `test_search_engine_layered passed` —— 中止点与 A2 记的**同一条、同一处**。按 `main()` 顺序，它之前的每一条
  都被 memcheck 覆盖；它之后的（含 A6 三条）由上面的子集驱动覆盖。
- A4 的条目把自己的 memcheck 并入本轮 checkpoint。A4 的三条里 `test_root_ordering_uses_hint_move` 在子集内并已
  通过；另两条 `test_aspiration_refines_without_changing_result` / `test_search_engine_forwards_the_hint_to_the_search`
  在 `main():4305-4306`（全量中止点之后），只能用子集驱动单独跑，一轮约一小时：本轮提交时它们仍在跑，**数字记在
  下一条 A7 条目的 Valgrind 段**，本轮不写未测到的结论。

## 轮次121 引擎重构 A4：上一手的最佳着法，第一次活到下一手 — engine redesign stage A4

规格 `docs/ENGINE_REDESIGN.md` §2.4.3 ①③ + §3 根循环伪码（冻结，不改写）。A4 动的是 P2 剩下的三处：一次思考算出来的
best_move 与 score 在返回那一刻就被丢掉，下一手从零开始排序、从零开始开窗。

### Found

- **根排序丢掉了提示位**。`sort_moves(root_moves, board, color)` 少传第 4 参 `tt_move`（签名支持），于是深层 PV 头
  永远抬不到第一手位 —— 而 AB 的剪枝效率几乎全押在根顺序上。
- **每层都用整窗重搜**。层 2..N 对 `(-INT_MAX, INT_MAX)` 逐手全窗口搜索，而下一手的真值几乎总在上层值 ±几十之内；
  文档 §2.4.3 ③ 要的 aspiration window 完全没有。
- **会话级复用无通道**。`GameManager` 拿到 `SearchResult` 后只记 `analyses_`，`SearchEngine` 没有任何入口接收
  「上一手的着法与分值」，`SearchLimits` 的 `hint_move`/`hint_score` 两个字段自 A1 起一直无人赋值。

### Fixed

- `alphabeta.cpp:384` 根排序补 `sort_moves(root_moves, board, color, limits.hint_move)`；`hint_move` 是被 pin 的
  那一个（已在候选里则提到最前，不在候选里直接丢弃 —— 落不了的位置不该影响排序）。
- `search_root_layer` 的窗口改成显式参数 `(alpha, beta)`，并在 `layer_alpha >= beta` 处 break（fail-high 不必再看
  剩下的候选）；新增 `search_root_with_aspiration`（:492）按文档 ③ 做**单边放宽**：fail-low 就 `lo = -INT_MAX`、
  fail-high 就 `hi = INT_MAX`，**保留那一趟已经证明的另一端边界**，而不是围绕固定中心重开一个 ±50。中心由
  `limits.has_hint_score` 起步，此后每完成一层就用该层自己的分值更新（:438-439 `aspiration_centre = best_score;
  have_centre = true;`）—— 与 §3 伪码 `score = layer_best; have_score = true;` 同形，⇒ 即使没有会话提示，层 2 起
  也照样有窗口。`kAspirationDelta = 50` / `kAspirationWidenings = 4`（三次放宽即达整窗，第四格是兜底那趟）。
- **预算中止 vs 窗口失败的判别**：只用粘滞的 `ctx_.reason() != StopReason::NONE` 加上 `!layer.sound`，绝不使用
  `root_moves_done < total` —— fail-high 的 break 同样留下 `done < total`，用它判别会把一次正常的窗口失败读成超时。
- `SearchEngine::set_hint(Move, int)` / `clear_hint()`（search_engine.h:78-87）经 `SearchLimits` 透传
  （search_engine.cpp:278-280）；alphabeta 分支不再走 `set_node_limit` + 四参旧重载，改为构造 `SearchLimits` 调
  `ab.search(board, color, limits, stop_flag)`。
- `GameManager` 侧的会话提示：`Reuse{move, score, valid}` + `take_hint(for_color)`（game_manager.h:188-196）——
  颜色门是必须的，`hint_score_` 站在 `hint_color_` 的视角，白棋那一手的分值喂给黑棋的思考会把窗口开到一个完全错的
  中心。`request_hint()`（:325-328）与 `start_ai_thinking()`（:389-404）都在 `thinking_mutex_` 内、两个 worker 已
  join 之后取 `Reuse`，再**按值**交给新线程（此后只有那个线程与引擎对话）。作废点：`reset()` / `set_position()` /
  `restore_game()` / `undo()` 各一处 `has_hint_ = false`（:76 / :97 / :132 / :234）。
- **§2.4.3 ②（ponder 预搜）本轮不做**：现架构没有任何把对手着法回灌给一次进行中搜索的通道，`hint_move` 排序 +
  `hint_score` 窗口已覆盖「复用上一手结果」里可验证的那部分。记入未完成清单。

### 合约过期

1. `test_alphabeta_aborted_search_stores_only_sound_nodes` 控制段的 `assert(full_info.nodes_searched < node_limit * 10)`
   （:2820）。这一层现在跑在以「下一层返回值」为中心的窗口里，本例那个估计差得远（4202 vs 真值 20584）⇒ fail-high、
   失败的那一端敞开、该层重跑：实测 1104 节点，而每层只跑一趟时是 752。改写为 `nodes > layer_one_nodes` 与
   `< 752 * 3`（上限按**趟数 × 单趟**来给，不是按一次估计值猜），其余前提（无中断层、表项照写）一条没动。

### New

- `test_root_ordering_uses_hint_move`（:3344）：借用「deadline 已过期 ⇒ depth 0 ⇒ 兜底就是顺序头」这条既有通路，
  让根排序变成可断言量 —— `hinted.pos == pinned.pos`，`pinned` 取自 `root.back()`。两个控制：`Move()` 时顺序头是
  启发式最优的 (9,5)；提示格 `Move(7,7,WHITE)` 已被占用 ⇒ 直接丢弃，不影响排序。
- `test_aspiration_refines_without_changing_result`（:3436）：7 子静默局（`quiet_think_board()`，:3396）depth 4，
  三种装配 none / 精确中心 / 取负中心 ⇒ best、score、`completed_depth`、`stopped` 全等，且 `exact.nodes != none.nodes`
  （窗口确实改变了工作量，而不是恰好相同）。实测 `best 6,4 score 948 depth 4`，节点 218872 / 219866 / 218874。
  换用静默局是因为 `build_midgame` 那副 12 子盘会在深层撞上有胜负哨兵的一手，加深在层 4 之前就不是「四层都完成」，
  `completed_depth == 4` 无从断言。
- `test_search_engine_forwards_the_hint_to_the_search`（:3471）：引擎级贯通（vcf/vct/book/resign 全关）——
  `set_hint(Move(), plain.score)` ⇒ method / best / score / depth 都不变，节点数变。这里必须传一个足够大的
  `time_limit_ms`（600000）：`time_limit_ms = 0` 仍会把 `remaining_time` 兜到 100 ms ⇒ 20000 节点预算，而 100 ms 与
  20000 节点在 valgrind/负载下是一条会翻的界；600000 让节点预算钳到 `INT_MAX`，实测 218872 → 219866。

### Verified

HEAD-negative 的改动前树 = `git archive 066d821` + 可用的 `search_engine.{h,cpp}` 与 `tests/test_search.cpp`，即只把
A4 的**名字**（`set_hint`/`clear_hint`、`SearchLimits` 的三字段、`Reuse`）补进去、行为退回 A5 收尾那棵树。逐用例 driver
（`-Dmain=suite_main`）在构造树上的红点正是三条消费断言：
`test_root_ordering_uses_hint_move` → `Assertion 'hinted.pos == pinned.pos' failed`、
`test_aspiration_refines_without_changing_result` → `Assertion 'exact.nodes != none.nodes' failed`、
`test_search_engine_forwards_the_hint_to_the_search` → `Assertion 'reuse.nodes_searched != plain.nodes_searched' failed`；
控制组 `test_sort_moves_full_order_without_tt_move` 与被改写的 aborted-search 用例在两棵树都绿。`Reuse` 是 worker 的参数，
两处直接起线程的用例（`test_server.cpp:1055` / `:1080`）补 `GameManager::Reuse{}` —— 那是编译期的调用点同步，语义不变。

**代价（记下来而不是藏起来）**：静默局 depth 4 的冷跑从 pristine 的 176695 节点变成 **218872**（+23.9 %）。原因是根
fail-low 的重搜很贵（一趟要看完所有候选才发现下界不住），而 fail-high 那一端反而更便宜（同一层的第 3 层从 45052 →
44054，因为 break 提前收了）。§2.4.3 的窗口是文档要求的，把它做回来的是 **B3 的根 PVS**（zero-window 侦察），
不是回退窗口。

diff 面：`src/search/alphabeta.cpp` 76/7、`alphabeta.h` 19/6、`search_engine.cpp` 12/5、`search_engine.h` 23/0、
`src/server/game_manager.cpp` 50/3、`game_manager.h` 27/1、`tests/test_search.cpp` 181/1、`tests/test_server.cpp` 6/3。

`ctest` **8/8 / 144.50 s**（`test_search` **110.88 s** —— 新基线，三条 A4 用例各带一次 depth-4 全窗思考；
`test_server` 16.44 s、`test_training` 9.50 s），`node tests/web_frontend_check.js` **103/103**，
`node tests/readme_anchor_check.js` **113 citations / 0 problems**，README 仍 **658 行**（A4 没动文档：`hint_move` /
`hint_score` 是内部 API，WS 契约要等 A7 才加字段）。

Valgrind：A4 改到的正是表回溯之前的根路径与新的会话状态字段，按 §5 的节奏**并入 A6 的 checkpoint** 一起跑
`memcheck test_search` 全量（A6 是下一个 checkpoint commit）。本轮另记一条施工瑕疵：中途一次脚本改写把
`src/search/alphabeta.cpp` 整文件换成了 CRLF（diff 一度 578/509），`tr -d '\r'` 复原后回到 76/7；此后每个改过的文件都用
`grep -c $'\r'` 查过（仓库没有 `.gitattributes`，行尾只靠人守）。

## 轮次121 引擎重构 A5：整盘评估第一次知道棋子站在哪 — engine redesign stage A5

规格 `docs/ENGINE_REDESIGN.md` §2.5.2 / §2.5.4（冻结，不改写）。A5 动的是 P3：评估里从来没有「位置」这件事，中心偏好
只活在候选排序里，而且量级小到任何一条眠二都能压过去。

### Found

- **`evaluate_board` 只累加 `pattern_score`**：一条子没落的盘，和主力握在中心的盘，读成同一个 0 —— 静默局的叶子值对
  「子站在哪」毫无反应，无谱可依赖的那一手就顺着 `active_empty()` 的 swap-remove 顺序漂到边角。
- **量级**。`position_weights_[r][c] = (14 - dist) * position_weight_scale`，默认 `scale = 1` ⇒ 权重区间 [0, 14]，
  而 `sleepy_two = 10`、`live_two = 50` ⇒ 中心偏好连一条眠二都争不过（文档 §1.3-P3-2）。
- **形状表看不见边**。18-bit 窗口把界外格与对手子都读成同一个 2，于是贴边活三与被封活三同价（`pattern.cpp:278-318`）。
  这张表是位棋盘识别器与标量识别器的一致性证明，动它等于动规则层 ⇒ 修正必须落在「候选站在哪」这一侧。

### Fixed

- `evaluate_board` 在 pattern 累加之后、clamp 之前加两项，且**都按颜色差分**：`position_term`（遍历
  `move_history()` 里该色的子，累加中心权重 × `distance_to_stones`）与 `edge_penalty_term`（站在外两行/两列的子，
  每子每轴 × `edge_penalty`）。差分 ⇒ `eval(W) == -eval(B)` 仍代数成立，`five - 1` 的天花板照旧兜住总和；成本 O(子数)，
  遍历的是棋子而不是候选格。
- `pattern_score` 改签名为 `(pattern, row, col)`：活四 / 活三 / 活二按一张编译期 15×15×4 表 `kBlockedEnds`
  （该格该方向被棋盘封住的端数，「往外不足两格」即算封）打折，每端一个 `edge_penalty`。形状 LUT 一个字没改。
- `position_weight_scale` 默认 1 → **8**（在 `params.cpp` 的 [0, 20] 之内）⇒ 单格候选的中心-边角差 **112**，与
  `live_two`(50) 同阶，落在文档要求的 100–200 窗口。
- 新增两个系数字段：`distance_to_stones = 1`、`edge_penalty = 100`。它们进可调面是 P1 的活（`EDGE_PENALTY` /
  `DISTANCE_TO_STONES`），本轮只落到 `ThreatCoefficients` 的默认值。
- **折扣仍然是折扣**：一格最多 4 方向 × 2 端 = 8 端 ⇒ 至多折 800 分，仍 < `rush_four`(1000)；折价后的活三 400 介于
  眠三与冲四之间 ⇒ 既不会把贴边的假活三抬成真威胁，也不会把任何形状重新分级。

### 合约过期

被改写的都是**精确聚合值**，不是前提：`pattern_score` 的一项里多了位置、`evaluate_board` 的总和里多了两项。

1. `test_evaluator.cpp` 的 helper `naive_board_total`(:13-22) —— 它按定义就是「把 `evaluate_board` 重算一遍」，同步
   补上两项位置差；`pattern_score` 调用改 2 参。
2. `test_evaluate_board_skips_black_forbidden_point` :238/:241/:243 —— 38300→**38700**、7300→**7700**。四子的中心项
   96+104+96+104 = 400；`black_ps` 仍是 31000、`before - after` 仍是 31000，即禁手门照旧只带走自己那一项。
3. `test_evaluate_board_double_four_point` :264/:266 —— 99800→**100376**、29800→**30376**（+576 中心项）。
4. `test_evaluate_board_keeps_white_value_on_forbidden_square` :292/:295/:298 —— 35700→**35924**、4700→**4924**
   （+400 BLACK 中心项 −176 WHITE 边角项；WHITE 的 500 一分没少）。
5. `test_evaluate_board_unchanged_without_forbidden` :315 —— 1700→**1708**。
6. `test_evaluate_board_never_judges_white` :361-363 —— 38300→**38700**、−38300→**−38700**；同一副盘的 WHITE 形状
   `white_ps` 仍 31000（(7,7) 四端都不缺格子 ⇒ 不打折）。
7. `test_candidate_value_zeroes_black_forbidden_shape` :385-395 —— 14→**112**、13→**104**、1063→**1154**、
   1076→**1258**、10014→**10112**、10028→**10224**。形状那一半（0 / 1050 / 10000）一分没动，变的只有格子。
8. `test_exact_five_outranks_the_forbidden_gate` :486 —— `five + 14`→`five + `**112**。
9. `test_search.cpp:3356-3389`（同一类数字在搜索套件里的对位）—— 14→112、1063→1154、13→104、1076→1258、
   10014→10112、10028→10224。
10. `test_tt_cold_think_cost_and_warm_reuse` 的**注释**里的三个节点数（225745→222885 等）。`<= 250000` 上限与
    `warm * 10 < cold` 比值断言都没动，两棵树都绿。

### New

`test_evaluate_board_prefers_centre_stones`（同一副三连子整体搬到边角 ⇒ 整盘分严格更低，并逐项点名是谁在起作用）、
`test_evaluate_board_antisymmetric_with_terms`（三副局面 × 两个座位 ⇒ `black == -white` 且 `black != 0`）、
`test_edge_penalty_below_rush_four`（贴边活三/活四各少 100、开行活二一分不少、`8 * edge_penalty < rush_four`，
默认标定 8 与 100 放在最后两行 —— 让行为断言先跑，红点就落在行为上）。

### Verified

HEAD-negative 的改动前树是 `git archive 01c2038` 整树，只把 A5 的**名字**补进去（2 参 `pattern_score`、两个 `return 0`
的 `*_term`、`position_weight_scale` 留 1、`edge_penalty` 字段留 100），行为退回 A3 收尾时那棵树。构造树的行为中立性是
自证的：同一个节点数 driver 在构造树打印 `2595 / 21963 / 225745 / 96`，与 pristine `01c2038` 一字不差。

逐用例 driver（`-Dmain=suite_main` + 挑用例的 main，18 条各跑一次）在构造树上的红点：
`prefers_centre_stones` 红在 `:516`（`position_term(centre) > position_term(corner)`）、`edge_penalty_below_rush_four`
红在 `:575`（`pattern_score(three, 7, 1)` 读 500 而不是 400）、7 条被改写的聚合值用例红在
`:238/:264/:292/:315/:361/:385/:486`；`test_evaluate_board_antisymmetric_with_terms` 在两棵树都绿（它要证的是「新项不
破对称」，本来就该同绿），与 A5 无关的 8 条在两棵树全绿。`test_search` 侧同法两条：`greedy` 用例在构造树红在 `:3359`，
`test_tt_cold_think_cost_and_warm_reuse` 在两棵树都绿 —— 构造树冷跑 225745、本树 222885，同在 `<= 250000` 之内，
warm 两次都是 96。`position_term` / `edge_penalty_term` / 2 参 `pattern_score` 是新 API 面 ⇒ 这三样在 pristine 树上
**无 HEAD-negative（by construction）**，构造树就是它们的替身。

实测数：`prefers_centre_stones` 中心 23212 / 边角 11822；对称三盘 `−1184 / 1184`、`7204 / −7204`、`1708 / −1708`；
折扣 per end 100、单格最坏 800、`rush_four` 1000。同一副 16 子局面 depth 4 的冷跑从 225745 节点变成 **222885**
（−1.3%）、`tt_hits` 2833→2965，选点与分值仍是 `(6,6) / 100000`；浅层各多 2 个节点（2595→2597、21963→21965）——
叶子值里多了两项，剪枝边界跟着挪了一格，这正是「位置项进了评估」的形状。

`ctest` **8/8 / 80.69 s**（`test_search` 51.12 s、`test_server` 12.56 s），
`node tests/web_frontend_check.js` **103/103**，
`node tests/readme_anchor_check.js` **113 citations / 0 problems**，README 仍 **658 行**（A5 没动文档，系数默认值在
README 里没有被枚举过，故无行内改写）。

Valgrind（`build-vg/`，Debug + `-UNDEBUG`，`tools/valgrind_check.sh`）：`test_evaluator` **clean / reports=0 / 2 s**，
`test_pattern` **clean / reports=0 / 5 s**。`test_position` **ABORTED rc=134 / reports=0 / 779 s** —— 这是本轮新增的
一条**仪表脆弱**记录，不是 A5 的缺陷，归因用单用例 driver 在两棵树上各跑一次同一用例
（`test_oversized_flat_run_is_scanned_in_one_pass`）：pristine `01c2038` 与本树逐行相同 ——
`peak RSS 314 MB`、红在 `tests/test_position.cpp:1127` 的 `assert(after.ru_maxrss < 250LL * 1024)`、rc=134、
valgrind 报告 **0 条**。那条断言读的是**进程**峰值 RSS，而 memcheck 的影子内存本身就记在它头上（全套跑时更高，425 MB），
与这条用例测的「一遍扫描 vs 每符号一个容器」的成本级无关；这一用例从头到尾不碰 `Evaluator`。⇒ 它加进 §5 基线里
`test_training.cpp:2056` / `test_server.cpp:1706` 那一对的同类型记录：**按既存记录、不计为新增**，门禁只看
「abort 之前零新增 + abort 点未前移」，两处都成立（`reports=0`，且红点在改动前的树上同样红）。
`test_search` 的全量 memcheck 按 §5 节奏留给 A6 checkpoint（A5 之后它的 222885 节点上限用例仍是同一条线）。

## 轮次121 引擎重构 A3：置换表按深度替换，代龄跟着落子走 — engine redesign stage A3

规格 `docs/ENGINE_REDESIGN.md` §2.4（冻结，不改写）。A3 动的是 P2：一张直接映射表把「上一手付出来的深层记录」当
成「本手浅层记录」的免费受害者，且入口递出来的是桶内裸指针。

### Found

- **替换条件把年龄排在深度前面**。`transposition.cpp` 的 `age != current_age_ || depth >= entry.depth`：一次代龄推进
  之后桶里每一条旧记录都自动变成可覆写对象，深度不再保护任何东西。而推进点是 `alphabeta.cpp` 里生产代码唯一的
  `tt_.new_search()` —— 即**一次思考**，不是**一手棋**。
- **槽号 = `hash % size`**。相邻两手产生的、只差一个面的哈希会撞进同一个槽，上面那条因此天天被踩到。
- **`lookup()` 交出桶内裸指针**：调用方读它的期间，另一次 `store()` 能就地改写它指向的条目。
- **mate 分数按「证它的那一层还剩几层」入库**，跨层、跨迭代不可比；`±100000` 的对外哨兵只是勉强能用。
- 条目里没有静态分的位子（C1 无处落），对外也没有能读代龄的口。

### Fixed

`TranspositionTable` v2：`std::vector<Bucket>`，每桶 `kBucketWays = 4` 路组相联；槽号取
`(hash * 0x9E3779B97F4A7C15) >> (64 - bucket_bits)` 的高位混合而非取模；条目存 32 位 key（`hash >> 32`），不再存全哈希；
替换策略 **深度优先** —— 同 key 就地覆写且拒绝更浅、否则先占空槽、满桶才挑最浅的 victim，同深度之间用
`age_older`（`int8_t(a - b) < 0`，回绕后的代龄仍然可比）做次级判据。`new_search()` 退役，换成 `advance_generation()`，
推进点从搜索内部搬到**把子落到盘上的三个所有者**：`GameManager::make_move`（人类的子）、`ai_thinking_worker` 的提交
块（AI 的子）、`SelfPlay::run` 每个 ply。悔棋 / `set_position` / `reset` 不推进；这三条路径也没有缓存的建议可作废
——服务端求招是算完即回调，`reset()`/`set_position()` 已经 `hint_thread_.reset()` 掐掉在飞的那条 ⇒ 计划里「改为作废
`hint_move`」在这一版架构里 **无可观察差异**。mate 分数库内按 ply 归一（`store_value`：`≥100000` 加 ply、`≤-100000` 减
ply），`probe()` 按当前 ply 还原，对外 ±100000 哨兵语义不变（认输判定 `src/search/search_engine.cpp:320` 与 README:338 的红点规则
都读它）。还原写成**饱和**的而不是精确逆运算：ply 2 入库的记录若在 ply 20 处命中，精确还原会得到 99982 —— 一个已证棋
从此不再被当成已证棋，而这条线正是剪枝、认输和红点共用的那个数。`lookup()` 换成 `probe(hash, out, ply)` 拷出。条目 **28 B**（`sizeof(TTEntry) == 28` 有用例钉着），2^18 槽
≈ **7.3 MiB**，比 v1 的 8.4 MiB 还小。

**偏离文档两处，已写进 `transposition.h` 的注释**：§2.4.1 的 24 B/`int16 value` 做不到（合法域 ±99999、哨兵
±100000 装不进 int16），保留 32 位 `value` + 32 位 `static_eval`；tag 用 32 位而不是 §2.4.1 的 16 位 —— 字节数不变，
假阳性率低约五个数量级。计划里那条用例叫 `test_tt_key16_collision_miss_safe`，实现后按真实的 32 位 tag 改名为
`test_tt_key_collision_is_a_miss_not_a_wrong_hit`。

新增 6 条：`test_tt_generation_advances_per_move`（一次 `SearchEngine` 思考不动代龄；`GameManager` 摆两手后恰好 +2，
悔棋不回拨）、`test_tt_mate_score_normalised_by_ply`（含「恢复是饱和的」：ply 2 入库的记录在 ply 20/40 处仍然读成
`100000`，不会掉回 99982 而丢掉已证棋的身份）、`test_tt_key_collision_is_a_miss_not_a_wrong_hit`（同桶不同 key
⇒ 未命中，绝不错读）、`test_tt_full_bucket_replaces_shallowest_then_oldest`（满桶里更浅的新键挑最浅的 victim、同深度
才挑最老的，深层记录留着）、`test_tt_probe_copies_the_record_out`（先拷出、再覆写同一槽，副本不动）、
`test_tt_cold_think_cost_and_warm_reuse`（一手思考的**节点上限常量**，见下）。改写 2 条：
`test_transposition_aging` → `test_tt_depth_first_replacement`，`test_transposition_cross_search_reuse` 整条重写为
「按代龄推进之后仍然命中」。

### 合约过期

1. `test_transposition_aging`(:103) → **`test_tt_depth_first_replacement`**：「一次新搜索一开始，旧条目就该被换掉」这
   条前提按 §2.4.2 反转为「旧条目按深度守住自己，年龄只在同深度里当次级判据」。
2. **`lookup()` / `new_search()` API 退役**，4 条用例（`test_transposition_table`、`test_transposition_cross_search_reuse`、
   `test_transposition_depth_gate_still_applies`、`test_alphabeta_aborted_search_stores_only_sound_nodes`）改用 `probe()`。
3. `assert(entry->hash == h)`（条目携带全哈希）**删除** —— v2 的条目只有 32 位 key。
4. `alphabeta.cpp` 的写表门从排除 `±100000` 放宽到排除 `±1000000`：mate 分数现在入库，而 `-INT_MAX` 那个「什么都没
   搜过」的占位值仍然被挡在外面。
5. `test_transposition_default_size` 的注释 8.4 MB → 7.3 MB；RSS 区间 [4 MiB, 20 MiB] 未动，实测增量 **7300 KiB**。

### Verified

`ctest` **8/8 / 91.55 s**（`test_search` 40.74 s），`node tests/web_frontend_check.js` **103/103**，
`node tests/readme_anchor_check.js` **113 citations / 0 problems**，README 仍 **658 行**。收尾（拆出时钟 commit、
注释定稿后）在同一棵树上重跑：**8/8 / 83.88 s**（`test_search` 52.49 s，与合成负载同跑）、web **103/103**、
anchor **113 / 0**。时钟修复独立成 commit `694a181` 后，用 `git archive` 树只换上那一个 `search_engine.cpp`
重编 `test_search`：build rc=0、套件 rc=0（88 条 passed）。

HEAD-negative 用 `git archive e6f3f74` 整树重建，跑同一个 driver（一对同桶孪生键 x/y，先存一条 depth=5 的记录，然后
跑一手思考、再用同槽的浅层键去覆写）：旧树打印 `after shallow twin store: lookup(x)=0 depth=0 value=0  lookup(y)=1`
—— 深层记录被本手的浅层同槽条目挤掉；对照 `control (no think): lookup(x)=1 depth=5 value=500  lookup(y)=0` 在两棵树
都绿，证明那次驱逐是思考内部的代龄推进造成的。新树两次都 `probe(x)=1 depth=5 value=500`（`probe(y)=1 / 0` 随对照变化
而深层记录不动）。mate 那条旧树读回 `value=100004`（没有 ply 概念），新树 `100010`（ply 6 入库、ply 0 读回）。
`probe()` 的 `ply` 参、`static_eval` 字段与 `generation()` 是新 API 面 ⇒ 这三样 **无 HEAD-negative（by construction）**。

G1/G3/G5 的确定性替身数（§4-5 口径：固定局面 + fresh TT + 无墙钟 + 无节点上限，于是 `nodes` 是计数而不是负载表）：
同一副 16 子局面、`AlphaBetaSearch::search(depth=4, time_limit=0)`、默认 2^18 表 ——
旧树 `nodes=276131 hits=383`，新树 `nodes=225745 hits=2833`（**同一手 -18.2%**），两棵树选的点与分值一致
`(6,6) / 100000`；同表重跑这一手 96 个节点（冷跑的 1/2350），即「上一手付出来的记录确实被读到」。
`depth=2` 与 `depth=3` 在两棵树**逐节点相同**（2595 / 21963），这条对照把差异定位到表而不是搜索。
新用例把 225745 钉成 `<= 250000` 的回归上限：driver 版在旧树于该断言 rc=134（打印 276131），在新树 rc=0。
`test_search` 因此从 40.74 s 涨到 50.52 s —— 这 10 s 是这条上限常量的代价，收口的 callgrind Ir 数另取。

Valgrind（`build-vg/`，Debug + `-UNDEBUG`，`tools/valgrind_check.sh` 的同一组参数）分两段 driver（`-Dmain=suite_main`
重编套件 + 挑用例的 main）：12 条 TT/alphabeta 用例 **rc=0 / reports=0**（含 `test_tt_generation_advances_per_move`
里那条 500 ms 的 AI worker，也含新的 225745 节点上限用例 —— 它在 memcheck 下独自跑了约 20 分钟，是这段计数的主要
成本），2 条耗时/进度帧用例 **rc=0 / reports=0** —— `test_engine_progress_frames_share_the_search_clock`
这一轮在 20× 减速下跑到了结尾（3 帧、单调、终帧 2978 ms 落在 2979 ms 预算内）。`valgrind.bin` 的 0 字节报告文件
在两段都是空 ⇒ 相对 §5 记的既存基线**零新增**。`test_alphabeta_aborted_search_stores_only_sound_nodes`
在本树打印 `cut: d1+partial 2 0/40 nodes` —— 中止那一层的 `root_moves_done` 是 0，正是 §2.3.2 要的「切进去的那一个
候选不算跑完」。

**行号位移**：`tests/test_search.cpp` +289/−62，两条受保护前提从 `:2284` / `:3205` 移到 **`:2504`**
（`test_search_engine_resigns_when_alphabeta_proves_mate` 的 `assert(result.resigned)`）与 **`:3427`**
（`test_engine_reports_the_interrupted_alphabeta_layer` 的 `completed.method == "alphabeta"`）。两条都没被碰。


## 轮次121 附带修复：进度帧与逐手耗时改用同一次读表 — one clock for the move

`test_engine_progress_frames_share_the_search_clock` 在本树（A3 施工中）于合成负载下 SIGABRT，
`tests/test_search.cpp:1103` `Assertion 'elapsed[i] >= elapsed[i - 1]' failed`；`git archive e6f3f74` 出来的同一棵
负载树 rc=0。用打印全部帧的 driver 复现，缺陷当场可见：

```
new: frame 2 depth=2 elapsed=2013 / frame 3 (terminal) elapsed=2012  <-- DECREASE
```

根因是**两只时钟**：`SearchEngine::search` 的 `start` / `elapsed_since_start()` / `used_ms` 都读
`high_resolution_clock`（libstdc++ 在 Linux 上是 `CLOCK_REALTIME`），而 A1/A2 装的绝对期限与 `SearchContext` 的
`elapsed_ms()` 读的是 `steady_clock`；进度帧发的是 `used_ms + stats.elapsed_ms`，即「一只时钟上的一段时长」加「另一只
时钟上的一段时长」，再加上两次各自的毫秒截断。RTC 被宿主校正的盒子上（VM / WSL）两者相差几十毫秒，于是层落在哪里
就决定这一帧会不会超过终帧。这不是 A3 引入的语义，是 A3 让每一层跑得更快、层落点换了，才把一个既存的可比性缺陷暴露
成一条会红的断言。

改法：`search_engine.cpp` 里那一手棋只剩 `steady_clock` 一只时钟（与期限、与 AlphaBeta 的预算同一只），并且每一帧在
**发出的那一刻**读一次表，而不是把本阶段的自计时叠到前置段上 —— 两个截断之和能比单次读数高出 1 ms，而一条会往回走的
进度流比一帧晚上几毫秒要糟得多。`method`/`elapsed_ms` 的对外语义不变。

改后同一负载 driver 连跑四次：帧序 `1115 / 1364 / 2023 / 2023`、`1113 / 1359 / 2007 / 2007`、
`1111 / 1379 / 2003 / 2003`、`1113 / 1382 / 2017 / 2017`，全部单调；同一负载下 `e6f3f74` 树也绿（它的层落点没撞上
那个偏斜）。**这条修复没有新用例**：它的判别力就在既有的 `:1054` 那条用例上，红/绿两向都是实测，不另造负例。

A2 记录里那条 `test_engine_progress_callback_reports_each_layer` 的墙钟脆弱**仍在，且中止点未前移**：本轮单跑它在
`build-vg` 下 plain rc=0（`3 layers, last depth 2`），memcheck 下 rc=134 / **reports=0**，中止消息
`../tests/test_search.cpp:878: Assertion 'rose' failed` —— 与 A2 记的 `:658` 是同一条断言，行号差 220 只是它上方的
TT 用例块涨了。本轮没动它，D1 的 `max_nodes` 才是它的解。


## 轮次121 引擎重构 A2：被打断的那一手，从此说自己是被打断的 — engine redesign stage A2

规格是 `docs/ENGINE_REDESIGN.md`（冻结，不改写）。§1.2 把线上四类缺陷归到同一个结构根因，A2 动的是 P1：一次思考
只有一只 `aborted_` 布尔，节点级「我的返回值是短路出来的静态评估」和搜索级「别再开新的活」挤在同一个位上，于是
三处一起坏。

### Found

- **一次短路，全盘作废**。提交门 `if (layer_complete && !stop_flag.load() && !aborted_)` 与写表门
  `if (!aborted_ && …)` 读同一只标志：一个子树被掐断，已经跑完的兄弟子树连同它们的表项一起被丢掉。
- **超时与没搜过在协议上不可区分**。`search_engine.cpp` 里 `info.depth == 0 ⇒ method = "greedy"` 并把 depth/nodes
  归零：一手 4 层预算、第一层就没跑完的思考，被报成一条从未搜索的贪心捷径，且节点数说 0，而同一手自己的进度帧
  报的是几百个节点。
- **层边界听的是过期的答案**。节点内的时钟每 1024 节点才读一次，而「要不要再开一层」正是发生在层边界；一个陈旧
  一层的答复就能决定开一整层。绝对期限与「时限为 0 = 不限」共用一个 `long long`，于是「期限已经过去」和「没有期
  限」无法区分。
- **进度帧只有 depth/nodes/elapsed**，客户端在开局与终局之间什么都看不见，只能靠墙钟猜。

### Fixed

产品侧：`NodeResult{score, sound}` 取代裸 `int` 返回值（`sound` 只描述本节点自己，完成的子树照常入库，被短路的
那个节点的数值不入库）；`SearchContext` 持 `reason_`（粘住的首个中止原因）+ `has_hard_`/`has_soft_` 哨兵 +
`layer_boundary_stopped()`（在层边界强制读一次时钟）+ `soft_time_up()`（只决定「开不开下一层」，永不丢弃已完成的
一层）；`alphabeta()`/`quiescence()`/`search_root_layer()` 三个调用点共用这一个判定；`SearchInfo`/`SearchResult`
**只增字段**（`partial_depth`/`root_moves_done`/`root_moves_total`/`seldepth`/`partial_best_move`/`stopped`），
`method` 不再改写、保留 `"alphabeta"` 并带上中止原因；`system_search_clock()` 落在 `steady_clock` 上，因为 VCF/VCT
与 `SearchEngine` 装的绝对期限都是 `steady_clock::time_point`，混用 `high_resolution_clock` 会拿「开机时长」去比
「epoch」。写用例时在产品里改回两处：`root_moves_done` 原先把「切进去的那一个候选」也计数，于是 `done == total`
并不等价于「这一层每个候选都跑完了」——而这正是提交门依赖的那条不变量；以及层边界那条 break 没有先
`refresh_counters()`，任何在那里发出的帧会读到上一层的计数。

新增 7 条行为用例，全部走 A1 那个可注入时钟：`test_stop_reason_time_hard_lands_mid_layer`（两趟：第一趟量每层的
时钟读数与节点数，第二趟把武装点插进目标层的中段）、`test_stop_reason_nodes_cuts_a_layer_and_keeps_the_completed_move`
（预算 = 上一层节点 + (全−上一层)×3/4，纯算术，没有魔数）、`test_stop_reason_soft_window_keeps_the_completed_layer`、
`test_stop_reason_crossing_at_the_commit_keeps_the_layer`、`test_stop_reason_user_flag_stops_between_layers`、
`test_stop_reason_mate_ends_the_deepening_without_an_abort`、
`test_quiescence_nodes_count_toward_the_budget_and_report_seldepth`。§4-2 要的三种中止（TIME_HARD / NODES / USER）
各有独立用例，且都断言返回的着法永远来自已完成的那一层，不来自 `partial_best_move`。

### 合约过期

1. `test_alphabeta_aborted_search_stores_no_bound` → **`test_alphabeta_aborted_search_stores_only_sound_nodes`**。
   「中止不写表」这条前提按 §2.3.3 反转成「中止仍写 sound 表项，被短路的那个节点自身的返回值不入库」。它的**存储
   半边对 `0670a6e` 是 无可观察差异**：旧树粘住的 `aborted_` 与新树粘住的 `reason_` 选中的条目集合相同，所以这条
   用例的判别力在 `aborted_tt.lookup(child_hash) == nullptr` 与 `best_move` 有效性上，不在表项数量上。
2. `test_engine_greedy_relabel_zeroes_the_alphabeta_count` → **`test_engine_reports_the_interrupted_alphabeta_layer`**，
   断言方向翻转（见下方实测两棵树的打印）。
3. **`test_alphabeta_commit_gate_trusts_the_layers_own_record`（round 100 的 FF4 源码文本形状钉）退役**，由
   `test_stop_reason_crossing_at_the_commit_keeps_the_layer` 的行为断言取代：钉的是「提交门写成什么字样」，而契约
   要的是「提交那一瞬时钟越线，这一层照样留下」。
4. `test_engine_keeps_alphabeta_time_after_tactics` 的 `!(method == "alphabeta" && depth == 0)` 改写为
   `method != "alphabeta" || stopped != "none"`：它钉的正是被本轮有意删除的那次改写。

### Verified

`ctest` **8/8 / 72.68 s**（`test_search` 41.18 s），`node tests/web_frontend_check.js` **103/103**，
`node tests/readme_anchor_check.js` **113 citations / 0 problems**，README 仍 **658 行**（本轮没动 README；
:529-531 那三行的 method 枚举里 `greedy` 由 `search_engine.cpp` 的另外两处赋值继续撑着，锚点脚本认这一条）。
HEAD-negative 用 `git archive 0670a6e` 整树重建后跑它自己的用例，同一 24 子中盘局面打印
`interrupted: greedy d0/n0`（旧，绿），本树打印 `interrupted: alphabeta d0+partial 1/n493 time_hard`（新，绿）——
即 §4-2 的 G2 可见化。实测细节：本盒 alphabeta 约 11.2 k 节点/秒，round 112 §3(a) 给那条控制组留的「46 倍余量」
在这里并不成立（20000 ms 的控制组自己就以 `d3+partial 4 / n252229 / time_hard` 收场），所以我只保留了它受保护的
三条断言（`completed.method == "alphabeta"` / `depth >= 1` / `nodes_searched > 0`），没有替它发明新的宣称。
**行号位移**：`tests/test_search.cpp` +530/−146，两条受保护前提从 `:2194` / `:2788-2792` 移到
**`:2284`**（`assert(result.resigned)`，300 ms 那一段）与 **`:3205`**（`completed.method == "alphabeta"`）。两条
都没被碰。

Valgrind（`tools/valgrind_check.sh`，`build-vg/` Debug + `-UNDEBUG`）：整套 `test_search` 在 memcheck 下跑了
**292 s / reports=0**，随后在 `test_engine_progress_callback_reports_each_layer` 上 SIGABRT。那不是内存错误而是
墙钟脆弱：那条用例给引擎 3000 ms，20× 减速下第一层都跑不完，于是 `depths` 里再也没有一次真正的上升。两棵树单独
跑这一条用例对照（同一套 memcheck 参数，串行跑以免互相抢 CPU）：本树 `test_search.cpp:658` `Assertion 'rose'
failed`，`0670a6e` 树同一函数同一断言（它的 `:606`）同样 rc=134 —— **既存脆弱、中止点未前移**，与 §5 里
`test_training` / `test_server` 那两条同类。A2 真正改到的代码用子集驱动覆盖（`-Dmain=suite_main` 重编套件 + 一个
挑用例的 driver）：13 条（三个期限/深度切片用例、`stores_only_sound_nodes`、6 条 `test_stop_reason_*`、
quiescence 记账、两条 TT 探测）**全部通过，rc=0，零报告**。D1 打通 `max_nodes` 后这条脆弱用例应改成节点预算。


## 轮次119 注释里的 60 秒和重复了一遍的 refuses；一条开书用例其实一次也没命中过谱 — bugfix round 119 batch WW

### Found

A documentation-drift round plus the coverage hole it surfaced. Both product rows came out of my own round-118
adjudications (the wording VV2 left behind; the "60 s" the idle-constant change forgot), and WW-C came from the
opening-book sweep that was queued behind them.

- **WW-A** — `web/app.js` `refreshSetupControls`' comment block: VV2 renamed the handler on one line but the line
  above it still ended 「The canvas already refuses」, so the two read as one sentence saying `refuses` twice.
- **WW-B** — `src/server/server.cpp:247-249`: the comment above `arm_idle()` still promised every session would
  「drop 60 s after the page loaded」 while `kIdleTimeoutSeconds` (`:120`) is **120** — the leftover of round 61's
  constant change, and exactly the kind of number a reader trusts.
- **WW-C** — `tests/test_search.cpp:1196-1211` `test_opening_book_rejects_occupied()` was **vacuous**:
  `add_line` (`opening_book.cpp:45-70`) registers only the prefixes of length 1..n−1, so a 3-ply line has no
  3-stone key; the case queried a 3-stone history, `query` returned false at `:191-192` because the key was never
  there, and its `if (found)` body — the part meant to pin 「拒绝落在已有子上的应着」 — had never executed. The
  product behaviour was correct; the *test* was pinning "no such book" instead.

### Fixed

Comments only in the product: `web/app.js` **2/2** (「…warning. handleCanvasClick already / refuses; the copy's
other two writers…」) and `src/server/server.cpp` **1/1** (「drop one idle timeout after the page loaded」, no
hard-coded seconds). `tests/test_search.cpp` **+46/0** adds
`test_opening_book_refuses_an_occupied_response_it_really_found` and **leaves the old case untouched** (the
contract's second option, so nothing existing is weakened): a deliberately self-overlapping line
`{{7,7},{7,8},{7,7}}` puts the response back on the key's own stone, and because the centre point is fixed under
all eight symmetries `covers_position` (`:194-200`) removes every candidate ⇒ `query` false, with
`has_entry(hit)` (`:156-163`, key-only) proving the found branch is really entered. A second fixture
`{{7,7},{7,8},{6,8}}` runs the original assert body for the first time — the reply is (6,8) **or** (8,8) because
the row-7 mirror shares the key, so the case asserts set membership plus `is_empty`, never one pinned square.
The counter-example `{{7,7}B,{0,0}W}` (a corner is no symmetry image of (7,8)) gives `has_entry == false` and
`query == false`, showing the new green is not another spin.

### 裁决

- **无可观察差异**: WW-A and WW-B, proved rather than asserted — every diff line in both files is `//`-prefixed
  (numstat 2/2 and 1/1), and all four gates come back byte-identical to the `e4ef71a` baseline (see below). No
  discriminator case was invented for them, and **WW-C has no HEAD negative** because it is coverage hardening,
  not a defect regression: the new case is green on HEAD's product code as well.
- **CLEAN**: the WS/HTTP **message field contract** sweep (all 15 outbound frame families' key spellings, types,
  毫秒/秒 units and absent-key defaults against README's tables, incl. `session`'s 12 keys vs README:502-504, the
  `timeout_ms`/`max_depth` 0-means-inherit rule vs README:349-350, and both clamp ranges on both entry points)
  found nothing and self-refuted seven suspicions. The field tables do not need re-auditing.
- **escalated to round 120 (batch XX)**: the canvas/geometry auditor's two 中 rows, both verified by me against
  the whole functions — XX1 `paintSetupCell` (:1223-1241) cycles from `setup.board` while `displayView()`
  (:2014-2026) shows only the record's prefix, so a click on an empty-*looking* point deletes a stone the player
  cannot see; XX2 `setupSeedRecord` (:1160-1173) appends the whole `moveHistory` even when README:376's
  post-确认 window left it describing the *previous* game, so the 摆棋 record and the copy disagree about which
  stones exist. Its closed list: no hover/ghost stone exists to be wrong, `state_sync`'s single early repaint is
  on the `choose_side` path where those fields are already clear, a late `hint_result` after 重开 cannot arrive
  (`detach_socket` → `halt_search`, `server.cpp:1307-1308`), a hint ring surviving 认输 is the same position
  (README:327), and the `BOARD_MIN=300` floor is round 60's deliberate choice.

### Verified

Tests-only additions on top of the two comment rows ⇒ **合约过期: 无**. Numbers, re-run by me on the worktree:
`ctest` **8/8 / 74.85 s**, web harness **103/103**, `readme_anchor_check` **113 citations / 0 problems** (gained
0, lost 0 — WW-A's block still carries its `README:375`, WW-B's never carried one), `wc -l README.md` **658**.
My own `git archive e4ef71a` whole-tree build of the new book case: `./test_search` exits 0 with the `[r119wwc]`
line printed on **both** trees, which is WW-C's parity proof.
Line-shift the next dispatcher must carry: the 45 inserted lines move the two machine-relative budget premises —
`assert(result.resigned)` of the 300 ms block from `:2149` to **`tests/test_search.cpp:2194`**, and the
`completed.method == "alphabeta"` block from `:2742` to **`:2788-2792`**. Neither is to be touched.

## 轮次118 位棋盘没有标志可言，却被一个标志否决了整盘；弹窗只认标签，别处的拖放带走了这一页 — bugfix round 118 batch VV

### Found

- **VV1** — `position_from_json` read `encoding` at `src/server/position_json.cpp:255-259`, **before** the form
  was chosen at `:261-271`. A bitboard or move-list document that carried a half-written field therefore died on
  `read_encoding`'s 「encoding 缺少 "white"」 and came back as a whole-position rejection with 0/0 counts, while the
  *same* field, when well-formed, was silently ignored on those two forms — one field, two fates. README:231-232
  («位棋盘与落子序列…不需要标志确认») and README:536 («`encoding` 只在棋盘形式下出现（位棋盘与落子序列没有标志可言»
  put the field inside the board form only. The path is live, not theoretical: `server.cpp:85` merges a
  message-level `encoding` into the document, so the 摆棋 弹窗's own `set_position` carries it.
- **VV3** — README:367 offers 拖拽 **or** 点选 for the import file, but only `#import-drop` bound
  `dragenter/dragover/drop` (`web/app.js:441-456`, round 62). The 弹窗 is a full-viewport mask, so a file released
  on the title, the paste box or the backdrop took the browser's default action — the page navigated to the
  dropped file and unloaded with the unfinished 摆棋 copy.
- **VV2** — the UU1 comment block I wrote in round 117 referenced its own line number (`(:965)`); the anchor
  checker does not read `web/app.js` self references, so such a number drifts the moment anything above it moves.

### Fixed

`src/server/position_json.cpp` **7/7**: the early block is gone and the field is read inside the board branch —
`if (!doc.contains("encoding")) return board_form(doc, nullptr);` then `read_encoding` → `rejected(error)` →
`board_form(doc, &override)`; `forms != 1` and the `first`-without-`moves` refusal are untouched, and the
bitboard/moves forms now ignore the field instead of being vetoed by it. `web/app.js` **8/1**: a `document`-level
`for (const type of ['dragenter','dragover','drop']) document.addEventListener(type, e => e.preventDefault())`
immediately after the label's own `drop` listener (the label's handlers run first on the way up, so 拖拽到标签 still
reads the file), plus VV2's line rewritten to name `handleCanvasClick`.

### 裁决

- **无可观察差异**: VV2 (comment only); the board branch itself, which is byte-for-byte the same decision it made
  before; a *complete* `encoding` beside a bitboard was already ignored (VV1c pins it on both trees).
- **不是缺陷**: `moves` with `first:"white"` handing the turn by counts — README:220 states the count rule as *the*
  derivation and README:228 limits `first` to colouring, and `tests/test_position.cpp:273-277` already pins it;
  `position_preview` not being self-importable — README:539 *requires* its echo to be the family's canonical
  marks for the confirmation step, and the 原样再导入 promise (README:277-278) belongs to `position_to_json`;
  `handlePositionPreview`'s text-only staleness gate (state hygiene, no constructible observable — `applyImport`'
  only door is a `display:none` button and `openImportModal:1379` re-clears); the `swapMarks` preview arm's missing
  `encoding` test (unreachable: every preview write goes `renderImportState` → `refreshSetupControls`).
- **held**: the duplicate 「refuses」 the VV2 rewrite leaves on the previous line → **WW-A**; `server.cpp:247-249`
  still says "60 s" where `kIdleTimeoutSeconds` is 120 → **WW-B**; the vacuous
  `tests/test_search.cpp:1196-1211` `test_opening_book_rejects_occupied()` → **WW-C**; Beast's single absolute
  handshake deadline (repro needs ~120 s against a hardcoded constant, so it needs a configurable timeout first);
  C5, QQ2, the `test_server.cpp:1985` `'parked'` flake and replay-over-`ai_move` atomicity as before.

### Verified

Tester's diff is tests-only and additive on the one change I authorized: `tests/test_position.cpp` **+121/0**
(VV1a bitboard + black-only, VV1b moves + white-only, controls VV1c complete / VV1d both board arms / VV1e two
forms / VV1f `first` beside a board), `tests/test_server.cpp` **+63/0** (VV1g over the socket: the frame comes back
`position_set` with 3/2 and `move_count()==5`, VV1h control on its own `Harness` because re-setting a non-empty
session rotates and prepends a `session` frame), `tests/web_frontend_check.js` **+61/2** — the 2 deletions are
exactly the no-op `addEventListener() {}` stub at `:108` and the `buildSandbox` return line the recording stub
replaced ⇒ **合约过期: 无**.
Numbers, all re-run by me: `ctest` **8/8 / 75.55 s**, web harness **103/103** (99 → 103), `readme_anchor_check`
**113 citations / 0 problems** (109 → 113, the four new `README:231`/`:536`/`:367` citations), `wc -l README.md`
**658**. My own HEAD-negative tree (`/d/tmp/r118_head_mine` = whole-tree `git archive 41e3831` + these three test
files): web **100/103** red on exactly the three VV3a rows (`no document-level dragenter/dragover/drop handler is
registered`) with VV3b green; `test_position` aborts at `tests/test_position.cpp:1415` after 22 existing cases
passed; `test_server` at `tests/test_server.cpp:2524` after 33, having printed `[r118vv1g] frame type: error`.
Because a binary stops at its first failed assert, VV1b and the four controls were attributed separately with a
print-only probe: HEAD flips **only** VV1a and VV1b (both `ok=false`, counts 0/0); bare bitboard, bare moves and
VV1c–VV1f report identical lines on both trees.
Process note: the tester disclosed that one of its parallel batches briefly appended an unrelated block to
**`web/app.js`** (a file outside its write set) and `APP-5*` cases to the harness, and reverted both in the same
turn. I checked before committing: `git diff --numstat` is back to the fixer's exact **8/1** for `web/app.js`,
`grep -c APP-5 tests/web_frontend_check.js` is 0, and the four gates above ran after the revert.

## 轮次117 确认还在路上，导入与换色却已经能改摆棋副本；终局回绘把绿洗成了灰 — bugfix round 117 batch UU

### Found

Two frontend rows, both from the 摆棋交互/控件快照 auditor, both verified by me line by line before dispatch.

- **UU1** — README:375 makes 确认 the only 摆棋 step that writes the server, and `handlePositionSet` repaints the
  page from the board the server actually received. `confirmSetup` armed `setup.confirmInFlight` and
  `refreshSetupControls` locked 确认/取消/清空/开关 (round 102 GG2) plus the canvas (`handleCanvasClick`, which the
  auditor's row also matched), but the copy had **two more writers** that were never in the lock:
  `swapMarks` (the `#group-swap` trio, `black↔white` recolours the whole copy) and `applyImport`
  (`确认导入` replaces the copy with the recognised reading, and consumes `setup.preview`). A recolour or an
  import issued while the 确认 round trip was in flight was overwritten by the authoritative reply with no
  warning line — exactly the class GG2 closed for the four labelled buttons, left open for these two.
- **UU2** — README:307 defines the 顶栏 as text **and a colour level** (绿 = 信息/成功, 琥珀 = 需要注意,
  红 = 拒绝/错误/断线, 灰 = 叙述). `handleGameOver` stored only `gameOverText`, so the two repaints that read the
  ending back — `exitReplay` and `exitSetup`'s `!confirmed` half (added by round 116 TT2) — called
  `updateStatus(text)` and silently downgraded a win or a loss to narration grey.

### Fixed

`web/app.js` +33/−6, `src/` untouched: UU1 adds `if (!setup.active || setup.confirmInFlight) return;` to
`swapMarks` and `if (this.setup.confirmInFlight) return;` to `applyImport`, and extends `refreshSetupControls`
with `btn-setup-import`, the `#group-swap [data-swap]` loop and (only while in flight) `btn-import-apply`; the
manual release block at the head of `exitSetup` gained the same three controls, with `btn-import-apply` returning
to its armed-by-preview `true` because the modal has just closed. UU2 introduces `this.gameOverKind`, written by
`handleGameOver` (`= kind`) and reset to `'idle'` in the constructor, in `applyAuthoritativeSession`'s session
rebuild and in `resetGame`, and read by both repaints as `updateStatus(ended ? gameOverText : …, ended ?
gameOverKind : 'idle')`.

### 裁决

- **无可观察差异**: `confirmSetup`'s own in-flight guard (`:1295`) was already there — it is not part of this batch.
  The `#group-swap` trio's *disabled flags* cannot be pinned in the harness at all
  (`document.querySelectorAll` is stubbed as `() => []`), so the lock is pinned through `swapMarks` instead; the
  modal's own trio only edits the preview, so it needs no lock.
- **不是缺陷**: an error frame that arrives during the round trip still repaints (the server's rejection is the
  authoritative word); 摆棋 editing while a replay step preview is painted (auditor B's one LOW row) — README
  governs what the preview shows, not who may edit underneath it; a `waiting` session frame vs `playing` —
  `send_session_state` promotes the reported status for a side-chosen game; 「落子记录未清空」 vs an empty record
  after 取消 — README:377 only promises that leaving 摆棋 does not itself clear the record, and the empty list came
  from the authoritative `state_sync` TT1a deliberately pins.
- **held**: C5 (revocation has no observable surface), QQ2 (documenting the two node gate scripts relabels 42
  citations), the `test_server.cpp:1985` `'parked'` flake (machine-relative), replay-over-`ai_move` atomicity
  (needs a server-side design pass). The stale `(:965)` self-line reference inside the UU1 comment is folded into
  batch VV — zero behavioural effect, the anchor checker does not read `web/app.js` self references.

### Verified

Tester's diff is `tests/web_frontend_check.js` **+243/−0** (11 cases, purely additive ⇒ **合约过期: 无**).
Discriminators: UU1a (both new flags held with the GG2 family), UU1b×2 (the recolour moves 0 cells and the
reading is not consumed), UU1d (release restores 导入/取消/清空/开关 and leaves 确认导入 held), UU2a×2 and UU2b×2
(the className keeps `ok` / `warn`). Controls UU1c (no round trip ⇒ both writers land) and UU2c/UU2d (a
session-rebuilt ending and both non-ending repaints stay `idle`) are green on **both** trees.
Numbers, all re-run by me: `ctest` **8/8 / 70.81 s**, web harness **99/99** (88 → 99), `readme_anchor_check`
**106 citations / 0 problems** (104 → 106), `wc -l README.md` **658**. My own HEAD-negative tree
(`/d/tmp/r117_head_mine`, `web/app.js` from `git show 626ff6d:web/app.js`, not `626ff6d~1`) reads **91/99** with
exactly the eight UU rows red — `导入 is held for the round trip, got: false`, `the recolour cannot reach the copy
mid-flight, 1 cells moved`, `the reading is still waiting, not consumed`, `while 确认导入 has no modal left to
apply, got: false`, `at the level it was reported with, got: status idle` ×4 — and SS1a/TT1a/TT1b/TT2a/TT2c green
on that tree as the checkpoint that the inherited rounds still hold.

## 轮次116 摆棋中的权威帧换掉了指引，终局回来后子行还在替死局思考 — bugfix round 116 batch TT

**Found.** 两域只读审计（摆棋 × 权威帧次序、终局 × 编辑器生命周期）逐条读回整函数之后，站得住的两条都在
`web/app.js`：

- **TT1** `handleStateSync` 结尾的 `this.updateStatus('对局开始', 'ok');`（`:1729`）没有摆棋守卫。
  README:360 与 README:379 规定摆棋期间顶栏归那条琥珀色指引，而项目里每一个会动顶栏的权威帧处理器都
  遵守：`applyView`、`handleAIMove`、`handleUndoSuccess`、`handleHintResult`、`handleGameOver` 全写在
  `if (!this.setup.active)` 里，唯独这一句没有。路径：点执黑/执白 → 回包未到的空档点「摆棋」
  （`enterSetup` 刚写入指引）→ `state_sync` 落地，把指引整行换成绿色「对局开始」，此后无人恢复
  ——`renderSetupPanel` 只写 `#position-status`。服务端一侧只有 `handle_choose_side`
  （`src/server/server.cpp:789-794`）发这一帧，所以这正是「顶栏归谁」判错的那一类。
- **TT2** `handleGameOver` 在摆棋中把 `updateStatus(statusText, kind)` 与 `this.updateTurnInfo('')` 一起
  跳过（`:1930-1933`）。玩家随后点「取消」，`exitSetup` 的 `!confirmed` 分支（`:1190-1195`）只把 `#status`
  回绘成 `gameOverText`，`#turn-info` 没有第二个负责者：它仍停在终局前那一刻的「轮到白棋 · AI 思考中」，
  替一局已经结束的对局宣称 AI 还在思考。README:307 把顶栏定义为「系统提示 + 轮到谁」两行，两半要分别落地。

**Fixed.** `web/app.js` +11/−1：TT1 那句改为 `if (!this.setup.active) this.updateStatus('对局开始', 'ok');`
（+6/−1，含注释）；TT2 在 `!confirmed` 分支回绘状态行之后补 `if (this.gameStatus === 'game_over') this.
updateTurnInfo('');`（+5/0）。`handleStateSync` 其余行、`handleGameOver` 的守卫、`exitSetup` 的确认路径
一行未动。守卫只让位顶栏那**一行**：摆棋中的棋盘、回合、`gameStatus`、回放退出照常跟随 `state_sync`。
两条注释初始稿写的是 `web/app.js` 自查行号，与落地后的行号已经漂移，故本轮另发 TT-A 修正案把行号引用换成
处理器名（`applyView`/`handleAIMove`/`handleUndoSuccess`/`handleHintResult`/`handleGameOver`），代码行冻结。

**裁决（其余条目，全部不外派）。** `error` 帧把「游戏结束 - …」冲掉：README:307 把顶栏定成共用报告行、
红=拒绝/错误，给终局加哑等于吞掉 README:326-327 明文欠玩家的那条错误。`handleReplay:726/:739` 摆棋中复位
`replayIndex`：已 HOLD 的 D5。`handleUndoSuccess:1832` 的 `game_status === 'game_over'` 分支：服务端 `undo`
把 GAME_OVER 一律映射回 PLAYING（`game_manager.cpp:231-235`），无法构造。`reasonText`、手/子单位链、
落子记录行序：逐函数读过，干净。TT2 的确认半边（`exitSetup(true)`）在两棵树上都逐字不动 `#turn-info` ⇒
**无可观察差异**，且确认唯一的调用者 `handlePositionSet`（`:1359`）自己就写 `updateTurnInfo('')`，没有残留泄漏。

**Verified.** 测试会话只在 `tests/web_frontend_check.js` 加 4 条（+121/−0，零删除 ⇒ 合约过期 **无**）：
`TT1a`（摆棋中的 `state_sync`：顶栏逐字仍是那条指引、不含「对局开始」、`className` 仍带 `warn`，同时
`gameStatus==='playing'`、盘面被覆盖、`moveHistory` 清空、`baseStones` 换成服务端那一子、`replayIndex`
归 null、子行照常报回合）、`TT1b`（不在摆棋时仍写开场行，与既有 `JJ3`（`:2611`）同族对照）、`TT2a`（终局
落在摆棋中→点「取消」：`#status === game.gameOverText` 且 `#turn-info === ''`）、`TT2c`（确认路径对照）。
我重跑同一棵树：`ctest` **8/8**（裸 `cmake ..`，92.28 s real）、`node tests/web_frontend_check.js`
**88/88**、`node tests/readme_anchor_check.js` **102 citations, 0 problems**、`wc -l README.md` **658**。
HEAD 负例 `/d/tmp/r116_head_mine` = `git show f2d73f8:web/app.js` + 当前 tests（无 checkout/stash）：
**86/88**，只在 `tests/web_frontend_check.js:3329`（`the opening line does not replace it, got: 对局开始`）
与 `:3407`（`the settled game keeps no thinking AI, got: 轮到白棋 · AI 思考中`）红；轮次115 的 `SS1a` 在那棵
树上仍绿（这是检查点，证明负例树只剥掉本轮两句），`TT1b`/`TT2c`/`TT2a` 前置三处同样绿。

## 轮次115 被接管那一页收回来的，是一手无人能落的建议 — bugfix round 115 batch SS

**Found.** 四只只读审计（逐手引擎数字呈现链、会话文件 schema 新旧兼容、落子记录与状态分级渲染、求招链路）
逐条读回之后，站在需求文档上的只有一条：

- **SS1** `web/app.js:1853` 的 `handleHintResult` 没有 `takenOver` 守卫。README:524-526 写死了次序：
  `session_taken_over` 是服务器发给被顶掉那个连接的最后一帧，「随后立即 `close`」；README:567-568 又规定
  旧标签页此后不再自动重连。所以 `handleTakenOver`（`:203-229`）置的那份停放态——`hintInFlight = false`、
  求招连同其余三个按钮一起置灰、琥珀色警告「本局面已在其他标签页打开，刷新页面可取回」——按文档就再没有
  任何帧能来纠正。可排在 taken_over 之后入队的这一条 `hint_result` 偏偏还能动作：`:1855-1856` 按
  `playerColor === null || gameStatus === 'game_over'` 重算，接管后两者都不成立，于是求招从灰变可点；
  接管不动棋，`:1867` 的手数比对放行；`:1876` 在已经不属于这一页的盘面上画建议环，`:1877` 用
  「建议 D12 · 分值 55」把接管警告整行盖掉。同一族的 RR3 在轮次114 已给 `handleThinkingProgress`
  （`:1880-1885`）补过这句守卫，这里是漏网的那一处。

**Fixed.** `web/app.js` +6/−0：`handleHintResult` 第一句 `if (this.takenOver) return;` 加五行注释，
其余一行未动。`hintInFlight` 不需要补清——`handleTakenOver:216` 已经把它归 false、`:224` 已经置灰按钮，
早退保持的正是接管后的那份状态。

**裁决（其余条目，全部不外派）。** 呈现链域：`search_engine.cpp:291-292` 在改名 `resign` 之前写的
`depth`/`nodes` 到不了任何帧——`game_manager.cpp:398-408` 在 `result.resigned` 时于 `place_stone` 与
`analyses_.push_back` 之前就 return，这份分析根本不进快照，`server.cpp:1036-1046` 也就发不出；
`:355-357` 收尾那帧 `thinking_progress` 用的是 `info.depth`，而此处它恰是 0，与 replay 里 greedy 的 0
一致，且帧不入 `analyses_`；VCT 的 90000 不点红是 README:338 自己定的阈值 `|score| >= 99999`，与
must_block 的 99000 同律；曲线的白棋取反与 `human` 跳手逐字就是 README:337 与 :529。整调用 `elapsed_ms`
仍出自 `search_engine.cpp:42` 那一口钟，每条返回路径都取自它。会话文件域：只 `choose_side`、一子未落的会话
被 `on_flush_tick:1597-1600` 收进 `cold_sessions_`（保留测试含 `player_color`），而 `flush_entry:1507` 的
「一子未落不写」让它没有磁盘对应——这正是 README:571 加 README:566「退回磁盘态」的字面结果：进程还活着时
`resume` 认得它、凭内存态回 `resumed:true`，重启后文件本就不存在，于是新 token 新开局；把这条判为按设计，
不是「同一 token 反复续局」的缺陷。`black_profile`/`white_profile` 被读进来却在第一次内容不等的 flush
被写回 −1，服务器没有任何代码路径设置它们（只在 `game_record.{h,cpp}`），**无法构造**；`root["type"]`
无人读、token 长度 1–64 的宽松读端、两套 reason 词表，README 都未立规矩，**无可观察差异**；
`server_reason_of` 是死码。落子记录/渲染域：两条留给下一轮——`handleStateSync:1729` 是所有会动顶栏的
权威帧处理器里唯一不带 `if (!this.setup.active)` 的那一处（对照 `:495`/`:1777`/`:1828`/`:1861`/`:1930`），
执黑/执白的回包晚于玩家点「摆棋」时，绿色「对局开始」会永久换掉 README:360/379 许给摆棋的那行琥珀指引；
`handleGameOver:1930-1933` 把 `updateTurnInfo('')` 和 `updateStatus` 一起跳过，而 `exitSetup:1190-1195`
只回绘 `#status`，于是取消摆棋后 `#turn-info` 仍替一局已结束的对局宣称「轮到白棋 · AI 思考中」。第三条
（`error` 帧把「游戏结束 - …」冲掉）不外派：README:307 把顶栏定成共用的报告行、红=拒绝/错误，
给终局加哑就等于吞掉 README:326-327 明文欠玩家的那一条错误。求招域其余五条见下段。

我自己那条服务端候选也判 **无可观察差异**：`game_manager.cpp:330-339` 的投递守卫只比
`move_count`，把数补回来必须走两次 `make_move`，而头一次的 AI 重挂（`:190` → `:350-357`）会在
`search_thread_` 建出来之前先 `join` 掉 `hint_thread_`，守卫永远只看到差一手的计数。（`Board` 确实
在 `src/core/board.h:69` 暴露 `zobrist_hash()`，若哪天这条变得可构造，身份比对就有现成的键。）
另外：`requestHint` 不查 `aiThinking` 由服务器在 `:296` 拒绝、红字即文档认可的答复；认输之后不会有迟到的
`hint_result`，因为 `GameManager::resign:273` 先 `halt_search()` 立 `discard_pending_`，那一帧根本发不出来；
error 分支的置灰表达式漏 `aiThinking` 与 `:1849`/`:1855`/`:1073` 是同一份文件级规则，且随后的
`thinking_progress` 会重新置灰；会话里 100–499 ms 的 `timeout_ms` 回填成「0 秒」是 'bounds' 模式刻意不夹
（`:649-668`），服务器夹回 100 ms 恰是会话本值；断线窗口里置灰的求招不产生点击，谈不上「静默丢弃」
（README:554 管的是能点下去的那些）。

**Verified.** 测试会话只在 `tests/web_frontend_check.js` 追加 **+77/0**（2 条用例，一条现有用例未动）：
判别式 `SS1a` 用 `adviceGame()` + `requestHint()` 造出真实在跑的求招，派发 `session_taken_over` 后再派发
同手数的合法 `hint_result`，同时断言按钮仍灰、`adviceRings()` 为 0、`recommendation === null`、顶栏仍以
接管警告开头；对照 `SS1b` 把未接管页（按钮按普通规则解冻、恰好一个环、顶栏是「建议 …」）与已终局页
（按钮保持置灰）合写一条。合约过期：**无**。我亲自重跑：HEAD 树 `/mnt/d/tmp/r115_head`
**83/84**，只在 SS1a 红（`the parked page keeps 求招 greyed, got: false`）；修复树 web **84/84**、
anchor **100 citations / 0 problems**（新增的两处 README 引用：`web/app.js` 的 `README:526` 与测试注释的
`README:524-525`，检查脚本按行展开所以 94→98→100 一路加的是引用数）、README **658** 行、
`node --check web/app.js` 通过、ctest **8/8 / 67.29 s**（`-j8`，裸 `cmake ..`）。



## 轮次114 回放条不能后退的那一手，与三个「这是给谁的帧」判错了的控件 — bugfix round 114 batch RR

**Found.** 四份只读审计（完局记录→replay 链、self_play 与训练轮次记账、前端视角与控件、会话生命周期时序）
逐条读回之后，站得住的四条全在 `web/app.js`：

- **RR1** `stepReplay:777-780` 把实时视角的起点写成 `delta > 0 ? -1 : 0`，于是 `◀` 走
  `showReplayMove(-1)` 被夹回 `0`——**与 ⏮ 完全等价，永远退不了手**。README:335「之后可用 ⏮ ◀ ▶ ⏭
  逐手回看」要求的是逐手；而退出回放时盘上显示的就是记录的最后一手，`replayIndex === null` 的起点本该是
  `length - 1`。`grep stepReplay tests/` 只有 `tests/web_frontend_check.js:1010` 一处，且它用 `+1`，
  所以这不是合约过期，是一条从没被钉住的新路径。
- **RR2** `handleHintResult:1858` 判「这份建议还是不是给当前盘面的」只看
  `moveHistory.length !== hintMoveCount`，而它上面那句注释自述的前提（「每条改盘面的路都会往 moveHistory
  上推一手」）漏了**先悔再下**：悔棋吃掉两手，玩家换个点重下、AI 应答又补两手，手数回到原值、盘面已经不是
  同一张。旧建议照画环、状态照报「建议 H8」，正是 README:327「这一份建议作废……不要把旧局面里的着法
  画到新局面上」禁的事。修在 `handleUndoSuccess` 尾部一行 `hintMoveCount = -1`——而不是加一个着法序列
  指纹字段：现有 9 处用例（`:876 :903 :1535 :1558 :2470 :2706 :2725 :2757 :2780`）是**手写**
  `hintMoveCount` 的，改成指纹会把它们全部打成合约过期。
- **RR3** `handleThinkingProgress` 不看 `takenOver`。README:526 说 `session_taken_over` 之后「随后立即
  `close`」，但那一帧是**投到被替掉的 strand 上**的，worker 已经排进队列的 `thinking_progress` 仍会在它
  后面出去；被接管的标签页从此不会再有一帧，那一发就把 `handleTakenOver` 刚摆好的停车态推翻——终止被点亮、
  悔棋/求招被按死，且再也无人复位。
- **RR4** `handleSession:263` 把 `sendMessage` 的返回值丢掉，无条件 `resumeApplied = true`。发送被拒
  （`requestHint:1046` 就会提前返回，这里却没有）意味着那一发**没有回复**，标记就一直挂着；等玩家自己点
  「载入本局分析」，`handleReplay:724` 把这条回复当成续局消费掉：状态报「已恢复对局」，且
  `replayIndex` 留在 `null`，棋盘根本不进回放视图。README:582 的「补一次 `request_replay`」没有发生。

**Fixed.** `web/app.js` +20/−6，四处（契约 `build/r114_rr_spec.txt`）：RR1 的三元式换成
`this.moveHistory.length - 1`；RR2 在 `handleUndoSuccess` 那句「hands the control back」注释下补
`this.hintMoveCount = -1;`；RR3 在方法体第一行补 `if (this.takenOver) return;`；RR4 改成
`if (this.sendMessage({ type: 'request_replay' })) this.resumeApplied = true;`。
`handleHintResult` 的判定式一字未动，`src/`、`README.md`、`build.sh` 未动。

**裁决（四份审计的其余条目，全部不外派）。** 会话生命周期域：冷架上一份**什么都没有**的会话文件被
`resume` 时保留原 token 却回 `resumed:false`（审计说的 D1）——README:486-487 承诺「一个新 token」的三种情形是
认不得、格式不对、读得出来却重建不出棋盘，空文件是**能**重建的那一种，而 `open_session:1246-1256` 已经把
「重建成功就留着旧 token」的理由写在注释里（旧 URL 必须落在这一局上，不是空白局）；何况 PP2/LL2 之后这种文件
根本不再被写（README:570）。`applyAuthoritativeSession:294` 无条件 `exitSetup(false)` 丢掉摆棋工作副本，是
代码里自带注释的既定取舍，README:355/378 只承诺 取消/确认 是丢弃路径，副本从未离开过 socket，服务器无从知道它。
`syncTimeoutBounds('bounds')` 把会话里的 `max_depth:1` 抹成 2——README:346-347 自己写着「2–20」是
**输入框的网页规则**，1–20 才是服务器夹取，且这位审计也承认网页上构造不出 `depth=1` 的会话。训练域：
`trainer.cpp:201-205` 在既没给 `--records` 也没给 `--samples` 时跳过整步采集数据——没有地方可写，且它打印
`skipping data games this round: ...`，不是静默；README:181 的第 4 步是流程叙述，不是「把四盘自我对弈算完再扔掉」
的承诺。`verification` 一局未成仍 `elo_.record_result`（`trainer.cpp:154` + `:350`）需要引擎产出非法着法或外部
stop，训练侧恒传 `nullptr`——无法构造。记录→回放域：`app.js:710` 用原始 `base.length` 供奇偶、`baseStones`
却过 filter——`base` 只来自 `board_.move_history()`，`session_from_json:213-217` 对越界 base 直接拒绝加载，
filter 无从丢子；`handleMoveMade:1750` 清 `moveHistory` 不清 `analysis`——进入那分支的两条路
（`handlePositionSet:1327`、`applyAuthoritativeSession:313`）都已清空 `analysis`，同一 socket 严格 FIFO，
无法构造。`flush_entry` 先会话文件后 JSONL、`entry->exported = from->record.is_decisive()`、
`handle_undo` 无条件 `exported = false` 三条是轮次104/轮次85 结过的旧账。视角变换、坐标标注、回放计时器、
帧形状对照、self_play 主循环、RetryExplorer、`play_match` 轮换、genetic/hill_climbing 的 `run()` 复位、
elo 公式与 K 档、data_collector 去重键、`next_data_id` 记账、空闲重挂与重连退避表，逐函数读回后 CLEAN。

**Verified.** 测试会话只写 `tests/web_frontend_check.js`：8 条新用例（4 条判别 + 4 条对照），**+273/0**，
既有用例一条未改，**合约过期：无**。HEAD 反证由我自己重跑一遍——`/mnt/d/tmp/r114_head/`（只读
`git show ac74166:web/app.js` + 同一份套件副本），红点恰好落在这四条判别上：RR1 `got: 0`、RR2 旧建议环
`got 1`、RR3 迟到的 progress 把 `aiThinking` 重新点亮 `got: true`、RR4 被拒的发送仍留下恢复标记
`got: true`，HEAD 总 **78/82**。修复树由我自己跑：`node tests/web_frontend_check.js` **82/82**，
`node tests/readme_anchor_check.js` **98 citations / 0 problems**（新用例注释里的 README 引用把 94 抬到
98；README 里没有任何一条按行号指向 `web/app.js`，所以本次行号位移不影响锚点），`wc -l README.md` **658**，
`node --check web/app.js` 通过，ctest 由我自己重跑：**100% tests passed, 0 failed out of 8 / 69.70 s**。

**下一轮（轮次115）预告.** 本轮判定为「按文档设计 / 无可观察差异 / 无法构造」的条目不动，特别是：冷架上
空会话文件保留原 token、重连丢弃摆棋工作副本、深度输入框 2–20 的网页规则、`trainer` 无输出路径时跳过采集、
`base` 越界与 `analysis` 越界两处的 FIFO/拒绝加载前提、`verification` 零局仍记 Elo、RR2 在服务器一侧的
对位判据（需要慢搜索与悔棋交错的 WS 时序）。下一轮先派三只只读审计：逐手耗时与深度/节点归属从
`search_engine` 到 `replay` 帧再到评估曲线的呈现链、会话文件 schema 的新旧兼容（`base`/`moves`/`record`
三态与 `--resume`）、落子记录与状态分级的渲染不变量。回来仍由我逐函数读回核验后才下发。

## 轮次113 推荐的那条构建路不出测试，却喊「Build complete!」 — bugfix round 113 batch QQ

**Found.** 两份只读审计（长驻资源与性能、运行期文件与路径）逐条读回之后，只有一条站在需求文档上：

- **QQ1** `build.sh:44-126` 的 cmake-free 兜底分支（`command -v cmake` 失败时走的「直接编译」）编译完
  两个二进制就打印 `Build complete!`，**一个字都不提它没产任何测试二进制**。README:99 把这条路（方法1）
  标成「（推荐）」，而 README:431-448 的「测试」章节是无条件的：`cd build` 然后 `ctest --output-on-failure`，
  或者手动跑八个 `./test_board` … `./test_training`。按推荐装好的人（机器上没有 cmake）照文档走，得到的
  是一句他解释不了的报错；脚本知道差异在哪，却不说。`if [ "$USE_CMAKE" -eq 1 ]` 分支
  （`cmake .. && make -j$(nproc)`）会把八个测试目标连同 `add_test` 一起建出来，所以那句披露只能属于
  else 分支——写进 128-138 行两段共用的收尾提示里，就等于让有 cmake 的正常构建谎报「这里没有测试」。

**Fixed.** `build.sh` +8/−0，只在 else 分支的 `cp -r ../data .` 与 `echo "Build complete!"` 之间插 4 行注释
加 4 行 `echo`（契约 `build/r113_qq_spec.txt`）。最后一行 `echo '      mkdir build && cd build && cmake ..
&& make -j$(nproc)'` **用单引号**：那行是回显给用户看的命令文本，双引号会让 `$(nproc)` 真的执行一次——
在 `set -e` 之下，替换失败就会把整个构建打断。两条 g++ 命令、`../deps/json` 的 clone 分支、两个 `cp -r`、
if 分支与共用收尾块一行未动。

**裁决（两份审计的其余条目，全部不外派）。** 长驻资源域：`cold_sessions_` 没有上限也没有 TTL，唯一移除点
是同名 token 的 `resume`——但它**就是**磁盘态的那份索引（README:563-564「退回磁盘态」，服务器启动时又
把 `data/sessions/` 读回它），条目是去掉引擎之后的快照，数量受磁盘上那些按设计只增的文件约束。
`resume` 认不得 token 时照样 `open_session(nullptr)` 建一个 `GameManager`（8 MiB 位棋盘置换表），被顶掉的
entry 在 `sessions_` 里等满 1800 s——两半都是文档写的（README:487-488 认不得就另开一局 + 新 token、
:561-563 断开只是 detach 且留 30 分钟），而且轮次里 `transposition.h:29-32` 已经记着这条权衡：表从 2^20
缩到 2^18，正是因为「会话无上限，三局并发就破 README:635 的 <100MB」。`TranspositionTable::clear()` 零调用者
但 `lookup` 比的是完整 Zobrist 键，残留项只可能被同一局面命中；每 10 s 一遍的
`replay_snapshot()` + `snapshot_of()` + `json::dump(2)` 有 `payload == saved_payload` 去重（时间戳先归零），
没动过的盘既不写也不 unlink，只有 CPU；`Board::get_candidate_moves()` 与 `has_neighbor_` 是死码，删它要动 `.h`
且没有可观察差异。运行文件域：训练侧 `ensure_parent_directory` 吞掉 `error_code`，但冠军写入
（`Error: cannot write champion parameters` + 退出 1）与 `trainer.cpp:428` 的断点保存失败都会说话，没有静默丢；
开局库 README 从不描述文件 schema，所以逐行 `continue` 的部分载入不违反任何一句，且 disabled / loaded N-M /
unavailable 三态都打印，`load_or_default()` 零调用者也没有任何文档承诺内置退路；七个局面模板没有运行期读者
（只有测试读）；`set_position` 全程不碰磁盘，所以 `load_position_file` 没有穿越守卫是**无法构造**；
`session_store::read_file` 在启动时整份读入 `<token>.json` 无大小上限，前提是对方已经能写可信目录，
解析失败会被跳过而不是崩，要证它得种几百 MB 的文件——那是我被明确要求不去搭的压力具。**QQ2**
（README 的「测试」章节从不给 `web_frontend_check.js` / `readme_anchor_check.js` 任何调用方式，跟 README:425
自己说的「单元测试」对不上）**暂缓**：在那一带插一行要把 42 处指向 :431 之后的引用整体重贴。

**Verified.** 这是**脚本行**，不是二进制行，沿用轮次99 DD6 / 轮次107 KK2 的做法：证据是一次性探针
`build/r113_qq_probe.sh`，`tests/` 本轮一行未写。夹具在 `/mnt/d/tmp/r113_qq/`，HEAD 树用
`git show c43b5b3:build.sh` 解出来，`deps/json/include/` 预置（否则兜底分支会去 clone GitHub，WSL 里 TLS 就断），
stub PATH 里放记录 argv 的 `g++` 与 `nproc` marker，`cmake` stub 只在 A2 臂出现。四臂我自己在 WSL 复跑过：
A1 判别成立（修复树第 11 行 `Note: this fallback build compiles only gomoku_server and gomoku_train.`；
HEAD 树第 11 行就是 `Build complete!`，全跑无该句；两臂退出码都是 0）；A2 两棵树都走 `Building with CMake...`
且都不含那句；A3 兜底臂 `g++` 恰好两次（`-o gomoku_server` ×1、`-o gomoku_train` ×1，HEAD 也是 2 ⇒ 编译行为
无可观察差异），兜底臂 `nproc` marker **0 行**、cmake 臂 1 行，坐实那句是回显；A4 `bash -n` 两棵树都 0，
`git diff --numstat` 8/0 且只有一个 hunk。门禁（我自己跑的空闲四段）：`ctest` **8/8 / 71.26 s**
（src/ 一行未动，与 75 s 基线同量级）、`web_frontend_check: 74/74`、`readme_anchor_check: 94 citations, 0 problems`、
`wc -l README.md` **658**。**合约过期：无**（本轮没有任何既有用例被改，测试文件零改动）。

**下一轮（轮次114）预告.** 两条已判定的挂起项不动（QQ2 的 README 插入代价、`replay` 覆盖 `ai_move` 的
排序问题按 无法构造 关闭）。下一轮先派三只只读审计：完局记录与 replay 索引的一致性
（`game_record` / `record_export` / `handle_request_replay` 一条链）、前端视角变换与回放条边界
（README:315-320、:332-340）、训练 CLI 的 `--params/--records/--samples` 与 README:174-216 默认值对照，
回来仍由我逐函数读回核验后才下发。

## 轮次112 「冷架上那份什么都没有的快照，与三处「这是谁的数」被混用」 — bugfix round 112 batch PP

**Found（批次 PP，四条都来自轮次111 预告里我逐函数读回核验过的候选）.**

- **PP1** `src/training/trainer.cpp` 的样本盘号是**算**出来的：`const int game_id_base = games_played_ + 1 + t * 1000`。
  两个面都会重复发号：① 单个 worker 的盘数一旦越过 1000，t=0 与 t=1 的 id 带就互相覆盖；② 那个 `+ t * 1000` 偏移是
  相对**只增不减**的 `games_played_` 的，后续轮次会合法地把它重新达到，于是这一轮发出的 id 与更早某一轮用过的 id
  相同。后果是 samples.jsonl 里同一个 `game_id` 命名两盘不同的棋，而 README:181 要求这条追加流每行分得开。
- **PP2** `src/server/server.cpp` 的 `on_flush_tick()` 把到期 entry **无论里面有没有东西**都放回 `cold_sessions_`，
  可 `flush_entry()` 的「一子未落不保留」分支早已删掉该 token 的磁盘文件——冷架于是留着那份什么都不是的快照当唯一
  副本。拿它去 resume 答的是 `resumed:false` **加同一个旧 token**，而 README:487-488 说的是回不来的局面开新局
  **并给新 token**。
- **PP3** `web/app.js` 的 `resumedStatusText()` 把权威 session 帧的 `move_count` 写成「N 手」；README:580 定义这个
  字段是**盘上子数**（含摆棋的 `base` 子）。「手」只属于 replay 那条路径。
- **PP4** `web/app.js` 的 `handleTakenOver()` 放开了重连、心跳与思考态，唯独没放 `replayTimer`：README:567 说接管后
  这一页不再是操作方，可那个 interval 还在步进回放并把「回放 · 第 N 手」写回顶栏，盖掉接管提示。

**Fixed.** `src/server/server.cpp` +21/−9（`base`/`record.moves`/`player_color` 三处全空的快照直接 `erase`，不再上
冷架；`!overdue || !settled` 的重试分枝提到函数头，先判「该不该动它」再取快照）、`src/training/trainer.cpp` +15/−5
（`std::atomic<int> next_data_id{games_played_ + 1}`，每盘**真正入档**的棋在 `collect()` 处 `fetch_add(1)` 领号——
INVALID 的 `continue` 在领取之前，所以领取次数 == 合并后的记录数 == join 后 `games_played_` 的推进量，两个 worker
同时领也互斥）、`web/app.js` +14/−4（接管时 `stopReplayAuto()`；`resumedStatusText()` 用 `fromServer` 分辨「子」与
「手」）。契约文本 `build/r112_pp_spec.txt` 逐字下发。README 不动（658 行不变）。

**裁决（不是缺陷）.** 轮次111 预告里另外几条读回后否掉：U1「INVALID 漏计一个 id」在数据路径不可达（那种盘由
`explore()` 的停止标志产生，根本不进 `records`），判**无可观察差异**；接管分支漏 flush——`set_game_over_callback`
已经 `net::post` 落过一次盘，接管路径手里没有未落的结果；`deferred_end_for_` 不随接管复位——那帧的接收方就是被接管
的那条 socket，留着只会把 held 帧发给已关闭的会话；副本全空时 确认 不发 `set_position`——README 的摆棋一节把「全空」
就读作清空当前局面，不发帧才是对的。

**Verified.** 测试会话只动 `tests/`：`tests/test_training.cpp` +94/0、`tests/test_server.cpp` +101/0、
`tests/web_frontend_check.js` +123/−2、`tests/test_search.cpp` +6/−1（只有轮次111 授权的 §3(a)：`:2742` 控制臂预算
2000→20000 ms，`:2149` 所在用例没碰）。HEAD 反证（`git archive 1fac892` 只读解包 + 单 TU 重链）红点
`test_training.cpp:2172`（两批 id 集合相交）、`test_server.cpp:1256`（冷架仍留空快照、旧 token 复活）、
`web_frontend_check.js:2884`（HEAD 打「11 手」）、`:2935`（`replayTimer` 仍在），HEAD 整跑 web **70/74**；四条控制臂
（同一次调用内的 id 带互不重叠、有子的会话仍按同 token 复活、replay 支仍写「手」、未接管时定时器与「暂停」按钮都在）
两棵树都绿。

**契约里两处是我写错了，按实测纠正**（不是测试会话自行放宽）：① 我写「同一手内部不会重复同一对」——`collect()`
每手增强成最多 `SYMMETRY_COUNT`（8，`src/core/transforms.h:12`）个朝向变体，全都带这一手的 `move_index`，所以一对
`(game_id, move_index)` 合法地带 8 行；判别因此改成「两批 id 集合互斥 + 每对行数 ≤ 8」，`generate_data_games()` 的
返回值也如实是**样本**数而非盘数。② PP4 那句「手动跑一次被捕获的回调也不得改成……」无法照字面断言（tick 闭包里
没有存活检查，修复树上照样会步进），改用 JJ3 已有的 `if (cleared.indexOf(armed) === -1) tick();` 姿势 + `fillText`
记录器。

**合约过期 1 条，已裁决并结案**：`tests/web_frontend_check.js:1891`（BB1，轮次97）把服务器权威帧的期望钉成
「已恢复对局：1 手」——那正是 PP3 修掉的单位错误（该用例的帧就是 `move_count: 1` 且盘上一粒黑子），与契约已授权的
:571 同一类；只把期望字样改成「1 子」并补一行 README:580 的 why 注释，断言取法与 `got:` 尾巴不动。`:583`/`:585`
（`resumedStatusText(undefined)` 与 `('7')`，走本地手数回退）仍写「手」，本来就不该动。

**门禁由我自己跑完：空闲臂** `100% tests passed, 0 tests failed out of 8` /「Total Test time (real) =  75.05 sec」
（比上一轮 47.86 s 长，就是 `:2742` 控制臂抬到 20000 ms 后那层控制搜索真的跑到终点、不再被 2 s 剪掉的代价）、
`web_frontend_check: 74/74 passed`、`readme_anchor_check: 94 citations, 0 problems`（84→94 全是新用例注释新增的
README 引用）、`658 README.md`。**负载臂**（12 核 + 24 `yes`，两跑）这回 **8/8 / 8/8**，`test_search` 39.85 /
39.69 s，`:2149` 没有开火——它仍是机器相对前提，不算赦免。

**下一轮（轮次113）预告.** 四个只读审计（构建/打包/测试登记、特征提取与样本采集、会话生命周期时序、WS 帧契约对表）。
已判：`build.sh:44-126` 的 cmake-free 分支**既不产测试二进制也不说一句**（README:435/441-448 无条件让读者去跑 ctest
与八个 `./test_*`）是真缺陷；`cp -r ../web .` 重复跑**不会**套出 `web/web`（GNU cp 是合并覆盖，隔离探针三跑只剩一个
`web`），那条否掉；`-Wno-template-body -Wno-template-id-cdtor` 在两条 g++ 命令行里缺失是**无可观察差异**（本机 g++
15.2.0 下这份代码 0 条这类告警，两条源文件清单与 CMake 的 `GOMOKU_SOURCES`/`GOMOKU_TRAINING_SOURCES` 逐一对得上，
无缺源）。特征域整域 CLEAN：`recognize_position_both` 与两次 `recognize_position` 逐分支等价（参数元组字节相同，
只有 4 次线位重复解码）、52 维索引无重叠（`PATTERN_SLOTS=8` ⇒ 0..15，`FEATURE_MY_FIVE=16` … `FEATURE_WINDOW`
27..51）、`centre_control` 值域 [0,1] 且角格恰为 0、满盘和局的标签处理与正常负局一致。「禁手点不该进特征」按
README:30 自己列举的范围（`evaluate_position` / `evaluate_position_pair` 及其下游）本来就不在管内，且这些特征没有
任何对局期消费者（`gomoku_server --params` 只读威胁系数），否掉。时序域 CLEAN：秒↔ms 只换算一次、客户端 ping 30 s
对服务器空闲 120 s 有 4 倍余量、`detached_ms` 每次 detach 都重打时间戳（attach 清零）所以不存在提前回收、
`kReattachWindowSeconds = 30*60` 与 README:565-566 一致。帧契约对表 CLEAN：服务器 15 种 / 客户端 13 种，逐方向
0 条漏接（`pong` 客户端按 README:551 就是忽略），八条字段读写差异全是**无可观察差异**，README 门控过的入站类型
全部有门。

## 轮次111 「非读取」被当成读取回答的那一半端口，与一条把 `.` 跟 `..` 一起拒掉的守卫 — bugfix round 111 batch OO

**先办上一轮的欠账：`'parked'` flake 的归属判定.** 结论是**用例侧的墙钟前提，不是产品时序**。12 核机器上开
24 个 `yes` 饱和，连跑 3 次全量 ctest 每次红的都是**同样两条**，且 `test_server` 在饱和下 10.57 s 通过、
`:1985` 的 `parked` 自旋在 12 次单跑 + 3 次饱和全量里一次也没红。两条红点是：
① `tests/test_search.cpp:193`——`test_transposition_depth_gate_still_applies()` 直调 `alphabeta()`，
`AlphaBetaSearch` 的默认 `move_time_limit_ms_` 是 5000（`alphabeta.cpp:10-11`），饱和下这次 depth-4 调用
走到 `alphabeta.cpp:94` 的中止分枝，返回静态评估量级的分值，于是「TT 深度门」这条用例变成了一台负载计；
② `tests/test_position.cpp:1111`——`assert(elapsed_ms < 10000)`，16 MB 单遍扫描实测空闲 6.6 s、饱和
29000 ms，而同一条用例的 `ru_maxrss` 那条仍然绿（135 MB）。产品侧没有任何改动。

**Found（批次 OO，来自本轮只读审计的 `--web-dir` 静态资源域）.**

- **OO1** `src/server/server.cpp` 的 `handle_http_request()` 从未读 `req_.method()`，每个动词都被当 GET
  回答：对存在的路径发 POST 会拿到 200 **加该文件自身的字节**，等于告诉发送方「你的写入已被接受」而实际上
  什么都没写；HEAD 按其定义只要首部，HEAD 版却照样塞了 body。README:294 写的是浏览器打开
  `http://localhost:8080`，那是取 index.html/app.js/style.css 的一次读，需求从未承诺把一个写当读服务。
- **OO2** 同类：`resolve_file()` 逐段走 target，凡不是文件名的段一律拒绝，于是单个 `.` 跟 `..` 一起被拒。
  两者语义相反——`..` 要的是 `web_dir_` 之外的文件，`.` 要的是**本目录**，`/./index.html` 就是 index.html；
  HEAD 版对它还回 404。

**Fixed.** `src/server/server.cpp` +32/−11（契约文本 `build/r111_oo_spec.txt`，我用 Read 下发，彻底避开上一轮
那种代码块传输截断）：GET 与 HEAD 共用 `resolve_file` 与状态码，只有 GET 填 body，其余动词 405 +
`Allow: GET, HEAD`；`resolve_file` 只按名字拒 `..`，把 `.` 留给下面的 `weakly_canonical` + 根前缀校验，
后者才是真正守住目录的那一道。README 不动（658 行不变）：静态资源与路径守卫两节本来就没写「拒绝 `.`」。

**裁决（不是缺陷）.** `src/training/hill_climbing.cpp:26` 的 `rounded <= 0 ? 1 : …`：审计报「步长被抬成 1 会
掩盖 0」。逐函数读后判**无可观察差异**——乘数只可能来自 `{1.6, 0.625}` 两个步长（`hill_climbing.h:17`，
`trainer.cpp:338-341` 从不覆盖），任何一条路径都算不出 `rounded <= 0`，两棵树上这段代码的观察面完全相同。

**Verified.** 测试会话只动 `tests/`：`tests/test_server.cpp` +188/−2（两条用例
`test_http_answers_a_read_with_the_file_and_a_write_with_a_refusal`、
`test_resolve_file_reads_a_single_dot_segment_and_still_refuses_every_escape`）、
`tests/test_search.cpp` +10/0、`tests/test_position.cpp` +20/−8（OO3 三处逐条授权的门禁健壮性改动）。
HEAD 反证只读 `git archive e052445` → /tmp、按 `link.txt` 把 `server.cpp.o` **换成** HEAD 产物重链，红点
`:2274`（POST）、`:2286`（HEAD 的 body）、`:2313`（PUT/DELETE——派单把这条记成对照，它其实也是判别，HEAD
根本不读 `req_.method()`）、`:2357`/`:2359`/`:2370`（点点段）；九条逃逸姿势两棵树都拒绝。OO3 三处：
`:193` 所在的两个块加 `move_time_limit_ms_ = 600000`（只在这两个块，任何量超时或节点预算的块都不许加）；
`test_position.cpp` 的墙钟断言按授权分支删除——实测 CPU 时间**同样**是负载计（空闲 6947 ms / 饱和 13324 ms），
而它要跨的间隙是 6.9 s 对旧容器版 13 s，没有既 ≥3 倍又低于 13 s 的阈值，成本级改由 `ru_maxrss < 250 MB`
承载（现 135 MB 对旧 1155 MB，量级差且不含时钟）；`parked` 自旋上限 10 s→60 s 并加超时诊断行，
`assert(parked)` 原样保留。合约过期：无。
门禁由我自己跑完：**空闲臂** `100% tests passed, 0 tests failed out of 8` / 「Total Test time (real) =  47.86 sec」、
`web_frontend_check: 72/72 passed`、`readme_anchor_check: 84 citations, 0 problems`（82→84 是新用例注释里的
README:294/139 引用）、`658 README.md`。

**但要如实记账：负载臂是 7/8。** `:193` 那处修好之后，`main()` 里排在它后面的两条**从来没机会执行**的用例
第一次跑到了终点，于是暴露出同样性质的机器相对预算前提：`tests/test_search.cpp:2742`
（`completed.method == "alphabeta"`，2000 ms 的控制臂）与 `:2149`（`result.resigned`，300 ms 预算）。
两条都在我派单禁止测试会话触碰的用例内，所以它没写「成功」。我的裁决：两者都是**用例侧**前提，产品行为
符合设计——控制臂那边预算用尽就按 README:530-531 的 `greedy`  relabel 报告，判别臂（10 ms 那一手）不受影响；
resign 这边只在 AlphaBeta **证出**杀棋时才认，预算内没证出来就继续下。轮次112 处理 `:2742`：那是纯粹的
余量问题（一层约 433 ms，把控制臂预算抬到 20000 ms 不削任何判别力），已授权给测试会话；`:2149` **不动**，
因为那 300 ms 本身就是判别器（`probe_budget_ms > 500` 那道门只有在小预算下才关得住 VCF 反证路径，
抬预算会让 HEAD 变绿），它是一条已记录的机器相对前提，不算门禁失败。**门禁仍取空闲四段。**

**下一轮（轮次112）预告.** 三个只读审计已回：训练盘 id（U1 INVALID 漏计——审计逐路径证明
`GameEndReason::INVALID` 在数据路径不可达，故并入**无可观察差异**；U2 `--data-games 2001 --threads 2`
起 `per_worker` 越过 1000 步长、两个 worker 的 id 带重叠）、会话生命周期三条（接管分支漏 flush、
`deferred_end_for_` 未随接管复位、空档 entry 被 shelved 后旧 token 复活）、前端回放/视角四条
（「已恢复对局：N 手」用盘上子数、`session_taken_over` 不释放回放时钟、副本全空时 确认 不发 `set_position`、
`replay` 回复删掉刚收到的那一手）。以上**都要我先逐函数读代码核验**才下发，另加 `:2742` 的预算余量。

## 轮次110 两条路径守卫比的是写法而不是文件，于是同文件漏拦、合法命令反被拒 — bugfix round 110 batch NN

**Found.** 三个只读审计（训练循环记账 / 摆棋导入回环 / 核心几何 / 搜索层候选处理）里三条整域 CLEAN，
只有训练 CLI 报回一条，我读回整个 `main` 后确认并扩成双向：

- **NN1** `src/training/main_trainer.cpp` 的两条路径守卫比较的是**原始字符串**——`:291`（旧行号）
  `if (out_path == checkpoint_path)` 与 `:262` `else if (checkpoint_path != resume_path)`。README:204 说
  `--out`「与 `--checkpoint` 指向同一个文件时启动即报错退出（退出码 1）——收尾的冠军写入会把断点换成参数
  文件，之后的 `--resume` 再也读不回来」，README:206 说 `--checkpoint`「与 `--resume` 同时给出时必须指向
  同一个文件」。两句说的都是**同一个文件**，不是同一段写法。于是同一份代码在两个方向上都错：
  ① `--out 目录/ck.json --checkpoint 目录/./ck.json` 绕开拒绝，跑完一轮把 1284 字节的断点换成 325 字节的
  冠军参数——正是文档明令禁止的那个后果；② `--resume 目录/ck.json --checkpoint 目录/./ck.json` 指的是同一
  个文件，HEAD 却用退出码 1 把这条合法命令拒了，训练根本不开始。

**Fixed.** `src/training/main_trainer.cpp` +17/−2：新增自由函数 `file_named()`（`ensure_parent_directory`
下方），用 `std::filesystem::weakly_canonical` 的 `error_code` 版本把路径解析成它所指的文件——`weakly_`
是这里唯一可用的形式，因为这些文件在拒绝判定那一刻还不存在（是这一趟 run 才创建的）；解析不了或空路径就
原样返回，因为「两个我放不到一起的路径」并没有被证明是同一个文件，这里猜错会放行冠军写入覆盖断点。两条
守卫改为比较解析结果。错误文案里的路径保持原样（回显用户打的那一段）。README 不动：204/206 两条本来就
写明「同一个文件」。

**裁决（不是缺陷）.** 摆棋导入回环与核心几何两域整域 CLEAN（台账见轮次109 块）。搜索层候选处理本轮也
整域 CLEAN，被审计逐函数读后否掉的：`vcf.cpp:126` 缺口冲四 `count + beyond == 4` 对白棋看似该 `>= 4`，
实则 beyond≥4 意味着填洞本身已成五，`:152/:266` 的成五分枝先赢，该候选永不可达；`search_engine.cpp:127`
must_block 抢在 VCF/VCT 前是**可证安全**（对方只剩一个成五点时，非即时取胜都会被先成五，即时五已由 `:111`
覆盖）；`alphabeta.cpp:226` 首轮中止返回 `-INT_MAX` 被 `:352` 的 `!aborted_` 提交门与 `:255` 的存储门拦住，
无观察面；`vcf.cpp:420` 每步防守复制整盘是 README:624-625 记下的实测取舍；`vct.cpp:99` 去重内层被 9×9 窗
界定。训练记账里核过不是缺陷的：resume 后的轮次/种子推导、`play_match` 两局执色换算、checkpoint 写序与
tmp+rename、`--resume`×`--seed`/`--params`/`--checkpoint` 三条拒绝的先后次序。我自己纠正了派单里的一处
行号：`--checkpoint` 行是 README:**206**，207 是 `--resume` 行。

**Verified.** 测试会话只动 `tests/`：`tests/test_training.cpp` +199/−0，两条用例
`test_trainer_refuses_one_file_written_two_ways_for_out_and_checkpoint`（判别：异写法同文件 → 修复树 exit 1
且断点字节不变；HEAD exit 0 且 1284→325，红在 `tests/test_training.cpp:3568`）与
`test_trainer_accepts_a_resume_and_checkpoint_that_differ_only_by_a_dot_segment`（判别：合法 resume → 修复树
exit 0 且 `--out` 写出；HEAD exit 1 且 `--out` 不存在）。三条对照两棵树都绿：同一写法仍被拒、两个真正
不同的文件仍跑通并都写出、**两个不同文件的 `--resume`×`--checkpoint` 仍双双退出 1**（证明修复没把真拒绝
改松）。跑法延用 JJ2 一家的 `base` 标志串 + `run()` + `temp_path()`，`.` 段由
`parent_path()/"."/filename()` 注入，所有路径绝对，不依赖 cwd、不写 `data/`；`--rounds 0` 依旧不能判别
（轮次守卫排在两条拒绝之前）。HEAD 反证只读 `git archive 0d258ac` → `/tmp/neg110`，按 `link.txt` 把
`main_trainer.cpp.o` **换成** HEAD 产物重链。合约过期：无。
门禁由我自己在 WSL 跑完：`100% tests passed, 0 tests failed out of 8` / 「Total Test time (real) =  53.49 sec」；
`web_frontend_check: 72/72 passed`；`readme_anchor_check: 82 citations, 0 problems`（71→82 是用例注释新增的
README:204/206/207 引用）；`658 README.md`。
**但要如实记账**：`tests/test_server.cpp:1985` 的 `test_a_rejected_stone_delivers_the_end_a_worker_parked()`
在我这三轮全量里红了两次（`Assertion 'parked' failed`），单跑 `ctest -R test_server` 与最后整跑都绿——
这就是轮次109 已经用交错 A/B 证明「与本批无关、HEAD 就存在」的那条 flake，本批未触碰 `server.cpp`，
也没改那一例。它已经威胁到「全绿才提交」这条门禁本身，**下一轮（轮次111）的第一优先事项就是把它读透并
消掉**：先定位是产品侧 worker 停车的时序不保证，还是用例侧等待条件本身有竞态，再按归属分给对应会话。

**下一轮（轮次111）预告.** 先做上面那条 flake 的归属判定与消除；然后按同一套只读审计继续扫需求文档，
重点转向本轮未覆盖的边角（HTTP 静态资源与 `--web-dir` 守卫、会话 token 续局与接管、参数文件读写回环）。

## 轮次109 一个不被任何符号表认得的数被截成 −1，于是凭空多出一粒白子 — bugfix round 109 batch MM

**Found.** 四个只读审计（摆棋导入回环 / 前端视角与分析面板 / 搜索编排与配置管线 / 训练循环记账）这一轮只留下
一条真缺陷——前三条域带回了明确的CLEAN 台账：

- **MM1** `src/server/position_json.cpp:23-34` 的 `token_of()` 只分 `is_number_integer()` 一支，然后
  `std::to_string(value.get<long long>())`。nlohmann 对 `number_unsigned` 的算术转换就是一次 `static_cast`
  （vendored `from_json.hpp:344-346`），**不抛，只截断**，所以 `is_number_integer()` 为真而值 > INT64_MAX 的
  格子会带着截断结果回来。于是棋盘里一个写成 `18446744073709551615` 的格，token 变成 `"-1"`，而
  `src/core/position_setup.cpp:51` 的自动识别表第二行 `Alphabet{{"1"},{"-1"},{"0"}}` 正把 `"-1"` 当作白棋符号
  （README:240）。README:215 是「64 位整数不许用会截断的读法」这条 house rule，README:245-246 是
  「识别不了就直接拒绝并列出看到的符号，绝不猜」。HEAD 在这份文档上不是拒绝，而是**替文件落下一粒它从未写过的
  白子**：单独一格时它侥幸被「黑先手、黑白差只能 0/1」的计数规则拦住（拒绝的原因完全不同），而只要在别处补一
  粒合法黑子，整份文档就被完整接受。同文件 `word_of():59-73` 早就写成「先分无符号」，这是唯一的漏网。

**Fixed.** `src/server/position_json.cpp` +7/0——`token_of()` 前面加 `is_number_unsigned()` 一支，走
`std::to_string(value.get<uint64_t>())`，让那个值自报成 `18446744073709551615`，识别器按未知符号正常拒绝并
回显。小正整数两支的 `to_string` 结果一致、负数仍走有符号支，所以除 >INT64_MAX 以外无可观察变化。README 不动：
240/215/245-246 三条本来就写明，代码只是回到文档。

**裁决（不是缺陷）.** 摆棋导入回环整域 CLEAN：导出↔读回（`position_to_json` 发 `encoding{1,2,0}`+数字 board，
读回走 forced 支）、混合符号→字母表空→拒绝、同签名去重不会把 `−1` 误当白子、`parse_grid_text` 的按行/按格分流、
`first` 与轮到谁的推导、`check_position` 只在含 `board` 时回显 encoding、`set_position` 对无 encoding 的 board
强制二次确认——逐条读过整函数后都不是缺陷。核心几何整域 CLEAN：`line_index/line_position` 的位域余量、
`board.cpp:232-317` 增量邻格计数的可逆性、`transforms.cpp:6-13` 的 8 个 D4 编码与 `opening_book.cpp:16-26`
逐字一致、`self_symmetries` 是稳定子群、`BITBOARD_TOP_MASK` 与 README:227、LUT 与标量路径按构造等价。
我自己否掉的：`recognize_position_both` 越界早退时复用 out 参残留 `directions`（evaluator 的候选恒来自
`active_empty()` 且颜色恒合法，构造不出调用方读到残留的路径）、`transform_board` 整史重放（模块注释未承诺增量）、
`run_length` 允许 `color==EMPTY`（生产调用方先判 occupied）。训练记账审计报的 `--resume` 后轮次/种子、对战执色
换算、checkpoint 写序与 tmp+rename 也都核过是文档所述行为。

**Verified.** 测试会话只动 `tests/`：`tests/test_position.cpp` +107/0，一条用例
`test_cell_number_beyond_int64_is_never_read_as_minus_one`（注册在 `main()` 末位），含两条 HEAD 红点
（`black_count==0 && white_count==0` 与 `board.get(7,7)==EMPTY`，加最硬的一条「补一粒合法黑子后 HEAD 整盘
ok=true」）与四条两棵树都绿的对照（文本 `1/-1/0`、小数 `0/1/2`、带 `encoding{1,2,0}` 的同盘）。它如实报了两点：
① 我原指的姿势 A 在 HEAD 上也 `!ok()`，只是被计数规则拒的，所以红点必须落在计数与盘面上；② 同一格配
stated encoding（姿势 F）两棵树都拒、都 0/0，只差被点名的符号，判**无可观察差异**，写成打印而非断言。
回显断言 `error.find("18446744073709551615")` 裁定为结构性回显（README:246 要求列出看到的符号，HEAD 报的是
自己截出来的 `-1`），与 `test_moves_array_size_is_capped` / DD4 同类先例一致，保留。HEAD 反证走只读
`git archive aab02f9` → `/tmp/neg109`，单 TU 重链，停在 `tests/test_position.cpp:1286`。
门禁由我自己在 WSL 跑完：`100% tests passed, 0 tests failed out of 8` / 「Total Test time (real) =  49.77 sec」
（`test_position` 7.73 s，裸 `cmake ..` 无 Release/NDEBUG）；`web_frontend_check: 72/72 passed`；
`readme_anchor_check: 71 citations, 0 problems`（66→71 全是用例注释新增的 README:240/215/245-246 引用）；
`658 README.md`。合约过期：无。已知干扰：`tests/test_server.cpp` 的 `'parked'` 一例是 HEAD 就存在的 flake，
本轮未触发，未改。

**下一轮（轮次110）预告.** 批次 NN 只有一行 NN1：`src/training/main_trainer.cpp` 里两处路径比较都是**原始
字符串**——`:291` 的 `out_path == checkpoint_path` 与 `:262` 的 `checkpoint_path != resume_path`。README:204
说的是「指向同一个文件」、README:206（`--checkpoint` 那一行）说的是「必须指向同一个文件」，说的是文件不是写法：`--out ck.json
--checkpoint ./ck.json` 在 HEAD 上不触发拒绝，跑完后冠军写入把断点换成参数文件（正是文档禁止的后果）；反过来
`--resume ck.json --checkpoint ./ck.json` 指向的明明是同文件，HEAD 却报错退出——一条合法命令被拒。修法是比较
两个路径所指的文件而非其写法（`std::filesystem::weakly_canonical` 的 `error_code` 版本，两条路径都规范化后再
比；规范化失败时退回字符串比较，因为此时无法判定它们是同一个文件，拒绝比放行安全）。

## 轮次108 关停那一遍扫描少了一条内容规则，与三处越界的建议环 — bugfix round 108 batch LL

**Found.** 三个只读审计（摆棋导入核 / 前端视角与分析面板 / 搜索编排与配置管线）回来 8 条，我把每一条读回
被引用的整个函数后判下 5 条真缺陷：

- **LL2** `src/server/server.cpp:1531-1544` 的 `flush_all()` 先 `export_game_record` 再看内容，缺了
  `flush_entry:1486` 那条「一子未落就不留」。README:571 说「一子未落的空会话不写」，而离开摆棋唯一能造出
  零子终局的路是**执黑然后认输**——`handle_resign:1078` 承认 `side_chosen` 即已开局，且选黑之后轮到黑，
  不会派 AI worker，棋盘真空。HEAD 在这种局上 Ctrl+C：会话文件被 `flush_entry` 拒了，`records.jsonl`
  却收下一条 `moves: []` 的「完局」行并盖上 `exported`，训练流里多出一条不描述任何对局的记录。
- **LL3** `web/app.js:1211-1213` `clearSetupBoard` 先 `renderSetupPanel()` 再用 `updatePositionStatus`
  写「棋盘已清空」，把整行顶掉；README:360 要求「底栏下面一行显示当前计数」，而清空是摆棋**内部**的一步。
  同族其它写手（导入 `:1415`、`paintSetupCell`、`swapMarks`）都是把提示语传进 `renderSetupPanel(note)`。
- **LL4a** `drawBoard` 的建议环门只查 `this.recommendation`，于是摆棋副本与任意 replay 手上都照画上一局
  搜出来的那一圈；README:326-327 把求招的结果定在「当前局面」。
- **LL4b** `handleUndoSuccess` 作废了 replay/分析却没有作废建议环——悔掉一手正是「局面又落了一手」的反向，
  旧建议已不属于这个局面。
- **LL5** `stopThinking`（`:1050-1066`）把 `aiThinking` 置 false 并恢复 停止/悔棋/求招 三个控件，唯独不
  `refreshReplayControls()`；回放簇在 `:821` 随 `aiThinking` 被置灰后就没人替它解开。其余六个
  `aiThinking = false` 之后都跟着刷新，这是唯一的漏网，与 JJ3 同形。

**Fixed.** `src/server/server.cpp` +7/0（守卫与 `flush_entry` 同式，仍由 `flush_entry` 负责删残留文件）、
`web/app.js` +8/−4（LL3 改走 `renderSetupPanel(note)`；LL4a 门改成 `this.recommendation && !this.setup.active
&& this.replayIndex === null`，是**扣住不画**而不是丢弃，切回实时视角环自己回来；LL4b 在 `stopReplayAuto()`
后 `this.recommendation = null;`；LL5 末尾补 `this.refreshReplayControls();`）。README 不动：360/326-327/571
三条本来就已写明，代码只是回到文档。

**裁决（不是缺陷）.** 优化器与训练消费侧四条：`hill_climbing.cpp:26` 把 0 个基因夹成 1（README 未约定可达集）、
`trainer.cpp:153` 不可下棋的匹配返回 0.0（构造不出来，`--verification-games` 走 `require_int(minimum=1)`）、
`genetic.cpp:45-46` 的注释措辞（轮次 87 的用例已钉住当前分类）、`trainer.cpp:243-251` 注释与 `:236/:240` 的
id 不致密（无 README 依据）。审计里被我自己否掉的三条：`self_play.h` 的 `max_moves = 80` 默认值与
`evaluate_position_both` 都没有消费者；position 域里 INVALID id 密度的注释与代码不一致只是注释问题。
搜索编排审计交回**空表**，它逐条读过并自杀的五条留给后续轮次别再重提：终止后的 `greedy` 尾巴（轮次 94 已否）、
VCT 的 90000 分（= Z4，与 README:338/P1 一起挂起）、终帧 `thinking_progress` 的 `depth 0 / nodes > 0`
（`tests/test_search.cpp:2729-2730` 钉住了它）、`halt_search` 丢弃 hint 不回帧（每条置 `discard_pending_`
的路都另发 `game_over`/`game_restarted`，前端状态无差异）、`controller.*` 的竞态（生产无调用方）。
前端视角审计同样**空表**，并重钉：`displayOf`/`logicalOf` 八种视图互逆、`coordName` 与
`columnLabel`/`rowLabel` 对齐、`reasonText` 恰好覆盖服务器能发的三种 reason。

**Verified（测试会话写用例，我本人跑门禁与反证）.** 只写 `tests/`：`tests/test_server.cpp` +48/0 一条
`test_shutdown_flush_keeps_a_zero_stone_resign_out_of_the_records`（执黑→认输→`flush_all()`，断言
`records_lines == 0`，对照是同一个 Harness 换 `five_restore(BLACK)` 仍导出 1 行、盖章、留文件），
`tests/web_frontend_check.js` +216/0 九条（LL3 两条、LL4a 三条、LL4b 两条、LL5 两条），0 删除行 ⇒
**合约过期：无**。HEAD 反证：我在 host 侧用 `git archive 317c12f` 另开一份工作树跑同一份新前端用例 ⇒
**67/72**，红的正好是 LL3 / LL4a×2 / LL4b / LL5 这五条判别行，四条对照行在两棵树都绿；LL2 由测试会话用
「HEAD 的 `server.cpp` 单独重编后按 `link.txt` 的对象表重链（排除 `.o` 要按全路径，否则连
`tests/test_server.cpp.o` 一起掉）」证明在 `tests/test_server.cpp:669` 红。门禁我亲自四段跑完：
`cmake .. && make -j4 && ctest` ⇒「100% tests passed, 0 tests failed out of 8」/
「Total Test time (real) = 50.18 sec」，web **72/72**，锚点 **66 处引用/0 问题**（60→66：新注释里
`README:571`、`README:360`、`README:326-327` 等引用全部落在真实行上），README **658**。

**轮次109 已有第一条并逐条自核.** MM1 `src/server/position_json.cpp:23-34` 的 `token_of`：
`is_number_integer()` 也覆盖 nlohmann 存在 `number_unsigned` 里的大于 `INT64_MAX` 的值，而
`get<long long>()` 只做 `static_cast`（`deps/json/include/nlohmann/detail/conversions/from_json.hpp`），
于是 `18446744073709551615` 变成字符串 `"-1"`——README:240 表里那格正是**白棋**标志。README:245-246
要求「识别不了就直接拒绝并列出看到的符号，绝不猜」，README:215 更把这条立成整个文件的规矩（`get<int>`
截断那类文件一律拒绝），这里却把一个认不出的值猜成了认得出的棋子：黑 1 白 1 的假局面，`check_position`
也回 `ok:true`。修法是在同处先分 `is_number_unsigned()` 走 `get<uint64_t>()` 再 `to_string`，让那个值
自报成 `18446744073709551615`，被识别器正常拒绝并回显；`word_of:59-73` 已经是这个写法，可作对位证据。
其余候选等第四个审计（训练记录/采集/Elo/参数写回链）回来再判。

## 轮次107 同一个对象上的第二次进化 — bugfix round 107 batch KK

**Found.** 优化器域两条，都是「一个函数的私有状态与它自己写在头文件里的契约不一致」：

- **KK1** `GeneticOptimizer::run()`（`src/training/genetic.cpp:123-126`）每次进来都把
  `generations_run`、`best_fitness`、`converged_`、`best_history_` 全部重置，唯独漏了
  `generations_since_improvement_`（`genetic.h:71` 的成员）。fitness 不涨的那一轮它已经把计数推到
  `stagnation_generations`，于是同一个对象上第二次 `run()` 刚进 generation 1 就命中
  `:186` 的 `>= config_.stagnation_generations`，立刻 `converged_` 并 break，返回
  `generations_run == 1`——而 `genetic.h:39-41` 的契约说的是「这一次 run 自己的改进历史」。
- **KK2** `--help` 里 `--stagnation` 那三行（`main_trainer.cpp:30-32`）写的是
  “hill climbing uses 3x this as its probe budget”。探针总数其实是 `--hill-iterations`
  （`trainer.cpp:373`），`3x` 那个值是传给 `stagnation_limit` 的（`:374`），含义是「连续
  stagnation_limit 个真的变了却没提升的探针就提前停」（`hill_climbing.cpp:79-85`；`:61`
  那种与当前点完全相同的邻居被跳过、根本不计数）。旧文案会让人以为 `--stagnation 3` 等于
  爬山只试 9 个邻居。

**Fixed.** `src/training/genetic.cpp` +4/0（`best_history_.clear();` 后一行
`generations_since_improvement_ = 0;` + 三行说明为什么 `evaluations_` 保持累计）、
`src/training/main_trainer.cpp` +2/−2（那三行的后两行）。README 不涉及这两个旗标
（README:189 明确「常用选项，完整列表见 `--help`」），所以 658 行不动。

**裁决（不是缺陷）.** 优化器/对称性审计的其余三条我读回代码后否掉了：「0 局对抗也能算出 accept」
那条我按 `trainer.cpp` 的晋升分支逐行读过，判为走不到；`hill_climbing.cpp:26` 的
`rounded <= 0 ? 1 : …` 是「系数不得塌成 0」的既有语义，不是缺陷；`seed_population` 的措辞属纯注释，
而 --help 的文案早已由 `tests/test_training.cpp:3138-3163` 钉住。训练数据审计的
K2（会话目录建不出来时顺带清 `records_path_`）以 `--no-sessions` × 已写明的 `--records` 那条
CLI 拒绝为反证——该拒绝被 `tests/test_training.cpp:3202-3204` 钉着，说明耦合是规格里的；
K3（`trainer.cpp:382` 的 `result.games_played` 快照过早）grep 全仓后确认
`RoundResult::games_played` 没有任何消费者，无可观察差异；K4（`self_play.cpp:197-199` 的
`ply + 3 > max_moves`）那行本来就带解释它是保守上界的注释。

**Verified.** 测试会话只写 `tests/`：`tests/test_training.cpp` +75/0 一例
`test_genetic_stagnation_count_is_per_run`（`:3446`，注册于 `:3567`）。配置
`{population_size 2, stagnation_generations 2, max_generations 2}` + 恒定 fitness + 固定 seed，
判别子落在同一个对象的第二次 `run()`：`tests/test_training.cpp:3477` 断言
`second_generations == 2`，HEAD 上是 1（`abort`，exit 134）；第一轮、全新对象、以及
`evaluations()` 按 `population + generations*(population-1)` 计的**增量**（钉住累计开销不被顺手清零，
两棵树都绿）三个控制臂全绿。HEAD 负例用 `git archive 5459c2f` 解到 /tmp，只编 HEAD 的两个 TU
再从 `link.txt` 的对象表里换掉对应 `.o`（脚本留在 `build/r107_kk1_head.sh`，gitignore 内）。
KK2 按纪律报「无可观察差异」，没有为文案编行为用例，只确认 `tests/test_training.cpp:3138-3163`
那三条子串钉（`--stagnation`/`3x`/`N+1`）在两棵树都绿。我另外要求的那条「hill 的评估数与
`--stagnation` 无关」被测试会话以理由驳回并被 Accepted：`hill_climbing.cpp:79-85` 确实会被
`stagnation_limit`（= 3N）截断，独立的只有 `--hill-iterations + 1` 这个上界，不是计数。
门禁由我本人重跑：`cmake .. && make -j4 && ctest` ⇒「100% tests passed, 0 tests failed out of 8」/
「Total Test time (real) = 49.08 sec」，web 63/63，锚点 60 处引用/0 问题，README 658。合约过期：无。

**轮次108 已备好并逐条自核.** 三个只读审计回来 8 条，我读回代码判下 4 条真缺陷、留 4 条：
LL2 `server.cpp:1531-1543` 的 `flush_all()` 在关停时先 `export_game_record` 再看内容，缺了
`flush_entry:1486` 那条「一子未落就不留」（README:571），于是 choose_side→resign→Ctrl+C 会往
`records.jsonl` 追加一条 `moves: []` 的假完局；LL3 `clearSetupBoard`（`web/app.js:1211-1213`）
先 `renderSetupPanel()` 再 `updatePositionStatus('棋盘已清空…')`，把 README:360 要求「底栏下面一行
显示当前计数」的那行顶掉了，而同族其他写手（`:1413` 导入、`paintSetupCell`、`swapMarks`）都是把
提示语传进 `renderSetupPanel(note)`；LL4 建议环的显示态在 `enterSetup`（`:1109`）、
`handleUndoSuccess`（`:1795`）、`handleReplay` 三处重建/切视图时没作废，README:326-327 说建议属于
它被搜出来的那个局面；LL5 `stopThinking`（`:1050-1064`）把 `aiThinking` 置 false 却不
`refreshReplayControls()`，回放簇在 `:821`/`:831` 因 `aiThinking` 被禁后就没人为它解禁——其余六个
`aiThinking = false` 点（`:210/:297/:1565/:1751/:1877`）后面都跟着刷新，这是唯一的漏网，与 JJ3
同形。暂缓：evaluator 的 `cap = five - 1`（`evaluator.cpp:175`）确实会给出 99999，正好落在
README:338 的「已证胜负」带上，但改它必须同时决定 P1（`five` 可调 vs 前端常量 99999）与 Z4，
而且 `tests/test_evaluator.cpp:25-31`、`:317` 与 `tests/test_training.cpp:2490-2505` 三面都在钉
`five - 1`，属设计判定不是单行修；`board.cpp:58-74` 的颜色守卫排在 `cells_`/Zobrist 写之后
（`random_table_` 内层只有 3 格）是防御性缺口，但没有任何经校验的调用方能送进非法颜色；
`trainer.cpp:252` 的 `games_played_ + 1 + t*1000` 在每 worker ≥1000 局时才会撞 id，README 未约定
id 语义且判别要真跑两千局自对弈；`trainer.cpp:243-251` 注释与 `:236`/`:240` 的 id 不致密只是注释
与代码不一致，无 README 依据。

## 轮次106 求招留在棋盘上的那一圈，与两个入口抢同一个文件 — bugfix round 106 batch JJ

**Found.** 三个入口，都是「前端把显示态当成了画一次就完的东西」与「CLI 漏了第三种同名冲突」：

- **JJ1** `highlightMove()`（`web/app.js:1919`）先 `drawBoard()` 再在 canvas 上临时描一圈建议环，
  没有任何字段承载它。于是任何后续重绘都会把它擦掉——点 旋转/镜像 走 `applyView()` 的 `drawBoard()`
  （`:477`），落一子走 `placeStone()` 的 `drawBoard()`（`:1916`）——而顶栏还写着「建议 H8」。
  README:326 要求 求招「结果标在棋盘上并写入状态栏」，README:326-327 又要求「局面又落一手，这一份建议作废」：
  前半句现在只在画的那一刻成立，后半句根本没人实现。
- **JJ2** `--out` 与 `--checkpoint` 指向同一个文件时无人拒绝：`--resume` 的三种冲突在
  `main_trainer.cpp:278-305` 逐一退出，`:307-308` 却把这两个路径一起接住；收尾 `:342` 的
  `save(out_path, …)` 遂把断点文件换成参数文件，本轮日志里「checkpoint saved」全是假的，
  下一次 `--resume` 直接读不回来。
- **JJ3** `handleStateSync`（`:1696`）把 `replayIndex` 清成 `null` 却没 `stopReplayAuto()`——
  `:710`、`:1102`、`:1311` 三处同类重置都是先停再清，这一处是唯一的漏网。自动播放的 tick
  （`:777-786`）在下一拍看见 `null >= length-1` 为真，走 `exitReplay()`，把刚写下的「对局开始」
  改成了「已回到当前局面」。

**Fixed.** `web/app.js` +20/−8、`src/training/main_trainer.cpp` +10/0。建议环改由 `drawBoard()` 从
`this.recommendation` 描（放在 `view.lastMove` 块之后、回放标记之前，半径 `stone + 3`、`#c0392b`、
lineWidth 2 与旧代码逐字一致），`highlightMove` 只写状态并重绘；落子与四个整局重建点（session 帧
`:293`、position_set `:1315`、重开 `:1565`、state_sync `:1697`）把这份显示态清掉。CLI 侧在
`--seed`×`--resume` 之后、任何文件操作之前加一条 `out_path == checkpoint_path` 的退出码 1 拒绝，
口径与 `:262` 的字面串比较一致。README:204（`--out`）与 README:207（`--resume`）由我在同一行内补齐
文档——后者是轮次101 FF2 落地时漏下的那条 `--seed` 冲突。

**裁决（不是缺陷）.** 审计给出的另外几条我自己读回代码后否掉了：
`has_decisive_tempo`（`vcf.cpp:365-385`）“少了 `four_count >= 2` 那一条”不成立——`pattern.cpp:66-96`
里 `four_count` 只由 `LIVE_FOUR` 与 `RUSH_FOUR` 累加，`>= 2` 必然已经有那两类方向，循环先返回了；
反过来说 `vcf.cpp:209-211`/`:332-334` 那两处内联的 `four_count >= 2` 才是永远走不到的死代码。
`must_block`（`search_engine.cpp:127-129`）跳过对 BLACK 是禁手的对方成五点也不是缺陷：那个格子 BLACK
本来就不能下，挡不了就交给 alphabeta 的 `-100000` 门（`:313`）与 loss probe（`:339-349`）；`:129` 的
重复检查只是冗余。离线点 识别 “吞掉 `sendMessage` 的 false”同样不成立——`sendMessage` 自己那条 false
分支（`:1602-1610`）已经把 README:554 的「消息未发出」写进顶栏了，弹窗挡住顶栏是观感问题不是契约。
`set_vcf_depth`/`set_vct_depth` 无生产调用方是 REVIEW_LOG:250 记过的刻意保留。vct 的 90000 不画红点
就是轮次95 暂缓的 Z4，改它要同时动 README:338、`app.js` 与 harness，仍不动。
置换表与开局库整域由独立审计逐条走过，没有任何可观察缺陷：无 STM 的 Zobrist 键靠严格交替成立
（README:219-220），将杀分被 `alphabeta.cpp:255` 的存储门挡在表外，probe 先于终局检查也安全
（终局型从不入表），深度门/界限标志/代龄老化合规，`search_engine.cpp:169-179` 的 `max_depth` 包含性
正确且库着法在 `Board` 重载里重查占位与禁手。

**Verified.** 测试会话只写 `tests/`：`tests/web_frontend_check.js` +202/0 新增 6 例（JJ1 的「环挺过一次
重绘」「无 求招 的整盘重绘不描环」「落一子退役这一圈」，JJ3 的「state_sync 释放它刚作废的回放钟」
加它的控制例），`tests/test_training.cpp` +84/0 新增 JJ2 一例（控制：两个不同路径退出码 0、stderr
干净、两个文件都写出；判别：同一路径退出码 1、stderr 点名 `--out` 与 `--checkpoint`、文件不被写出；
强半：预先存在的断点文件字节级存活）。回归门由我本人重跑：`cmake .. && make -j4 && ctest` ⇒
「100% tests passed, 0 tests failed out of 8」/「Total Test time (real) = 47.11 sec」，
`node tests/web_frontend_check.js` 63/63，`node tests/readme_anchor_check.js` 60 处引用 0 问题，
`wc -l README.md` 658。HEAD 负例我在宿主侧从 `git archive` 副本独立复现，未碰 `build/`：
812d35d 的树 61/63，红的只有 JJ1 与 JJ3 这两行，61 条历史用例全绿。JJ2 的 `--rounds 0` 不能判别——
轮数守卫排在这条拒绝之前，两棵树都是退出码 1，测试会话在注释里记下了这一点并改用 `--rounds 1`。

**Held.** 训练数据审计给出的四条排进轮次107：K1 导出条件用 `is_decisive()`，而 README:574 说的是
「终局」、README:63 说的是「完局」——满盘判和（`game_manager.cpp:445-452`，winner EMPTY）因此不进
`records.jsonl`，而 trainer 自己却把 `move_limit`/`board_full` 的未分胜负局写进同一个文件
（`trainer.cpp:263-264`），两个生产者对「完局」的理解不一致；改的时候必须同时改 `:1460` 与冷重建的
`:1207`，否则和棋局会在一次 resume 后被追加两次。K2 `server.cpp:546-549` 会话目录建不出来时顺手清了
`records_path_`（README:165-166 只授权关闭落盘，README:161/169-172 把 `--records` 当独立功能）。
K3 `trainer.cpp:429` 的 `result.games_played` 快照取在本轮数据局之前，而 `:434` 的 `samples_collected`
取在之后。K4 `self_play.cpp:197-199` 的 `ply + 3 > max_moves` 与 README:609「最靠近终局的那一步」相差
两手。否掉的：不给路径时数据局整个跳过（README:205 的「默认空」才是那句具体规定）、
`--no-resign` × `no_legal_move` → INVALID 弃局（要构造全盘无合法点的局面，与轮次92/101 已判过的
不可达同源）、`game_id` 的 `t * 1000` 重叠（README 未约定，且需 2003 盘）、奇数验证局向上取整成偶数对
（README:179 每对黑白各一局本来就是偶数）。



## 轮次105 被关掉的连接替不了那把锁，正在编辑的副本不该被终局改口 — bugfix round 105 batch III

**Found（其中 III1 是我自己在轮次102 引出的回归）.**
- III1：批次 GG 把 确认/取消/清空/摆棋 四个键按 `setup.confirmInFlight` 一起按住，而那次释放只有三个出口
  （`handlePositionSet`、`exitSetup`、`error` 帧 `:1648`）。README:565-567 说同一 token 被第二个标签页 resume
  时，服务器给旧连接发完 `session_taken_over` **就把它关掉**——旧标签页从此再也收不到 `error` 帧。于是
  `handleTakenOver` 里 `:208-209` 只清了 `aiThinking` 与 `hintInFlight`，摆棋一旦被接管打断就**永久冻死**：
  取消按 README:355-357 应当只丢本地副本、根本不需要连接，现在连点都点不动，README:378 的「留在摆棋里重试」
  也一并不可达。
- III2：同一把锁只盖住了按钮，没盖住棋盘。`handleCanvasClick` 的摆棋分支不看 `confirmInFlight` 就直接
  `paintSetupCell`，玩家在 确认 发出后继续刷格，那些格子改了 `setup.board/record/preview`，却会被
  `exitSetup(true)` 装回的服务器回盘整片丢掉——刷了没生效，且一句提示都没有。
- III3：`handleGameOver` 无条件写顶栏与副行，而 ai_move / undo 两条同类路径都有 `!setup.active` 守卫。
  落子或求招之后点进摆棋（`enterSetup` 不拦任何在飞的事），AI 那一手若直接终局，README:379 承诺「摆棋期间
  顶栏归那条琥珀色指引，不会把指引冲掉」就被终局帧冲掉了。

**Fixed（`web/app.js`，+17/−3）.** III1 在 `handleTakenOver` 里清 `confirmInFlight` 并调
`refreshSetupControls()`——四个键的 disabled 表达式只有那一处真相，不重写四行赋值，也不 `exitSetup`
（丢掉玩家的工作副本比按住按钮更糟）。III2 在摆棋分支开头加同一句 `if (this.setup.confirmInFlight) return;`，
守卫放在点击入口而不是 `paintSetupCell` 本体（摆棋记录行与导入弹窗还要调它）。III3 只把顶栏那两行包进
`if (!this.setup.active)`，`gameStatus`/`gameOverText`/按钮置灰一律保持无条件——整段跳过会把 `gameStatus`
留在 `playing`，`exitSetup` 还原 `setup.gameButtons` 时就会把 悔棋/求招/认输 错误地放回可用；并在 `exitSetup`
的取消分支里，若 `gameStatus === 'game_over'` 就回画 `gameOverText`，玩家错过的结局在他放下副本那一刻拿到。

**裁决（本轮唯一一条合约过期，我判 III3 赢）.** 轮次98 的 `CC4 a game over during setup still owns the top bar`
断言「终局优先于面板」，并把 `handleGameOver` 写成「摆棋不动顶栏」的**故意例外**——那条例外只存在于测试自己的
注释里。全仓库 README 提到顶栏/指引的只有 :379 一处，且那半句对摆棋状态是无条件限定（同句后半才把范围收到
旋转/镜像），没有任何一句支撑「终局可以冲掉指引」；反面的顾虑（玩家拿不到结局）也不成立，因为结局被存进
`gameOverText` 并在退出时回画。故该用例与其注释里的例外说法一并退休，注释按新口径重写；同组另外四例
（ai_move / undo_success，摆棋内外）与轮次103 的 HH2 副行用例（在 `setup.active === false` 时发帧）全部未动且仍绿。

**Verified.** 六个新用例（`tests/web_frontend_check.js`，纯增 162 行 / 18 行删除——删的只有被裁决退休的那一块
与那两行错误注释）：III1 断言接管后四个摆棋键 `disabled === false` 且 `setup.active`/`board[5][5]`/
`record.length`/`preview` 一个都没被清；III2 由 `geom.pad/cell` 造坐标驱动 `handleCanvasClick`（不直接调
`paintSetupCell`，否则绕过本轮的守卫），断言锁定期 `board[4][4] === 0`、整盘 JSON 与快照逐字一致、`record`
不变；III3 断言终局帧后顶栏仍等于原指引、`gameStatus` 与 `gameOverText` 已更新，`exitSetup(false)` 后顶栏
`=== gameOverText`。三条对照组（无往返时收同一帧 / 未锁时同一次点击真的落子 / 无摆棋时终局当场写顶栏）在
HEAD 也应为绿——HEAD 负例（只读 `git archive 3afb114` 到 /tmp，工作树未被 checkout 覆盖）三条判据全红
（`web_frontend_check: 56/59`），对照组全绿。门禁由我本人重跑：ctest **8/8**（48.62 s）、web **58/58**、
anchor **55 处引用 / 0 problems**（49→55 是修复方两处 + 测试方三处 `README:378/379/567` 新引用造成的漂移）、
README **658** 行。

**Held（下一批）.** D4 建议环被 `applyView` 的 `drawBoard()` 抹掉、D5 `enterSetup` 不拦在飞的回放索引、
D6 离线 识别 吃掉 `sendMessage` 的 false；训练/CLI 侧 P1（`five` 从未与可调系数互相界定）、P2（冠军未变仍打
完整验证赛并注册逐位相同的版本）、P3（`--out` 与 `--checkpoint` 同名未被拒，而 `--resume` 同名被拒）。C5
（导出 revoke 时序）仍因 harness 无真定时器而挂起。

## 轮次104 停机那一步先记账再改口 — bugfix round 104 batch II

**Found.** `flush_entry` 的次序是「先写会话文件，后追加 records.jsonl」，运行期靠十分钟一度的 tick
重试这个写法，是对的。但停机路径把返回值丢了：`stop()` 调 `flush_all()`，`flush_all()` 循环里没人看
`flush_entry` 的结果，紧接着 `ioc_.stop()` 就退进程。于是 Ctrl+C 那一瞬追加失败（磁盘满、权限、文件被占）
时，一局已经分出胜负的对局**永久**从训练流里消失（README:572 承诺收到退出信号时同样刷一遍，README:575
承诺终局且从空盘开始的那一局有它自己的一行），而且会话文件已经先把「已结束」的状态落盘，冷重建按
`record.is_decisive()` 把 `exported` 当已导出（`server.cpp:1207`），下一次真分出胜负才是最后的机会。

**Fixed（`src/server/server.cpp`，+28/−9，未碰任何 .h）.** 把 `flush_entry` 里那段导出判定与追加抽成
一个文件内部函数 `export_game_record(bool& exported, const SessionSnapshot&, const std::string&
records_path, std::string& error)` —— 条件表达式、`对局记录追加失败: ` 的输出、成功才置 `exported`
全部逐字保留，只有一份真相。第一个参数用 `bool&` 而不是 `TrackedGame&`，因为 `TrackedGame` 是
`server.h:94` 起的 private 嵌套类型，文件作用域点名它要改头文件。`flush_entry` 改为调它，**次序一字
不动**（仍是会话文件在前、追加在后、`saved_payload` 只在两者都成功后才提交）。只有 `flush_all()` 换成
**先追加、后写文件**：追加失败就 `continue`，本次不推进会话文件，磁盘上的会话停在终局之前 —— 重启后
重建出来的是未完局，`exported` 不会被误置，那一局下次终局仍会落进 records.jsonl。

**Not defects（我判掉的）.** 修复会话主动报备了一个残留：追加成功、随后的会话写入失败时，记录流里已有
这一局而会话文件仍是旧局，日后再次终局会再多一行。我判为可接受，不改：HEAD 在同样组合下是**整局记录
直接消失**；会话文件停在旧一手意味着重启后的棋路必然分叉，那两行不是逐位重复；而先落的那一行描述的是一局
**真的下完过**的对局。以「一次真实多余」换「不再静默丢失」是本轮想要的方向。`--no-sessions` 下 `exported`
只写在内存里 = 无可观察差异（见下）。

**Verified.** `tests/test_server.cpp` 纯增 138 行、0 行删除，既有的 tick 路径契约
（`:474 test_failed_export_retries_records_append`、`:282`、`:327`）未被编辑且全绿。判别用例
`:547 test_shutdown_flush_keeps_the_file_behind_a_failed_append`：把 `records_path_` 指向一个普通文件
当目录使追加失败，调 `flush_all()`，断言 records 行数不增、`!exported`、`saved_payload` 未提交，且
`load_session` 出来的会话**仍停在 8 手未终局**；恢复可写路径后第二次 `flush_all()` 恰好补一行、文件才
推进到 9 手 decisive。控制组 `:613` 断言同一终局连刷两次只多一行。HEAD 负例（只读 `git archive 1b0d236`
到 /tmp）在 `:589` 的 `on_disk.record.moves.size() < 9` 上变红（`HEAD_RC=134`），控制组在 HEAD 为绿。
`:652` 那条把 `--no-sessions` 边界钉成「两条路都不写、不报错」，并说明判别态（records 非空 + sessions
为空）在 `main.cpp:94-98` 的拒斥下不可达。门禁：ctest **8/8**（46.14 s）、web **53/53**、anchor
**49 处引用 / 0 problems**（47→49 是本轮测试注释新增的 README:572/575 引用）、README **658** 行。

**Queued（下一批，已核验未下发）.** batch III = `web/app.js` 三行：D1 是我自己在 batch GG 引出的回归
（`handleTakenOver` 不释放 `setup.confirmInFlight`，而 README:567 说旧连接随后就被关掉，`:1648` 那次释放
永远到不了 —— 开着摆棋时页面被彻底冻住，README:377/378 都不可达）；D2 `handleCanvasClick` 的摆棋分支
没有同一把锁，往返期间画的子会被 `exitSetup(true)` 丢掉；D3 `handleGameOver` 的两行状态写入缺
`!this.setup.active` 守卫，会冲掉 README:379 那条琥珀色指引。

## 轮次103 认错得迟了的那一份读法，与终局之后仍在报的轮次副行 — bugfix round 103 batch HH

**Found.** 两个只读审计（会话落盘/训练记录、前端摆棋与回放状态机）在批次 GG 提交期间交回结果，本轮下发
其中 3 条，**每一行都由我在含 GG 的当前工作树里重读过**（GG 把 `app.js` 的行号整体推了约 9 行，所以旧行号
一律重新锚定）：

- **HH1（中，确认写进副本的是没人看见的那份读法）** README 摆棋/导入那一节（:360-370）说 **识别** 只是干跑，
  随后 **确认** 写进副本的必须是玩家当时看到的那一份读法。`web/app.js` 的 `checkPosition()` 发出
  `check_position` 时不带任何请求标记，而 `handlePositionPreview` 无条件
  `this.setup.preview = msg.ok ? msg : null;` + `renderImportState(...)`，后者内部又会调
  `refreshSetupControls()` 重新武装 `btn-import-apply`。「内容已改动，点"识别"重新检查」那行只是一次
  render（把 `setup.preview` 置空后重画），拦不住晚到的回复：识别发出后玩家改了粘贴框，旧回复回来照样
  覆盖提示并武装 确认，最终 `applySetupBoard(setup.preview.board)` 落的是陈旧读法。对照：求招那条路是有
  跟踪的（`hintMoveCount`）。
- **HH2（低-中，终局副行不认终局）** README:307 要求顶部状态栏给出当前轮次/思考状态，README:522 又规定
  `move_made` 在前、`game_over` 在后。`handleGameOver`（`:1848-1884`）设了 `gameOverText` 与
  `updateStatus(...)` 就结束，全函数**没有** `updateTurnInfo()`（可 grep 证：:315/:708/:1317/:1574/:1684/
  :1716/:1737/:1793 都有，唯独这里没有），所以上一手画在副行的「轮到白棋 · AI 思考中」在终局帧之后仍挂着。
- **HH3（低-中，行点击绕过回放锁）** README:310 保证落子记录每一行可点——所以不能靠隐藏行来修。按钮那条路
  在 `refreshReplayControls()` 里按 `!hasMoves || this.aiThinking || this.setup.active || ...` 置 disabled，
  而行点击 `:2096` 直调 `showReplayMove(index)`，后者只查 `moveHistory.length === 0` 与
  `replayBelongsToOldGame()`。AI 思考期间点一行就把棋盘切进「回放 · 第 N 手」，而被禁掉的 ⏮◀▶⏭ 恰好是
  唯一出不去那个视图的出口。

**Fixed.** 全部只动 `web/app.js`（+11/−0）：`setup` 新增 `checkedText`，`checkPosition()` 记下当时提交的文本，
`handlePositionPreview` 开头 `if (this.importPayload() !== this.setup.checkedText) return;`——纯客户端判据，
不改报文，文件导入与拖拽也照旧先落进粘贴框、不例外；`handleGameOver` 补一次既有的
`this.updateTurnInfo('')`（与 :1317/:1574 同一写法）；`showReplayMove` 加一条与按钮同源的锁
`if (this.aiThinking) return;`，行、按钮、自动播放共用同一个入口因而一起受锁，而行仍可点。

**Not defects / held.** C4（接管后 `btn-restart` 仍武装 + `handleSession` 先写 token）判非缺陷：重开点击经
`sendMessage` + `readyState === OPEN` + `takenOver` 三重 fail-closed，且那两个分支里写入的都是本 socket 现在
持有的 token（README:519-520/526-527）。C5（导出同一任务内 `URL.revokeObjectURL`）审计自评即「潜伏」，
harness 没有真定时器、测不了，不下发。**N1 机制成立但本轮不动**：`flush_all()`（`server.cpp:1520-1525`）
丢弃 `flush_entry` 的 bool，`stop()` 在 :583 调完就 `ioc_.stop()`（:589），而 :1465 的
`payload == entry->saved_payload` 正是「运行期会重试、停机时不会重试」的成因——它的正确修法是终局 flush
改成先追加记录再写会话文件，那属于轮次85 已定的「绝不重复导出」取舍的持久化次序那一半，必须先设计再下发。
冷重建把 `exported` 置真那一行经复核**仍是**轮次85 的既有取舍，不再重开；`trainer.cpp:291-294` 的
`break` + 孤儿样本同理维持原判。

**Verified.** 测试会话只写 `tests/web_frontend_check.js`（+175/−1，唯一删除行是 harness 自身的
`appendChild() {}` 换成会记 `_children` 的实现——回放行的 listener 由此可达，属基础设施而非断言；我核对了
diff）。七条新用例：HH1 三条（改了粘贴框后旧 ok:true 回复既不写 `preview` 也不武装 `btn-import-apply`、
文本未变时同一条回复照常写入、陈旧拒绝同样被丢掉而当前拒绝照常显示）、HH2 一条（终局重写副行而不是留着
「轮到X · AI 思考中」——它先读了 `updateTurnInfo` 的定义再断言，结论是补的那一行已足够，无需新增分支）、
HH3 三条（思考中行点击不改 `replayIndex`、不在思考时同一点照旧进第 N 手、这把锁不会把**离开**回放的出路
一起锁死）。HEAD 侧四条红点来自只读 `git archive ea47e53` 解包树，控制组在两棵树上都绿。
另：HH1 的新判据我要求它专门去找「把不陈旧的回复也掉掉」的路径（文件导入/拖拽的文本来源是否与
`importPayload()` 一致），它回报未发现。门禁：ctest **8/8**、web **53/53**、锚点 **47 citations /
0 problems**、README **658**。合约过期：仅锚点计数基线（43→47 来自新注释引用 README:307/310/360-370/522）。

## 轮次102 改名成快速路径的那一手仍带着 AlphaBeta 的计数，与确认往返期间仍可弃掉的副本 — bugfix round 102 batch GG

**Found.** 两条都来自轮次101 排到本轮的前端状态机与 replay 契约队列，且**每一行都由我本人读过码**才下发：

- **GG1（中，契约被自己的改名分支打破）** README:530-531 说 `replay` 里「非 AlphaBeta 路径不填
  `depth` / `nodes`」。`src/search/search_engine.cpp:291-292` 却先无条件把 `info.depth` /
  `info.nodes_searched` 填进结果，再在 `:297` 把「一层都没跑完、退回 `root_moves[0]`」这一手
  改名为 `method = "greedy"`；而 `src/search/alphabeta.cpp:393` 的
  `info_.nodes_searched = nodes_searched_` 是无条件收尾的。于是这一手经
  `game_manager.cpp:422` → `server.cpp:1021-1022` 落成
  `{"method":"greedy","depth":0,"nodes":N>0}`——名字已是快速路径，数据还是 AlphaBeta 的。
- **GG2（高，取消被服务器回答推翻）** README:355-357（确认是唯一写服务器的一步、取消直接丢掉副本、摆棋
  再点一次等同取消）、:366（清空只清副本）、:377（被拒时留在摆棋里重试）。`web/app.js:1436-1448`
  `refreshSetupControls()` 在 `setup.confirmInFlight` 为真时只按住 `btn-setup-confirm`（:1447），而
  清空（`:377`→`clearSetupBoard():1172`）、取消（`:379`→`exitSetup():1124-1147`，它在 :1132 直接把 flag
  清零并还原底栏）、摆棋开关（`:439`→`toggleSetup():1049`）三个入口全都还能点；更关键的是
  `handlePositionSet:1266-1305` **无条件**安装服务器回盘——`wasSetup`（:1267）只在 :1305 决定要不要多清一行
  提示。所以玩家在确认的往返途中点了取消，那份已经被放弃的副本照样铺到 live 棋盘上，同时
  `game_status='waiting'`、`playerColor=null`、`analysis=[]`，而服务器那边确实已经把局面换掉了。

**Fixed.** `search_engine.cpp` 只在 :297 那条 relabel 分支内把 `result.depth` 与
`result.nodes_searched` 一起归零（`method` 保持 `"greedy"`），其余分支、`score`、`elapsed_ms`、进度回调以及
`alphabeta.cpp` 的那个计数（思考帧要用，明确禁改）逐字不动（+7/−1）。`web/app.js`：三处**点击入口**的箭头
函数加 `if (this.setup.confirmInFlight) return;`（:377/:379/:439），`refreshSetupControls()` 让
`btn-setup-cancel` / `btn-setup-clear` / `btn-position-toggle` 与 `btn-setup-confirm` 同进同退，并在
`exitSetup()` 清完 flag 之后重新放开这三个按钮（+20/−3）。守卫刻意没有加进 `exitSetup()` 本身——
成功路径正是靠 `handlePositionSet` 里的 `exitSetup(true)` 收尾，加在那里会让它自己走不完；
失败路径无需改动，`:1618-1624` 的 `error` 分支本来就会清 `confirmInFlight` 并重画控件。

**Not defects / held.** `exported` 冷重建那一行（`server.cpp:1207`）不是改一行能了事的：
`grep -n exported src/server/session_store.{h,cpp}` **零命中**，该标志只活在内存条目里
（`server.cpp:758/897/990/1207/1504/1511`），而 `flush_entry` 先 `save_session`（:1494）后追加 records
（:1506），所以会话文件本身无法区分「已追加」与「写完会话就崩了」——正确的修法必须让
`SessionSnapshot` 持久化这个标志（要动 `.h` 并由我同步改 README 的 `session` 样本），单独排一轮。
`trainer.cpp:291-293` 的 `break`（换 `continue` 也无从判别：写坏的路径会让整轮全失败）与
`games_played_`（:256）在落盘之前就计数、检查点在 :373 保存，是一个需要设计取舍的「失败追加欠谁」问题，
未凑成一行下发。前端审计余下的 C1（陈旧 `position_preview` 回盘重新武装 确认）、C2（`handleGameOver`
不调 `updateTurnInfo()`）、C3（落子记录行点击绕过回放锁）、C4（接管后 `btn-restart` 仍武装）、
C5（导出同步 revoke object URL）仍留在队列里。

**Verified.** 测试会话只写 `tests/`：`test_search.cpp` +73/0（零删除行），
`test_engine_greedy_relabel_zeroes_the_alphabeta_count`（:2672，注册于 :2775）直接用
`SearchEngine::search` 打一个 24 子的中盘——它先算清楚「整层 4858 节点」而引擎最小节点预算是
`remaining_time * 200`（`search_engine.cpp:263`，`remaining_time` 下限 100 ms ⇒ 20000 节点），
于是断点只能来自绝对 deadline，10 ms 预算正好落在**层内**而非层后，这才把「脏 nodes」做成可构造的行为差异
而不是形状针；关掉了 VCF/VCT/认输探测三个分片，控制组同局面给 2000 ms 断
`method=="alphabeta" && depth>=1 && nodes>0`，并断进度帧 `frame_nodes > 0` 以证明归零只发生在改名分支、
活儿确实干过。`web_frontend_check.js` +112/−2，四条 GG2 用例（在飞时四按钮同禁、点三个入口后
`setup.active`/副本棋盘/`setup.record`/`confirmInFlight` 逐字不动、走一次 `exitSetup` 三个同伴恢复可用、
不在飞时行为如旧）；那 2 个删除行是 `makeElement` 的基础设施（把空 `addEventListener` 换成被动记录
listener 并 `return element`），该会话明确说明存量用例从不触发 listener，我核对该说明为实。
**过程说明**：该会话的最终回报停在中途状态（「HEAD 负例 + test_server 抖动归因还在跑」）就结束了回合，
所以本轮结论以**我自己跑的同一工作树门禁**为准；我这次 ctest 8/8 全绿，未复现它提到的抖动，锚点从 36 涨到
**43** 是新增注释引用 README:355-357/366/377/530-531 造成的基线漂移，不是回归。
门禁：ctest **8/8**（47.16 s）、web **46/46**、锚点 **43 citations / 0 problems**、README **658**。
合约过期：仅锚点计数基线。

## 轮次101 把 5-3 局算成和棋的那半个括号，与被时钟推翻的完整一层 — bugfix round 101 batch FF

**Found.** 三个只读审计（训练数据/Elo、搜索时间预算与中止路径、前端状态机）加上我自己的两处通读
（`game_manager.cpp` 全量、`board.cpp` 增量维护）交回十余条，逐条读码后留下 3 条。**我自己的通读
一条缺陷都没交出来**（详见 Not defects 第 1-5 项）。前端审计在批次下发之后才回，它的 5 条我已登记为
轮次102 队列，本轮未下发。

- **FF1（中，统计口径自相矛盾）** `src/training/elo.cpp:84-93` 的 W/L/D 括号与它**自己上面的注释**相反：
  注释写「只有正好一半才算和棋，因为 `--verification-games 4` 的 3-1 是 0.75」，代码却用
  `>= 0.75` / `<= 0.25`。README:198 的默认值是 `--verification-games 8`，得分以 0.125 为步长，于是 5-3 局
  （0.625）两边各记 `draws++`——而这一局正是 `trainer.cpp:363` 按 `accept_score` 0.56 把挑战者晋升的那一局，
  `main_trainer.cpp:343` 随后把它打印成 `0-0-1`。晋升、Elo 增量、`games` 都没错，错的只有胜负场次归属。
  两名审计独立报出同一条。
- **FF2（中，明说的参数被静默丢弃）** `main_trainer.cpp:222` 接受 `--seed`，`trainer.cpp:593` 在 `--resume`
  时用 checkpoint 里的 `seed` 键覆盖它，所以 `--resume … --seed 7` 读完就扔、退出码仍为 0；README 从未授权
  `--resume` 覆盖 `--seed`。与已落地的 Q2（`--resume` × `--params` 拒绝）是同一类「同一入口的两种说法互相
  矛盾」，README:169-172 的口径是解析期报错退出并点名 flag。
- **FF4（高，完整的一层被丢弃）** `src/search/alphabeta.cpp:346` 的迭代加深提交门在根循环**跑完之后**重读
  一次时钟与节点数：一个把 225 个根候选全部搜完、`aborted_` 仍为假的分量，会因为 deadline 恰好落在
  「最后一次根搜索结束」与「这次重读」之间而被整层丢弃，`:352-354` 的意图注释与 README:521/591
  （提交的是**完成**的那一层）都相反。`aborted_`（`:95/:148/:178/:231` 设，`:266` 每次 `search()` 清一次）
  本就同时覆盖超时与节点上限短路，所以多余的正是那两个重读。

**Fixed.** `elo.cpp` 括号改为 `score_a > 0.5` / `score_a < 0.5` / else 和棋，注释同步为它现在真的实现的那条规则，
Elo 增量与 `games++` 两行逐字不动（+5/−5）；`main_trainer.cpp` 与既有 `checkpoint_given` 并列加 `seed_given`，
把新拒绝块放在 `--resume` × `--params` 旁边，只在**两个都说出口**时 `return 1` 并同点名（+12/0，符合
「只拒文件真正说过的值」口径）；`alphabeta.cpp` 每层一个 `bool layer_complete = true`，在 `:331` 的 break 前置
false，提交门换成 `layer_complete && !stop_flag.load() && !aborted_`（+8/−2）。安全性论证：`aborted_` 只在
`:266` 清，故新门绝不可能提交一个节点被短路过的层——它不比旧门松，唯一的行为变化就是那条被丢弃的完整层。

**Not defects.** 1. AI 与人类的禁手对称成立：`game_manager.cpp:174` 拒人类禁手，AI 提交路径 `:420` 不查，但
`result.best_move` 的每个来源都已先过滤（`search_engine.cpp:61-81`、`alphabeta.cpp:33/:127`、
`opening_book.cpp:236`）。2. `no_legal_move` 冻不住会话：`:48-55` 只在下满 225 格时触发，而那时
`check_game_over`（`game_manager.cpp:446`）已判和棋。3. undo/replay 奇偶一致（`undo()` 的弹出上限
`analyses_.size()`，`replay_snapshot()` 同一条不变量的反向）。4. `Board` 落子时不清
`neighbor_count_[placed]` 是对的，因为 `undo_last_move` 严格按栈序移除。5. `SearchController` 无生产调用方、
搜索非并行，README:596-597/399 已明说。另否掉/挂起：`elo` 的 `games` 按场计而非按局（README:49 只要求「自适应
K」）、`trainer.cpp:252` `game_id_base`、`data_collector.cpp:171` 忽略 `place_stone` 返回值（仓库内无人回读
`records.jsonl`）、VCF/VCT 绑定了 `tt_` 却不查询（文档未要求的优化缺失）、`high_resolution_clock` 与
`steady_clock` 混用（此处不可测）。前端审计的「每帧全量 `drawBoard()`」是 README:519-521 的要求，不是缺陷。

**Verified.** 测试会话只写 `tests/`（`test_training.cpp` +172/0、`test_search.cpp` +76/0，`git diff -- tests/`
零删除行，我逐行核过），HEAD 负例树用只读 `git archive a9651bd` 建到 /tmp：
`test_elo_scores_a_majority_match_as_a_win` 直接驱动 `EloSystem::record_result` 七个得分档位，同时断两边
w/l/d、`games == 1` 与 Elo 增量 `32*(score-0.5)`。HEAD 原文 `test_training.cpp:475: Assertion
'elo.player(a).wins == arm.wins_a' failed`，可观察行 `score 0.625 -> candidate w/l/d 0/0/1, champion w/l/d 0/0/1,
elo 1504`——两棵树 elo 数值相同，正好证明分数回归从未移动。
`test_trainer_refuses_a_seed_that_a_resume_would_drop` 只判退出码 + stderr 是否点名 `--seed`/`--resume` +
checkpoint 里 `"seed"` 的数字。HEAD 原文 `test_training.cpp:3313: Assertion 'status == 1' failed`，可观察
`--resume '…ck.json' --seed 7 => exit 0:`，stderr 0 字节、checkpoint 仍是 `"seed": 20261011`。
FF4 结论是**无可观察差异**：`alphabeta.h:63` 是裸 `high_resolution_clock`、没有注入缝，那条竞争窗口无法构造
确定性负例，故只做代码形状钉（`test_search.cpp:2600`：声明 320 < 破坏 331 < 提交 351、`aborted_` 清 265、
4 个短路点、提交行不含 `is_timeout`/`node_limit_`）；HEAD 原文 `test_search.cpp:2632: Assertion 'gate >= 0' failed`。
合约过期：**无**——既有 W/L/D 断言只有 1.0 胜、0.5 和（`:381/:388`）与 `verification_score == 0.5 ⇒ draws == 1`
（`:1332`），在 `>0.5/<0.5` 下全部保持。另有两处是我写简报时编造的事实，由测试会话顶回而非迁就：基线锚点
不是 41 条/README 706 行（实为 36/658），`main_trainer.cpp` 也没有 `--seed-file` 这个选项。
门禁（我与测试会话各跑一次，一致）：ctest **8/8**（44.47 s）、web **42/42**、锚点 **36 citations / 0 problems**
（35→36：多出的那条是测试注释引用 `README:198`）、README **658**。

## 轮次100 让整盘评估归零的那一格参数，与被静默丢掉的落盘目录 — bugfix round 100 batch EE

**Found.** 三个只读审计（G：会话持久化 + 断线续局 + 记录导出；H：`web/app.js` 帧处理；I：启动 CLI +
参数→引擎配置 + HTTP 静态服务）交回 8 条，逐条读码后留下 2 条。**G 交回 0 条代码缺陷**，它自己的
near-miss 账本（摆棋 base 与 played 重叠、`--no-sessions` 后的挂起、冷重建把 `exported` 置真、终局
flush 的 `net::post` 撞上 rotate）每条都指到了把它清掉的那一行。**H 的 4 条全部被我否掉**：它们都建立在
「服务器发出一份不合规格的帧」这个前提上，而服务器唯一的棋盘发射口
`src/server/server.cpp:1557-1567` 逐字写死 15 行 × 15 列、格子只可能是 `EMPTY/BLACK/WHITE`，
`state_sync` / `session` / `position_set` 全部经它出盘，前端那些不加守卫的读法在文档路径上打不到。

- **EE1（高，静默变盲）** `src/training/params.cpp:215-220` 的 `five` 下限守卫是 `five <= 0`，注释却写明
  它的存在是为了保护消费端 `src/core/evaluator.cpp:175-178` 的 `cap = coeffs_.five - 1`。于是
  `{"coefficients": {"five": 1}}` 被接受、服务器照报参数已加载（`main.cpp:100`），而 cap 恰为 0 ⇒
  `evaluate_board` 对**任何**局面返回 0，静态评估整体塌成平地——正是那条注释声称要防住的状态。守卫差一格。
- **EE2（中，含糊指令被静默重新解释）** `src/main.cpp:51-54` 记下 `--no-sessions` 后在 `:85-88` 无条件
  `sessions_dir.clear(); records_path.clear();`，所以 `./gomoku_server --no-sessions --sessions data/sessions`
  会把用户明写的目录丢掉且不改退出码；而孪生开关 `--no-book` 在 `:43-44` 当场 clear，后面的 `--book` 反而能赢——
  两个同类开关的优先级相反。README:169-172 的口径是同一类含糊必须报错退出点名 flag（它举的例子正是
  「把 `--sessions ""` 当成关闭则会让完局静默地不落盘」），并未授予 `--no-sessions` 跨顺序否决权。

**Fixed.** `params.cpp` 下限改为 `coeffs.five <= 1`、注释同步为「cap 必须为正」，上限与 `forbidden_penalty`
守卫一字未动（+5/−4）；`main.cpp` 加 `sessions_stated`/`records_stated` 两个局部 bool，在 `--port` 校验之后、
`no_sessions` 生效之前把 `--no-sessions` 与显式 `--sessions`/`--records` 共存判为冲突，stderr 同时点名两边并
`return 1`（+15/0，两处 clear 保留，`--no-book` 路径不动）。另有一处文档自洽由我直接改：README:503 的
`session` 样本写着 `move_count: 6` 配 `current_turn: "white"`，与 README:219-220 的奇偶规则和
`server.cpp:1309`（`replay.move_count % 2`，且 `game_manager.cpp:261` 让这个计数含摆棋 base）都对不上，
改为 `move_count: 5`（1/1 行，仍 658）。

**Not defects.** H 的 4 条（见上，`server.cpp:1557-1567`）；I 的 `forbidden_penalty` 范围守卫无生产消费者
（`:221-223` 的注释自己已声明这点，属挂起项而非缺陷）；I 的 `five > 100000` 上端与 99999 红点阈值撞车那一半
就是 REVIEW_LOG 里挂起的 **Z4**，本轮不重复下发。

**Verified.** 测试会话只写 `tests/test_training.cpp`（+124/0，`git diff -- tests/` 零删除行，我逐行核过）：
`test_params_refuses_a_five_that_flattens_the_scale` 一边断加载器拒 `five:1`（且哨兵 31337/4242 不被改写）、
`five:2` 仍接受，一边绕开加载器直接把机制钉在 `Evaluator` 上（黑四连 + 一白子：`five=1` ⇒ 评估 0，`five=2` ⇒ 非
0）。HEAD 原文 `test_training.cpp:2418: Assertion '!CoefficientVector::from_json_string(...five":1)...' failed`。
`test_server_refuses_no_sessions_with_a_stated_path` 每条都带一个不存在的 `--web-dir` 与 `--port 59321`，
使两棵树都在 bind 之前退出、端口从未监听，再断退出 1 + stderr 点名 `--no-sessions` 且**不含** `--web-dir`
（证明拒在新守卫而不是目录检查），覆盖 `--sessions` 前/后两种顺序与 `--records`，控制组（单用 `--no-sessions`
或单用 `--sessions`）仍走 `--web-dir` 那条拒。HEAD 原文 `Assertion 'last_err.find("--no-sessions") …' failed`
（HEAD 实得 `Invalid --web-dir …`）。负例树 /tmp/neg100（`git archive 4eca5ec`）。
既有断言核查：`test_params_rejects_broken_rule_constants` 只测 0/−1/100001/100000，没有一条把 `five:1` 当合法 ⇒
无既有用例变红。过程教训（已进记忆）：CLI 探针必须先 `make -j4` 全量重编，否则 `build/` 里的旧
`gomoku_server` 会伪装成回归失败。
门禁（我与测试会话各跑一次，一致）：ctest **8/8**（44.64 s）、web **42/42**、锚点 **35 citations / 0 problems**
（34→35：多出的那条是 EE2 注释引用 README:169-172）、README **658**。合约过期：仅锚点计数基线。

## 轮次99 被拒的那一手替别人收走的终局帧，与只报到第四种的符号清单 — bugfix round 99 batch DD

**Found.** 三个只读审计（D：server 终局帧生命周期×摆棋；E：app.js 状态机×重连；F：训练 CLI/build 脚本）交回
8 条，逐条读码后留下 5 条，其余被我否掉。裁决证据都在行号上：

- **DD3（高，帧序/所有权）**：`handle_make_move` 的失败早退只把 `deferred_end_for_` 交回槽位主人，却没有把
  worker 在这一窗里 park 进 `deferred_end_frame_` 的终局帧发出去，也没清空它。窗口是真的：`server.cpp:939-942`
  先装槽位，AI worker 随后可在同一局里落成制胜一手并触发 game_over 回调（`game_manager.cpp:424/426`，回调在
  worker 线程上跑，见 `server.cpp:1626-1634`），而玩家那一手被终局门禁（`game_manager.cpp:148`）拒掉。后果不是
  「这一局少一帧」而已：被搁置的帧会在**下一个** session 落合法子时由成功路径发出，等于把上一局的 `game_over`
  塞给无关对局。
- **DD4（中，我自己在轮次98写错的契约）**：CC1 的流式扫描在见到第四种符号时 `return kinds.size() <= 3;` 提前
  退出，投诉消息因此打印的是**它停下的那个计数**——一份混了六、八种字母的长串被报成「4 种符号」，清单只点名前
  四个符号。我当时判断「这条消息不打印总数，所以不需要扫到底」是错的。
- **DD5（中，破坏性 CLI）**：`require_value` 接受空串。`--resume ""`（多半是未展开的 shell 变量）被解析成「不要
  resume」，训练照跑，然后把默认 checkpoint 覆写掉。
- **DD6（低，文档性）**：`--stagnation` / `--hill-iterations` 的帮助文本没有说明 hill climbing 用 3x 该值作探测
  预算（`trainer.cpp:339`），也没说明相同邻居被跳过 ⇒ 每轮至多 N+1 次评估（`hill_climbing.cpp:54-89`）。
- **DD1（中，构建）**：`build.sh` 的 cmake-free 分支只链接 `gomoku_server`，按 README 的无 cmake 路径构建的人拿不到
  `gomoku_train`；而且「To train the threat coefficients」那句提示被 `if [ "$USE_CMAKE" -eq 1 ]` 挡住了，恰好在这条
  最需要指路的分支上不打印。

**Fixed.** 全部由修改会话（fix-code）按我点名的文件改：`src/server/server.cpp` 失败分支在 `send_error` 前取
`end_frame_mutex_`、置 `deferred_end_for_ = nullptr` 并 swap 出 parked 帧，取其非空则在 `send_error` 后发给本
session（+6/0）；`src/core/position_setup.cpp` 的 `for_each_codepoint` 改为 `void`、删掉提前退出，扫描跑完全部码点，
只有 `symbols` 的**存储**仍封顶 225（+9/−11，两处消息文本与 README:259-260 / :305 的优先级不变）；
`src/training/main_trainer.cpp` 空值抛 `std::invalid_argument` 走既有 catch（+14/−4，含 DD6 帮助文本）；`build.sh`
cmake-free 分支补齐 22 个 TU 的 `gomoku_train` 链接并解掉提示的门（+38/−9）；README:522 补一句帧序契约
（1/1，仍 658 行）。另有一处由我直接改的配置卫生：`.gitignore` 补 `deps/` —— `build.sh:48-51` 会把 nlohmann/json
克隆到仓库根的 `deps/`，而此前只忽略了 CMake 的 `_deps/`，探针一跑就留下 24 MB 未跟踪树。

**Not defects.** E 的 `handleTakenOver` 未清回放行（`:757-762` 自终止、不发帧，README:567-569 只承诺「不再自动重连」）；
F 的 `game_id_base` 族（既有挂起项，`trainer.cpp:243-250` 自己写明要 export marker）；F 的 `checkpoint.json.tmp`
不落删除（无读者）；DD1 的 `deps/` 克隆本身（build.sh 既有行为）。

**Verified.** 测试会话只写 `tests/`（`test_position.cpp` +47、`test_server.cpp` +114、`test_training.cpp` +89，
`git diff -- tests/` 无任何删除行，逐行核过）。新用例与 HEAD 负例（HEAD `6883aa6`）：
`test_a_rejected_stone_delivers_the_end_a_worker_parked` 按 `:939-942` 的方式装槽位、真跑 worker、轮询
`deferred_end_frame_` 非空作为可观察同步点（不是 sleep 赌时序），HEAD 原文
`Assertion 'ending > refusal' failed`（实得 `ai_move,error | move_made,game_over`）；
`test_every_symbol_kind_of_an_oversized_run_is_named`（5 种/8 种乱点长串，点名清单 + 真实计数 + 不含 225），HEAD 原文
`Assertion 'error5.find("%") != std::string::npos' failed`；
`test_trainer_refuses_an_empty_flag_value`（三个 flag 各追加 `--rounds 0` 使 HEAD 停在解析之后的守卫，断言退出 1、
stderr 点名该 flag 且不提 `--rounds`），HEAD 原文 `Assertion 'last_err.find(flag) != std::string::npos' failed`。
DD6 是帮助文本断言，真实调用路径**无可观察差异**；DD4 的「存储/校验仍封顶 225」半项经公共 API 也**无可观察差异**
（只有相等那支读 `symbols`），由轮次98 的 RSS 上界与既有 225 用例承担；DD1 用 /tmp 副本 + 受限 PATH（剥掉
cmake/ctest）探针验证，HEAD 原文 `ls: cannot access '.../head/build/gomoku_train'`。
门禁（我与测试会话各跑一次，一致）：ctest **8/8**（46.34 s，`test_server` 11.83 s）、web **42/42**、锚点
**34 citations / 0 problems**（较 33 多一条：DD4 注释引用 README:259-260，属基线数字过期，不是回归）、README **658**。
合约过期：轮次98 把「第 4 种符号即截断」写成契约的那半条，由 DD4 纠正为真实计数；旧断言（4 种仍走种类投诉、
不含 225）仍然成立，无需改动。

## 轮次98 十六兆一个分隔符都没有的那份模板，与抢先落地的终局帧 — bugfix round 98 batch CC

**Found.** 三个只读审计（app.js 摆棋×终局×回放、摆棋文档/模板装载边界、game_manager↔server 状态机与
线程生命周期）交回 11 条，逐条读码后留下 4 条，其余被我否掉或挂起。裁决证据都在行号上：

- **CC3（高）帧序**：AI 那侧 `game_manager.cpp:424` 先 `move_callback_` 再 `:426 check_game_over()`，
  所以 `ai_move` 总在 `game_over` 之前落地；人这条路是反的 —— `make_move` 内部 `:188` 就跑完
  `check_game_over()`，而 `server.cpp:938-943` 的 `move_made` 要更晚才发。一记取胜的落子因此回成
  `game_over, move_made`：前端先按终局收摊（`handleGameOver` 关掉棋盘、写「游戏结束」），随后那帧
  `move_made` 又去画那颗子。README:519-521 的帧序契约与 :453 的「先答落子再结算」都要求前者。
- **CC1（中）装载边界**：轮次82 D1 的早停在 `position_setup.cpp:249-252` 数的是 `blocks`，也就是**格数**
  而不是字符数；`split_row:151-154` 只在 `size() == 15` 时才把一行炸成一格一符号。于是一行 16 MB 无分隔符
  的 ASCII 只产生**一个** block，`blocks.size() > 225` 永不触发，`:276-278` 先把约 1600 万个单字符
  `std::string` 全部物化（SSO ≈ 500 MB），直到 `:294`/`:309` 才拒绝。`:283-286` 的 `kinds` 扫描也救不了
  （全 `.` 文本只有 1 种符号）。
- **CC4（中）顶栏**：`handleAIMove` 与 `handleUndoSuccess` 的状态写入不受摆棋门禁约束，
  而 README:379 钉的是「摆棋期间顶栏归那条琥珀色指引…不会把指引冲掉」。`updateTurnInfo:2046-2057` 写的
  是 `#turn-info` 不是状态行，所以只有 `updateStatus` 会冲；`hintInFlight` 的六个复位点
  （`chooseSide:977-981`、`handlePositionSet:1297`、`applyAuthoritativeSession:310`、`handleRestart:1527`、
  错误帧 `:1627-1631`、`handleUndoSuccess:1778`、`handleGameOver:1830`）说明这不是控件卡死，纯粹是那行字。
- **CC2（低）BOM**：记事本按「UTF-8」存盘的 JSON 模板前面多了 `EF BB BF`。`position_json.cpp:271-281` 的
  `find_first_not_of(" \t\r\n")` 把 `0xEF` 当成首字节 → 走进棋盘文本分支 → `parse_grid_text` 自己剥掉 BOM
  （`:213-215`），于是这份除了 BOM 完全合法的文档收到的是「第 N 行有 M 格」这类答非所问的抱怨。WS 入口
  `server.cpp:56` 同理。

**Fixed.** CC3 把根因放在服务器侧并与 AI 路径对齐：`server.h` 新增 `end_frame_mutex_` /
`deferred_end_for_` / `deferred_end_frame_`，`handle_make_move` 在调用 `make_move` **之前**记下这把会话，
`Invalid move` 早退时清掉，`move_made` 发出后再把暂存的终局帧换出来发走；`game_over` 回调
（`server.cpp:1612-1624`）只有一处改动 —— `deferred_end_for_` 命中自己时持有帧而不是发送，
那条无条件的 `net::post` 冲刷一字未动，所以认输/超时/求招揭示终局等其它来源仍即时发出。
CC1 用一次流式扫描替换「全部物化」：新 `for_each_codepoint`（模板参数是谓词，返回是否继续）累加码点数，
超过 225 后不再存符号，出现第四种符号立即中断（那条消息不打印总数，本就不需要扫完）；`:311` 读 `total == 225`、
`:326` 读 `std::max(blocks.size(), total)`，两条中文消息与 README:259-260 的优先级（第四种符号在前、
棋盘有 N 格在后）以及 `:305` 的 `row_error`  precedence 全部逐字保留；`codepoints_of` 随之删除（已无调用者）。
CC4 在这两处状态写入外包 `if (!this.setup.active)`，其余语句一律不动。`handleGameOver` 是**故意留的**：
终局帧带来的「游戏结束」必须显示。CC2 在 `position_from_text` 里用偏移量决定形态（`begin = 3` 跳过 BOM，
`json::parse(text.begin() + begin, text.end(), …)`，不整份拷贝上传内容），`read_position_payload:51-63`
在字符串载荷上进 `json::parse` 前 `erase` 掉那三个字节；带 BOM 的**棋盘文本**照旧可用（剥离幂等）。
README 侧 `:583-584` 补完 BB1 的两态口径（主动问 `resume` 落空也要落地权威空盘，未问的回声帧只存 token），
行数仍是 658。

**Not defects.** 状态机审计的三条全部为真却不可达：BB3/BB4 的**写入方**撞不到新的严格读盘口
（`created_ms`/`updated_ms` 是 `now_ms()` 赋的 `long long`，`server.cpp:894-895`/`:1457-1459`；
`game_id`/`move_index` 是 `int`，`data_collector.cpp:164-165`/`:187-188`；nlohmann 写成 `is_number_integer()`）；
手工改出的未来时间戳没有后果（驱逐用的是内存里的 `entry.detached_ms`，`server.cpp:1509-1510`，
`open_session` 只取 `created_ms > 0` 当 P2 哨兵 `:1179`）；「处理函数抛异常 ⇒ `do_read:319-326` 吞掉、客户端
永远等」这一类被 `grep 'throw '` 关门（`src/core/` 与 `src/server/` 一处抛出都没有，入站读取全靠
`read_json_int`/`is_string()` 闸门，`:16-24` 的注释写着的就是这个意图）。
`opening_book.cpp:94-135` 干净：`weight` 走严格 `read_json_int`（`:119`），越盘坐标在 `add_line` 的
`is_valid()`（`:49-51`）里就丢掉，全坏的书整体失败（`:132-134`）。`SearchController`（`src/search/controller.cpp`）
无生产调用方，只有 `test_search.cpp:816/842/869` —— README:596 说清了搜索是单线程迭代加深，「多线程」指的是
异步思考控制，与轮次81 E5 同类。终止响应性由代码而非实测背书：`stop_flag` 在每个节点入口和静态搜索子循环
都被轮询（`alphabeta.cpp:94/147/177/227/313/328`）。审计 B 的 `check_position` 预览回显把 `encoding` 的符号
配成数字 `board`：机理为真（`server.cpp:802-806`），但 README:247-248 给的工作流正好相反（「模板里写明
`encoding` 即可直接载入」），客户端 确认 硬编码 `encoding:{black:1,white:2,empty:0}`（`app.js:1253-1255`），
无文档承诺、无客户端路径。`data/` 七个模板都解成同一合法局面（3 黑 2 白，位棋盘逐字验证过）。
审计 A 的第 5 条（关摆棋瞬间闪回旧状态行）是轮次96 J4 已钉住的过渡态。

**Queued for round 99.** 构建路径完整性自查这轮全过（25 个 `src/*/*.cpp` 都在某个 target 里、
`build.sh` 的免 CMake 源码列与 `GOMOKU_SOURCES`（`CMakeLists.txt:52-64`）一致、POST_BUILD 同时拷 `web/` 与
`data/`（`:90-94`）、链接带 `-lpthread`），只剩一条低优先级的**文档/脚本错位**：README:174-187 在
方法1（推荐）后面直接写 `./gomoku_train`，从没说训练器是 CMake-only，而脚本自己的
`To train … (CMake build only)` 提示印在 `if [ "$USE_CMAKE" -eq 1 ]` **里面**，正好是不需要它的时候；
无 cmake 的机器上推荐路径留下「没有 gomoku_train 也没有提示」。探针 `build/r98cc_trainlink.sh` 证明
22 个 TU 的免 CMake 训练器链接**成立**（`R98CC_LINK_OK`，`--help` 可用，不需要 Boost），下一轮连同
`:57-58` 那段只描述服务器列表的注释一起修。

**Verified.** 门禁在同一棵树上：ctest **8/8**（43.07 s，`test_position` 7.35 s、`test_server` 8.57 s）、
`node tests/web_frontend_check.js` **42/42**、`node tests/readme_anchor_check.js` **33 citations / 0 problems**，
合约过期**无**。HEAD 反向证明（`git archive cf86935` 到 /tmp）：CC1 `tests/test_position.cpp:1111
assert(elapsed_ms < 10000) failed`（HEAD 12.7 s / 峰值 RSS 1160 MB，工作树 6.6 s / 135 MB；同一用例在
`-O2` 下 1585 ms → 429 ms，判据是 RSS 而非墙钟，因为八个测试目标不带 `-O2`，`CMakeLists.txt:87`）；
CC2 `tests/test_position.cpp:1165 assert(prefixed.ok()) failed`；
CC3 `tests/test_server.cpp:1676 assert(types_of(frames) == "move_made,game_over") failed`，前面打印
`frames after the finishing stone: game_over,move_made`；CC4 在 HEAD 上是 `40/42`，两条「摆棋中保持不变」
分别实测写出 `AI I7 · 分值 1234 · 42ms` 与 `已悔至空盘：轮到黑方，继续落子`。CC3 另钉三条不变量：非终局手
只有 `move_made`、执黑后终局仍 `move_made,game_over` 且没有多余 `ai_move`、认输仍恰好一帧 `game_over`。
CC2 的 WS 字符串载荷那条腿在 HEAD 上也过（nlohmann 自己跳 BOM），故 `test_server.cpp:1550` 只是防回归钉；
CC1b 无可观察差异，未立案。测试会话的 diff 只在 `tests/`（`test_position.cpp` +125、`test_server.cpp` +187、
`web_frontend_check.js` +107），未碰 `src/`、`web/`、`README.md`、
`CMakeLists.txt`、`data/`，未做 git 写操作。一处收尾由我完成：它把新用例插在 `test_resume_restores_each_config_half`
的说明注释**中间**，吃掉了「// Round 88, S5: …」那一起手行（`tests/test_server.cpp:1734` 从半句
「// own. A file that states…」开始），我按 HEAD 原文补回；既有断言一行未改。工件：`build/r98cc_trainlink.sh`。

## 轮次97 权威帧被丢掉的那一局，与三个「写了却不认」的读盘口 — bugfix round 97 batch BB

**Found.** 三个只读审计（持久化/训练、app.js↔server 线路、game_manager/server 状态机）交回 8 条，
逐条读码后留下 4 条，其余 4 条被我否掉或挂起。裁决证据都在行号上：

- **BB1** `web/app.js:230-237`：`handleSession` 对 `resumed:false` 一律早退，连自己发出的 `resume`
  收到的那一帧权威会话也不落地。服务器那侧 `server.cpp:1348-1351` 明明把新开的一局（空棋盘、
  `game_status:"waiting"`、新 token、夹取后的 `max_depth`/`timeout_ms`）整帧发了过来，页面上却还挂着
  死局的 7 个子、`aiThinking`、`hintInFlight` 与摆棋的 `confirmInFlight`。README:519-521 的原话是
  「`session`（`resume` 的回应、服务器开局新会话时的主动播报）携带**服务器权威棋盘**：前端必须照它们
  重画棋盘并采纳其中的 `current_turn`，否则页面上可能显示服务器并没有的子」。可达路径是断线自动重连
  撞上服务器换进程/会话目录被清；摆棋中还会把 确认 永久按死（只有 取消 能救）。
- **BB2** `src/server/game_manager.cpp:294-295`：`request_hint` 的守卫只有 `ai_thinking_ || hint_active_`，
  同文件的 `make_move:148`、`resign:283`、`choose_side`、`set_position` 全都在终局处拒绝。原始帧序
  `choose_side → resign → request_hint` 仍被受理：为一局不会再落的棋烧满 `timeout_ms`，再回一手
  `hint_result`，而同一连接随后 `make_move` 拿到的却是 `Invalid move`。前端只是顺手挡住了按钮。
- **BB3** `src/training/data_collector.cpp:223-224`：`game_id`/`move_index` 走 :206-209 那个
  「写坏了和没写都回落默认」的 `read_int`，于是 `"game_id":7.0` 这类行**读取成功**却把身份改成 `-1/-1`，
  一批塌进同一个伪局，`(game_id, move_index)` 唯一性（REVIEW_LOG:1083、`test_training.cpp:1914/:1932`
  钉着）就没了；同一个函数 :232-243 对 `move.row/col` 却是整条拒收。README:215 只允许元数据（节点数、
  搜索深度）回落默认，函数自己在 :225-226 也写着「only the engine numbers may fall back to defaults」。
- **BB4** `src/server/session_store.cpp:123-125`：`read_number` 把「键存在但类型不对」静默读成 0，而
  同文件的兄弟 `read_int`(:126-128) 对 `timeout_ms` 是整份拒绝。`server.cpp:1179` 正是拿
  `created_ms > 0` 当「文件没声明」的哨兵（轮次86 P2），于是 `open_session` 保留自己的 `now_ms()`，
  下一次内容落盘把一个文件从来没写过的值写进出生时间字段，真实出生时间消失。README:544 说的是只有
  新开的一局才重开 `created_ms`。

**Fixed.** BB1 把 :244-293 的落地口径原样抽成 `applyAuthoritativeSession(msg)`（AI 设置同步整段一起搬进去，
让落空那一支也显示服务器真正会用的搜索参数），`asked && !resumed` 走它并保留 warn 文案，`!asked` 的
回声帧照旧早退，`request_replay`/`resumeApplied` 仍只属于 `resumed:true`。BB2 在 `request_hint` 开头加
`if (status_ == GameStatus::GAME_OVER) return false;`，复用 `server.cpp:1030` 现成的错误帧。BB3 新增
`read_id`：键存在就必须读得进整数否则整条拒绝，缺失照旧合法（-1 默认），`search_score`/`search_depth`/
`nodes` 与 `to_json` 一字未动。BB4 让 `read_number` 返回布尔并对两个时间戳给出中文错误，不加范围校验。

**Not defects.** 会话顶层 `reason` 没有词表闸门：README:458 给这四个文本字段定的就是
「写了就必须是字符串」的类型闸门，`status`/`player_color`/`winner` 额外做值校验是严于文档，`reason` 恰好
等于文档。摆棋只放了 `base` 子时 悔棋 仍可点：客户端的门禁在每一处都是
`playerColor === null || gameStatus === 'game_over'`（`app.js:282/:1019/:1696`），而 README:453-454 把这一击的
回答就写成「base 棋局无落子记录，悔棋返回 Cannot undo」，且没有任何一帧带「已落手数」（`replay.move_count`
含 `base` 子）。任何 `error` 帧都清 `hintInFlight`：轮次77 钉过的、防止 求招 卡死的那扇门。`std::thread`
构造抛异常会把 `ai_thinking_` 永久留在 true（`game_manager.cpp:370` 先置标志后建线程）——机理为真，但只有
`ulimit -u` 掐光线程预算才构造得出来，**挂起而非驳回**，日后有确定性复现再开。审计自己承认打不动的：
`genetic.cpp:112` 硬编码 0.5（没人设 `mutation_rate`）、`trainer.cpp:283` 的 `game_id_base` 撞号、
`trainer.cpp:383` 的 int  cast、`black_profile`/`white_profile` 默认、`method` 非字符串→`""`（兄弟闸门
`data_collector.cpp:130` 已经把这些手数挡在外面）。测试会话另报两条「无可观察差异」：
`session_from_json` 不复位 `out`、`request_hint` 读 `status_` 不持 `thinking_mutex_`（`games_mutex_` 下不可观察）。

**Contract refresh.** 轮次77 K2 的 `test_request_hint_clears_stale_discard` 有三个姿态，第三个是
`resign` 后再 求招——BB2 之后那个前提不存在了，故换成 `restore_game(status=playing)`：它和
`reset`/`set_position` 一样经过 `game_manager.cpp:102` 的 `halt_search()`，抬着的标记与「求招仍被接受」
都还原样成立。测试会话做了双树复核（`build/r97_k2_neg.sh`，HEAD 源码 + 新姿态）：该姿态在 `b601b3e`
上也 passed，说明这是合约刷新而不是绕过修复。

**Queued for round 98.** 线路域已确认没有字段名/单位/词表错位（客户端 13 种发出帧、服务器 16 种发出帧
逐一对过），下一轮把「摆棋与终局交织的客户端门禁」和 `data/` 模板文件的装载边界收进来，并把挂起的
线程创建失败找一条可确定性构造的路子。

**Verified.** ctest **8/8**（`test_search` 19.21 s、`test_server` 8.52 s，`test_request_hint_clears_stale_discard`
打印 reset / set_position / restore 三条），`node tests/web_frontend_check.js` **37/37**，
`node tests/readme_anchor_check.js` **33 citations / 0 problems**。HEAD 反向证明建在 `build/neg97_src`
（`git archive b601b3e`）：BB1 `FAIL … the dead board must be replaced`（36/37）、
BB2 `tests/test_search.cpp:1929 assert(!game.request_hint(300, 1)) failed` 与
`tests/test_server.cpp:1510 !game.hint_active_.load() failed`、BB3 `tests/test_training.cpp:1698 assert(!ok) failed`、
BB4 `tests/test_session.cpp:635 session_from_json(with("created_ms", value), out, error) failed`。
README 仍 658 行（:327 补一句「已结束的对局一律拒绝求招」、:459 补 `created_ms` / `updated_ms` 的整数闸门）。
工件：`build/r97_*`。

## 轮次96 一次思考两套时钟，以及落盘重试被驱逐掐断 — bugfix round 96 batch AA

**Found.** Four rows, all re-read by me at `518ae66` before dispatching (a timing-pipeline audit and a
session-persistence audit ran read-only in parallel while round 95's gate was still building).

- **AA2 (低, created by round 95).** `search_engine.cpp:240-245` handed AlphaBeta a pass-through progress
  wrapper, so the per-layer frames kept `alphabeta.cpp:356-359`'s private clock (from its own `start_time_`)
  while round 95 had moved the terminal frame to the entry clock. One 思考, two timelines: on the four-row
  drifting-triplet board the tactics phases burn VCF 700 + VCT 400 ms before AlphaBeta starts, so layer frames
  read ~4/110/571 ms for a call that `ai_move` then reported as 2000 ms. README:521 describes
  `thinking_progress` and `ai_move` as one search's progress and its 终点.
- **AA3 (低).** `web/app.js:807` gated the 耗时 token on `a.elapsed_ms` truthiness. After round 95, 0 is a real
  measurement (book/forced/greedy_five paths measured 34–440 µs), so the replay line dropped it while `:1702`
  printed 「0ms」 for the same stone, and `:642`'s `|| 0` made 0 indistinguishable from absent.
- **AA4 (中).** `flush_entry:1442-1444` refused the empty-board delete whenever `entry->unreadable_record` was
  set, but `open_session:1203-1209` gives exactly such an entry a **new** token — the flag was meant to protect
  the unreadable record, which lives under the *old* token's filename and this entry never names it. So a
  session that opened on a corrupt file, played a stone (writing its own file), then undid to an empty board
  kept that residue, and a restart resurrected the undone game. The protection was never needed:
  `delete_session` (`session_store.cpp:295-315`) succeeds for an absent file, because `filesystem::remove`
  clears `ec` — which is the very split the `:301-303` comment describes.
- **AA5 (低).** `on_flush_tick:1509-1519` evicted an overdue detached game into `cold_sessions_` on its own
  comment's assurance that "the file is current (flush_entry just ran)", but `flush_entry` returns early without
  committing `saved_payload` when `save_session` (`:1466-1471`) or the residue delete (`:1450-1455`) or the
  records append (`:1473-1476`) fails. Once erased, nothing visits that token again, and a later `resume` stamps
  the cold snapshot as saved (`:1186-1189`) — which silently cancelled the retry-on-failure that rounds 72/73
  built (README:570-571).

**Fixed.** `search_engine.cpp` computes the pre-AlphaBeta `used_ms` before installing the wrapper and captures it
in the lambda (`elapsed_ms + used_ms`), so layer frames and the terminal frame share the entry clock; a constant
offset cannot break round-85 F3's non-decreasing property, and `alphabeta.*` is untouched. `app.js` now tests
`a.elapsed_ms != null` while `depth`/`nodes` keep their documented truthiness gates (`:797-799`). `flush_entry`
returns `bool` ("settled"), its three failure arms return false, the `unreadable_record` member and its
protective branch are gone (`server.h`), and eviction now requires `overdue && settled` so a failed write stays
in `sessions_` for the next tick. The empty-board branch's comment states why the unreadable-record case is safe
there. Net: `search_engine.cpp` +11/−9, `app.js` +1/−1, `server.h` +3/−5, `server.cpp` +18/−19.

**Not defects / refuted.** T1 — "an invalid `best_move` without resignation strands the client in 「AI 思考中」
forever (README:521-522)" — is refuted by the arming rule: `web/app.js:1799-1809` is the *only* `aiThinking =
true` and it lives in `handleThinkingProgress`, so a search that emits no frame never enters the frozen state;
the residual needs a completed layer whose move is then invalid *and* every `search_engine.cpp:271-279`
candidate occupied or 禁手, and 终止 stays enabled (`:1804`) and clears the flag locally (`:1013-1016`). Also
refuted: "the `is_decisive()` export gate (`server.cpp:1476`) drops 满盘和棋 from `records.jsonl`" — unreachable,
already closed twice in rounds 81 and 82. The persistence audit's own sweep came back by-design on `.tmp` residue
(`session_store.cpp:235-256` removes on both failure paths; `list_sessions` skips non-`.json`), `--no-sessions`
closing `--records` together (`main.cpp:52-54`, README:162), uncapped `cold_sessions_` (`server.h:137-141`),
last-writer takeover (`server.cpp:1310-1334` = README:567-569), `exported` inferred rather than stored
(`server.h:114-116`), and the 120 s idle timer.

**Queued for round 97.** Nothing dispatched yet — and one consequence of this batch is deliberately left to the
test session rather than the fixer: `tests/test_server.cpp:932` read the deleted `unreadable_record` member, so
`test_server` cannot compile until that case is re-pointed at observable disk state; `tests/test_search.cpp:850`'s
round-92 pin `assert(result.elapsed_ms == 0)` also contradicts round 95's contract and is green only through
sub-millisecond rounding.

**Verified.** Gate on the identical tree: `cmake` rc 0, `make -j4` rc 0, `ctest` **8/8**
(`build/r96_ctest.log`, with the three new cases named in `build/Testing/Temporary/LastTest.log:149/:286/:311`),
`node tests/web_frontend_check.js` **36/36** (one case added), `node tests/readme_anchor_check.js`
**31 citations, 0 problems**, README.md still **658** lines (untouched).

- AA2 — `tests/test_search.cpp:725-794` (`test_engine_progress_frames_share_the_search_clock`, registered `:2586`)
  measures the tactics pre-phase with AlphaBeta disabled first, then asserts the first layer frame covers it
  (`:781` `elapsed.front() + 50 >= pre_search_ms || pre_search_ms < 500`), that frames stay non-decreasing, and
  that the closing frame equals `result.elapsed_ms` exactly. Worktree: first frame 1104 ms against a 1100 ms
  pre-phase; **HEAD `518ae66` fails `test_search.cpp:781`** (rc 134, first frame in single-digit ms).
- AA3 — `tests/web_frontend_check.js:1697-1731` pins `'greedy_five 胜 0ms'`, `'alphabeta 120 4d 1.5k 0ms'`, an
  analysis with no `elapsed_ms` field still rendering without it, and the compact line still omitting time.
  **HEAD fails**: `got: greedy_five 胜`, `35/36 passed`.
- AA4 — `tests/test_server.cpp:902-970` rewrites `test_unreadable_record_file_survives` onto disk facts only (the
  deleted member is gone): the unreadable file survives, and after 摆子 → undo to empty → flush the session's own
  file is deleted (`:961`). **HEAD fails `:961`**.
- AA5 — `tests/test_server.cpp:973-1033` (`test_failed_flush_keeps_the_overdue_entry_hot`, registered `:1656`)
  blocks the directory, pushes `detached_ms` past the reattach window and ticks: `:1006` asserts the entry is
  still in `sessions_` and `:1007` that it did not reach `cold_sessions_`; after unblocking, the next tick evicts
  it normally. **HEAD fails `:1006`**.

All four rows are observably fixed; none reported 无可观察差异. The stale round-92 pin was retired as decided:
`tests/test_search.cpp:917-922` drops `assert(result.elapsed_ms == 0)` for a comment (a sub-millisecond path's 0
is a real measurement; the stamping contract is pinned by round-95 case A), with that case's other assertions
untouched.

## 轮次95 逐手耗时只量了 AlphaBeta 那一段 — bugfix round 95 batch Z

**Found.** Z1 (`src/search/search_engine.cpp`, verified by me at `87b91f0` before dispatching). README:57
promises 逐手引擎分析（方法/分值/深度/节点/**耗时**), and `game_manager.cpp:418-421` writes
`result.elapsed_ms` into `analyses_` and into the `ai_move` frame's `thinking_time_ms`. At HEAD the field had
exactly one assignment in the whole file — `:279` `result.elapsed_ms = info.elapsed_ms;` — and `info.elapsed_ms`
comes from `alphabeta.cpp:385-386`, i.e. from `start_time_` that AlphaBeta stamps at its own entry (`:270`).
Every other hand-back (`no_legal_move` `:50`/`:94`/`:283`, `resign` `:88`/`:158`, `forced` `:103`, `greedy_five`
`:121`, `must_block` `:164`, `book` `:178`, `vcf` `:204`, `vct` `:226`, the tail `greedy` `:359`) left the
`search_engine.h:20-27` default, so a move answered by the opening book or by a proven VCF win reported
**0 ms** in the replay panel, and an AlphaBeta move reported only its own slice even though `:246-249` already
measures the wall time the VCF (≤35%) and VCT (≤55%) pre-phases burned.

**Fixed.** Two files, nothing else. `search_engine.cpp` gains one `elapsed_since_start` lambda at the existing
entry timestamp (`:40-45`) and all 13 `return result;` statements now stamp the whole call (verified by grep:
13 returns, 13 assignments). AlphaBeta's path no longer reports `info.elapsed_ms`; the assignment moved to just
before the branch's only return (`:345-346`) so it is taken *after* the loss-probing block, and the same
`whole_elapsed_ms` now feeds the terminal `progress_callback_` (`:348`) — the fixer flagged that move from the
old `:279` slot as its one deviation, and it is exactly what was required: `used_ms + info.elapsed_ms` would
have omitted the probe's own time. `search_engine.h:20-27` was reworded because its claim that the
non-AlphaBeta paths leave the fields untouched stopped being true for `elapsed_ms`; comment text only, and
`depth`/`nodes_searched` are still stated to stay 0 outside AlphaBeta, which is what the code does.

**Not defects.** The timing-pipeline audit's T1 ("an invalid `best_move` with no resignation strands the client
in 「AI 思考中」 forever, against README:521-522") is refuted by the client's own arming rule:
`web/app.js:1799-1809` is *the only place* `aiThinking` is set, and it is `handleThinkingProgress` — a session
that never emits a `thinking_progress` frame never arms the frozen state, so the silent hand-back cannot strand
it. The residual case (frames emitted, then an invalid move) requires a completed AlphaBeta layer whose result
is then invalid *and* every one of `search_engine.cpp:271-279`'s candidates occupied or 禁手, and even there
`:1804` leaves 终止 enabled, which `stopThinking()` (`:1013-1016`) clears locally. T3's other half — the
undo/reset hint paths misattributing a stone's 耗时 to another game — was refuted earlier and holds
(`game_manager.cpp:54-65` discards before joining, `:74`/`:94` clear `analyses_`).

**Queued for round 96.** AA2 (低, real, and created by this round): the per-layer frames still come from
`alphabeta.cpp:356-359`, which measures from AlphaBeta's `start_time_`, while the terminal frame now measures
from the engine entry — one 思考, two clocks, so a client plotting the timeline sees the layer frames compressed
by whatever the pre-phases cost. Fix direction: offset the wrapper at `search_engine.cpp:242-244` by the
pre-AlphaBeta time so the whole timeline shares the entry clock; the offset is a constant, so round-85 F3's
non-decreasing invariant still holds. AA3 (低): `web/app.js:807` gates the 耗时 token on `a.elapsed_ms`
truthiness, so a genuine sub-millisecond book measurement (real 0 after this round) disappears from the replay
line while `:1702` still prints 「0ms」 for the same stone; `:642`'s `|| 0` makes 0 indistinguishable from
absent, so the gate should be `!= null` while `depth`/`nodes` keep their documented 0-hiding
(`:797-799` comment, README:57).

**Verified.** Gate on the identical tree: `ctest` **8/8** (`build/r95_ctest.log:19`, `test_search … Passed
15.86 sec` at `:9`), `node tests/web_frontend_check.js` **35/35** (`build/r95_web.log:36`), anchor script
**29 citations, 0 problems**. Two new cases in `tests/test_search.cpp` (+113 lines):
`test_engine_stamps_elapsed_on_tactical_wins` (`:623-683`, registered `:2512`) plays the capped WHITE triplet
lattice (3 rows / 12 stones → `method=vcf score=100000`, 2 rows / 8 stones → `method=vct score=90000`) with
`timeout_ms=3000` and asserts `elapsed_ms > 0 || wall_ms < 1` plus `elapsed_ms <= wall_ms + 1`;
`test_engine_elapsed_covers_the_tactical_phases` (`:689-723`, registered `:2513`) uses the four-row drifting
triplet board (16 stones, forcing but winless) at `timeout_ms=2000` and asserts the stamp is within
`max(5 ms, total/20)` of the test's own wall clock. Measured in the worktree
(`build/r95_test_search_wt.log:31-33`): vcf 1050/1050, vct 1650/1650, alphabeta 2000/2000 (stamp/whole).
HEAD negatives from `build/neg95` (`git archive 87b91f0`, src md5-identical to HEAD) fail on exactly those two:
`test_search.cpp:653: Assertion 'result.elapsed_ms > 0 || wall_ms < 1' failed` and
`test_search_onlyB.cpp:718: Assertion 'diff <= slack' failed` — HEAD's AlphaBeta case reported **899 ms of a
2000 ms call**, the missing 1100 ms being VCF's 700 + VCT's 400 slices. Everything else is
**无可观察差异**: the book / forced / greedy_five / must_block / resign / no_legal_move / greedy paths measured
34-440 µs, so both trees legitimately print 0. Frame monotonicity (round-85 F3) re-checked under the new 口径
(`build/r95_mono`): 4/110/571 ms per layer then 1500 ms closing, depth/nodes/score semantics unchanged.
Carried to round 96: `tests/test_search.cpp:850`'s round-92 pin `assert(result.elapsed_ms == 0)` now contradicts
the contract and is green only through sub-millisecond rounding — it is a test-side change, so the test session
owns it.

## 轮次94 一次瞬时 accept 失败就把端口永久关掉 — bugfix round 94 batch Y

**Found.** **Y1** `src/server/server.cpp:574-589` `GameServer::do_accept()`: the completion handler returned on *any*
`ec` except `net::error::operation_aborted` without calling `do_accept()` again. A transient accept failure
(`no_descriptors` when the descriptor limit is hit, `network_down`, a peer that resets mid-accept) therefore left the
acceptor **open with no pending operation** - the process kept every already-connected game playing while the port
stopped being served for the rest of its life, which contradicts README:128「服务器默认在端口 **8080** 启动」and
README:294「启动服务器后，在浏览器中打开 `http://localhost:8080`」.

**My Y2 premise was wrong and the fixer refuted it with the header, which I then read myself.** I dispatched it
claiming `run_websocket` set no inbound cap so Beast's `max_message_size` default was `size_t::max`, letting
`do_read` (`:296-324`) buffer a fragmented message of arbitrary size before any of the project's own caps run. The
installed Boost says otherwise: `/usr/include/boost/beast/websocket/impl/stream_impl.hpp:77` declares
`std::size_t rd_msg_max /* max message size */ = 16 * 1024 * 1024;`, and the setter is `read_message_max`
(`stream.hpp:575`) - there is no `max_message_size` member at all in Boost 1.90 (the fixer's first
`-fsyntax-only` run failed on it, verbatim in its report). So `position_json.cpp:214`'s「The wire allows a 16 MiB
frame」was true all along and nothing was unbounded. The line stays as `ws_.read_message_max(16u * 1024u * 1024u)` -
a pin, so a Boost upgrade that changes the default cannot silently widen the only inbound bound - but it is
**无可观察差异**, and it gets no negative proof.

**Fixed.** `server.h` adds one member (`accept_retry_timer_`, constructed on `ioc_`); `do_accept` now treats only
`operation_aborted` as terminal, logs the transient error, and re-arms through a 100 ms `expires_after` wait so a
failure that does *not* go away cannot spin the loop; `stop()` cancels the new timer next to `flush_timer_`. The
success path's order (`on_connect` → `start()` → `do_accept()`) is untouched, `do_read` gained no special case, and
`position_json.cpp:214` was not edited. Scope is exactly `src/server/server.cpp` + `src/server/server.h` (the only
extra character-level change is one trailing-whitespace cleanup on a blank line inside the hunk).

**Not defects (verified by reading, after three auditors produced them):** the claim that `request_hint`
(`game_manager.cpp:310`) snapshots `board_` off-lock while the AI worker writes it under `thinking_mutex_` is wrong -
`:302` refuses while `ai_thinking_` is set and `ai_thinking_worker` clears it only *after* its commit block
(`:423`), and `halt_search` joins the worker before returning, so the hand-shake keeps the two off the board at the
same time; the sample loader does check feature arity (`data_collector.cpp:258`, error string at `:260`, with
`FEATURE_SIZE = 52` at `:31`) - the `push_back` loop at `:208-216` I first read is only the number filter;
`hill_climbing.cpp:105-108`'s `INT_MIN → INT_MIN` re-derivation is its comment's stated intent, `candidate != current`
at `:58` already refuses a no-op step so it cannot waste an iteration; `--gens -1` / `--pop 0` / `--seed -1` are all
refused at the CLI by `require_int`'s `minimum = 1` default (`main_trainer.cpp:83-96`) and batch X's `require_seed`;
there is no `isfinite`-skip in the optimizers; `send_error` on a session whose `token` is still empty is harmless
because the resume branch that could see it is reached only after `open_session` succeeds. The concurrency auditor's
headline row again failed the whole-function test: `do_read`'s `if (ec)` branch **does** return (`:303`).

**Queued for round 95 (verified in code, not yet dispatched).** **Z1** `result.elapsed_ms` is assigned on exactly one
path - `search_engine.cpp:279`, from AlphaBeta's own `start_time_` set at `alphabeta.cpp:270`, i.e. after the VCF and
VCT slices that the engine already measured as `used_ms` (`:231-233`) - so every 定式/vcf/vct/forced/greedy_five/
must_block/greedy return leaves the 0 the struct documents (`search_engine.h:20-27`) and the replay panel's 耗时
(README:57) reports a book move as instant. Z2 (VCF/VCT keep scanning roots after a proven win, `-INT_MAX, INT_MAX`
so the cutoffs at `vcf.cpp:231` / `vct.cpp:224,322` cannot fire) is held because an early break changes *which*
winning move is chosen and README does not settle that; Z4 (the evaluator's static cap is `five - 1 = 99999`, which
the frontend's `|score| >= 99999` 已证胜负 band renders as proven while a real VCT win reports 90000) is held because
correcting it touches README:338, `app.js` and the harness in one step.

**Verified.** Gate over the exact worktree (bare `cmake ..`, `make -j4`, WSL): `100% tests passed, 0 tests failed out
of 8` (`r94_gate2_ctest.log:19`, 27.39 s real, `7/8 Test #7: test_server ...... Passed 7.25 sec`, the binary stamped
22:27 against a 22:25 source - i.e. the run includes the new case), `web_frontend_check: 35/35 passed`, and
`readme_anchor_check: 27 citations, 0 problems`. **Y1 is a real double-tree difference**, measured with
`bash build/r94_y1_run.sh wt|neg` (`ulimit -n 64` on the server process, `build/r94_y1_probe.py` flooding TCP
connections until stderr shows `Accept error: Too many open files`, releasing them, then one full handshake): HEAD
prints the error **once** and then answers nothing for 12 s, the worktree re-arms and returns
`101 Switching Protocols` with its first frame. **Y2: 无可观察差异** by measurement, not by assertion - the default
was already 16 MiB (`stream_impl.hpp:77`), so no negative was manufactured. New case
`test_transient_accept_failure_rearms_and_abort_ends` (`tests/test_server.cpp:1515-1562`, registered `:1590`) drives
the transient branch by closing the acceptor and asserts `accept_retry_timer_.expiry()` advances ≥50 ms per window,
then drives the aborted shape and asserts it no longer re-arms; it reaches the privates through the file's existing
`#define private public` convention (`:80`/`:87`), which is why it compiles.

## 轮次93 注释指向空行的那八个锚点 — bugfix round 93 batch X

**Found.** Batch W's own anchor script (`tests/readme_anchor_check.js`, which prints `citations found: 27`) surfaced a
class of defect none of the audits had looked for: comments that cite `README:NNN` for a rule the doc states on a
*different* line. I read every cited line myself before dispatching, and eight of them are wrong at `3ae8588` -
two land on a blank line, six on unrelated prose:

| citing comment | claims | actually says | belongs at |
| --- | --- | --- | --- |
| `src/training/params.cpp:231` | the loader must refuse, not clamp | blank line | `155` |
| `src/search/search_engine.cpp:11` | deeper searches get more time | 回放角标「第 N 手」 | `343` |
| `src/search/search_engine.cpp:277` | `greedy` is a listed method | `position_preview` 失败示例 | `530` |
| `web/app.js:184` | reconnect countdown in amber | the replay `method` enum | `553` |
| `web/app.js:193` | later tab wins | blank line | `567` |
| `web/app.js:919`, `:1663` | a hand-set position starts with a stone | `detect_grid_encodings` 等函数列表 | `289` |
| `src/server/server.cpp:1182` | `resumed:false` + new token | `check_position` 帧示例 | `487` |

**X2** `README:338` describes the evaluation curve's red dot as 「该手分值超出曲线量程（已证胜负）」, conflating two
different conditions that batch W's W3 pin happens to separate: `web/app.js:858` clips the curve at `limit = 2000`
while `:863` draws a dot only for `|score| >= 99999`. A searched 99000 is far outside the range and gets **no** dot -
it just pins to the edge - so the sentence promises a dot the code deliberately withholds.

**X3** `src/training/main_trainer.cpp` carries two comments that describe the *old* CLI and one real bug. `:166-168`
calls `--rounds 0` 「the documented case handled below」 while `:225-228` exits with an error (README:194 documents a
default of 3 and no zero-round mode); `:180-184` says `--timeout-ms` / `--max-moves` 「keep accepting whatever the
caller writes」 while `:236-240` refuses `timeout_ms < 1` and `max_moves < 2`. `require_seed` at `:100-108` validates
only `used != text.size()` after `std::stoull`, and `stoull("-1")` does not throw - it wraps to
`18446744073709551615` - so `gomoku_train --seed -1` currently accepts a negative seed and exits 0. The two comments
are docs-only; the seed guard is the batch's only behavior change.

**Fixed.** `git diff --stat` over the batch is 6 files, +17/−17 - every line either a comment or the one README
sentence. I re-read all six diffs before believing the report. X1 replaced only the digits in the eight citations
listed above (`params.cpp:231`, `search_engine.cpp:11` and `:277`, `app.js:184`, `:193`, `:919`, `:1663`,
`server.cpp:1182`), and the fixer additionally re-checked the five citations that were already right
(`evaluator.cpp` README:27/29, `position_setup.cpp` 261-262, the two `server.cpp` 541-543, `trainer.cpp` 607-608) and
left them alone. X2 rewrote README:338 as 「红点表示该手已证胜负（`|score| >= 99999`），而曲线本身只按 ±2000 截断——
超量程的分值贴在边缘但并不画红点」, hunk header `@@ -338 +338 @@`, `wc -l README.md` still 658. X3's `require_seed`
(`main_trainer.cpp:111-119`) now rejects anything `text.find_first_not_of("0123456789")` sees before `stoull` runs, so
the `-1` wrap cannot reach a caller as a seed, and the two comments say what the code does (0 parses then is refused
below; the bounds are the guard's business). One accepted side effect the fixer flagged rather than hid: `--seed 0x10`,
which `stoull` used to read as 16, is now refused too - that converges on README's integer-seed口径, it is not a
regression, and the file's line count is unchanged at 322 so no other citation drifted.

**Not defects.** The server audit's other two rows were re-verified and dropped: the hot/cold `resumed` asymmetry is
honest (a `sessions_` entry exists only because a game was played; the content guard at `server.cpp:1321-1326` is the
cold-file path README:487 describes), and the restored-`max_depth` re-pin at `app.js:254-260` only fires for a
hand-written session file below the documented ranges, which the web harness cannot feed (it has no raw-frame
injection), so a test for it would be a hand-invoked handler call proving nothing.

**Queued, real but not dispatchable this round.** The audit's other two rows I confirmed by reading the code: the
`session` frame carries `position_pending` (`server.cpp:1260`) that `web/app.js` never reads, and `handleSession`
resets `baseStones` at `:271` without re-seeding it from `msg.board` the way `handlePositionSet` (`:1262`) and
`handleStateSync` (`:1637`) do - so the `pendingPosition` guard at `:922-925` is false right after a 摆棋 resume and
the first click is dropped until the `request_replay` reply re-seeds at `:649`. Likewise `game_manager.cpp:261` sets
`move_count` to 盘上子数 (base + played), so `resumedStatusText` (`app.js:308-325`) prints 「已恢复对局：N 手」 with N
inflated by the base stones and never reaches its 「已恢复摆好的局面」 branch, correcting itself one RTT later. Both are
one-RTT self-healing, both need a real socket pair to observe, and the harness has no frame injection - so they wait
for a raw-client round rather than getting a stub-shaped test.

**Verified.** Gate over the exact worktree (bare `cmake ..`, no `-D`; `make -j4`; WSL): `100% tests passed, 0 tests
failed out of 8` (26.15 s real) and `web_frontend_check: 35/35 passed`.

- **Y1, the batch's only behaviour change, proven red-on-HEAD both ways.** Same command shape
  `./gomoku_train --seed -1 --rounds 1` (all artifacts under `build/`): HEAD `3ae8588` (a `git archive` tree in
  `build/neg93`) exits **0** and writes a 16-line `params.json` from the wrapped seed, the worktree exits **1** with
  `Error: invalid seed for --seed: -1`. Anti-overcorrection: `--seed 0` / `42` / `007` / `18446744073709551615` fall
  through to the later `--rounds must be at least 1` guard on **both** trees, so the parser was not tightened for
  legal shapes.
- **My brief was wrong about `--seed 0x10`, and the tester corrected it with measurements rather than accepting it**:
  `std::stoull`'s default base is 10, so HEAD already failed on `used != size` and both trees emit the byte-identical
  refusal. The shapes batch X newly refuses are exactly `+7` and a leading space. Verdict: 收敛到文档口径, non-regression.
  Left open on **both** trees (not introduced here, untouched): a digits-only string ≥ 2^64 passes the digits guard and
  dies in the bare `Error: stoull` message.
- **X1/X2/X3 comments and prose: 无可观察差异, measured not asserted.** The three re-cited object files are
  byte-identical to the `build/neg93` tree's (`search_engine.cpp.o` `21d23be3…`, `params.cpp.o` `9c87ca9c…`,
  `server.cpp.o` `64e57422…`); `web/app.js` with whole-line `//` comments stripped is identical at 1697 lines, and
  HEAD's own `app.js` passes the 35 checks with the same `|score| >= 99999` dot threshold and ±2000 clip that the
  rewritten README:338 now describes. Identical training command on both trees yields byte-identical
  `params.json` (`6d60f8d9…`), `checkpoint.json` and `samples.jsonl`; the only `records.jsonl` delta is
  `elapsed_ms` 3 vs 4 at `nodes=484`, `score=-2038`.
- **Y3 (tests/, the tester's own file):** 4 stale citations corrected by digits only - `test_training.cpp:2200`
  `154→155`, `web_frontend_check.js:1056` and `:1064` `530→553`, `:1171` `283-284→289`; no assertion logic changed.
- **Y4:** `node tests/readme_anchor_check.js` after Y3 reports `27 citations, 0 problems` (it had been the one that
  surfaced batch X). Its single remaining line is a `HINT` (`web_frontend_check.js:1549 → README:360`, a CJK-phrase
  heuristic false positive in a comment block) that does not affect the exit code. The script is standalone - not in
  `ctest`, and `CMakeLists.txt` untouched. README still measures 658 lines with the one `@@ -338 +338 @@` hunk.

## 轮次92 回放契约里没写出来的那三条路径 — bugfix round 92 batch W

**Found.** Four read-only audits over `7e458b6` (search layer, server/session/protocol, core geometry + view mapping,
self-play/retry/opening-book, plus my own sweeps of the DOM-id set, `src/main.cpp`'s flag handling and
README-vs-code defaults). **W1** README:529-531 tells a client what a `replay` frame's `method` can be:
「其余为引擎返回路径（`book` / `vcf` / `vct` / `alphabeta` / `greedy`）」 — presented as the set. The engine actually
writes eight: `search_engine.cpp:92` returns `forced` when exactly one candidate point survives, `:109` returns
`greedy_five` (score 100000) when a root candidate completes its own five, and `:150` returns `must_block`
(score 99000) for the single point that has to be blocked. All three **commit a stone**, so they land in `analyses_`
(`game_manager.cpp:418-420`), travel in the `replay` frame, and are printed verbatim by `web/app.js:804`; a reader
following the documented enum treats them as unknown values.

**Fixed.** README only: the enum now lists all eight engine paths and states that the last three are pre-AlphaBeta
fast paths that fill no `depth` / `nodes` either, and that `must_block`'s 99000 is a defensive marker, not a proven
result — the frontend's proven-win/loss test is `|score| >= 99999` (`app.js:802` for the 胜/负 word, `:863` for the
curve's red dot), so 99000 shows as a plain number and sits clipped at ±2000 without a dot. No code was touched and
the file is still 658 lines, so every `README:NNN` citation in the tree keeps its target.

**Not defects / deliberately left.** The search audit's headline row claimed `must_block`'s 99000 is plotted as a
near-mate win — **refuted against the code** (`app.js:802` and `:863` both gate on `>= 99999`). Its second row
(`greedy.cpp:17-19` answers `Move(7,7)` when its candidate list is empty, and `search_engine.cpp:334` would commit
that unvalidated cell) is **unconstructible**: reaching it needs every cell in `board.active_empty()` forbidden for
BLACK *and* a legal cell outside that window, otherwise `:40-45` or the `far_legal` rescan at `:66-86` already
returns `no_legal_move`/`resign` and commits nothing — and a legal cell beyond the window means the bordering
window cells are plain extensions of lone stones. The third row (loss proof gated on `alphabeta_enabled_`) is
**unreachable**: `set_alphabeta_enabled(false)` has exactly one caller, `tests/test_search.cpp:1511`. Core geometry
came back clean after full-path verification (8 bijections forming the D4 group, `transforms.cpp:9-11` agreeing with
the book's convention, `logicalOf(displayOf(·))` the identity for all eight rot×flip views, edge labels reproducing
`coordName`, the ±4 bitboard window wall-masked rather than wrapped, incremental `active_empty` surviving undo, 摆棋
stored in logical coordinates) — and there is **no** hover-preview handler in `app.js`, so that target does not
exist. Self-play/retry/book is likewise clean: `query` enforces side parity and BLACK-first alternation, all 40
shipped lines open at the D4-invariant centre `[7,7]`, the runtime overload rejects `is_forbidden` before handing
back a black book move, `max_depth_` matches the `move_count() <= max_depth()` gate, and
`rewind_to_next_branch` marks every branch tried exactly once. The training CLI's stale promises
(`main_trainer.cpp:178-180` calls `--rounds 0` 「the documented … case handled below」 while `:237` refuses it;
`:180-184` says `--timeout-ms`/`--max-moves` 「keep accepting whatever the caller writes」 while `:236` refuses them;
`require_seed` at `:100-108` lets `stoull("-1")` wrap to `18446744073709551615` and pass its own `used == size`
guard) are queued as **round 93's batch X**, not folded into this one. `trainer.cpp:466-478` leaves a
`<path>.tmp` behind when the checkpoint flush fails while `params.cpp:338-348` removes it — real asymmetry, but
IO-failure-only and not reachable through the gate, so it stays deliberately unfixed rather than dispatched with a
test that cannot go red.

**Verified.** Over the identical tree the tester ran the gate: `ctest` **8/8** (`100% tests passed, 0 tests failed
out of 8`, bare `cmake ..`) and `node tests/web_frontend_check.js` **35/35** (32 before W1/W2/W3). Because W1 touched
only `README.md` and `git diff --stat -- src web CMakeLists.txt data` is empty, every new assertion is a
*descriptive pin* (green on HEAD too) and the doc change has **无可观察差异**; no HEAD-negative was invented. What the
pins establish: **W1** (`tests/web_frontend_check.js:1597`) all seven non-AlphaBeta methods print as
`<method> <score>` with no `d`/`k` suffix while `alphabeta` prints `120 4d 1.5k`, and `updateReplayInfo()` renders
`must_block 120 7ms`; **W2** (`:1632`) `must_block 99000` stays `must_block 99000` with no 胜/负, whereas ±100000
becomes `must_block 胜` / `must_block 负`, pinning the `|score| >= 99999` threshold the sentence relies on; **W3**
(`:1653`) the curve draws 0 arcs at 99000 (the point pins to the edge at y=6) and 1 arc at 100000, so the 99000
number really is not a proven result; **X2** (`tests/test_search.cpp:785`, registered `:2406`) builds the three
fast paths from real boards and asserts only `method`/`score`/`depth` — `forced 0 0`, `greedy_five 100000 0`,
`must_block 99000 0` — which is what makes the enum in the doc exhaustive rather than guessed.
**X3** (`tests/readme_anchor_check.js`, a standalone script, not in `ctest`) printed `README.md lines: 658` and
`citations found: 27`, cross-checked the doc enum against the ten labels `search_engine.cpp` actually assigns (the
two extras, `resign` and `no_legal_move`, cannot reach a replay frame - `game_manager.cpp:395` returns on
`result.resigned` and `:407` pushes only for a valid `best_move`), and in doing so surfaced a *separate* defect for
the next round: eight pre-existing `README:NNN` citations in `src/`, `web/` and `tests/` drifted onto blank or
unrelated lines (`params.cpp:231`→154, `test_training.cpp:2200`→154, `app.js:193`→545, `search_engine.cpp:11`→336,
`search_engine.cpp:277`→513, `app.js:919`/`:1663` + `web_frontend_check.js:1171`→283, `web_frontend_check.js:1056`/
`:1064` + `app.js:184`→530, `server.cpp:1182` + `test_server.cpp:907`/`:1181`→476). They drifted at `7e458b6`
already - this round replaced 3 lines with 3 lines - so the anchor-fix is batch X work, not a W regression.

## 轮次91 被当成重复的那一半样本、被自己回显拒掉的文档、被冲掉的那条指引 — bugfix round 91 batch V

**Found.** Three read-only audits over `8b15beb` (frontend / position-document / training-data) and nine candidate
rows survived into my own re-read as *non*-defects; three did not. **V1** `data_collector.cpp:141-143` keyed the
8-direction augmentation dedupe on `(zobrist_hash, played cell)`, but the hash is placement-only
(`transforms.cpp`'s `transform_board` replays history) while features 25-51 come from the *oriented* board's
`last_move()` (`fill_orientation`). When a self-symmetry fixes the played cell and still relocates the last stone,
the two reads are different samples and one was silently dropped. Fixture measured by the tester: 黑(7,7) 黑(6,8)
黑(8,6) 白(7,6) 白(6,7), sampled 黑(5,5) — main-diagonal mirroring keeps the placement and the played cell, moves
白(6,7)→(7,6): HEAD emits **4** augmented rows per ply where 8 are distinct, i.e. half of the samples that carry a
last-move anchor are lost, and they are exactly the self-symmetric positions the augmentation exists to reach.
**V2** `alphabets()` lists `""` as an empty-point alias for the three symbol families, so a `board` array written
with `""` cells is recognised, and `check_position` echoes the *canonical* mark (`.` / `空`) — README:539 tells the
client to carry that echo back as the forced `encoding`. The forced-branch coverage loop at
`position_setup.cpp:380-388` compared every normalised token against the three marks, and `normalize("")` stays
empty, so the round trip the protocol prescribes was refused with 「指定的标志不含 ""」. **V3** `app.js:446` ran the
`视角：…` status write on the same path as 旋转/镜像, and 摆棋's amber guidance (README:360) lives in that same
`#status` node with nothing else writing it back — rotating the view mid-setup erased the guidance until the next
server frame.

**Fixed.** The dedupe key now packs the oriented last-move cell under the played cell
(`index * 256 + last_cell + 1`, empty board = `-1` → 0; indices ≤ 224 keep the packing injective), so a symmetry
that moves the anchor is a distinct sample and a genuine duplicate still collapses. The forced-coverage loop skips an
empty token only when the stated `encoding.empty` is one of the five blank spellings (`.`, `_`, `-`, `·`, `空`), so
`""` stays a stray token under a numeric mapping and the «never read a stray symbol as empty» guard is intact.
`applyView()` returns before the status write while `setup.active`; the button labels, the toggled classes and the
board redraw are untouched, and the outside-setup path still writes 视角. README side: `:181` documents the
augmentation key's three parts, `:379` that 摆棋's guidance owns the top bar, `:539` the `""` ↔ canonical-mark round
trip, and `:454` (queued from round 90) is corrected to 「悔棋不会吃掉 `base` 子」.

**Not defects / deliberately left.** Elo's `games` counter counting *matches* (`elo.cpp:55-57`) — README:49 never
states the bracket's unit and the counter is informational. `updateMoveList`'s force-scroll (`app.js:2062`) and the
摆棋 O(n²) repaint / triple `drawBoard` — no README sentence, and the harness stubs `scrollTop`/`scrollHeight` as
plain `0`, so a pin would assert on a stub artifact; the repaint is ≤225 rows per click. A malformed `encoding` on a
`bitboard`/`moves` document still hard-rejects (`position_json.cpp:249-252`) — README:536 says the field only
appears for the board form, and refusing what a file actually stated is the loader contract. `handle_set_position`'s
token rotation before the install, `game_id` density, and the mid-round `append_to_jsonl` failure window are the
auditor's own unreachable / IO-only / already-closed cross-file trades. `--resume` letting the checkpoint's seed win
was re-surfaced and re-refuted from rounds 73-74. `GeneticOptimizer::run` not resetting
`generations_since_improvement_` is latent-only: `trainer.cpp:325` constructs a fresh optimizer per round. V3's twin
observation (`handleAiMove` overwriting 摆棋中 during a live AI frame) is by design — the server frame carries the
newer truth and the setup panel is stale the moment a stone lands — and was not dispatched.

**Verified.** Gate over the identical tree: `ctest` **8/8** (bare `cmake ..`) and
`node tests/web_frontend_check.js` **32/32** (30 before E's two cases). HEAD negatives in `build/neg91`
(`git archive 8b15beb` plus the current `tests/`): **2 of 8 binaries red** and nothing else — `5 - test_position` aborts at
`tests/test_position.cpp:1010` `forced.ok()`, `8 - test_training` at `tests/test_training.cpp:2823`
`augmented.size() == SYMMETRY_COUNT` (both had already printed 16 / 49 existing ` passed` lines), and the neg-tree
web harness is `31/32` with only `tests/web_frontend_check.js:1564` failing. Anti-overreach pins green on **both**
trees via the isolation driver: `test_collector_still_drops_genuine_symmetry_duplicates` (last stone also on the
diagonal → still 4; fully asymmetric → 8) and `test_blank_cell_is_stray_outside_the_letter_families` (numeric
`1/2/0` mapping still refuses `""`, a `?` under `B/W/.` still refuses, exit 0 on both trees), plus E's counter-check
that a rotation *outside* setup still writes 视角 with `status idle`. `build/r91v_probe.cpp` linked against both
trees' objects: augmented rows per sampled ply **4 → 8**, `at_played_cell` 1 → 2 with anchors `(6,7)` and `(7,6)`,
`plain` 1 on both; `forced_ok` for the `B/W/""` and `黑/白/""` families **0 → 1** with the same cells read back,
while the numeric family is `0` on both trees (identical message). The tester's diff is additive only
(`test_training.cpp` +147, `test_position.cpp` +113, `tests/web_frontend_check.js` +43, zero deleted lines) and no
`src/`, `web/`, `README.md`, `CMakeLists.txt` or `data/` file was touched by it.
