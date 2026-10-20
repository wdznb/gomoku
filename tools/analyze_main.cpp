// 设计文档 docs/ENGINE_REDESIGN_V2.md §6.10 的 gomoku_analyze。
//
// 三个子命令对应漏斗的三段：
//   --replay PATH        段 A（零搜索筛查）；加 --review 跑段 B/段 C
//   --cards PATH         把复核结果写成题卡（JSONL，追加）
//   --bench DIR          摆棋测试：对题库跑 solve_rate@budget（§6.7 b）
//
// --export-samples 仍然拒绝：它要 TrainingSample 长出 teacher_move（§6.7 a），
// 那是下一刀的事。拒绝比假装能跑好——脚本必须知道自己在跑哪一段。
#include "core/board.h"
#include "core/evaluator.h"
#include "search/search_engine.h"
#include "training/game_record.h"
#include "training/params.h"
#include "training/replay_analyzer.h"
#include "training/tactics_card.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace gomoku;

namespace {

constexpr int kExitUsage = 2;

void print_usage() {
    std::cout <<
        "gomoku_analyze - replay analysis, design doc ENGINE_REDESIGN_V2.md 6.2/6.5/6.7\n"
        "\n"
        "stage A (zero search):\n"
        "  --replay PATH        read a records.jsonl written by gomoku_train\n"
        "  --swing N            |swing| that makes a candidate (default 5000)\n"
        "  --opening-plies N    plies always dropped unless tactical (default 6)\n"
        "  --quiet-band N       swing below this is 'no information' (default 500)\n"
        "  --depth-dip N        shallower than the median by this much (default 2)\n"
        "\n"
        "stage B/C (needs --review):\n"
        "  --review             re-search every candidate at a fixed node budget\n"
        "  --budget N           that budget (default 20000, nodes not milliseconds)\n"
        "  --depth D            depth cap for the review (default 8)\n"
        "  --gap N              lead over the second move that makes a 妙手 (default 3000)\n"
        "  --cards PATH         append the surviving cards to one JSONL file\n"
        "  --cards-dir DIR      route by kind: missed_mate -> proven/, rest -> judged/,\n"
        "                       plus DIR/index.json (design doc 6.6)\n"
        "  --ladder-max N       top of the difficulty ladder (default 256000)\n"
        "\n"
        "bench (stage 6.7 b):\n"
        "  --bench DIR          read every *.jsonl under DIR as cards, report solve_rate\n"
        "  --layer easy|normal|hard   only cards at this layer or above (default normal)\n"
        "\n"
        "common:/n"
        "  --params PATH        coefficients to judge with (default: built-in)\n"
        "  --limit N            stop after N games / cards (0 = all)\n"
        "  --verbose            one line per candidate / per card\n"
        "  --json               machine-readable output\n"
        "  --help               this text\n"
        "\n"
        "Stage A screens, stage B judges, stage C prices. --export-samples is not\n"
        "built yet (it needs TrainingSample::teacher_move, design doc 6.7 a).\n";
}

bool parse_int(const std::string& text, int& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    if (value < -2147483647L || value > 2147483647L) return false;
    out = static_cast<int>(value);
    return true;
}

std::string percent(int part, int whole) {
    if (whole <= 0) return "n/a";
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << (100.0 * part / whole) << "%";
    return out.str();
}

bool same_cell(const Move& a, const Move& b) {
    return a.pos.is_valid() && b.pos.is_valid() && a.pos.row == b.pos.row && a.pos.col == b.pos.col;
}

std::string cell_name(const Move& move) {
    if (!move.pos.is_valid()) return "--";
    std::ostringstream out;
    out << static_cast<char>('A' + move.pos.col) << move.pos.row;
    return out.str();
}

struct Totals {
    int games = 0;
    int plies = 0;
    int candidates = 0;
    int noise = 0;
    int opening = 0;
    int replayed = 0;
    int flat = 0;
    int parse_errors = 0;
    int reasons[kScreenReasonCount] = {0};
};

bool load_cards(const std::string& dir, std::vector<TacticsCard>& out, int& parse_errors) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".jsonl") continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& path : files) {
        std::ifstream input(path);
        if (!input) continue;
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            TacticsCard card;
            std::string error;
            if (!TacticsCard::from_json(line, card, error)) {
                ++parse_errors;
                std::cerr << path.string() << ": " << error << "\n";
                continue;
            }
            out.push_back(card);
        }
    }
    return true;
}

int run_bench(const std::string& dir, const ThreatCoefficients& coeffs, int budget, int depth,
              const std::string& layer_floor, int limit, bool verbose) {
    std::vector<TacticsCard> cards;
    int parse_errors = 0;
    if (!load_cards(dir, cards, parse_errors)) {
        std::cerr << "cannot read " << dir << "\n";
        return 1;
    }

    std::map<std::string, std::pair<int, int>> per_layer;  // layer -> (solved, total)
    int solved = 0;
    int total = 0;
    int skipped = 0;
    const std::string fingerprint = coefficients_fingerprint(coeffs);
    int fingerprint_mismatch = 0;

    for (const TacticsCard& card : cards) {
        if (limit > 0 && total >= limit) break;
        if (!TacticsCard::layer_at_least(card.layer, layer_floor)) {
            ++skipped;
            continue;
        }
        if (!card.coeffs_sha.empty() && card.coeffs_sha != fingerprint) ++fingerprint_mismatch;

        Board board;
        bool legal = true;
        for (const Move& stone : card.stones) {
            if (!stone.pos.is_valid() || !board.is_empty(stone.pos)) { legal = false; break; }
            board.place_stone(stone.pos.row, stone.pos.col, stone.color);
        }
        if (!legal) {
            ++skipped;
            continue;
        }

        SearchEngine engine(coeffs);
        engine.set_book_enabled(false);
        engine.set_max_nodes(budget);
        std::atomic<bool> stop{false};
        const SearchResult result = engine.search(board, card.side, depth, 60000, stop);
        const bool hit = !result.root_scores.empty() &&
                         same_cell(result.root_scores.front().move, card.answer);
        ++total;
        if (hit) ++solved;
        per_layer[card.layer].second += 1;
        if (hit) per_layer[card.layer].first += 1;
        if (verbose) {
            std::cout << "  " << std::setw(18) << std::left << card.id << std::right
                      << "  layer " << std::setw(6) << card.layer
                      << "  kind " << std::setw(15) << card.kind
                      << "  answer " << cell_name(card.answer)
                      << "  got " << (result.root_scores.empty() ? "--"
                                                                 : cell_name(result.root_scores.front().move))
                      << "  " << (hit ? "SOLVED" : "missed") << "\n";
        }
    }

    std::cout << "\nsolve_rate@" << budget << " = " << solved << "/" << total << " ("
              << percent(solved, total) << ")"
              << "   skipped " << skipped << " (below layer floor or illegal stones)\n";
    for (const auto& entry : per_layer) {
        std::cout << "  layer " << std::setw(6) << entry.first << "  " << entry.second.first << "/"
                  << entry.second.second << " (" << percent(entry.second.first, entry.second.second)
                  << ")\n";
    }
    if (parse_errors > 0) std::cout << "  parse errors " << parse_errors << "\n";
    if (fingerprint_mismatch > 0) {
        // 判断题的正解依赖系数：指纹不同不是错误，但必须说出来，否则读者会把
        // 「换了一套系数之后掉分」当成棋力退化（§6.6）。
        std::cout << "  WARN " << fingerprint_mismatch << "/" << total
                  << " cards were judged under different coefficients (" << fingerprint << ")\n";
    }
    if (total > 0 && (solved == 0 || solved == total)) {
        std::cout << "  WARN design doc 3.3 kill line: solve_rate is 0 % or 100 % -> 分层失败，"
                     "该层作废重造\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string replay_path;
    std::string cards_path;
    std::string cards_dir;
    std::string bench_dir;
    std::string params_path;
    std::string layer_floor = "normal";
    bool json_out = false;
    bool verbose = false;
    bool review = false;
    int limit = 0;
    ScreenConfig screen_config;
    ReviewConfig review_config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value_for = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << name << " needs a value\n";
                std::exit(kExitUsage);
            }
            return argv[++i];
        };
        auto int_for = [&](const char* name, int& target) {
            const std::string text = value_for(name);
            if (!parse_int(text, target) || target < 0) {
                std::cerr << "Invalid " << name << " " << text << "\n";
                std::exit(kExitUsage);
            }
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--replay") {
            replay_path = value_for("--replay");
        } else if (arg == "--cards") {
            cards_path = value_for("--cards");
            review = true;
        } else if (arg == "--cards-dir") {
            // 6.6 的目录布局：missed_mate 落 proven/（杀棋题，与系数无关，可当门禁），
            // 其余落 judged/（判断题，系数换代即失效）。混在一张表里，门禁会在每次
            // 调参后无意义地变红。
            cards_dir = value_for("--cards-dir");
            review = true;
        } else if (arg == "--bench") {
            bench_dir = value_for("--bench");
        } else if (arg == "--params") {
            params_path = value_for("--params");
        } else if (arg == "--layer") {
            layer_floor = value_for("--layer");
            if (layer_floor != "easy" && layer_floor != "normal" && layer_floor != "hard") {
                std::cerr << "Invalid --layer " << layer_floor << "\n";
                return kExitUsage;
            }
        } else if (arg == "--review") {
            review = true;
        } else if (arg == "--json") {
            json_out = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--limit") {
            int_for("--limit", limit);
        } else if (arg == "--swing") {
            int_for("--swing", screen_config.swing_threshold);
        } else if (arg == "--opening-plies") {
            int_for("--opening-plies", screen_config.opening_plies);
        } else if (arg == "--quiet-band") {
            int_for("--quiet-band", screen_config.quiet_band);
        } else if (arg == "--depth-dip") {
            int_for("--depth-dip", screen_config.depth_dip);
        } else if (arg == "--budget") {
            int_for("--budget", review_config.budget);
        } else if (arg == "--depth") {
            int_for("--depth", review_config.max_depth);
        } else if (arg == "--gap") {
            int_for("--gap", review_config.brilliant_gap);
        } else if (arg == "--ladder-max") {
            int_for("--ladder-max", review_config.ladder_max);
        } else if (arg == "--export-samples") {
            std::cerr << "--export-samples needs TrainingSample::teacher_move (design doc 6.7 a) "
                         "and is not built yet. Refusing rather than pretending.\n";
            return kExitUsage;
        } else {
            std::cerr << "Unknown argument " << arg << "\n";
            print_usage();
            return kExitUsage;
        }
    }

    ThreatCoefficients coeffs;
    if (!params_path.empty()) {
        std::string error;
        if (!CoefficientVector::load(params_path, coeffs, error)) {
            std::cerr << "cannot read " << params_path << ": " << error << "\n";
            return 1;
        }
    }

    if (!bench_dir.empty()) {
        return run_bench(bench_dir, coeffs, review_config.budget, review_config.max_depth,
                         layer_floor, limit, verbose);
    }
    if (replay_path.empty()) {
        print_usage();
        return kExitUsage;
    }

    std::ifstream input(replay_path);
    if (!input) {
        std::cerr << "cannot open " << replay_path << "\n";
        return 1;
    }

    ReplayAnalyzer analyzer(screen_config);
    ReplayReviewer reviewer(coeffs, review_config);
    std::ofstream cards;
    if (!cards_path.empty()) {
        cards.open(cards_path, std::ios::app);
        if (!cards) {
            std::cerr << "cannot append to " << cards_path << "\n";
            return 1;
        }
    }
    const std::string fingerprint = coefficients_fingerprint(coeffs);

    std::vector<TacticsCard> card_buffer;
    Totals totals;
    std::map<std::string, int> kinds;
    std::string line;
    int game_index = 0;
    int cards_written = 0;

    if (!json_out) {
        std::cout << "gomoku_analyze - design doc 6.2 stage A" << (review ? " + 6.7 stage B/C" : "")
                  << "\ninput " << replay_path << "\n"
                  << "swing>=" << screen_config.swing_threshold << "  quiet<"
                  << screen_config.quiet_band << "  opening<" << screen_config.opening_plies
                  << "  depth_dip>=" << screen_config.depth_dip << "\n";
        if (review) {
            std::cout << "review: budget " << review_config.budget << " nodes  depth "
                      << review_config.max_depth << "  gap " << review_config.brilliant_gap
                      << "  coeffs " << fingerprint << "\n";
        }
        std::cout << "\n";
    }

    while (std::getline(input, line)) {
        if (line.empty()) continue;
        if (limit > 0 && game_index >= limit) break;

        GameRecord record;
        std::string error;
        if (!GameRecord::from_json(line, record, error)) {
            ++totals.parse_errors;
            if (verbose) std::cerr << "game " << game_index << ": " << error << "\n";
            ++game_index;
            continue;
        }

        const GameScreen screen = analyzer.screen(record);
        ++totals.games;
        totals.plies += screen.total_plies;
        totals.candidates += screen.candidates;
        totals.noise += screen.noise;
        totals.opening += screen.opening;
        totals.replayed += screen.replayed;
        totals.flat += screen.flat;
        for (int r = 0; r < kScreenReasonCount; ++r) totals.reasons[r] += screen.reasons[r];

        if (!json_out) {
            std::cout << "game " << std::setw(3) << game_index << "  plies " << std::setw(3)
                      << screen.total_plies << "  candidates " << std::setw(3) << screen.candidates
                      << " (" << percent(screen.candidates, screen.total_plies) << ")"
                      << "  noise " << std::setw(3) << screen.noise << "  opening " << std::setw(2)
                      << screen.opening << "  replayed " << std::setw(3) << screen.replayed
                      << "  flat=" << (screen.flat ? "yes" : "no") << "\n";
        }

        for (const PlyScreen& ply : screen.plies) {
            if (!ply.candidate) continue;

            if (!review) {
                if (verbose && !json_out) {
                    std::cout << "    ply " << std::setw(3) << ply.ply
                              << (ply.side == BLACK ? " B" : " W") << "  " << std::left
                              << std::setw(11) << ply.method << std::right << "  score "
                              << std::setw(7) << ply.score << "  swing " << std::setw(7)
                              << ply.swing << "  depth " << std::setw(2) << ply.depth << "  "
                              << ReplayAnalyzer::reason_name(ply.reason) << "\n";
                }
                continue;
            }

            const PlyReview result = reviewer.review(record, ply.ply);
            kinds[result.kind] += 1;
            if (verbose && !json_out) {
                std::cout << "    ply " << std::setw(3) << ply.ply << "  " << std::left
                          << std::setw(15) << result.kind << std::right << "  best "
                          << cell_name(result.best) << " (" << std::setw(7) << result.best_score
                          << ")  played " << cell_name(result.played) << " (" << std::setw(7)
                          << result.played_score << ")  loss " << std::setw(7) << result.loss
                          << "  gap " << std::setw(7) << result.gap << "  roots "
                          << std::setw(2) << result.root_moves << "  " << result.method
                          << " d" << result.depth << "\n";
            }

            // 只有五种有训练价值的判定才产卡；unlisted / none 不产（§6.3.2）。
            const bool keep = result.kind == "blunder" || result.kind == "brilliant" ||
                              result.kind == "forced" || result.kind == "counterexample" ||
                              result.kind == "missed_mate";
            if (!keep) continue;

            Board position;
            if (!reviewer.position_before(record, ply.ply, position)) continue;
            const LadderResult ladder = reviewer.difficulty(position, result.side, result.best);

            TacticsCard card;
            card.id = "g" + std::to_string(game_index) + "-p" + std::to_string(ply.ply);
            card.kind = result.kind;
            card.side = result.side;
            card.ply = ply.ply;
            for (int i = 0; i < ply.ply && i < static_cast<int>(record.moves.size()); ++i) {
                card.stones.push_back(record.moves[i].move);
            }
            if (!card.stones.empty()) card.last_move = card.stones.back();
            card.answer = result.best;
            card.best_score = result.best_score;
            card.played_score = result.played_score;
            card.second_score = result.second_score;
            card.gap = result.gap;
            card.loss = result.loss;
            card.budget = result.budget;
            card.depth = result.depth;
            card.played = result.played;
            card.second = result.second;
            card.method = result.method;
            card.solve_budget = ladder.solve_budget;
            card.failed_at = ladder.failed_at;
            card.layer = TacticsCard::layer_for(ladder.solve_budget);
            card.coeffs_sha = fingerprint;
            card.source_game = replay_path;
            card.source_index = game_index;

            if (cards.is_open()) {
                cards << card.to_json() << "\n";
                cards.flush();
                ++cards_written;
            } else if (!cards_dir.empty()) {
                card_buffer.push_back(card);
            }
            if (verbose && !json_out) {
                std::cout << "        -> card " << card.id << "  layer " << card.layer
                          << "  solve_budget " << card.solve_budget << "\n";
            }
        }
        ++game_index;
    }

    // 目录模式：按 (表, 层) 分组落盘，再写一份 index.json。分组在内存里做，
    // 因为一次复盘的卡通常只有个位数，为它开一堆 ofstream 不划算。
    if (!cards_dir.empty() && !card_buffer.empty()) {
        namespace fs = std::filesystem;
        std::map<std::string, std::vector<const TacticsCard*>> by_table_layer;
        for (const TacticsCard& card : card_buffer) {
            const std::string table = card.kind == "missed_mate" ? "proven" : "judged";
            by_table_layer[table + "/" + card.layer].push_back(&card);
        }
        nlohmann::json index;
        index["schema"] = "tactics-index/1";
        index["generator"] = "analyze/1";
        index["coeffs_sha"] = fingerprint;
        nlohmann::json tables = nlohmann::json::object();
        for (const auto& entry : by_table_layer) {
            const fs::path path = fs::path(cards_dir) / (entry.first + ".jsonl");
            fs::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::app);
            if (!out) {
                std::cerr << "cannot append to " << path.string() << "\n";
                return 1;
            }
            for (const TacticsCard* card : entry.second) {
                out << card->to_json() << "\n";
            }
            tables[entry.first] = static_cast<int>(entry.second.size());
            cards_written += static_cast<int>(entry.second.size());
        }
        index["cards"] = tables;
        std::ofstream idx(fs::path(cards_dir) / "index.json");
        idx << index.dump(2) << "\n";
    }

    if (json_out) {
        std::cout << "{\"summary\":{\"games\":" << totals.games << ",\"plies\":" << totals.plies
                  << ",\"candidates\":" << totals.candidates << ",\"replayed\":" << totals.replayed
                  << ",\"flat_games\":" << totals.flat << ",\"parse_errors\":" << totals.parse_errors
                  << ",\"cards\":" << cards_written << ",\"kinds\":{";
        bool first = true;
        for (const auto& entry : kinds) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "\"" << entry.first << "\":" << entry.second;
        }
        std::cout << "}}}\n";
        return 0;
    }

    std::cout << "\n----------------------------------------------------------------------\n"
              << "games " << totals.games << "  plies " << totals.plies << "  candidates "
              << totals.candidates << " (" << percent(totals.candidates, totals.plies) << ")"
              << "  noise " << totals.noise << "  opening " << totals.opening << "  replayed "
              << totals.replayed << "  flat_games " << totals.flat << "  parse_errors "
              << totals.parse_errors << "\n";
    if (review) {
        std::cout << "kinds:";
        for (const auto& entry : kinds) std::cout << "  " << entry.first << " " << entry.second;
        std::cout << "\ncards written " << cards_written << " -> "
                  << (cards_dir.empty() ? cards_path : cards_dir) << "\n";
    }
    if (totals.games > 0) {
        std::cout << "candidates per game: "
                  << (static_cast<double>(totals.candidates) / totals.games) << "\n";
    }
    if (totals.plies > 0 && 100.0 * totals.candidates / totals.plies > 40.0) {
        std::cout << "WARN design doc 6.8 kill line 1: 筛出率 "
                  << percent(totals.candidates, totals.plies) << " > 40 % -> 阈值太松\n";
    }
    if (totals.games > 0 && totals.flat * 2 > totals.games) {
        std::cout << "WARN design doc 6.4: 超过一半的对局是 flat_game\n";
    }
    return 0;
}
