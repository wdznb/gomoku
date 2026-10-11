#include "pattern.h"
#include <algorithm>

namespace gomoku {

bool PatternRecognizer::initialized_ = false;
std::array<uint8_t, 1 << 18> PatternRecognizer::pattern_lut_;

namespace {

constexpr uint32_t kWindowMask = 0x1FF;
constexpr int kCenterBit = 4;
constexpr size_t kDescCount = BOARD_SIZE * BOARD_SIZE * 4;

// Where a square's 9-cell window lives inside the bitboard line words, plus
// which of its cells fall off the board. Shift and wall only depend on the
// square and the direction, so they are resolved once instead of per scan.
struct WindowDesc {
    uint8_t line;
    uint8_t shift;
    uint16_t wall;
};

const std::array<WindowDesc, kDescCount>& window_descs() {
    static const std::array<WindowDesc, kDescCount> table = [] {
        std::array<WindowDesc, kDescCount> t{};
        for (int row = 0; row < BOARD_SIZE; ++row) {
            for (int col = 0; col < BOARD_SIZE; ++col) {
                for (int dir = 0; dir < 4; ++dir) {
                    uint16_t wall = 0;
                    for (int i = -4; i <= 4; ++i) {
                        const int r = row + i * DIRECTIONS[dir][0];
                        const int c = col + i * DIRECTIONS[dir][1];
                        if (r < 0 || r >= BOARD_SIZE || c < 0 || c >= BOARD_SIZE) {
                            wall |= static_cast<uint16_t>(1u << (i + 4));
                        }
                    }
                    t[(row * BOARD_SIZE + col) * 4 + dir] = WindowDesc{
                        static_cast<uint8_t>(line_index(dir, row, col)),
                        static_cast<uint8_t>(line_position(dir, row, col) + LINE_MARGIN - 4),
                        wall};
                }
            }
        }
        return t;
    }();
    return table;
}

}

PositionPattern::PositionPattern() {
    four_count = 0;
    three_count = 0;
    live_four_count = 0;
    rush_four_count = 0;
    live_three_count = 0;
    has_four_four = false;
    has_four_three = false;
    has_three_three = false;
    has_live_four = false;
    has_rush_four = false;
    has_live_three = false;
}

void PositionPattern::analyze() {
    four_count = 0;
    three_count = 0;
    live_four_count = 0;
    rush_four_count = 0;
    live_three_count = 0;
    
    for (int i = 0; i < 4; ++i) {
        PatternType t = directions[i].type;
        
        if (t == PatternType::LIVE_FOUR) {
            live_four_count++;
            four_count++;
        } else if (t == PatternType::RUSH_FOUR) {
            rush_four_count++;
            four_count++;
        } else if (t == PatternType::LIVE_THREE) {
            live_three_count++;
            three_count++;
        } else if (t == PatternType::SLEEPY_THREE) {
            three_count++;
        }
    }
    
    has_live_four = (live_four_count > 0);
    has_rush_four = (rush_four_count > 0);
    has_live_three = (live_three_count > 0);
    has_four_four = (four_count >= 2);
    has_four_three = (four_count >= 1 && live_three_count >= 1);
    has_three_three = (live_three_count >= 2);
}

void PatternRecognizer::init() {
    if (initialized_) return;
    // Touch both tables here. They are function-local statics, so they build
    // themselves on first use even if a caller forgets to init(); warming them
    // at startup just keeps the first searched move out of that cost.
    (void)pattern_lut();
    (void)window_descs();
    initialized_ = true;
}

bool PatternRecognizer::fill_pattern_lut(std::array<uint8_t, 1 << 18>& lut) {
    for (size_t key = 0; key < lut.size(); ++key) {
        const uint32_t own = static_cast<uint32_t>(key >> 9);
        const uint32_t blocked = static_cast<uint32_t>(key & kWindowMask);

        int window_code = 0;
        for (int i = 0; i < 9; ++i) {
            const uint32_t bit = 1u << i;
            const int cell_code = (own & bit) ? 1 : ((blocked & bit) ? 2 : 0);
            window_code = window_code * 3 + cell_code;
        }

        lut[key] = static_cast<uint8_t>(match_pattern(window_code, true));
    }
    return true;
}

const std::array<uint8_t, 1 << 18>& PatternRecognizer::pattern_lut() {
    static const bool built = fill_pattern_lut(pattern_lut_);
    (void)built;
    return pattern_lut_;
}

int PatternRecognizer::extract_window(const Board& board, int row, int col, 
                                       int color, int dir) {
    int window = 0;
    int dr = DIRECTIONS[dir][0];
    int dc = DIRECTIONS[dir][1];
    
    for (int i = -4; i <= 4; ++i) {
        int r = row + i * dr;
        int c = col + i * dc;
        
        int cell_code;
        if (i == 0) {
            cell_code = 1;
        } else if (!board.is_valid(r, c)) {
            cell_code = 2;
        } else {
            int cell = board.get(r, c);
            if (cell == EMPTY) {
                cell_code = 0;
            } else if (cell == color) {
                cell_code = 1;
            } else {
                cell_code = 2;
            }
        }
        
        window = window * 3 + cell_code;
    }
    
    return window;
}

namespace {

// The run through the placed stone, with no hole inside it. `total` is its
// stone count (the centre included), already known to be below five.
PatternType match_contiguous_run(const int* cells, int count_left, int count_right, int total) {
    if (total == 4) {
        int left_end = (4 - count_left - 1 >= 0) ? cells[4 - count_left - 1] : 2;
        int right_end = (4 + count_right + 1 <= 8) ? cells[4 + count_right + 1] : 2;

        if (left_end == 0 && right_end == 0) {
            return PatternType::LIVE_FOUR;
        } else if (left_end == 0 || right_end == 0) {
            return PatternType::RUSH_FOUR;
        } else {
            return PatternType::NONE;
        }
    }

    if (total == 3) {
        int left_pos = 4 - count_left - 1;
        int right_pos = 4 + count_right + 1;

        bool has_left_space = (left_pos >= 0 && cells[left_pos] == 0);
        bool has_right_space = (right_pos <= 8 && cells[right_pos] == 0);

        if (has_left_space && has_right_space) {
            bool left_open = (left_pos - 1 >= 0 && cells[left_pos - 1] == 0);
            bool right_open = (right_pos + 1 <= 8 && cells[right_pos + 1] == 0);

            if (left_open && right_open) {
                return PatternType::LIVE_THREE;
            } else {
                return PatternType::SLEEPY_THREE;
            }
        } else {
            return PatternType::SLEEPY_THREE;
        }
    }

    if (total == 2) {
        int left_pos = 4 - count_left - 1;
        int right_pos = 4 + count_right + 1;

        bool left_open = (left_pos >= 0 && cells[left_pos] == 0 &&
                          left_pos - 1 >= 0 && cells[left_pos - 1] == 0);
        bool right_open = (right_pos <= 8 && cells[right_pos] == 0 &&
                           right_pos + 1 <= 8 && cells[right_pos + 1] == 0);

        if (left_open && right_open) {
            return PatternType::LIVE_TWO;
        } else if (left_open || right_open) {
            return PatternType::SLEEPY_TWO;
        } else {
            return PatternType::NONE;
        }
    }

    return PatternType::NONE;
}

// One empty cell sitting directly against the run, with own stones beyond it:
// XXX_X, XX_XX, X_XXX (four stones around a hole) and XX_X, X_XX (three).
// Filling the hole makes a contiguous run of `stones + 1`, so a four-stone gap
// shape is a rush four - the hole is its only five point, which is exactly what
// the contiguous scan above misses - and a three-stone one is a broken three,
// live only when the hole fill would yield a live four.
//
// Shapes holding five or more stones are deliberately left to the contiguous
// label: X_XXXX is a four (two completion points, one of them checked by
// finish_direction), not a hole story, and relabelling it would hide that.
PatternType match_gapped_run(const int* cells, int count_left, int count_right, int total) {
    PatternType best = PatternType::NONE;

    for (int side = 0; side < 2; ++side) {
        const bool low = side == 0;
        const int outer = low ? 4 - count_left : 4 + count_right;

        // One empty cell, then the group beyond it. `stop` ends on the first
        // cell outside that group, so it is also the far end cell of the shape.
        const int hole = low ? outer - 1 : outer + 1;
        if (hole < 0 || hole > 8 || cells[hole] != 0) continue;

        int stop = low ? hole - 1 : hole + 1;
        int extra = 0;
        while (stop >= 0 && stop <= 8 && cells[stop] == 1) {
            ++extra;
            stop += low ? -1 : 1;
        }
        if (extra == 0) continue;

        const int stones = total + extra;
        if (stones == 4) {
            best = std::max(best, PatternType::RUSH_FOUR);
        } else if (stones == 3) {
            // The two cells outside the whole shape once the hole is filled:
            // `stop` is already the far end on the hole side, and the run's own
            // end cell is one step beyond its outermost stone.
            const int low_end = low ? stop : 4 - count_left - 1;
            const int high_end = low ? 4 + count_right + 1 : stop;
            const bool lo_open = low_end >= 0 && low_end <= 8 && cells[low_end] == 0;
            const bool hi_open = high_end >= 0 && high_end <= 8 && cells[high_end] == 0;
            best = std::max(best, (lo_open && hi_open) ? PatternType::LIVE_THREE
                                                       : PatternType::SLEEPY_THREE);
        }
    }

    return best;
}

}

PatternType PatternRecognizer::match_pattern(int window_code, bool /*is_center*/) {
    int cells[9];
    int temp = window_code;
    for (int i = 8; i >= 0; --i) {
        cells[i] = temp % 3;
        temp /= 3;
    }
    
    int center = cells[4];
    if (center != 1) {
        return PatternType::NONE;
    }
    
    int count_left = 0;
    for (int i = 3; i >= 0; --i) {
        if (cells[i] == 1) count_left++;
        else break;
    }
    
    int count_right = 0;
    for (int i = 5; i <= 8; ++i) {
        if (cells[i] == 1) count_right++;
        else break;
    }
    
    int total = count_left + 1 + count_right;
    
    if (total >= 6) {
        return PatternType::LONG_FIVE;
    }
    
    if (total == 5) {
        return PatternType::FIVE;
    }

    // The enum is ordered by threat strength, so a hole that outranks the run
    // through the centre (XX_XX is a four, not a two) wins and a weaker one
    // never overwrites it.
    return std::max(match_contiguous_run(cells, count_left, count_right, total),
                    match_gapped_run(cells, count_left, count_right, total));
}

DirectionPattern PatternRecognizer::recognize_direction(const Board& board, int row, int col,
                                                         int color, int dir) {
    if (row < 0 || row >= BOARD_SIZE || col < 0 || col >= BOARD_SIZE) {
        return DirectionPattern(PatternType::NONE, dir);
    }

    // The bitboard lines are indexed by colour and direction family, so a stray
    // value here would read outside them instead of just returning no pattern.
    if ((color != BLACK && color != WHITE) || dir < 0 || dir >= 4) {
        return DirectionPattern(PatternType::NONE, dir);
    }

    const WindowDesc& desc = window_descs()[(row * BOARD_SIZE + col) * 4 + dir];
    const int opp = opponent(color);

    uint32_t own = static_cast<uint32_t>(
        (board.line_bits(color, dir, desc.line) >> desc.shift) & kWindowMask);
    uint32_t blocked = static_cast<uint32_t>(
        (board.line_bits(opp, dir, desc.line) >> desc.shift) & kWindowMask) | desc.wall;

    return direction_from_bits(board, row, col, color, dir, own, blocked);
}

DirectionPattern PatternRecognizer::direction_from_bits(const Board& board, int row, int col,
                                                         int color, int dir,
                                                         uint32_t own, uint32_t blocked) {
    // The window describes the square as if `color` already had a stone there:
    // every caller evaluates empty candidate squares. Guard bits outside a
    // line's real cells are never set by a legal stone, so the wall mask alone
    // accounts for the off-board cells, exactly like extract_window's bounds test.
    own |= 1u << kCenterBit;
    blocked &= ~(1u << kCenterBit);

    const PatternType matched = static_cast<PatternType>(pattern_lut()[(own << 9) | blocked]);
    return finish_direction(board, row, col, color, dir, matched);
}

DirectionPattern PatternRecognizer::recognize_direction_scalar(const Board& board, int row, int col,
                                                                int color, int dir) {
    const int window = extract_window(board, row, col, color, dir);
    return finish_direction(board, row, col, color, dir, match_pattern(window, true));
}

DirectionPattern PatternRecognizer::finish_direction(const Board& board, int row, int col,
                                                     int color, int dir, PatternType type) {
    // match_pattern is color-blind and calls any 6+ run inside the 9-cell
    // window LONG_FIVE. Under Renju that is only true for BLACK (overline is
    // forbidden and does not win). WHITE wins on any run of five or more, so
    // the LONG_FIVE label hides WHITE's gap-split winning completions
    // (XXXX_X, XXX_XXX, ...) from must_block, generate_attacks,
    // has_decisive_tempo and the evaluator's five-bonus. Demote those back
    // to FIVE for WHITE.
    if (color == WHITE && type == PatternType::LONG_FIVE) {
        type = PatternType::FIVE;
    }

    // Renju: BLACK wins on exactly five and forbids overlines; WHITE wins on
    // any run of five or more. The window only spans +/-4 cells around the
    // placed stone, so a stone played at the end of an existing five fits
    // inside the window and looks like a FIVE when it is really a 6-stone
    // overline. Measure the real run for BLACK only - reclassifying a
    // WHITE overline as LONG_FIVE would hide WHITE's winning threat from
    // every branch that matches on PatternType::FIVE.
    if (type == PatternType::FIVE && color == BLACK) {
        if (board.run_length(dir, row, col, color) >= 6) type = PatternType::LONG_FIVE;
    }

    // A BLACK four is only a threat if one of its completion points makes
    // EXACTLY five. XXXX_X labels as LIVE_FOUR because both end cells are empty,
    // but filling the hole is a six-overline, so just one point wins; when every
    // point overruns (X_XXXX_X) there is no four to speak of. This needs the
    // board rather than the window: the window labels the shape, not its
    // completions. WHITE is not affected - a six there is a win.
    if (color == BLACK && (type == PatternType::LIVE_FOUR || type == PatternType::RUSH_FOUR)) {
        const int dr = DIRECTIONS[dir][0];
        const int dc = DIRECTIONS[dir][1];

        // The run length a fill at (r, c) would produce, counting the square
        // under recognition as already BLACK - the probe stone is part of the
        // four, but it is still empty on the board, so board.run_length() alone
        // would stop at it and report one stone short.
        auto fill_run = [&](int r, int c) {
            int len = 1;
            for (int sign = -1; sign <= 1; sign += 2) {
                for (int k = 1; k <= 5; ++k) {
                    const int rr = r + sign * k * dr;
                    const int cc = c + sign * k * dc;
                    if (!board.is_valid(rr, cc)) break;
                    if (board.get(rr, cc) != BLACK && !(rr == row && cc == col)) break;
                    ++len;
                }
            }
            return len;
        };

        const int run = fill_run(row, col);
        // Only a contiguous four is in scope: a gap shape's completion point is
        // its hole, which this end-cell probe cannot reach, and match_gapped_run
        // already proved that hole makes exactly five.
        if (run == 4) {
            int five_points = 0;
            for (int sign = -1; sign <= 1; sign += 2) {
                int r = row + sign * dr;
                int c = col + sign * dc;
                while (board.is_valid(r, c) && board.get(r, c) == BLACK) {
                    r += sign * dr;
                    c += sign * dc;
                }
                if (board.is_valid(r, c) && board.get(r, c) == EMPTY && fill_run(r, c) == 5) {
                    ++five_points;
                }
            }
            if (five_points == 0) {
                type = PatternType::NONE;
            } else if (type == PatternType::LIVE_FOUR && five_points == 1) {
                type = PatternType::RUSH_FOUR;
            }
        }
    }

    return DirectionPattern(type, dir);
}

PositionPattern PatternRecognizer::recognize_position(const Board& board, int row, int col, 
                                                       int color) {
    PositionPattern pattern;
    
    for (int d = 0; d < 4; ++d) {
        pattern.directions[d] = recognize_direction(board, row, col, color, d);
    }
    
    pattern.analyze();
    
    return pattern;
}

void PatternRecognizer::recognize_position_both(const Board& board, int row, int col, int color,
                                                PositionPattern& first, PositionPattern& second) {
    const int opp = opponent(color);

    if (row < 0 || row >= BOARD_SIZE || col < 0 || col >= BOARD_SIZE ||
        (color != BLACK && color != WHITE)) {
        first.analyze();
        second.analyze();
        return;
    }

    const size_t base = (row * BOARD_SIZE + col) * 4;
    for (int d = 0; d < 4; ++d) {
        const WindowDesc& desc = window_descs()[base + d];
        const uint32_t mine = static_cast<uint32_t>(
            (board.line_bits(color, d, desc.line) >> desc.shift) & kWindowMask);
        const uint32_t theirs = static_cast<uint32_t>(
            (board.line_bits(opp, d, desc.line) >> desc.shift) & kWindowMask);

        // Same pairing as recognize_direction: own stones for one colour are the
        // blocked cells for the other, so the two windows come out of two loads.
        first.directions[d] =
            direction_from_bits(board, row, col, color, d, mine, theirs | desc.wall);
        second.directions[d] =
            direction_from_bits(board, row, col, opp, d, theirs, mine | desc.wall);
    }

    first.analyze();
    second.analyze();
}

CombinedPattern PatternRecognizer::recognize_combined(const Board& board, int row, int col, 
                                                       int color) {
    CombinedPattern combined;
    
    if (!board.is_empty(row, col)) {
        return combined;
    }
    
    PositionPattern black_pattern = recognize_position(board, row, col, BLACK);
    PositionPattern white_pattern = recognize_position(board, row, col, WHITE);
    
    combined.four_four = black_pattern.has_four_four || white_pattern.has_four_four;
    combined.four_three = black_pattern.has_four_three || white_pattern.has_four_three;
    combined.three_three = black_pattern.has_three_three || white_pattern.has_three_three;
    
    if (color == BLACK) {
        bool has_exact_five = false;
        for (int d = 0; d < 4; ++d) {
            if (black_pattern.directions[d].type == PatternType::FIVE) {
                has_exact_five = true;
                break;
            }
        }
        
        if (!has_exact_five) {
            if (black_pattern.has_three_three || black_pattern.has_four_four) {
                combined.forbidden = true;
            }
            
            for (int d = 0; d < 4; ++d) {
                if (black_pattern.directions[d].type == PatternType::LONG_FIVE) {
                    combined.forbidden = true;
                }
            }
        }
    }
    
    return combined;
}

bool PatternRecognizer::is_forbidden(const Board& board, int row, int col, int color) {
    if (color != BLACK) return false;
    if (!board.is_empty(row, col)) return false;

    // The forbidden flag is derived exclusively from BLACK's own pattern (the
    // double-three / double-four / overline tests in is_forbidden_from_pattern).
    // This filter runs on every candidate of every VCF/VCT/alphabeta node.
    PositionPattern black_pattern = recognize_position(board, row, col, BLACK);
    return is_forbidden_from_pattern(black_pattern);
}

bool PatternRecognizer::is_forbidden_from_pattern(const PositionPattern& black_pattern) {
    bool has_exact_five = false;
    for (int d = 0; d < 4; ++d) {
        if (black_pattern.directions[d].type == PatternType::FIVE) {
            has_exact_five = true;
            break;
        }
    }
    if (has_exact_five) return false;

    if (black_pattern.has_three_three || black_pattern.has_four_four) {
        return true;
    }
    for (int d = 0; d < 4; ++d) {
        if (black_pattern.directions[d].type == PatternType::LONG_FIVE) {
            return true;
        }
    }
    return false;
}

}
