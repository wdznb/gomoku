#ifndef GOMOKU_PATTERN_H
#define GOMOKU_PATTERN_H

#include "types.h"
#include "board.h"
#include <array>
#include <vector>

namespace gomoku {

enum class PatternType {
    NONE = 0,
    SLEEPY_TWO,
    LIVE_TWO,
    SLEEPY_THREE,
    LIVE_THREE,
    RUSH_FOUR,
    LIVE_FOUR,
    FIVE,
    LONG_FIVE
};

struct DirectionPattern {
    PatternType type;
    int direction;
    
    DirectionPattern() : type(PatternType::NONE), direction(-1) {}
    DirectionPattern(PatternType t, int d) : type(t), direction(d) {}
};

struct PositionPattern {
    std::array<DirectionPattern, 4> directions;
    
    int four_count;
    int three_count;
    int live_four_count;
    int rush_four_count;
    int live_three_count;
    
    bool has_four_four;
    bool has_four_three;
    bool has_three_three;
    bool has_live_four;
    bool has_rush_four;
    bool has_live_three;
    
    PositionPattern();
    void analyze();
};

struct CombinedPattern {
    bool four_four;
    bool four_three;
    bool three_three;
    bool forbidden;
    
    CombinedPattern() : four_four(false), four_three(false), 
                        three_three(false), forbidden(false) {}
};

class PatternRecognizer {
public:
    static void init();
    
    static PositionPattern recognize_position(const Board& board, int row, int col, int color);
    static CombinedPattern recognize_combined(const Board& board, int row, int col, int color);
    
    static bool is_forbidden(const Board& board, int row, int col, int color);

    // Forbidden decision given an ALREADY-recognized BLACK pattern (the caller
    // has run recognize_position for BLACK and wants to reuse it instead of
    // paying a second 4-direction scan). Encodes the same tests as
    // is_forbidden so there is a single source of truth.
    static bool is_forbidden_from_pattern(const PositionPattern& black_pattern);
    
    static DirectionPattern recognize_direction(const Board& board, int row, int col, 
                                                 int color, int dir);

    // The original per-cell scan (extract_window + the base-3 decoder), kept as
    // the differential oracle for the bitboard path above: tests assert the two
    // agree on randomised positions. Production callers use recognize_direction.
    static DirectionPattern recognize_direction_scalar(const Board& board, int row, int col,
                                                       int color, int dir);
    
private:
    static bool initialized_;
    
    static int extract_window(const Board& board, int row, int col, int color, int dir);
    static PatternType match_pattern(int window_code, bool is_center);

    // The colour-specific corrections applied to a raw window match (WHITE
    // overline demotion, BLACK overline promotion). Shared by both recognition
    // paths so they cannot drift apart.
    static DirectionPattern finish_direction(const Board& board, int row, int col, int color,
                                             int dir, PatternType type);

    // 18-bit pattern table: each of the 9 window cells is either a stone of the
    // recognised colour (own) or an opponent stone / board edge (blocked).
    // Indexed (own << 9) | blocked, so one load replaces the base-3 decode and
    // the whole shape-matching branch chain. It is filled by running every key
    // through match_pattern, which makes the bitboard path equivalent to the
    // scalar one by construction rather than by a second hand-written matcher.
    static const std::array<uint8_t, 1 << 18>& pattern_lut();
    static bool fill_pattern_lut(std::array<uint8_t, 1 << 18>& lut);

    static std::array<uint8_t, 1 << 18> pattern_lut_;
};

}

#endif
