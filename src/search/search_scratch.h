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
// （`search_engine.cpp:291`、`ponder_search` 在 `:524`），所以那些槽每趟思考都从容量 0
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

}  // namespace gomoku

#endif  // GOMOKU_SEARCH_SCRATCH_H
