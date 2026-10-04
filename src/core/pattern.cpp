#include "pattern.h"
#include <algorithm>

namespace gomoku {

bool PatternRecognizer::initialized_ = false;

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
    initialized_ = true;
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
    
    if (total == 4) {
        int left_end = (4 - count_left - 1 >= 0) ? cells[4 - count_left - 1] : 2;
        int right_end = (4 + count_right + 1 <= 8) ? cells[4 + count_right + 1] : 2;
        
        if (left_end == 0 && right_end == 0) {
            return PatternType::LIVE_FOUR;
        } else {
            return PatternType::RUSH_FOUR;
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
            } else if (left_open || right_open) {
                return PatternType::LIVE_THREE;
            } else {
                return PatternType::SLEEPY_THREE;
            }
        } else {
            bool can_live_four_left = false;
            bool can_live_four_right = false;
            
            if (left_pos >= 0 && cells[left_pos] == 0 && left_pos > 0) {
                can_live_four_left = true;
            }
            
            if (right_pos <= 8 && cells[right_pos] == 0 && right_pos < 8) {
                can_live_four_right = true;
            }
            
            if (can_live_four_left || can_live_four_right) {
                return PatternType::LIVE_THREE;
            } else {
                return PatternType::SLEEPY_THREE;
            }
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

DirectionPattern PatternRecognizer::recognize_direction(const Board& board, int row, int col,
                                                         int color, int dir) {
    int window = extract_window(board, row, col, color, dir);
    PatternType type = match_pattern(window, true);
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
    
    CombinedPattern combined = recognize_combined(board, row, col, color);
    return combined.forbidden;
}

}
