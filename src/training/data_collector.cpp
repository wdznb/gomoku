#include "data_collector.h"
#include "../core/pattern.h"
#include "json_int.h"
#include "../core/transforms.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>
#include <sstream>
#include <algorithm>

using json = nlohmann::json;

namespace gomoku {

namespace {

float centre_control(const Board& board, int color) {
    const int middle = BOARD_SIZE / 2;
    long long total = 0;
    int stones = 0;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            if (board.get(r, c) != color) continue;
            stones++;
            const int distance = std::max(std::abs(r - middle), std::abs(c - middle));
            total += middle - distance;
        }
    }
    if (stones == 0) return 0.0f;
    return static_cast<float>(total) / static_cast<float>(middle * stones);
}

}

std::vector<float> DataCollector::extract_features(const Board& board, int side_to_move) const {
    std::vector<float> features(FEATURE_COUNT, 0.0f);
    fill_invariants(board, side_to_move, features);
    fill_orientation(board, side_to_move, features);
    return features;
}

void DataCollector::fill_invariants(const Board& board, int side_to_move,
                                    std::vector<float>& out) const {
    const int opp_color = opponent(side_to_move);

    int my_shapes[PATTERN_SLOTS] = {0};
    int opp_shapes[PATTERN_SLOTS] = {0};

    const auto& actives = board.active_empty();
    for (const auto& pos : actives) {
        if (!board.is_empty(pos)) continue;

        const PositionPattern mine =
            PatternRecognizer::recognize_position(board, pos.row, pos.col, side_to_move);
        const PositionPattern theirs =
            PatternRecognizer::recognize_position(board, pos.row, pos.col, opp_color);

        for (int d = 0; d < 4; ++d) {
            const int my_type = static_cast<int>(mine.directions[d].type);
            if (my_type >= 1 && my_type <= PATTERN_SLOTS) my_shapes[my_type - 1]++;
            const int their_type = static_cast<int>(theirs.directions[d].type);
            if (their_type >= 1 && their_type <= PATTERN_SLOTS) opp_shapes[their_type - 1]++;
        }

        for (int d = 0; d < 4; ++d) {
            if (mine.directions[d].type == PatternType::FIVE) out[FEATURE_MY_FIVE] = 1.0f;
            if (theirs.directions[d].type == PatternType::FIVE) out[FEATURE_OPP_FIVE] = 1.0f;
        }
    }

    const float scale = 1.0f / static_cast<float>(std::max<size_t>(1, actives.size()));
    for (int i = 0; i < PATTERN_SLOTS; ++i) {
        out[i] = static_cast<float>(my_shapes[i]) * scale;
        out[PATTERN_SLOTS + i] = static_cast<float>(opp_shapes[i]) * scale;
    }

    int my_stones = 0;
    int opp_stones = 0;
    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            const int cell = board.get(r, c);
            if (cell == side_to_move) my_stones++;
            else if (cell == opp_color) opp_stones++;
        }
    }
    out[FEATURE_MY_STONES] = static_cast<float>(my_stones) / 20.0f;
    out[FEATURE_OPP_STONES] = static_cast<float>(opp_stones) / 20.0f;
    out[FEATURE_MY_CENTRE] = centre_control(board, side_to_move);
    out[FEATURE_OPP_CENTRE] = centre_control(board, opp_color);
    out[FEATURE_ACTIVE] = static_cast<float>(actives.size()) / 40.0f;
    out[FEATURE_PLY] = static_cast<float>(board.move_count()) / 40.0f;
    out[FEATURE_SIDE] = side_to_move == BLACK ? 1.0f : 0.0f;
}

void DataCollector::fill_orientation(const Board& board, int side_to_move,
                                     std::vector<float>& out) const {
    if (board.move_count() > 0) {
        const Move last = board.last_move();
        out[FEATURE_LAST_ROW] = static_cast<float>(last.pos.row - BOARD_SIZE / 2) / 7.0f;
        out[FEATURE_LAST_COL] = static_cast<float>(last.pos.col - BOARD_SIZE / 2) / 7.0f;

        for (int dr = -2; dr <= 2; ++dr) {
            for (int dc = -2; dc <= 2; ++dc) {
                const int row = last.pos.row + dr;
                const int col = last.pos.col + dc;
                float value = 0.0f;
                if (board.is_valid(row, col)) {
                    const int cell = board.get(row, col);
                    if (cell == BLACK) value = 1.0f;
                    else if (cell == WHITE) value = -1.0f;
                }
                out[FEATURE_WINDOW + (dr + 2) * 5 + (dc + 2)] = value;
            }
        }
    }
}

std::vector<TrainingSample> DataCollector::collect(const GameRecord& record, int game_id,
                                                   bool with_augmentation) const {
    std::vector<TrainingSample> samples;
    if (record.reason == GameEndReason::INVALID) return samples;

    Board board;
    for (size_t i = 0; i < record.moves.size(); ++i) {
        const MoveRecord& entry = record.moves[i];
        const int side = entry.move.color;

        const bool engine_chose = entry.method != "replay" && entry.method != "forced" &&
                                  entry.method != "default" && entry.move.pos.is_valid();
        if (engine_chose && entry.depth >= min_search_depth_) {
            const float value = record.outcome_for(side);
            if (value != 0.0f || keep_undecided_) {
                // 0-24 are the same under every square symmetry: compute them once
                // for the ply, and let only 25-51 follow each transform.
                std::vector<float> invariants(FEATURE_COUNT, 0.0f);
                fill_invariants(board, side, invariants);

                std::set<std::pair<uint64_t, int>> seen;
                const int variants = with_augmentation ? SYMMETRY_COUNT : 1;
                for (int variant = 0; variant < variants; ++variant) {
                    const Board oriented = variant == 0 ? board : transform_board(board, variant);
                    const Move oriented_move(transform_position(entry.move.pos, variant), side);
                    // The hash only describes the placement, while features 25-51 come
                    // from the oriented board's last move. A symmetry can fix the played
                    // cell and still move that last stone, so the two variants are
                    // different samples: the last cell (-1 on an empty board) joins the
                    // key, packed below the played cell since both stay under 256.
                    const int last_cell =
                        oriented.move_count() > 0 ? oriented.last_move().pos.to_index() : -1;
                    const int key = oriented_move.pos.to_index() * 256 + last_cell + 1;
                    if (!seen.insert({oriented.zobrist_hash(), key}).second) {
                        continue;
                    }

                    TrainingSample sample;
                    sample.features = invariants;
                    fill_orientation(oriented, side, sample.features);
                    sample.value = value;
                    sample.move = oriented_move;
                    sample.search_score = entry.score;
                    sample.search_depth = entry.depth;
                    sample.nodes = entry.nodes;
                    sample.weight = 1.0f;
                    sample.game_id = game_id;
                    sample.move_index = static_cast<int>(i);
                    samples.push_back(sample);
                }
            }
        }

        board.place_stone(entry.move.pos, side);
    }

    return samples;
}

std::string TrainingSample::to_json() const {
    json root;
    root["features"] = features;
    root["value"] = value;
    root["move"] = {{ "row", move.pos.row }, { "col", move.pos.col },
                    { "color", move.color == BLACK ? "black" : "white" }};
    root["search_score"] = search_score;
    root["search_depth"] = search_depth;
    root["nodes"] = nodes;
    root["weight"] = weight;
    root["game_id"] = game_id;
    root["move_index"] = move_index;
    return root.dump();
}

bool TrainingSample::from_json(const std::string& text, TrainingSample& out, std::string& error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        error = std::string("invalid sample JSON: ") + e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("features") || !root["features"].is_array()) {
        error = "sample needs a features array";
        return false;
    }

    TrainingSample sample;
    auto read_int = [&root](const char* key, int fallback) {
        int value = fallback;
        return root.contains(key) && read_json_int(root[key], value) ? value : fallback;
    };

    for (const auto& value : root["features"]) {
        if (!value.is_number()) {
            error = "sample features must be numbers";
            return false;
        }
        sample.features.push_back(value.get<float>());
    }
    if (root.contains("value") && root["value"].is_number()) sample.value = root["value"].get<float>();
    sample.search_score = read_int("search_score", sample.search_score);
    sample.search_depth = read_int("search_depth", sample.search_depth);
    sample.nodes = read_int("nodes", sample.nodes);
    if (root.contains("weight") && root["weight"].is_number()) sample.weight = root["weight"].get<float>();
    // 身份键写了就必须读得进来：静默回落 -1 会让一批这样的行塌进同一个伪局，
    // (game_id, move_index) 的唯一性就没了。缺失照旧合法，仍是 -1 默认。
    auto read_id = [&root](const char* key, int& dst) {
        return !root.contains(key) || read_json_int(root[key], dst);
    };
    if (!read_id("game_id", sample.game_id)) {
        error = "sample game_id must be an integer";
        return false;
    }
    if (!read_id("move_index", sample.move_index)) {
        error = "sample move_index must be an integer";
        return false;
    }
    // The move is the label the features describe, so a sample without one is not
    // a sample; only the engine numbers may fall back to defaults.
    if (!root.contains("move") || !root["move"].is_object()) {
        error = "sample needs a move object";
        return false;
    }
    const json& move = root["move"];
    if (!move.contains("row") || !move["row"].is_number_integer() ||
        !move.contains("col") || !move["col"].is_number_integer() ||
        !move.contains("color") || !move["color"].is_string()) {
        error = "sample move needs integer row/col and a color string";
        return false;
    }
    int row = 0;
    int col = 0;
    if (!read_json_int(move["row"], row) || !read_json_int(move["col"], col)) {
        error = "sample move coordinates are outside the supported range";
        return false;
    }
    const std::string color = move["color"].get<std::string>();
    if (color != "black" && color != "white") {
        error = "sample move color must be black or white";
        return false;
    }
    sample.move = Move(row, col, color == "black" ? BLACK : WHITE);
    if (!sample.move.pos.is_valid()) {
        error = "sample move is off the board";
        return false;
    }

    // The vector length is the model input and the two floats are the outcome, so
    // neither may be absent: a short vector would train on another layout.
    if (sample.features.size() != static_cast<size_t>(DataCollector::FEATURE_COUNT)) {
        error = "sample has " + std::to_string(sample.features.size()) + " features, needs " +
                std::to_string(DataCollector::FEATURE_COUNT);
        return false;
    }
    if (!root.contains("value") || !root["value"].is_number()) {
        error = "sample needs a numeric value";
        return false;
    }
    if (!root.contains("weight") || !root["weight"].is_number()) {
        error = "sample needs a numeric weight";
        return false;
    }

    out = sample;
    return true;
}

bool DataCollector::write_jsonl(const std::vector<TrainingSample>& samples,
                                const std::string& path, std::string& error) const {
    std::ofstream file(path, std::ios::app);
    if (!file.is_open()) {
        error = "cannot append to " + path;
        return false;
    }
    for (const auto& sample : samples) {
        file << sample.to_json() << "\n";
    }
    // Buffered writes only report a full disk at close, which is after this
    // check would have called the append a success.
    file.flush();
    if (!file.good()) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

bool DataCollector::read_jsonl(const std::string& path, std::vector<TrainingSample>& out,
                               std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open " + path;
        return false;
    }

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        line_number++;
        if (line.empty()) continue;
        TrainingSample sample;
        if (!TrainingSample::from_json(line, sample, error)) {
            error += " on line " + std::to_string(line_number);
            return false;
        }
        out.push_back(sample);
    }
    return true;
}

}
