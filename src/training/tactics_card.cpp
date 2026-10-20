#include "tactics_card.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace gomoku {

namespace {

const char* color_name(int color) {
    if (color == BLACK) return "b";
    if (color == WHITE) return "w";
    return ".";
}

int color_from_name(const std::string& name) {
    if (name == "b" || name == "black") return BLACK;
    if (name == "w" || name == "white") return WHITE;
    return EMPTY;
}

json move_to_json(const Move& move) {
    return json::array({move.pos.row, move.pos.col});
}

Move move_from_json(const json& node) {
    if (!node.is_array() || node.size() < 2) return Move();
    return Move(node[0].get<int>(), node[1].get<int>(), EMPTY);
}

// 棋谱用三元组，因为它要能独立摆回棋盘：颜色不能由下标推——records.jsonl 里的
// 颜色字段是权威的（换手、悔棋、摆棋都会让奇偶不再等于颜色）。
json stones_to_json(const std::vector<Move>& stones) {
    json out = json::array();
    for (const Move& stone : stones) {
        out.push_back(json::array({stone.pos.row, stone.pos.col, color_name(stone.color)}));
    }
    return out;
}

bool stones_from_json(const json& node, std::vector<Move>& out) {
    if (!node.is_array()) return false;
    for (const json& item : node) {
        if (!item.is_array() || item.size() < 3) return false;
        out.push_back(Move(item[0].get<int>(), item[1].get<int>(),
                           color_from_name(item[2].get<std::string>())));
    }
    return true;
}

}  // namespace

std::string coefficients_fingerprint(const ThreatCoefficients& coeffs) {
    // 顺序就是 params.h 的 TunableParam 顺序，改顺序等于改指纹，这是有意的：
    // 指纹只用来判断「这张卡是不是在这套系数下判的」。
    const int values[14] = {
        coeffs.live_four, coeffs.rush_four, coeffs.live_three, coeffs.sleepy_three,
        coeffs.live_two, coeffs.sleepy_two, coeffs.double_four, coeffs.double_three,
        coeffs.four_three, coeffs.position_weight_scale, coeffs.edge_penalty,
        coeffs.distance_to_stones, coeffs.cand_width_root, coeffs.cand_width_deep,
    };
    unsigned long long hash = 1469598103934665603ULL;  // FNV-1a 64 offset basis
    for (int value : values) {
        unsigned int bits = static_cast<unsigned int>(value);
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= (bits >> (byte * 8)) & 0xFFu;
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream out;
    out << std::hex;
    for (int shift = 28; shift >= 0; shift -= 4) {
        out << ((hash >> shift) & 0xFu);
    }
    return out.str();
}

std::string TacticsCard::layer_for(int solve_budget_value) {
    if (solve_budget_value <= 0) return "hard";        // 梯子跑完也没解出
    if (solve_budget_value <= 8000) return "easy";
    if (solve_budget_value <= 64000) return "normal";
    return "hard";
}

bool TacticsCard::layer_at_least(const std::string& layer, const std::string& floor) {
    auto rank = [](const std::string& name) {
        if (name == "easy") return 0;
        if (name == "normal") return 1;
        if (name == "hard") return 2;
        return 1;  // 不认识的一律按 normal，宁可多跑不少跑
    };
    return rank(layer) >= rank(floor);
}

std::string TacticsCard::to_json() const {
    json root;
    root["schema"] = schema;
    root["id"] = id;
    root["layer"] = layer;
    root["kind"] = kind;
    root["side"] = side == BLACK ? "black" : (side == WHITE ? "white" : "none");
    root["ply"] = ply;
    root["stones"] = stones_to_json(stones);
    root["last_move"] = move_to_json(last_move);
    root["answer"] = move_to_json(answer);
    json alt = json::array();
    for (const Move& move : alt_answers) alt.push_back(move_to_json(move));
    root["alt_answers"] = alt;

    json evidence;
    evidence["best_score"] = best_score;
    evidence["played_score"] = played_score;
    evidence["second_score"] = second_score;
    evidence["gap"] = gap;
    evidence["loss"] = loss;
    evidence["budget"] = budget;
    evidence["depth"] = depth;
    evidence["played"] = move_to_json(played);
    evidence["second"] = move_to_json(second);
    evidence["method"] = method;
    root["evidence"] = evidence;

    json difficulty;
    difficulty["solve_budget"] = solve_budget;
    difficulty["failed_at"] = failed_at;
    root["difficulty"] = difficulty;

    root["coeffs_sha"] = coeffs_sha;
    root["generator"] = generator;
    json source;
    source["game"] = source_game;
    source["index"] = source_index;
    root["source"] = source;

    return root.dump();
}

bool TacticsCard::from_json(const std::string& text, TacticsCard& out, std::string& error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        error = std::string("invalid tactics card JSON: ") + e.what();
        return false;
    }
    if (!root.is_object()) {
        error = "tactics card must be an object";
        return false;
    }
    for (const char* key : {"id", "kind", "answer", "stones"}) {
        if (!root.contains(key)) {
            error = std::string("tactics card is missing ") + key;
            return false;
        }
    }

    TacticsCard card;
    card.schema = root.value("schema", std::string("tactics/1"));
    card.id = root["id"].get<std::string>();
    card.layer = root.value("layer", std::string("normal"));
    card.kind = root["kind"].get<std::string>();
    const std::string side = root.value("side", std::string("none"));
    card.side = color_from_name(side);
    card.ply = root.value("ply", 0);

    if (!stones_from_json(root["stones"], card.stones)) {
        error = "tactics card stones must be [row, col, colour] triples";
        return false;
    }
    if (!card.stones.empty()) card.last_move = card.stones.back();
    card.answer = move_from_json(root["answer"]);
    if (root.contains("alt_answers") && root["alt_answers"].is_array()) {
        for (const json& item : root["alt_answers"]) card.alt_answers.push_back(move_from_json(item));
    }

    if (root.contains("evidence") && root["evidence"].is_object()) {
        const json& e = root["evidence"];
        card.best_score = e.value("best_score", 0);
        card.played_score = e.value("played_score", 0);
        card.second_score = e.value("second_score", 0);
        card.gap = e.value("gap", 0);
        card.loss = e.value("loss", 0);
        card.budget = e.value("budget", 0);
        card.depth = e.value("depth", 0);
        card.method = e.value("method", std::string());
        if (e.contains("played")) card.played = move_from_json(e["played"]);
        if (e.contains("second")) card.second = move_from_json(e["second"]);
    }
    if (root.contains("difficulty") && root["difficulty"].is_object()) {
        card.solve_budget = root["difficulty"].value("solve_budget", 0);
        card.failed_at = root["difficulty"].value("failed_at", 0);
    }
    card.coeffs_sha = root.value("coeffs_sha", std::string());
    card.generator = root.value("generator", std::string("analyze/1"));
    if (root.contains("source") && root["source"].is_object()) {
        card.source_game = root["source"].value("game", std::string());
        card.source_index = root["source"].value("index", 0);
    }
    out = card;
    return true;
}

}
