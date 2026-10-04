#ifndef GOMOKU_PROTOCOL_H
#define GOMOKU_PROTOCOL_H

#include <string>
#include <vector>

namespace gomoku {

struct MoveJson {
    int row;
    int col;
    std::string color;
};

struct ClientMessage {
    std::string type;
    
    std::string color;
    int row;
    int col;
    int timeout_ms;
    int max_depth;
    
    ClientMessage() : row(-1), col(-1), timeout_ms(5000), max_depth(8) {}
};

struct ServerMessage {
    std::string type;
    
    int row;
    int col;
    std::string color;
    int score;
    int thinking_time_ms;
    int depth;
    int nodes;
    int elapsed_ms;
    std::string reason;
    std::string winner;
    std::string message;
    int move_count;
    std::vector<MoveJson> principal_variation;
    std::vector<std::vector<int>> board;
    std::vector<MoveJson> move_history;
    std::string current_turn;
    std::string game_status;
    
    ServerMessage() : row(-1), col(-1), score(0), thinking_time_ms(0), 
                      depth(0), nodes(0), elapsed_ms(0), move_count(0) {}
};

}

#endif
