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
    
    static DirectionPattern recognize_direction(const Board& board, int row, int col, 
                                                 int color, int dir);
    
private:
    static bool initialized_;
    
    static int extract_window(const Board& board, int row, int col, int color, int dir);
    static PatternType match_pattern(int window_code, bool is_center);
};

}

#endif
