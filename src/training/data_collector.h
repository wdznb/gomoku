#ifndef GOMOKU_DATA_COLLECTOR_H
#define GOMOKU_DATA_COLLECTOR_H

#include "../core/board.h"
#include "game_record.h"
#include <string>
#include <vector>

namespace gomoku {

// One position seen during training, described to a parameter optimizer or a
// future value model.
struct TrainingSample {
    std::vector<float> features;
    float value = 0.0f;     // outcome for the side to move: +1 won, -1 lost, 0 undecided
    Move move;              // the move that was played here
    int search_score = 0;
    int search_depth = 0;
    int nodes = 0;
    float weight = 1.0f;
    int game_id = -1;
    int move_index = -1;
    // 设计文档 6.7 a：深搜蒸馏的老师着法。可选字段——旧样本没有这两个键，
    // 读回是无效着法 / 空串 = 没有老师，行为与今天一致。
    Move teacher_move;
    std::string teacher_source;

    std::string to_json() const;
    static bool from_json(const std::string& text, TrainingSample& out, std::string& error);
};

class DataCollector {
public:
    // Feature layout. Slots 0-15 are shape statistics, which are invariant under
    // rotation; 27-51 describe the neighbourhood of the last move, which is not,
    // so those are what the eight-way augmentation actually multiplies.
    static constexpr int PATTERN_SLOTS = 8;      // PatternType values 1..8, mover then opponent
    static constexpr int FEATURE_COUNT = 52;

    static constexpr int FEATURE_MY_FIVE = 16;    // mover can make five immediately
    static constexpr int FEATURE_OPP_FIVE = 17;
    static constexpr int FEATURE_MY_STONES = 18;
    static constexpr int FEATURE_OPP_STONES = 19;
    static constexpr int FEATURE_MY_CENTRE = 20;
    static constexpr int FEATURE_OPP_CENTRE = 21;
    static constexpr int FEATURE_ACTIVE = 22;     // candidate cells
    static constexpr int FEATURE_PLY = 23;        // game progress
    static constexpr int FEATURE_SIDE = 24;       // 1 when BLACK to move
    static constexpr int FEATURE_LAST_ROW = 25;
    static constexpr int FEATURE_LAST_COL = 26;
    static constexpr int FEATURE_WINDOW = 27;     // 5x5 around the last move

    // A move whose search was cut off before reaching this depth carries no
    // signal about the coefficients being trained, so it is dropped.
    void set_min_search_depth(int depth) { min_search_depth_ = depth; }
    void set_keep_undecided(bool keep) { keep_undecided_ = keep; }

    std::vector<float> extract_features(const Board& board, int side_to_move) const;

    // Replays the record and emits one sample per informative ply. INVALID
    // records yield nothing; plies that were forced, replayed, or searched too
    // shallowly are skipped. With augmentation each ply contributes up to eight
    // orientation variants, duplicates from a symmetric position removed.
    std::vector<TrainingSample> collect(const GameRecord& record, int game_id,
                                        bool with_augmentation = true) const;

    bool write_jsonl(const std::vector<TrainingSample>& samples, const std::string& path,
                     std::string& error) const;
    static bool read_jsonl(const std::string& path, std::vector<TrainingSample>& out,
                           std::string& error);

private:
    // 0-24 are the same under every square symmetry, so a ply computes them once
    // and each orientation variant only rewrites the last-move slots (25-51).
    void fill_invariants(const Board& board, int side_to_move, std::vector<float>& out) const;
    void fill_orientation(const Board& board, int side_to_move, std::vector<float>& out) const;

    int min_search_depth_ = 1;
    bool keep_undecided_ = true;
};

}

#endif
