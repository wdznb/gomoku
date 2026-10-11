#include "elo.h"
#include "json_int.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

namespace gomoku {

EloSystem::EloSystem(double initial_elo, double base_k)
    : initial_elo_(initial_elo), base_k_(base_k) {}

int EloSystem::add_player(const std::string& name) {
    return add_player(name, initial_elo_);
}

int EloSystem::add_player(const std::string& name, double elo) {
    for (int i = 0; i < static_cast<int>(players_.size()); ++i) {
        if (players_[i].name == name) return i;
    }
    Player player;
    player.name = name;
    player.elo = elo;
    players_.push_back(player);
    return static_cast<int>(players_.size()) - 1;
}

int EloSystem::player_id(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(players_.size()); ++i) {
        if (players_[i].name == name) return i;
    }
    return -1;
}

bool EloSystem::has_player(const std::string& name) const {
    return player_id(name) >= 0;
}

const EloSystem::Player& EloSystem::player(int id) const {
    if (id < 0 || id >= static_cast<int>(players_.size())) {
        throw std::out_of_range("unknown elo player");
    }
    return players_[id];
}

double EloSystem::expected_score(double rating_a, double rating_b) {
    return 1.0 / (1.0 + std::pow(10.0, (rating_b - rating_a) / 400.0));
}

double EloSystem::k_factor(const Player& subject) const {
    if (subject.games < 30) return base_k_;
    if (subject.games < 100) return base_k_ * 0.75;
    return base_k_ * 0.5;
}

void EloSystem::record_result(int id_a, int id_b, double score_a) {
    if (id_a == id_b) return;
    if (id_a < 0 || id_b < 0 ||
        id_a >= static_cast<int>(players_.size()) ||
        id_b >= static_cast<int>(players_.size())) {
        throw std::out_of_range("elo result references an unknown player");
    }

    const double score_b = 1.0 - score_a;
    Player& a = players_[id_a];
    Player& b = players_[id_b];

    // Both sides are updated from the pre-move ratings, so the order of the two
    // adjustments must not matter.
    const double expected_a = expected_score(a.elo, b.elo);
    const double expected_b = 1.0 - expected_a;
    const double k_a = k_factor(a);
    const double k_b = k_factor(b);

    a.elo += k_a * (score_a - expected_a);
    b.elo += k_b * (score_b - expected_b);

    a.games++;
    b.games++;
    if (score_a > 0.75) {
        a.wins++; b.losses++;
    } else if (score_a < 0.25) {
        a.losses++; b.wins++;
    } else {
        a.draws++; b.draws++;
    }
}

std::vector<EloSystem::Player> EloSystem::standings() const {
    std::vector<Player> sorted = players_;
    std::sort(sorted.begin(), sorted.end(), [](const Player& lhs, const Player& rhs) {
        if (lhs.elo != rhs.elo) return lhs.elo > rhs.elo;
        return lhs.name < rhs.name;
    });
    return sorted;
}

std::string EloSystem::to_json() const {
    json root;
    root["initial_elo"] = initial_elo_;
    root["base_k"] = base_k_;

    json entries = json::array();
    for (const auto& subject : players_) {
        entries.push_back({
            {"name", subject.name},
            {"elo", subject.elo},
            {"wins", subject.wins},
            {"losses", subject.losses},
            {"draws", subject.draws},
            {"games", subject.games},
        });
    }
    root["players"] = entries;
    return root.dump();
}

bool EloSystem::load_json(const std::string& text, std::string& error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        error = std::string("invalid elo JSON: ") + e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("players") || !root["players"].is_array()) {
        error = "elo state must contain a players array";
        return false;
    }

    EloSystem restored;
    if (root.contains("initial_elo") && root["initial_elo"].is_number()) {
        restored.initial_elo_ = root["initial_elo"].get<double>();
    }
    if (root.contains("base_k") && root["base_k"].is_number()) {
        restored.base_k_ = root["base_k"].get<double>();
    }

    // Counters decide only the K-factor bracket, so an unusable one falls back to
    // zero instead of truncating; the names and ratings themselves are what matter.
    for (const auto& entry : root["players"]) {
        if (!entry.is_object() || !entry.contains("name") || !entry["name"].is_string()) {
            error = "elo player entry needs a name";
            return false;
        }
        auto read_int = [&entry](const char* key) {
            int value = 0;
            return entry.contains(key) && read_json_int(entry[key], value) ? value : 0;
        };
        Player subject;
        subject.name = entry["name"].get<std::string>();
        subject.elo = entry.contains("elo") && entry["elo"].is_number()
                          ? entry["elo"].get<double>() : restored.initial_elo_;
        subject.wins = read_int("wins");
        subject.losses = read_int("losses");
        subject.draws = read_int("draws");
        subject.games = read_int("games");
        restored.players_.push_back(subject);
    }

    *this = restored;
    return true;
}

bool EloSystem::save(const std::string& path, std::string& error) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        error = "cannot write " + path;
        return false;
    }
    file << to_json();
    if (!file.good()) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

bool EloSystem::load(const std::string& path, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return load_json(buffer.str(), error);
}

}
