#include "param_schema.h"

#include <cmath>

namespace gomoku {

namespace {

// 这张表就是 v2 §5 的「面板内容 = 代码内容」那一节能落地的那一半。
//
// 只列**代码真的会读**的行，每行都在注释里写清读它的那一处（file:line 是按本轮读到的位置写的，
// 改动那些文件时要把行号一起核对——指导书 戊-2 注意 3：表格改而代码不改等于没有单一真源，
// 反过来说，把没有代码的行登记进来同样是假真源）。
//
// 区间一律取**代码实际允许的**那一个，而不是文档写的那一个；两者不一致的行在下面逐条标明，
// 这些不一致本轮都写进了指导书 戊-2 的状态行（文档要跟着代码改，不是代码跟着文档许愿）。
const ParamSpec kRegistry[] = {
    // —— 组一：人类对局（服务端每次 think 之前重新取，所以都是 live / next_think）——
    {"max_depth", "play", ParamKind::Int, 2, 20, 8, "", "", 1, "层", WhenLive::NextThink,
     "想让 AI 想多深。越大越强也越慢；时间给不够时会中途停，界面会告诉你停在第几层。",
     "深度 +1 ≈ 每手节点数乘 3～6：一次 think 变贵，但不会改变已经排队的下一次。",
     "6,8,12"},
    {"timeout_ms", "play", ParamKind::Int, 1000, 30000, 16000, "", "", 500, "ms", WhenLive::Live,
     "AI 一手最多想几秒。到点就停，用已经想好的最好的那手。",
     "只改变时间预算，不改变棋力上限——深度不够时它本来就到不了底。",
     "4000,16000,30000"},
    {"soft_time_ms", "play", ParamKind::Int, 0, 30000, 0, "", "", 100, "ms", WhenLive::Live,
     "软时限：到这一点就不再往下深搜，留余地收尾，防连续超时。0 = 由硬时限的 65 % 自动推。",
     "设得越接近硬时限，越像「没有软时限」；超过硬时限的那一半会被代码按硬时限收住"
     "（game_manager.cpp 的 clamp_soft_time_ms）。",
     "0,6500,12000"},
    {"max_nodes", "play", ParamKind::Int, 0, 2000000, 0, "", "", 1000, "节点", WhenLive::NextThink,
     "一手最多搜多少个节点。给正数时每手工作量定死，机器忙也稳定；0 = 只按时钟。",
     "节点帽是**成本尺**不是强度尺：定死之后两次 think 可比，代价是忙的时候想得更浅。",
     "50000,0,0"},
    {"ponder_percent", "play", ParamKind::Int, 0, 100, 0, "", "", 5, "%", WhenLive::Restart,
     "等你落子时让它先猜一手，猜中下一手几乎免费，费的是等待时的电。",
     "每次最多花 20 万节点（≈0.88 s 单核）；缺省 **0**（文档 §5.1 建议 25，但那是建议值，"
     "代码仍是关着的——本轮按代码登记）。它只在启动时读一次（`main.cpp` 的 `--ponder` 一路传到 "
     "`GameServer`），所以这一条要重启，面板禁用并写明。",
     "0,25,50"},

    // —— 组三：复盘（analyze_request 读的那三条；§5.3 其余几行没有代码入口，本轮不登记）——
    {"analyze_depth", "analyze", ParamKind::Int, 2, 16, 8, "", "", 1, "层", WhenLive::Live,
     "复盘时每一手重算多深。越深越准，一局也越久。",
     "复盘是按手付费的：一局 60 手 × 深度 +1，成本是乘法。",
     "6,8,12"},
    {"analyze_budget", "analyze", ParamKind::Int, 1000, 500000, 20000, "", "", 1000, "节点",
     WhenLive::Live,
     "复盘时每一手最多花多少节点。定死节点而不是时钟，才能让两次复盘可比。",
     "这条是「可比」的来源：按时钟复盘会在机器忙时给出另一份答案。",
     "8000,20000,60000"},
    {"analyze_max_candidates", "analyze", ParamKind::Int, 1, 40, 12, "", "", 1, "个",
     WhenLive::Live,
     "一局最多标几个候选，控制报告长度。",
     "调高只是报告更长，不会让判断变准——难度阶梯也按这一条封顶。",
     "6,12,24"},

    // —— 组四：系统 / 资源（重启才生效的那一类，面板要禁用并写明）——
    {"reserved_cores", "system", ParamKind::Int, 1, 4, 3, "", "", 1, "核", WhenLive::Restart,
     "留给服务和正在思考的玩家的核数，训练只用剩下的那些。",
     "缺省是 **3**，不是文档 §5.4 写的 2：轮次163 在这台 6 核虚机上量到 2/3 份并发保住单进程 "
     "nps 的 100/96 %、4 份只剩 88 %，配额取 3 才站在 90 % 这一侧。机器核少就别调高。",
     "1,3,3"},
    {"shared_tt_bits", "system", ParamKind::Int, 10, 22, 18, "", "", 1, "2^n 槽",
     WhenLive::Restart,
     "训练共享置换表的容量（2^n 槽）。越大命中越多、内存越大。",
     "18 实测 ≈ 7.3 MB/worker；上限按代码是 **22**（main_trainer 拒 2^22 以上），"
     "文档 §5.4 的 24 比代码允许的更高，本轮按代码登记。",
     "16,18,20"},
};

const char* const kKindNames[] = {"int", "double", "bool", "choice", "path"};
const char* const kWhenLiveNames[] = {"live", "next_think", "restart"};

bool nearly_integral(double value) {
    return std::fabs(value - std::round(value)) < 1e-9;
}

}  // namespace

const std::vector<ParamSpec>& ParamRegistry::all() {
    static const std::vector<ParamSpec> table(kRegistry, kRegistry + sizeof(kRegistry) / sizeof(kRegistry[0]));
    return table;
}

const ParamSpec* ParamRegistry::find(const std::string& key) {
    for (const ParamSpec& spec : all()) {
        if (key == spec.key) return &spec;
    }
    return nullptr;
}

std::vector<const ParamSpec*> ParamRegistry::in_group(const std::string& group) {
    std::vector<const ParamSpec*> out;
    for (const ParamSpec& spec : all()) {
        if (group.empty() || group == spec.group) out.push_back(&spec);
    }
    return out;
}

const char* ParamRegistry::kind_name(ParamKind kind) {
    const int index = static_cast<int>(kind);
    if (index < 0 || index >= static_cast<int>(sizeof(kKindNames) / sizeof(kKindNames[0]))) return "int";
    return kKindNames[index];
}

const char* ParamRegistry::when_live_name(WhenLive when) {
    const int index = static_cast<int>(when);
    if (index < 0 || index >= static_cast<int>(sizeof(kWhenLiveNames) / sizeof(kWhenLiveNames[0]))) {
        return "next_think";
    }
    return kWhenLiveNames[index];
}

namespace {

std::vector<std::string> split_choices(const std::string& joined) {
    std::vector<std::string> out;
    std::string current;
    for (char c : joined) {
        if (c == ',') {
            if (!current.empty()) out.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

}  // namespace

bool ParamRegistry::check(const ParamSpec& spec, const nlohmann::json& value, ParamCheck& out) {
    out = ParamCheck();
    // 拒绝话术里要有区间，而且整数行不许写成「2.000000」：面板把这句话原样给人看。
    const bool integral = spec.kind == ParamKind::Int || spec.kind == ParamKind::Bool;
    auto fmt = [integral](double v) {
        return integral ? std::to_string(static_cast<long long>(v)) : std::to_string(v);
    };
    const std::string range = std::string("[") + fmt(spec.min_value) + ", " +
                              fmt(spec.max_value) + "]";
    switch (spec.kind) {
        case ParamKind::Int: {
            if (!value.is_number_integer() && !value.is_number_unsigned()) {
                out.problem = std::string("`") + spec.key + "` 需要整数，收到的是 " + value.type_name();
                return false;
            }
            // 64 位存整数而 `get<int>` 只做截断：参数文件里那条 (`five: 4294967396` 读成 100) 的
            // 教训在这里同样成立，所以先比 double 再比区间，截断的那个数根本不配进区间判断。
            const double as_double = value.get<double>();
            if (!nearly_integral(as_double)) {
                out.problem = std::string("`") + spec.key + "` 需要整数，收到的是带小数的 " +
                              std::to_string(as_double);
                return false;
            }
            if (as_double < spec.min_value || as_double > spec.max_value) {
                out.problem = std::string("`") + spec.key + "` 超出范围 " + range;
                return false;
            }
            out.numeric = as_double;
            out.ok = true;
            return true;
        }
        case ParamKind::Double: {
            if (!value.is_number()) {
                out.problem = std::string("`") + spec.key + "` 需要数值";
                return false;
            }
            const double as_double = value.get<double>();
            if (as_double < spec.min_value || as_double > spec.max_value) {
                out.problem = std::string("`") + spec.key + "` 超出范围 " + range;
                return false;
            }
            out.numeric = as_double;
            out.ok = true;
            return true;
        }
        case ParamKind::Bool: {
            if (!value.is_boolean()) {
                out.problem = std::string("`") + spec.key + "` 需要 true/false";
                return false;
            }
            out.numeric = value.get<bool>() ? 1.0 : 0.0;
            out.ok = true;
            return true;
        }
        case ParamKind::Choice: {
            if (!value.is_string()) {
                out.problem = std::string("`") + spec.key + "` 需要候选之一";
                return false;
            }
            out.text = value.get<std::string>();
            const std::vector<std::string> choices = split_choices(spec.choices);
            for (size_t i = 0; i < choices.size(); ++i) {
                if (choices[i] == out.text) {
                    out.numeric = static_cast<double>(i);
                    out.ok = true;
                    return true;
                }
            }
            out.problem = std::string("`") + spec.key + "` 的取值不在候选里（可选：" + spec.choices + "）";
            out.text.clear();
            return false;
        }
        case ParamKind::Path: {
            if (!value.is_string() || value.get<std::string>().empty()) {
                out.problem = std::string("`") + spec.key + "` 需要一个非空路径";
                return false;
            }
            out.text = value.get<std::string>();
            out.ok = true;
            return true;
        }
    }
    out.problem = std::string("`") + spec.key + "` 的类型不认识";
    return false;
}

nlohmann::json ParamRegistry::schema_json() {
    nlohmann::json items = nlohmann::json::array();
    for (const ParamSpec& spec : all()) {
        nlohmann::json entry;
        // 八个必填字段：key / group / type / range / default / step / when_live / desc_plain。
        // 少任何一个，前端就得回去硬编码——`test_server` 那条数段数的臂就是钉这件事的。
        entry["key"] = spec.key;
        entry["group"] = spec.group;
        entry["type"] = kind_name(spec.kind);
        entry["min"] = spec.min_value;
        entry["max"] = spec.max_value;
        entry["default"] = spec.kind == ParamKind::Bool
                               ? nlohmann::json((spec.default_value != 0.0))
                               : (spec.kind == ParamKind::Choice || spec.kind == ParamKind::Path
                                      ? nlohmann::json(spec.default_text)
                                      : nlohmann::json(spec.default_value));
        entry["step"] = spec.step;
        entry["unit"] = spec.unit;
        entry["when_live"] = when_live_name(spec.when_live);
        entry["desc_plain"] = spec.desc_plain;
        entry["effect_note"] = spec.effect_note;
        entry["suggest"] = split_choices(spec.suggest);
        if (spec.kind == ParamKind::Choice) entry["choices"] = split_choices(spec.choices);
        items.push_back(entry);
    }
    nlohmann::json root;
    root["schema"] = "config_schema/1";
    root["params"] = items;
    return root;
}

}  // namespace gomoku
