#include "alphabeta.h"
#include "../core/transforms.h"
#include <algorithm>
#include <climits>

namespace gomoku {

namespace {

// The half-width of the first aspiration window, and how many passes one layer is
// allowed before it is taken as searched (design doc 2.4.3 ③). Fifty points is the
// gap between two quiet-position evaluations one ply apart, so a correct estimate
// usually needs no re-search at all. Each re-search opens only the end the previous
// pass failed against, so a layer that keeps failing reaches the whole window in
// three passes, and the fourth slot is the fallback that finishes it.
constexpr int kAspirationDelta = 50;
constexpr int kAspirationWidenings = 4;

}

AlphaBetaSearch::AlphaBetaSearch(const Evaluator& evaluator, TranspositionTable& tt)
    : evaluator_(evaluator), tt_(tt), move_gen_(evaluator), node_limit_(2000000),
      clock_(&system_search_clock()) {}

std::vector<Move> AlphaBetaSearch::generate_moves(const Board& board, int color, int ply) {
    // The tiered list (design doc 2.5.1): Tier0/Tier1 in full, Tier2 down to the
    // layer's width, Tier3 only where the position has no threat at all. What the
    // width cannot keep is a candidate this layer never searches, which is the
    // branch-factor cut P1's depth-6 timeout is asking for; the ordering pass below
    // then ranks what survived.
    return move_gen_.generate_moves(board, color, ply, /*in_quiescence=*/false);
}

void AlphaBetaSearch::sort_moves(std::vector<Move>& moves, const Board& board, int color,
                                  const Move& tt_move, int ply) {
    // ① The TT move earns its place by the previous search, so it must stay at
    // index 0 and out of the heuristic re-ordering below. Looking for it
    // anywhere but the front used to leave it unpinned when it already was the
    // first move, which is exactly the common case.
    bool tt_swapped = false;
    if (tt_move.pos.is_valid()) {
        auto it = std::find_if(moves.begin(), moves.end(),
            [&](const Move& m) { return same_cell(m, tt_move.pos); });
        if (it != moves.end()) {
            if (it != moves.begin()) std::iter_swap(moves.begin(), it);
            tt_swapped = true;
        }
    }

    // ② The two moves that cut this same layer before, in slot order. They come
    // straight after the TT move and keep their own rank: a killer is a statement
    // about this layer, and slot 0 is the more recent one, so sorting the pair by
    // any other key would lose that.
    size_t head = tt_swapped ? 1 : 0;
    for (int slot = 0; slot < MoveHeuristics::kKillerSlots; ++slot) {
        const Move& killer = heuristics_.killer(ply, slot);
        if (!killer.pos.is_valid()) continue;
        // A killer already sitting in the pinned seat (the common case: the TT move
        // and the last cut are usually the same move) must not be pulled up beside
        // itself, and one that the width limit cut is simply not a candidate here.
        auto it = std::find_if(moves.begin() + head, moves.end(),
                               [&](const Move& m) { return same_cell(m, killer.pos); });
        if (it == moves.end()) continue;
        if (it != moves.begin() + head) std::iter_swap(moves.begin() + head, it);
        ++head;
    }

    // ③/④ Everything else, on what the cell is worth (the static price) and then on
    // what this search has seen of it (history).
    //
    // evaluate_position_pair runs four pattern recognitions per call; the
    // comparator used to invoke it O(n log n) times per node. Score once up
    // front and sort the pairs instead.
    //
    // Callers pass a move list that generate_moves has already filtered
    // against is_forbidden for BLACK, so calling evaluate_position_both here
    // would rerun is_forbidden (which does a second recognize_position) on
    // every node of every ply. Score both sides directly and skip that
    // redundant work.
    //
    // The price now comes from the generator's cell cache (design doc 2.6 step 2),
    // which is the pass that produced these candidates in the first place: the
    // square's four lines are unchanged between generate() and this loop, so the
    // read is a hit and the number is the one the tier was cut with. It is still
    // asked per candidate rather than taken out of the tier list, because the
    // three stages above this point can pin a square that generate() did not rank,
    // and the fallback has to price whatever it is given.
    int start = static_cast<int>(head);
    int count = static_cast<int>(moves.size()) - start;
    if (count <= 1) return;

    std::vector<std::pair<int, size_t>> scored;
    scored.reserve(count);
    for (int i = 0; i < count; ++i) {
        const Move& m = moves[start + i];
        int s = move_gen_.candidate_price(board, m.pos);
        scored.emplace_back(s, static_cast<size_t>(start + i));
    }
    // The price is the first key and history the second, then the two geometric
    // keys of the A6 triple. That is the reverse of the order design doc 2.4.5
    // lists ③ and ④ in, and it is a measured call rather than a reading: a cell's
    // history says what happened in the position the *previous* node stood in,
    // while evaluate_position_pair is recomputed from the board this node is on, so
    // promoting ③ above ④ makes the search re-open weak cells that the price had
    // already ranked. Same probe, history as the first key: a positional midgame at
    // depth 8 goes 2396300 -> 8895598 nodes for BLACK and 1298311 -> 5941620 for
    // WHITE. With history under the price instead: 221380 against the 230474 the
    // same arm cost before B2 (-3.9 %), and every other arm inside +-3 %. The table
    // is in REVIEW_LOG's B2 section.
    //
    // What the ordering must still be is a function of the board, not of a
    // container. Score alone is not a total order: two candidates of the same value
    // compare equal, and std::sort then leaves their relative order down to the
    // input order - which is active_empty()'s swap-remove order, and that order
    // changes once a board has played and undone stones. The triple the root chooses
    // on is what closes that off below the price; history only ever reorders cells
    // the triple would have had to decide geometrically, and on the first think of a
    // session every history entry is 0, which makes that think exactly the price
    // order the A6 pins already describe.
    // stable_sort is the same belt from the other side, with nothing left to
    // stabilise.
    std::stable_sort(scored.begin(), scored.end(), [&](const std::pair<int, size_t>& a,
                                                        const std::pair<int, size_t>& b) {
        if (a.first != b.first) return a.first > b.first;
        const int ha = heuristics_.history_value(color, moves[a.second].pos);
        const int hb = heuristics_.history_value(color, moves[b.second].pos);
        if (ha != hb) return ha > hb;
        const Position pa = moves[a.second].pos;
        const Position pb = moves[b.second].pos;
        const int da = center_distance(pa);
        const int db = center_distance(pb);
        if (da != db) return da < db;
        return pa.to_index() < pb.to_index();
    });

    std::vector<Move> reordered(moves.size());
    for (size_t i = 0; i < static_cast<size_t>(start); ++i) reordered[i] = moves[i];
    for (size_t i = 0; i < scored.size(); ++i) {
        reordered[start + i] = moves[scored[i].second];
    }
    moves = std::move(reordered);
}

int AlphaBetaSearch::static_score(const Board& board, int color, uint64_t hash,
                                  int32_t cached) {
    int32_t stored = cached;
    if (stored == NO_STATIC_EVAL) stored = tt_.static_eval_of(hash);
    if (stored != NO_STATIC_EVAL) {
        ctx_.count_eval_reuse();
        return color == BLACK ? stored : -stored;
    }
    ctx_.count_eval_scan();
    // Design doc 2.6 step 3: the scan reads the shapes step 2's records already
    // hold, so a node pays for the squares that share a line with the stone just
    // placed and nothing else - and the number is still the one
    // `evaluate_board(board, color)` returns, because both passes go through the
    // same two halves of that function. The counter keeps its old name because it
    // counts the same event: an evaluation this node could not answer from the table.
    const int score = move_gen_.board_score(board, color);
    // The scan is kept, whether or not this position has a record yet - put_static_eval
    // writes onto the record if there is one and otherwise holds the number in a slot
    // of the same bucket that every bound-facing reader of the table calls empty.
    // Keeping it is what makes the reuse happen: of the 513725 evaluations the six C1
    // measurement arms ask for, 195334 (38.0 %) are answered by a bucket walk instead
    // of a scan, and the tree is the same tree it was without any of this.
    if (tt_.put_static_eval(hash, color == BLACK ? score : -score)) {
        ctx_.count_eval_patch();
    }
    return score;
}

NodeResult AlphaBetaSearch::quiescence(Board& board, int alpha, int beta, int color,
                                       std::atomic<bool>& stop_flag, int qdepth,
                                       uint64_t hash, int32_t tt_eval) {
    // Quiescence can recurse for tens of moves on a still-live position. Without
    // a node increment here, the node budget only bounds the alphabeta tree and a
    // timeout or abort flag set at a shallow depth can still be trapped inside
    // an unbounded quiescence subtree.
    ctx_.count_quiescence_node();
    ctx_.note_quiescence_depth(qdepth);
    if (ctx_.hard_stopped()) {
        // A static evaluation of one position: the number the parent shows the
        // player, and nothing the parent may build a bound out of. This is the node
        // the budget dies in, and it is the one step 1 most obviously pays for - the
        // unwind after a cut short-circuits at every node it passes back through.
        return {static_score(board, color, hash, tt_eval), false};
    }

    // Design doc 2.6 step 4's tail bound, armed after the stop test so a dead budget
    // still unwinds through the branch above. The node it caps is a leaf in exactly
    // the sense a depth-0 alphabeta node is one: the number is the position's own
    // static score, and nothing was abandoned mid-calculation, so it is reported as
    // sound. Marking it unsound would be the damage - `sound` gates the bound a node
    // writes to the table, the line a parent extends out of the PV table, and the layer
    // that commits, so one capped ply would cost its whole ancestor chain its
    // bookkeeping.
    if (qdepth >= kMaxQdepth) {
        return {static_score(board, color, hash, tt_eval), true};
    }

    int stand_pat = static_score(board, color, hash, tt_eval);

    // Fail-soft on the stand-pat cutoff: returning `beta` (the window edge)
    // would let the parent store that bound as TTFlag::EXACT. Returning
    // stand_pat is the node's true lower value and stays sound when the
    // parent records it after any alpha/beta adjustment.
    if (stand_pat >= beta) return {stand_pat, true};
    if (stand_pat > alpha) alpha = stand_pat;

    // Design doc 2.5.1's quiescence row: Tier1 in full, and nothing else. The
    // filter and the price come off one four-direction pass (the generator's
    // decorate-sort-undecorate is what keeps this ordering, and so this subtree's
    // cut pattern and the bounds it hands back, a function of the position rather
    // than of the candidate list's order).
    CandidateSet forcing;
    move_gen_.generate(board, color, /*ply=*/0, /*in_quiescence=*/true, &forcing);
    if (forcing.threats.empty()) return {stand_pat, true};

    bool node_sound = true;
    for (const Candidate& entry : forcing.threats) {
        const Move& move = entry.move;
        if (ctx_.hard_stopped()) {
            node_sound = false;
            break;
        }

        board.place_stone(move.pos.row, move.pos.col, color);

        if (board.has_five_in_a_row(move.pos.row, move.pos.col, color)) {
            board.undo_last_move();
            // A completed five is a fact about the position, not an estimate, so
            // it stays sound even when the budget is what ended the search.
            return {100000, true};
        }

        // The child's own layout, read after the stone is on the board: the hash is a
        // field of the board, so this costs nothing the recursion did not already pay
        // for, and it is what lets the child look its evaluation up instead of
        // scanning for it.
        NodeResult child = quiescence(board, -beta, -alpha, opponent(color), stop_flag,
                                      qdepth + 1, board.zobrist_hash(), NO_STATIC_EVAL);
        board.undo_last_move();
        if (!child.sound) node_sound = false;

        int score = -child.score;
        if (score > alpha) {
            alpha = score;
        }
        // Fail-soft on the beta cutoff. Returning `alpha` (which now holds
        // the best score seen) rather than `beta` avoids poisoning the TT
        // with a value that is only a bound.
        if (alpha >= beta) return {alpha, node_sound};
    }

    return {alpha, node_sound};
}

// One child of a tree node, asked in the shape design doc 2.7 calls PVS. `board`
// already carries this node's move, `color` is the side that played it, and `scout`
// says whether this is a later move of the node (the first one has no line to scout
// around, and a node whose window has no bottom yet cannot narrow it: -INT_MAX is
// the nothing-searched value, and one point above it is still below every score the
// position can produce).
//
// The probe's window is the single point above what the node already has, so its
// answer is only ever "this move beats that" or "it does not". Only the first answer
// is worth a second pass, and only when it does not already end the node: a value
// that reaches the top is a bound strong enough to cut with, and a value below the
// bottom is a bound the node's own upper bound is built out of. Both are the same
// bound a full-window search would have handed back.
NodeResult AlphaBetaSearch::search_child_pvs(Board& board, int depth, int alpha, int beta,
                                             int color, int ply, bool scout,
                                             std::atomic<bool>& stop_flag) {
    const int other = opponent(color);
    // A probe is only asked where the one point it raises can cut something. Its
    // whole extra power over the full window is a bottom of alpha+1 for the
    // child's own descendants, and that only pays where the child has descendants
    // deep enough to be cut by it - elsewhere the probe and the re-search it forces
    // cost two passes over a tree the single full-window pass would have finished.
    // `depth` is the depth still to search *here*, so the floor says: probe a child
    // that keeps at least four plies of its own. Measured on this engine's fail-soft
    // windows, and the numbers are nowhere near design doc 2.7's -20~35 % for PVS -
    // REVIEW_LOG's B3 section records why the width cap and the ordering already
    // spend most of what a null window would buy. Probing at every ply left the
    // depth-6 arms bigger than the same think with no probe at all (quiet BLACK
    // 221380 -> 228822, +3.4 %) while the depth-8 arms improved (quiet BLACK
    // 1451817 -> 1415010); this floor keeps the deep gains (quiet BLACK d8
    // 1408083, -3.0 %; pos24 WHITE d8 1261498, -2.8 %; quiet WHITE d6 156428,
    // -2.4 %), leaves five of the nine positional arms identical to the baseline,
    // and costs the worst arm +0.18 %. That last number is why this floor and not a
    // shallower one: `depth <= 3` aggregates 0.2 % better over the nine arms
    // (-1.43 % against -1.23 %, from 9244367 to 9112420 against 9130501) but does so
    // by making quiet BLACK d6 cost 2 %, and a probe that is a net loss on some pose
    // is a probe the think cannot reason about. The table is in REVIEW_LOG's B3
    // section.
    if (!scout || depth <= 4) {
        NodeResult child = alphabeta(board, depth - 1, -beta, -alpha, other, stop_flag,
                                     ply + 1);
        return {-child.score, child.sound};
    }

    NodeResult probe = alphabeta(board, depth - 1, -alpha - 1, -alpha, other, stop_flag,
                                 ply + 1);
    // A probe the budget ran out inside answers nothing, and the node it came from
    // has just been marked unsound. Asking again would spend nodes the think does not
    // have on a subtree that already stopped for want of them.
    if (!probe.sound) return {-probe.score, false};

    const int score = -probe.score;
    // A window one point wide has no integer strictly inside it, so a probe either
    // raises the node's alpha - and then only the re-search can say by how much - or
    // it does not, and the bound stands as the move's value.
    if (score <= alpha || score >= beta) return {score, true};

    NodeResult exact = alphabeta(board, depth - 1, -beta, -alpha, other, stop_flag,
                                 ply + 1);
    return {-exact.score, exact.sound};
}

NodeResult AlphaBetaSearch::alphabeta(Board& board, int depth, int alpha, int beta, int color,
                                      std::atomic<bool>& stop_flag, int ply) {
    // Before anything that can return: a node that leaves no line must leave none,
    // not the line some earlier node at this same ply left.
    pv_.clear(ply);
    ctx_.count_node();
    if (ctx_.hard_stopped()) {
        // Only the last-placed stone can have produced a five; check_winner()
        // would scan all 225 cells and then is_game_over() would scan them
        // again. This abort path fires very frequently during iterative
        // deepening, so the last-move check is important.
        const Move& lm = board.last_move();
        if (lm.pos.is_valid() &&
            board.has_five_in_a_row(lm.pos.row, lm.pos.col, lm.color)) {
            if (lm.color == color) return {100000 + depth, true};
            return {-100000 - depth, true};
        }
        // Step 1's read is not wired here on purpose, and it is this node's own hash
        // that makes the choice: the probe below is what would answer, and moving the
        // probe above the stop test would put a table hit - and a PV slot write -
        // inside the path every aborted node of a cut think takes, which is a change
        // to the frames that think reports. Quiescence's abort branch does reuse,
        // because that node has its hash and its cached score in hand already.
        // What this branch does share, from step 3 on, is the records: the scan
        // itself costs the squares whose lines changed and nothing else, which is
        // what makes it safe to leave in a path every aborted node takes.
        return {move_gen_.board_score(board, color), false};
    }

    uint64_t hash = board.zobrist_hash();
    // Copied out, not pointed into the table: the store below can land in this very
    // bucket and evict the record this node is still reading.
    TTEntry entry;
    Move tt_move;
    // The score this position's record carries, if it carries one (design doc 2.6
    // step 1). Two things read it: the leaf handoff below, which is the same layout
    // and therefore does not need to look it up twice, and the store, which hands it
    // back so that a deeper record of this position replaces the shallower one with
    // its evaluation still on it. A record written by a node that never evaluated the
    // position keeps the number its slot already had either way - store() reads
    // NO_STATIC_EVAL as "this node has nothing to say", not as "forget it".
    int32_t node_eval = NO_STATIC_EVAL;
    bool node_sound = true;
    if (tt_.probe(hash, entry, ply)) {
        ctx_.count_tt_hit();
        node_eval = entry.static_eval;
        tt_move = entry.best_move;
        // A node that answers out of the table has no line of its own in this think's
        // slots: everything below the entry's move belongs to whichever node wrote it.
        // That move is still the one the search that paid for the entry proved, so the
        // parent's variation continues one further instead of stopping here.
        pv_.set_single(ply, tt_move);
        if (entry.depth >= depth) {
            if (entry.flag == TTFlag::EXACT) {
                ctx_.count_tt_cutoff();
                return {entry.value, true};
            }
            if (entry.flag == TTFlag::LOWERBOUND && entry.value >= beta) {
                ctx_.count_tt_cutoff();
                return {entry.value, true};
            }
            if (entry.flag == TTFlag::UPPERBOUND && entry.value <= alpha) {
                ctx_.count_tt_cutoff();
                return {entry.value, true};
            }
        }
    }

    // Terminal check: only the last move can have produced a five.
    const Move& lm = board.last_move();
    if (lm.pos.is_valid() &&
        board.has_five_in_a_row(lm.pos.row, lm.pos.col, lm.color)) {
        if (lm.color == color) return {100000 + depth, true};
        return {-100000 - depth, true};
    }
    if (board.move_count() >= MAX_MOVES) return {0, true};

    if (depth <= 0) {
        return quiescence(board, alpha, beta, color, stop_flag, 0, hash, node_eval);
    }

    auto moves = generate_moves(board, color, ply);
    if (moves.empty()) return {static_score(board, color, hash, node_eval), true};

    sort_moves(moves, board, color, tt_move, ply);

    Move best_move;
    int best_score = -INT_MAX;
    TTFlag flag = TTFlag::UPPERBOUND;

    for (size_t i = 0; i < moves.size(); ++i) {
        const Move& move = moves[i];
        if (ctx_.hard_stopped()) {
            // The scores that made up best_score came out of short-circuit static
            // evaluations, so not even a LOWERBOUND is sound for this node. It is
            // this node's own record, not the search's: a sibling subtree that
            // finished is sound and has already written its own entries.
            node_sound = false;
            break;
        }

        board.place_stone(move.pos.row, move.pos.col, color);

        // PVS (design doc 2.7): the move that ordering put first is searched against
        // the whole window because it is the line everything else is measured
        // against, and each later move is first asked whether it beats what this
        // node already has.
        NodeResult child = search_child_pvs(board, depth, alpha, beta, color, ply,
                                            /*scout=*/i > 0 && alpha != -INT_MAX,
                                            stop_flag);

        board.undo_last_move();
        if (!child.sound) node_sound = false;

        const int score = child.score;
        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
            flag = TTFlag::EXACT;
            // The variation this node now reports is this move followed by whatever
            // the child one ply deeper left in its own slot (design doc 2.4.4). A
            // short-circuited child is not copied: its score was a static evaluation
            // of one position, so the moves under it are not a line the search
            // proved, and a reported line whose every move came from a subtree that
            // finished is worth more than a longer one that mixes the two.
            if (child.sound) pv_.extend(ply, move, ply + 1);
        }
        if (alpha >= beta) {
            flag = TTFlag::LOWERBOUND;
            // The move that ended this layer's job is the one the next layer of this
            // think should try first, and the deeper the subtree it cut was the more
            // strongly it says so (design doc 2.4.5 ②/③). Recorded for this node's
            // ply and the side to move here, which is what makes a killer found at
            // ply 3 of layer 6 the same statement as one found at ply 3 of layer 7.
            if (depth >= 2) heuristics_.record_cutoff(ply, move, color, depth);
            break;
        }
    }

    // A sound node's value is a bound on its subtree even when the search as a
    // whole was cut short, so an aborted search keeps its finished work in the
    // table. What it does not keep is this node's own short-circuited number.
    //
    // Mate scores are stored too (design doc 2.4.2): they used to be excluded, so a
    // proven line was recomputed from scratch on every move that walked back into
    // it. The window is wide enough to hold +-100000 plus the ply correction and
    // narrow enough to keep the -INT_MAX nothing-was-searched value out.
    if (node_sound && best_score > -1000000 && best_score < 1000000) {
        tt_.store(hash, depth, best_score, best_move, flag, ply, node_eval);
    }

    return {best_score, node_sound};
}

SearchInfo AlphaBetaSearch::to_info(const SearchStats& stats) {
    SearchInfo info;
    info.depth = stats.completed_depth;
    info.nodes_searched = static_cast<int>(stats.nodes);
    info.score = stats.score;
    info.best_move = stats.best_move;
    info.principal_variation = stats.pv;
    info.elapsed_ms = stats.elapsed_ms;
    info.partial_depth = stats.partial_depth;
    info.root_moves_done = stats.root_moves_done;
    info.root_moves_total = stats.root_moves_total;
    info.seldepth = stats.seldepth;
    info.partial_best_move = stats.partial_best_move;
    info.stopped = stats.stopped;
    return info;
}

void AlphaBetaSearch::refresh_counters() {
    stats_.nodes = ctx_.nodes();
    stats_.qnodes = ctx_.quiescence_nodes();
    stats_.tt_hits = ctx_.tt_hits();
    stats_.tt_cutoffs = ctx_.tt_cutoffs();
    stats_.seldepth = ctx_.seldepth();
    stats_.elapsed_ms = static_cast<int>(ctx_.elapsed_ms());
}

void AlphaBetaSearch::emit_progress() {
    if (!progress_callback_) return;
    refresh_counters();
    progress_callback_(stats_);
}

Move AlphaBetaSearch::search(const Board& board, int color, int max_depth,
                             int time_limit_ms, std::atomic<bool>& stop_flag) {
    // The legacy shape: this phase's share of the budget as a hard limit and the
    // node ceiling from set_node_limit(). An absolute deadline set through
    // set_deadline() overrides the share below, exactly as it did when is_timeout()
    // knew only about the share.
    SearchLimits limits;
    limits.max_depth = max_depth;
    limits.hard_time_ms = time_limit_ms;
    limits.max_nodes = node_limit_;
    return search(board, color, limits, stop_flag);
}

Move AlphaBetaSearch::search(const Board& board, int color, const SearchLimits& limits,
                             std::atomic<bool>& stop_flag) {
    // Reset the reported accounting each search. An aborted or early-return
    // search must not hand the caller the previous search's depth/score.
    stats_ = SearchStats{};
    info_ = SearchInfo{};

    ctx_.start(limits, *clock_, stop_flag);
    if (has_deadline_) ctx_.set_absolute_deadline(deadline_ms_);
    // Killers are numbered by how far a node sits from the root of *this* think, so
    // the previous think's slots describe positions this one will not visit; they
    // start empty. History is keyed by cell rather than by ply, which is exactly why
    // it survives a think - and why it ages by half instead of being kept whole or
    // thrown away.
    heuristics_.start_think();
    // The line slots are keyed by the same ply the killers are, so they are per-think
    // too: a slot left over from the previous hand describes a position this hand
    // will not stand in, and a parent that copied it down would report a variation
    // nothing in this think searched.
    pv_.start_think();

    // No generation step here, on purpose. Ageing the table at the start of a think
    // is what let that think's own shallow records evict the deep ones the previous
    // move had paid for; the owners that commit a stone to a board advance the clock
    // (GameManager, SelfPlay).
    Move best_move;
    int best_score = 0;

    auto root_moves = generate_moves(board, color, /*ply=*/0);
    if (root_moves.empty()) {
        // An invalid move says "nothing to play". The centre square may already
        // be occupied, or be a forbidden point for BLACK.
        return Move();
    }

    if (root_moves.size() == 1) {
        return root_moves[0];
    }

    stats_.root_moves_total = static_cast<int>(root_moves.size());

    // Symmetric positions are searched once per mirror otherwise. When the
    // board maps onto itself under a D4 variant, every candidate has twins that
    // score alike, branch alike and win alike, so keep one cell per orbit.
    // is_symmetry_representative keeps the lowest-index member, which after A6 is
    // also the member the triple would pick: a self-symmetry of the board is a
    // row flip, a column flip or a transpose, and all three preserve
    // |row-7| + |col-7|, so every member of an orbit ties on the centre key and
    // the index key decides. Positions with no self-symmetry - almost every
    // midgame - are untouched, which is what keeps the chosen move identical to
    // what the undeduplicated root would have played.
    const std::vector<int> symmetries = self_symmetries(board);
    if (symmetries.size() > 1) {
        root_moves.erase(
            std::remove_if(root_moves.begin(), root_moves.end(),
                           [&](const Move& m) {
                               return !is_symmetry_representative(symmetries, m.pos);
                           }),
            root_moves.end());
        stats_.root_moves_total = static_cast<int>(root_moves.size());
    }

    sort_moves(root_moves, board, color, limits.hint_move);

    // The value the next layer is expected near (design doc 2.4.3 ③): the score the
    // previous think of this session handed back, and from the first layer that
    // finishes onward, this think's own last layer. A wrong estimate is not a
    // wrong move - it costs a widened re-search - so the caller can hand in a
    // number that describes a board two plies back.
    int aspiration_centre = limits.hint_score;
    bool have_centre = limits.has_hint_score;

    // Copied once for the whole think. Every candidate list this board hands back
    // is now ordered by a total triple, so the order `active_empty()` happens to be
    // in - which place/undo cycles shuffle, because undo restores by swap-remove -
    // reaches no decision any more. Re-copying per layer bought a board whose
    // candidate order was pristine for each depth, and that was the reason it
    // existed, not the stone layout: place_stone/undo_last_move leave the stones,
    // the hash, the neighbour counts and the move stack exactly as they were.
    Board work_board = board;

    for (int depth = 1; depth <= limits.max_depth; ++depth) {
        if (ctx_.layer_boundary_stopped()) {
            // The budget was already gone before this layer started: the layer
            // below is the last complete record, and there is nothing partial to
            // report. root_moves_done keeps the previous layer's full count, which
            // is what says "no layer was interrupted" rather than "layer 1 broke
            // after its first candidate".
            stats_.stopped = ctx_.reason();
            refresh_counters();
            break;
        }
        // The soft window is a decision about starting a layer, never a reason to
        // discard a finished one (design doc 2.3.2).
        if (stats_.completed_depth >= 1 && ctx_.soft_time_up()) {
            stats_.stopped = StopReason::TIME_SOFT;
            break;
        }

        // A deeper iteration re-asks the same plies with more tree below them, so
        // the layer before it is the evidence that still applies; ageing here is what
        // keeps a cutoff found at depth 2 from ordering depth 6.
        heuristics_.start_layer();

        Move layer_move;
        NodeResult layer = search_root_with_aspiration(work_board, root_moves, depth,
                                                      color, layer_move, have_centre,
                                                      aspiration_centre, stop_flag);
        const bool layer_complete =
            stats_.root_moves_done == static_cast<int>(root_moves.size());

        if (layer_complete && layer.sound) {
            best_move = layer_move;
            best_score = layer.score;
            stats_.completed_depth = depth;
            stats_.score = best_score;
            stats_.best_move = best_move;
            // The variation that made this layer answer, and only a committed layer
            // gets to say one: a cut pass has moves in its slots whose values came
            // out of short-circuited subtrees, and a line the player can read is
            // worth more than a long one the search did not finish.
            pv_.copy_out(0, &stats_.pv);
            // A layer that finished replaces any earlier cut-short record.
            stats_.partial_depth = 0;
            stats_.partial_best_move = Move();
            // Iterative deepening says this layer's value sits near the one it
            // replaced, so the layer after it is centred here rather than on the
            // number the session handed in.
            aspiration_centre = best_score;
            have_centre = true;
            emit_progress();

            // Feed the shallow iteration's winner back into the root order so
            // deeper iterations inherit the cutoff / TT-move effect that makes
            // iterative deepening worth the extra work.
            if (layer_move.pos.is_valid()) {
                auto it = std::find_if(root_moves.begin(), root_moves.end(),
                    [&](const Move& m) {
                        return m.pos.row == layer_move.pos.row &&
                               m.pos.col == layer_move.pos.col;
                    });
                if (it != root_moves.begin() && it != root_moves.end()) {
                    std::iter_swap(root_moves.begin(), it);
                }
            }

            if (best_score >= 100000 || best_score <= -100000) {
                // A proven result is a deliberate end, not a budget running out.
                stats_.stopped = StopReason::MATE;
                break;
            }
        } else {
            // The interrupted layer is recorded and reported, and not submitted:
            // its scores came partly out of short-circuited subtrees. The caller
            // still plays the last completed layer's move, so a cut think cannot
            // silently become a weaker one.
            stats_.partial_depth = depth;
            stats_.partial_best_move = layer_move;
            stats_.stopped = ctx_.reason();
            emit_progress();
            break;
        }
    }

    // A think that completed no layer must say why. Reaching this with no reason
    // means the layer loop never ran (a request for zero depth) rather than that
    // time or nodes ran out.
    if (stats_.completed_depth == 0 && stats_.stopped == StopReason::NONE &&
        limits.max_depth >= 1) {
        stats_.stopped = ctx_.reason();
        if (stats_.stopped == StopReason::NONE) stats_.stopped = StopReason::USER;
    }

    if (!best_move.pos.is_valid() && !root_moves.empty()) {
        // A think that completed no layer still has to answer with a cell, and
        // index 0 is now a principled first: sort_moves ordered the root by the
        // same triple this would pick (value, centre, index) and only a hand-in
        // hint can sit ahead of it.
        best_move = root_moves[0];
    }

    refresh_counters();
    info_ = to_info(stats_);
    return best_move;
}

NodeResult AlphaBetaSearch::search_root_with_aspiration(
    Board& board, const std::vector<Move>& root_moves, int depth, int color,
    Move& best_move, bool use_aspiration, int centre, std::atomic<bool>& stop_flag) {
    if (use_aspiration) {
        int lo = centre - kAspirationDelta;
        int hi = centre + kAspirationDelta;
        for (int attempt = 0; attempt < kAspirationWidenings; ++attempt) {
            NodeResult layer = search_root_layer(board, root_moves, depth, color, best_move,
                                                 lo, hi, stop_flag);
            // A pass the budget ran out inside is reported as it stands: widening
            // its window would re-search the same candidates and stop on the same
            // node. This is the one test that separates that from a fail-high,
            // which also ends the pass early but leaves the reason unset.
            if (!layer.sound || ctx_.reason() != StopReason::NONE) return layer;
            // A value strictly inside the window is this layer's exact best: a
            // candidate only becomes the layer's best when its own value was
            // inside the window it was given, and one that reaches the top ends the
            // pass as a fail-high instead.
            if (layer.score > lo && layer.score < hi) return layer;
            // Only the bound the pass failed against opens up (design doc 2.4.3 ③'s
            // fail-low / fail-high relaxation). The other end is a bound this pass
            // proved, so keeping it is what makes the re-search cheaper than running
            // the whole window again - and a fail-high pass stops at the first
            // candidate that reaches its top, so a wrong estimate costs a cut, not a
            // layer.
            if (layer.score <= lo) lo = -INT_MAX;
            else hi = INT_MAX;
        }
    }
    // The estimate missed often enough that no window held the value. The last
    // pass is the one an un-aspirated think would have run, so a wrong hint can
    // cost work but never a different move.
    return search_root_layer(board, root_moves, depth, color, best_move,
                            -INT_MAX, INT_MAX, stop_flag);
}

NodeResult AlphaBetaSearch::search_root_layer(Board& board,
                                              const std::vector<Move>& root_moves, int depth,
                                              int color, Move& best_move,
                                              int alpha, int beta,
                                              std::atomic<bool>& stop_flag) {
    int layer_alpha = alpha;
    int layer_best_score = -INT_MAX;
    Move layer_best_move;
    bool layer_sound = true;
    stats_.root_moves_done = 0;

    for (const auto& move : root_moves) {
        if (ctx_.hard_stopped()) break;

        board.place_stone(move.pos.row, move.pos.col, color);

        // A root child sits one ply from the root, whatever the layer's depth is.
        // Full window, not the one-point question the tree asks (design doc 2.7's
        // PVS): the tie test below needs a candidate's value to say "level with the
        // best", and a null-window answer cannot tell an exact tie from a loss - it
        // would have to be re-asked for every candidate to keep A6's rule, which is
        // the work the probe was meant to avoid.
        NodeResult child = alphabeta(board, depth - 1, -beta, -layer_alpha, opponent(color),
                                     stop_flag, 1);

        board.undo_last_move();

        if (!child.sound) layer_sound = false;
        int score = -child.score;
        // Which pass this candidate's number came out of is what its line has to
        // match: the tie probe below replaces the value, so the variation the root
        // reports has to be the one that produced the replacement.
        bool line_valid = child.sound;

        // Every candidate after the first is searched against a window whose bottom
        // is already the best's value, so one that comes back level with it has
        // proved only "not worse than this score reads as" - an exact tie and a
        // loss look identical. Where the tie decides nothing, that is fine; where it
        // decides the move, it is not. A rival that loses the centre-and-index
        // comparison needs no proof either way: its value is bounded by the best's
        // exact one, so it cannot overtake it, and equal-or-worse with a worse centre
        // loses on both readings. A rival that would win the tie on being nearer the
        // centre has to be asked again before the centre is allowed to choose.
        //
        // Asking again means a window whose bottom is one point *below* the tie, and
        // nothing wider: with the bottom at value-1, a subtree that truly is worth
        // the tie value no longer fails low, so it returns at least it, while a
        // strictly worse one returns at most value-1. That single point is what makes
        // "tied" and "worse" come out as different numbers, and it is why the proof
        // costs about what the failed pass already cost - a whole-window re-search
        // measured 954038 nodes for the same quiet depth-4 think that a plain pass
        // spends 218872 on, because it re-opens every subtree the cut had closed.
        if (child.sound && layer_best_move.pos.is_valid() && score == layer_best_score &&
            RootScore(move, score) > RootScore(layer_best_move, layer_best_score)) {
            const int tie_probe = layer_best_score - 1;
            board.place_stone(move.pos.row, move.pos.col, color);
            NodeResult exact = alphabeta(board, depth - 1, -beta, -tie_probe, opponent(color),
                                          stop_flag, 1);
            board.undo_last_move();
            // A re-search the budget ran out inside answers nothing, and this layer
            // is now a partial record: the caller reports it and plays the last
            // completed layer's move.
            if (!exact.sound) layer_sound = false;
            score = -exact.score;
            line_valid = exact.sound;
        }

        if (!layer_best_move.pos.is_valid() ||
            RootScore(move, score) > RootScore(layer_best_move, layer_best_score)) {
            layer_best_score = score;
            layer_best_move = move;
            // The root's own line: this candidate followed by the ply-1 slot, which
            // is the subtree that just produced this candidate's value (every node
            // clears its slot on entry, so nothing older can be in it). An
            // unanswered candidate leaves no line, and the slot is emptied rather
            // than left holding the previous best's.
            if (line_valid) pv_.extend(0, move, 1);
            else pv_.clear(0);
        }
        if (score > layer_alpha) layer_alpha = score;
        // What the interrupted layer would have played, kept as it is built: the
        // player sees it, the returned move never comes from it.
        stats_.partial_best_move = layer_best_move;
        // Only a candidate whose own subtree ran to the end counts as searched. A
        // short circuit sets the stop reason for the whole rest of the think, so the
        // cut candidate is always the last one attempted and the count is exactly
        // "how far into the layer the interrupt landed" - which is what the client
        // shows as 第 N 层（12/34）.
        if (child.sound) ++stats_.root_moves_done;
        // The window's top is reached: every candidate after this one is searched
        // against a window it cannot raise, so the pass ends here and the caller
        // decides between widening the window and reporting the layer. With the
        // whole window this test can never be true, which is why an un-aspirated
        // think searches every candidate as it always did.
        if (layer_alpha >= beta) break;
    }

    best_move = layer_best_move;
    return {layer_best_score, layer_sound};
}

}
