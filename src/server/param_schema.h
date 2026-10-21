#ifndef GOMOKU_PARAM_SCHEMA_H
#define GOMOKU_PARAM_SCHEMA_H

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace gomoku {

// 戊-2（v2 §5 与 §4.5.2、指导书 §4 戊-2）：面板内容 = 代码内容。
//
// 这一份表是**唯一**说得出「某个参数能取到哪儿、默认多少、什么时候生效」的地方：
// 服务端拿它校验 `config_set`，前端拿它渲染输入框，训练器拿它取 CLI 的界。
// 文档 §5 那张表改了而这里不改，就等于没有单一真源——所以这里的行**只列代码真的能动的**，
// 一条都不能是「面板上能改、代码里没人读」的那种装饰（指导书 戊-2 注意 3 的反面）。
//
// 三条口径：
// 1. **不截断、不静默改**：越界一律 `reject` 并点名那一条与它的区间（沿用轮次74/86 的哨兵风格）。
//    既有的 `set_config` 那条路保留它的 clamp（那是旧契约，§9.3 的「旧行为不变」由它自己的用例守着）；
//    严格校验只属于新的 `config_set`。
// 2. `when_live` 是三档而不是布尔：`live`（下一手就用得上）/ `next_think`（下一次思考起效）/
//    `restart`（要重启进程，面板禁用并写明）。档是从代码读的：`timeout_ms` 在每次 think 之前被
//    重新取一次所以是 live，`max_depth` 走的是同一个口子但会改变已排队的搜索所以标 next_think，
//    共享置换表的容量只能在建引擎之前定所以标 restart。
// 3. `suggest` 不是第四个真值：它是三档预设（保守/均衡/激进），滑到即用，不参与校验。

enum class ParamKind { Int, Double, Bool, Choice, Path };
enum class WhenLive { Live, NextThink, Restart };

struct ParamSpec {
    const char* key = "";
    const char* group = "";        // play / train / analyze / system（v2 §5 的四组）
    ParamKind kind = ParamKind::Int;
    double min_value = 0.0;        // Bool/Choice/Path 不用它
    double max_value = 0.0;
    double default_value = 0.0;    // Bool：0 或 1；Choice：候选下标
    const char* choices = "";      // 逗号分隔的候选（Choice 才用）
    const char* default_text = ""; // Choice/Path 的默认值文本
    double step = 1.0;             // 前端滑块的粒度，Path/Choice 不用
    const char* unit = "";         // 空 = 无量纲；面板把它跟在数值后面
    WhenLive when_live = WhenLive::NextThink;
    const char* desc_plain = "";   // 原样下发的白话（§5 的「白话」那一栏）
    const char* effect_note = "";  // 改它会付出什么（面板的「效果」那一行）
    const char* suggest = "";      // 三档预设，逗号分隔
};

// 校验结论：ok = false 时 `problem` 是一句能直接下发给人看的话，里面带着那条的区间。
struct ParamCheck {
    bool ok = false;
    std::string problem;      // 人话一句，进 `config_reject` 的 `why`
    double numeric = 0.0;     // 归一化后的数值（Bool/Choice 也给出：0/1 与下标）
    std::string text;         // Path/Choice 的文本值
};

class ParamRegistry {
public:
    // 整张表：顺序就是面板的渲染顺序（组内按 v2 §5 的行序）。
    static const std::vector<ParamSpec>& all();
    // 找不到返回 nullptr——调用方必须把「这条参数不存在」与「这条越界」分开报，
    // 否则前端收到一句「超出范围」会去检查一个根本不存在的旋钮。
    static const ParamSpec* find(const std::string& key);
    // 按 §5 的四组过滤（group 为空 = 全都要）。
    static std::vector<const ParamSpec*> in_group(const std::string& group);
    // 值必须与 `kind` 同形且落在区间里；Choice 必须是候选之一；Path 必须非空。
    static const char* kind_name(ParamKind kind);
    static const char* when_live_name(WhenLive when);
    static bool check(const ParamSpec& spec, const nlohmann::json& value, ParamCheck& out);
    // 下发给前端的那份清单（§4.5.2 的 `config_schema` 帧的载荷）：每条都带满 8 个必填字段，
    // 少一个就是前端要回去硬编码——`test_server` 里有一条臂专门数这件事。
    static nlohmann::json schema_json();
};

}  // namespace gomoku

#endif
