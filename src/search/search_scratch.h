#ifndef GOMOKU_SEARCH_SCRATCH_H
#define GOMOKU_SEARCH_SCRATCH_H

#include "move_generator.h"

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace gomoku {

// 轮次213 给 alpha-beta 配的常驻缓冲，轮次217 把它的**属主**从「这一次思考」抬到
// 「这个引擎」：`AlphaBetaSearch` 是每次思考在 `SearchEngine::search()` 栈上新建的
// （`SearchEngine::search()` 与 `SearchEngine::ponder_search()`），所以那些槽每趟思考都从容量 0
// 重新几何扩容一次。实测（`/tmp/r218/hand_probe2.cpp`，20 局自对弈，59,850 次分配里）：
// `vector<Candidate>` 的两个扩容站点占 15,937 次 = 26.6 %，`pair<int,Move>` 打分表
// 首触占 5,344 次 = 8.9 %，输出表 `reserve` 首触占 2,099 次 = 3.5 % —— 按每趟思考折算
// 约 44 + 15 + 6 次，全都属于「同一批缓冲被反复从零预热」而不是搜索本身要付的钱。
//
// 这些缓冲里没有答案：每一张都是先整个覆写再读，槽号按 `ply` / `qdepth` 取，
// 号段互不相交（下面的注释就是轮次213 那份不变量），所以让它在思考之间活着不改变
// 任何排序、任何数值。默认情况下 `AlphaBetaSearch` 仍然自带一份（用例就是这么建的），
// 只有引擎主动注入时才换成引擎那一份 ⇒ 变的只有生命周期。
//
// 一个引擎为**每一条可能并发的搜索路径**各备一份：主思考一份、`ponder_search` 一份。
// 同一个引擎被两根线程同时搜（预搜线程与 io 线程）时，它们不会共用同一张表。
struct SearchScratch {
    // 与 search_state.h 的 kMaxPly 同一约定；静止层用不相交的号段
    // [kScratchPlySlots, kScratchPlySlots + kScratchQdepthSlots)。
    static constexpr int kPlySlots = 64;
    static constexpr int kQdepthSlots = 9;  // kMaxQdepth(8) + 1
    static constexpr int kTierSlots = kPlySlots + kQdepthSlots;

    std::array<CandidateSet, kTierSlots> tiers;
    std::array<std::vector<Move>, kPlySlots> flat;
    // `sort_moves` 的每一层要地方放它的 decorate 数组、重排结果与归并工作区。
    // decorate 与归并区成对，都是同一种 `pair<int, size_t>`。
    std::array<std::vector<std::pair<int, std::size_t>>, kPlySlots> sort_index;
    std::array<std::vector<Move>, kPlySlots> sort_reorder;
    std::vector<std::pair<int, std::size_t>> sort_merge;

    // 观察面：这张表目前**按住**多少字节。用例靠它钉「搜索确实写在被交出去的那份缓冲里」
    // （注入被忽略 ⇒ 这里还是 0 ⇒ 红），总账靠它报「把缓冲抬到引擎属主换来多少常驻」。
    std::size_t retained_bytes() const {
        std::size_t bytes = 0;
        for (const CandidateSet& set : tiers) {
            bytes += (set.forced.capacity() + set.threats.capacity() + set.vicinity.capacity() +
                      set.remote.capacity() + set.merge_scratch.capacity()) * sizeof(Candidate);
        }
        for (const auto& v : flat) bytes += v.capacity() * sizeof(Move);
        for (const auto& v : sort_index) bytes += v.capacity() * sizeof(std::pair<int, std::size_t>);
        for (const auto& v : sort_reorder) bytes += v.capacity() * sizeof(Move);
        bytes += sort_merge.capacity() * sizeof(std::pair<int, std::size_t>);
        return bytes;
    }
};


// 轮次223 #118 档五：战术层生成器（VCF 的冲四表、VCT 的攻击表/防守表）的槽表。
// 那三个对象是每趟思考在 `SearchEngine::search()` 里新建的（`SearchEngine::search()` 的 VCF / VCT 两段，与 `:465` 区段的 loss_probe），
// 所以槽表自带的容量每趟都归零重来：实测 20 局自对弈里 `pair<int,Move>` 打分表的首触扩容
// 占 5,344 次 = 该档残余的 8.9 %（`/tmp/r218/hand_probe2.cpp`，轮次218 表）。
// 与 SearchScratch 同一条路：缓冲区交给引擎，对象仍然每趟新建 —— 因为 `deadline_` /
// `nodes_searched_` 一旦跨趟活着，「上一趟装过期限」会让下一趟不带期限也立刻中止，那是语义决定。
struct TacticalSlots {
    std::vector<std::pair<int, Move>> scored;
    std::vector<Move> moves;
};
static constexpr int kTacticalDepthSlots = 24;   // 覆盖出厂值 vcf_max_depth_=20 / vct_max_depth_=12

// 轮次225 #118 档六：VCT 装配变例的三张线表（`best` / `worst` / `child`），与
// `VCTSearch::lines()` 的按深度分槽同一个族。过去它是 `VCTSearch` 的成员、跟着那个对象
// 每趟思考生灭 ⇒ 槽里的容量每趟从 0 重新几何扩容。实测（同一台具、确定性节点帽档
// `20 0 4 50000`，本轮树）：`vector<Move>::_M_range_insert`@`vct.cpp` 20 局共 2,226 次
// = 每局 111.3 次，是该档前六名里唯一**随工作量增长**的站点。
struct TacticalLines {
    std::vector<Move> best;
    std::vector<Move> worst;
    std::vector<Move> child;

    // 观察面，与 `SearchScratch::retained_bytes()` 同一条用途：用例靠它钉「线表确实写在
    // 被交出去的那份里」（注入被忽略 ⇒ 恒为 0 ⇒ 红）。
    std::size_t retained_bytes() const {
        return (best.capacity() + worst.capacity() + child.capacity()) * sizeof(Move);
    }
};

struct TacticalScratch {
    std::array<TacticalSlots, kTacticalDepthSlots> vcf;
    std::array<TacticalSlots, kTacticalDepthSlots> vct_attacks;
    std::array<TacticalSlots, kTacticalDepthSlots> vct_defenses;
    // 求招那条路（`SearchEngine::search()` 里 loss_probe 那一段）单独一份：它与主思考里
    // 那一趟 VCF 是顺序的，但两份就不必去论证「有没有哪天变成同时」。
    std::array<TacticalSlots, kTacticalDepthSlots> loss_probe;
    // 轮次225：VCT 的线表一份。`VCTSearch` 只在 `SearchEngine::search()` 的 VCT 那一段建，
    // `ponder_search()` 只跑 alpha-beta，所以可能同时活着的 VCT 对象只有一个 —— 与
    // `SearchScratch` 那边「每条路径一份」是同一条规矩，这里那条路径只有一个对象。
    std::array<TacticalLines, kTacticalDepthSlots> vct_lines;
};
}  // namespace gomoku

#endif  // GOMOKU_SEARCH_SCRATCH_H
