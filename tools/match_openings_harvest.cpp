// 整树赛开局集的采集器（轮次171，方案 C 的样本量那一半）。
//
// 为什么要有这台仪器：赛具的读数按「有效样本」算力量，而**有效样本的单位是一条开局×座位这一对，
// 不是局**（轮次170 的 null 对照里，两台相同引擎逐局同解 ⇒ 同一条开局反复跑不加信息）。6 条开局
// 撑不起方案 C 的 ≈384，所以要按 docs/ENGINE_REDESIGN.md 保障 1 的口径「模板 + 若干中盘快照」扩集。
// 快照不手工挑，从 `gomoku_train --records` 写的对局记录里按固定手数取，这样开局集和引擎实际会
// 走到的形状是同分布的。
//
// 取出来的开局只收「偶数子、黑先行、盘面还活着」的三件条件，全部不满足的记录逐条计数并报出来 ——
// 赛具那边（tools/tree_match.cpp 的 load_openings）对奇数子开局是直接拒不收的，两头口径要一致。
#include "core/board.h"
#include "core/types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using nlohmann::json;
using namespace gomoku;

namespace {

struct Options {
    std::vector<std::string> records_paths;
    std::string out_path;
    std::vector<int> plies;
    int per_band = 8;
    bool verbose = false;
};

std::vector<int> parse_plies(const std::string& text) {
    std::vector<int> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const int v = std::atoi(item.c_str());
        if (v > 0 && v < 225) out.push_back(v);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// 一条记录里取出的候选开局。key 是「同一盘面」的判据：格子按 (r,c,color) 排序拼串，
// 与落子顺序无关 —— 换坐重下的两条线会落到同一个键上，那正是我们要合掉的东西。
struct Candidate {
    std::string key;
    std::string doc;
    int plies = 0;
    unsigned long long tie = 0;
};

unsigned long long mix(const std::string& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

struct Stats {
    long records = 0, games_bad = 0, ply_hits = 0, dead = 0, parity = 0, dup = 0, kept = 0;
};

// 把 moves 数组按顺序摆到盘上；返回 false 表示这条记录本身摆不出合法盘面。
bool replay(const json& moves, Board& board, std::vector<std::pair<int, int>>& order,
            std::vector<std::string>& colors) {
    for (const auto& entry : moves) {
        if (!entry.contains("row") || !entry.contains("col") || !entry.contains("color")) return false;
        const int row = entry["row"].get<int>();
        const int col = entry["col"].get<int>();
        const std::string color = entry["color"].get<std::string>();
        const int c = color == "black" ? BLACK : (color == "white" ? WHITE : EMPTY);
        if (c == EMPTY || !board.is_empty(row, col)) return false;
        if (!board.place_stone(row, col, c)) return false;
        order.emplace_back(row, col);
        colors.push_back(color);
    }
    return true;
}

std::string doc_of(const std::vector<std::pair<int, int>>& prefix) {
    json moves_arr = json::array();
    for (const auto& p : prefix) moves_arr.push_back({p.first, p.second});
    json doc;
    doc["version"] = 1;
    doc["type"] = "gomoku_position";
    doc["first"] = "black";
    doc["moves"] = moves_arr;
    return doc.dump();
}

void harvest_path(const std::string& path, const Options& opt, std::vector<Candidate>& out,
                  std::set<std::string>& seen, Stats& st) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "读不到记录文件 %s\n", path.c_str());
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        ++st.records;
        json rec = json::parse(line, nullptr, false);
        if (rec.is_discarded() || !rec.contains("moves") || !rec["moves"].is_array()) {
            ++st.games_bad;
            continue;
        }
        Board board;
        std::vector<std::pair<int, int>> order;
        std::vector<std::string> colors;
        if (!replay(rec["moves"], board, order, colors)) { ++st.games_bad; continue; }

        for (int ply : opt.plies) {
            if (ply > static_cast<int>(order.size())) continue;
            ++st.ply_hits;
            if (ply % 2 != 0) { ++st.parity; continue; }
            // 黑先行的第 ply 手之后，最后一子是白 ⇒ 现在轮到黑。盘面上已经有人成五就别收：
            // 那种位置一 set_position 就局终，赛具会把它读成「一步都没下就结束」。
            const Board snapshot = [&] {
                Board b;
                for (int i = 0; i < ply; ++i)
                    b.place_stone(order[i].first, order[i].second, colors[i] == "black" ? BLACK : WHITE);
                return b;
            }();
            if (snapshot.check_winner() != EMPTY || snapshot.move_count() != ply) { ++st.dead; continue; }

            std::vector<std::string> cells;
            for (int i = 0; i < ply; ++i) {
                cells.push_back(std::to_string(order[i].first) + "," + std::to_string(order[i].second) +
                    ":" + colors[i]);
            }
            std::sort(cells.begin(), cells.end());
            std::string key;
            for (const auto& c : cells) key += c + ";";
            key += "|ply=" + std::to_string(ply);
            if (!seen.insert(key).second) { ++st.dup; continue; }

            std::vector<std::pair<int, int>> prefix(order.begin(), order.begin() + ply);
            Candidate cand;
            cand.key = key;
            cand.doc = doc_of(prefix);
            cand.plies = ply;
            cand.tie = mix(key);
            out.push_back(std::move(cand));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](std::string* out) {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s 缺参数\n", arg.c_str()); return false; }
            *out = argv[++i];
            return true;
        };
        std::string v;
        if (arg == "--records" && value(&v)) opt.records_paths.push_back(v);
        else if (arg == "--out" && value(&v)) opt.out_path = v;
        else if (arg == "--plies" && value(&v)) opt.plies = parse_plies(v);
        else if (arg == "--per-band") { if (!value(&v)) return 1; opt.per_band = std::atoi(v.c_str()); }
        else if (arg == "--verbose") opt.verbose = true;
        else if (arg == "--help" || arg == "-h") {
            std::printf("用法: %s --records FILE.jsonl [--records FILE.jsonl ...]\n"
                        "        [--plies 4,6,8,...] [--per-band N] [--out FILE.jsonl] [--verbose]\n",
                        argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "不认识的参数: %s\n", arg.c_str());
            return 1;
        }
    }
    if (opt.records_paths.empty()) { std::fprintf(stderr, "至少要一份 --records\n"); return 1; }
    if (opt.plies.empty()) opt.plies = parse_plies("4,6,8,10,12,14,16,18,20,24,28");

    std::vector<Candidate> all;
    std::set<std::string> seen;
    Stats st;
    for (const auto& p : opt.records_paths) harvest_path(p, opt, all, seen, st);

    // 每个手数档里按哈希取前 N 个：确定性的伪随机抽样，既不手工挑局面，也不让开局集被
    // 某一段连续的同源局面占满。
    std::vector<Candidate> kept;
    for (int ply : opt.plies) {
        std::vector<Candidate> band;
        for (auto& c : all) if (c.plies == ply) band.push_back(c);
        std::sort(band.begin(), band.end(), [](const Candidate& a, const Candidate& b) {
            return a.tie < b.tie;
        });
        if (opt.per_band > 0 && static_cast<int>(band.size()) > opt.per_band)
            band.resize(static_cast<size_t>(opt.per_band));
        kept.insert(kept.end(), band.begin(), band.end());
        if (opt.verbose) std::fprintf(stderr, "  档 %2d 子：候选 %zu 取 %zu\n", ply, band.size(), band.size());
    }
    st.kept = static_cast<long>(kept.size());

    std::ofstream out(opt.out_path);
    if (!out) { std::fprintf(stderr, "写不出 %s\n", opt.out_path.c_str()); return 2; }
    for (const auto& c : kept) out << c.doc << "\n";
    std::printf("记录=%ld 摆不出的局=%ld 命中档=%ld 奇数子拒收=%ld 已死盘=%ld 重复盘面=%ld ⇒ 开局=%ld 条\n",
                st.records, st.games_bad, st.ply_hits, st.parity, st.dead, st.dup, st.kept);
    return 0;
}
