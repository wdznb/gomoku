#ifndef GOMOKU_PREVIEW_ARMS_H
#define GOMOKU_PREVIEW_ARMS_H

#include "../core/board.h"

#include <utility>
#include <vector>

namespace gomoku {

// 戊-2 的预览针臂（v2 §5 的「预览跑固定针臂」）。抽成头是因为 `test_server` 要逐子核对
// pos24 那一副与 `tests/pinned_positions.h` 的 `pinned_pos24()` 是同一摆法：生产码不引测试头
// （那等于把压测夹具链进服务端），所以两边各有一份定义，靠这条断言钉住不漂移。
// 臂序：quiet×{黑,白}、scattered×{黑,白}、pos24×黑。
const std::vector<std::pair<Board, int>>& preview_arms();

// 针臂的节点帽与时钟兜底：预览要能跑两遍给出同样的数字，所以**按节点定价**（指导书 §9.2）。
// 帽取 20k：一次预览五条臂的最坏总节点 ≤ 100k，深度不敏感时靠节点帽区分 —— d4 在同一批
// 针臂上每臂自然结束在 15k 附近（总 ~77k），d8 五条臂全部撞在 20k ⇒ 总 100k，d4 与 d8
// 仍然分得开；把帽再抬到 100k 会让 memcheck 下每条臂跑 200 s（valgrind × 20 慢），
// 一次预览 × 三次 = 1200 s 直接顶到 `valgrind_check.sh` 的 3600 s 时限。
// 墙取 5 min：**只是兜底**，不是定价那一半；把它调小到 5 s 会让「两次预览校验和一致」
// 那条抓的其实是墙而不是帽 —— valgrind 下 20k 节点要 20 s，5 s 墙在两条重复预览之间落在
// 不同的搜索阶段，读数就不再逐字相同。
constexpr int kPreviewNodeCap = 20000;
constexpr int kPreviewWallMs = 300000;

}  // namespace gomoku

#endif
