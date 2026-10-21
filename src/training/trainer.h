#ifndef GOMOKU_TRAINER_H
#define GOMOKU_TRAINER_H

#include "../core/evaluator.h"
#include "../search/search_engine.h"
#include "data_collector.h"
#include "elo.h"
#include "game_record.h"
#include "genetic.h"
#include "hill_climbing.h"
#include "sprt.h"
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace gomoku {

enum class OptimizerKind {
    GENETIC,
    HILL_CLIMBING,
};

struct TrainingConfig {
    // Rounds to run when the CLI is not given --rounds. Each round optimises and
    // then verifies one candidate.
    int rounds = 3;

    // How hard the engines think during training matches. Everything in a
    // training round is multiplied out by population x games x plies, so these
    // are the knobs that decide how long a run takes.
    int search_depth = 4;
    int timeout_ms = 400;
    // Nodes one think may spend during training matches. Zero keeps today's cost
    // model, where the ceiling is what the remaining milliseconds derive. A named
    // value makes a ply cost the same on an idle and a loaded machine, and
    // timeout_ms then only backs the think up if it runs long.
    int max_nodes = 0;
    int max_moves = 60;
    bool resign_enabled = true;

    OptimizerKind optimizer = OptimizerKind::GENETIC;
    int population_size = 8;
    int generations = 4;
    int stagnation_generations = 3;
    int hill_climbing_iterations = 12;

    // Games played to score one candidate, and games played before a candidate
    // is allowed to replace the champion. Both are rounded up to an even count
    // because every pairing is played once with each side.
    int fitness_games = 2;
    int verification_games = 8;
    // A candidate must beat the champion by this score to be promoted.
    double accept_score = 0.56;
    // 丁-1（v2 §2.1 S3）：适配度里连续分量的权重 w。默认 0 就是今天的形状。
    // 它只进**优化器看到的那个数**（`measure_fitness`）；验证赛那条 SPRT 序列永远是纯胜负——
    // 把小数喂给一个按胜率写的顺序检验，_promote_ 判据就不再是它声称的东西了。
    double fitness_margin_weight = 0.0;
    // The Elo gap a match's results are tested against (design doc 2.8 #3). Both
    // doors above are ceilings now: a match stops at a pair boundary as soon as
    // the sequential test leaves the bounds, so an eight-game verification costs
    // four games when the candidate loses both colours twice. 0 plays every pair
    // the counts ask for, which is what both flags meant before. sprt.h carries
    // the arithmetic that makes 300 the smallest default the ceiling can reach.
    int sprt_elo = 300;
    // Plies of a fixed opening every match pair replays before it starts thinking
    // (design doc 2.8 #4). Each pair takes one position from a fixed set and plays it
    // once with each colour, which cancels that position's first-move advantage inside
    // the pair and lets different pairs probe different positions.
    //
    // The shipped default is 0 - the mechanism is armed but off - because the doc's
    // promise ("let the coefficients, not the first move, decide") measures backwards
    // on this engine. Screened at depth 4 / 400 ms with both engines holding the same
    // coefficients, 45 of the first 48 four-ply lines end with one colour winning every
    // game: at that strength a four-stone position is usually already a forced win, so
    // the colour swap does not turn the pair into a coefficient test, it turns the
    // forced win into a pair that scores exactly 0.5 whoever is better. The full run
    // agrees - plies 4 raised the share of fitness matches pinned at 0.5 from the
    // control's 32.5 % to 38.8 % and the colour-decided pair share to 66.9 %. 0 is the
    // pre-#4 contract and the arm those readings are compared against; a named count is
    // the way to probe a position class the screen has not ruled out yet.
    //
    // When it is armed, four plies is the floor, not a guess: legal_moves() walks only
    // the cells near a stone, so with BLACK on the centre the reply is 24 neighbourhood
    // cells folded into 5 symmetry classes - the generator's whole 2-ply vocabulary -
    // and the eight set indices in fact land on 4 of them. At four plies the eight
    // indices give eight positions that stay distinct after folding the symmetries.
    int match_opening_plies = 0;

    // Self-play games generated per round purely to collect training data.
    int data_games = 4;
    int data_threads = 2;
    int data_opening_plies = 2;
    bool collect_data = true;
    // Slots in the one transposition table every data worker shares (design doc 2.8
    // #1's 进程级共享 TT；0 turns sharing off and each worker keeps its own). The
    // default is a worker's own table size, 2^18 x 28B ~= 7.3MB, because that is what
    // makes the claim true rather than bigger: sharing at this size costs 7.3MB once
    // instead of 7.3MB per --threads, and the doc's 2^22 example would need 16
    // concurrent workers before it beat the tables it replaces. Naming a larger number
    // buys hit rate on a machine with the memory for it.
    int shared_tt_entries = 1 << 18;

    // 丁-2（v2 §3.4 S4）：深搜蒸馏。每一手在学生think完之后、落子之前，让一个更贵的
    // 老师也think一次，两者的答案分开记（`MoveRecord::teacher_*`）。
    // **teacher_depth = 0 是缺省，也就是关**：一局都不多搜一次，样本、记录、每轮对局数
    // 逐字节是 S4 之前的形状。开的时候它只进数据采集那几局——适配度与验证赛的对局不蒸馏，
    // 因为那两处的分数是判据，不是数据。
    // 老师的预算是节点数（与 D-1 同一口径：让一手在空机与满载机上花一样的钱），
    // 不是毫秒。深度差 k 倍就应当看到每轮对局数掉 k 倍上下——那是杀线要量的读数。
    int teacher_depth = 0;
    int teacher_max_nodes = 0;

    // 丁-6（v2 §3.2 S2）：开局集按双方胜机筛选。**0 = 关**（缺省），这时一条筛选局
    // 都不跑，开局仍是上面那套固定 8 条轮换（`match_opening_line`），逐字节是今天的样子。
    // 开了就是「跑之前先花 2×N 局问一遍：这条开局能不能区分这两个玩家」，留下的才用；
    // 结果按 sha 落盘复用（`openings_path`），系数换代自动重筛。
    int opening_screen_candidates = 0;
    std::string openings_path = "data/training/openings.json";

    uint64_t seed = 20261011;

    std::string records_path;
    std::string samples_path;
    std::string params_path;
    // Written at the end of every round so an interrupted run can be resumed.
    std::string checkpoint_path;
};

struct TrainerRound {
    int round = 0;
    // 丁-3：一轮的双方各是一对系数（`CoefficientPair`），不是一套。两侧同值时
    // 这就是改之前的那个形状。
    CoefficientPair champion;
    CoefficientPair candidate;
    double champion_fitness = 0.0;
    double candidate_fitness = 0.0;
    double verification_score = 0.0;
    // What the sequential test had to say about the match that produced that
    // score, and how many games it needed to say it. OPEN means the ceiling was
    // reached with no decision and --accept-score was the rule that fired; the
    // game count is the match's own, not the run's running total.
    SprtVerdict verification_verdict = SprtVerdict::OPEN;
    double verification_llr = 0.0;
    int verification_match_games = 0;
    bool accepted = false;
    int games_played = 0;
    int samples_collected = 0;
    double champion_elo = 0.0;
    double candidate_elo = 0.0;
};

// One match's answer. `score` is what the optimisers rank candidates on and what
// the Elo table books; the three fields after it are the sequential test's own
// reading of the same games, kept so a round can show *why* a candidate was
// promoted instead of only the fraction that cleared the bar.
struct MatchOutcome {
    double score = 0.0;
    int games = 0;
    SprtVerdict verdict = SprtVerdict::OPEN;
    double llr = 0.0;
};

// 3.6 training main loop: optimise coefficients, prove the result over the table
// by playing it against the champion, keep the ratings, and bank games plus
// samples for later analysis.
class Trainer {
public:
    using LogFn = std::function<void(const std::string& line)>;

    Trainer(const ThreatCoefficients& base, const TrainingConfig& config);
    // 丁-3：候选是一对系数。两侧同值的那一对与改之前的一套是同一个形状。
    Trainer(const CoefficientPair& base, const TrainingConfig& config);

    // Score coefficient sets without playing: used by the test suite and by any
    // tool that optimises a different objective. When set, verification becomes
    // a direct comparison instead of a match.
    void set_fitness_override(CoefficientFitness fitness);
    void set_log_callback(LogFn callback) { log_ = std::move(callback); }

    TrainerRound run_round();
    std::vector<TrainerRound> run(int rounds);

    const CoefficientPair& champion() const { return versions_[best_version_]; }
    const std::vector<CoefficientPair>& versions() const { return versions_; }
    const EloSystem& ratings() const { return elo_; }
    size_t samples_collected() const { return samples_collected_; }
    int games_played() const { return games_played_; }
    int rounds_completed() const { return rounds_completed_; }
    const TrainingConfig& config() const { return config_; }

    // The table the data workers are handed, or nullptr when --shared-tt 0 left each
    // worker with its own. Reading it from outside is how a test tells a shared table
    // from a table that merely happens to be the same size.
    TranspositionTable* shared_transposition_table() const { return shared_tt_.get(); }

    // One-line progress summary, used by the CLI and by resume messages.
    std::string status_text() const;

    // A short random opening so parallel workers do not all replay one line.
    static std::vector<Move> random_opening_line(int plies, uint64_t seed);

    // How many distinct openings a match rotates through. Pairs index into the set
    // by (round + pair), so a candidate measured in the same round as the champion
    // is measured on the same positions, and the four pairs a
    // --verification-games 8 match plays each get a different opening. Eight is a
    // budget, not a ceiling: at four plies the generator keeps producing new
    // positions well past it (16 indices land on 16 classes even after folding the
    // symmetries). A small set is chosen so a long run comes back to positions it
    // has already measured instead of never measuring one twice.
    static constexpr int kMatchOpeningSet = 8;

    // The opening one match pair is played from, fixed by its index in that set:
    // legal by construction (it is generated move by move out of the same
    // legal_moves() the replay validates against) and identical for every run.
    static std::vector<Move> match_opening_line(int index, int plies);

    bool save_checkpoint(const std::string& path, std::string& error) const;
    bool load_checkpoint(const std::string& path, std::string& error);

private:
    double measure_fitness(const CoefficientPair& coefficients);
    // `margin_weight` 是丁-1 的连续分量权重：> 0 时每一手胜负被换成
    // 「胜负 × (1-w) + 连续分量 × w」，= 0（默认，也是验证赛那一侧）逐字节回到今天。
    MatchOutcome score_match(const CoefficientPair& candidate,
                             const CoefficientPair& reference, int games,
                             double margin_weight = 0.0);
    double play_match(const CoefficientPair& candidate, const CoefficientPair& reference,
                      int games, double margin_weight = 0.0);
    // 一局棋「赢得多漂亮」：三个读数都从已有产物取——终局的分差（赢方最后一手与输方
    // 最后一手各自报出的搜索分）、用了多少手、认输时两侧的搜索深度差。赢方视角，折进 [0,1]；
    // 未分胜负与作废记录给中性 0.5。
    static double game_margin(const GameRecord& record, int max_moves);
    // 混合本身单列成一个纯函数：`w = 0` 必须**逐字**回到胜负，这一句话值得被单独钉住
    //（指导书 §4 丁-1 实现思路 3 的后半）。
    static double blend_fitness(double binary, double margin, double weight);
    // Score for whoever holds the stones passed as `black_owner` (1 = the
    // candidate, 2 = the reference). `played` says whether the game happened at
    // all: a discarded INVALID one is not a draw and leaves the denominator.
    // Both games of a pair get the same `opening`, once with each colour.
    double play_game(SearchEngine& black, SearchEngine& white, int black_owner,
                     const std::vector<Move>& opening, bool* played = nullptr,
                     double margin_weight = 0.0);
    SearchEngine& prepare(SearchEngine& engine) const;
    int generate_data_games(int games, int threads);

    // 这一场配对局从哪条开局起。关着筛选（缺省）时就是 `match_opening_line(index,
    // config_.match_opening_plies)`，一个字的差别都没有。
    std::vector<Move> match_opening_for(int index);
    // 第一次需要开局时跑一次筛选（或从落盘读回来），整个 run 只付一次成本。
    void ensure_openings_screened();
    // 筛出来留下的开局；少于 `kMatchOpeningSet` 条时视为「筛不出可用的集」，
    // 退回固定轮换（指导书 §4 丁-6 杀线的后半段）。
    std::vector<std::vector<Move>> screened_openings_;
    bool openings_screen_done_ = false;

    TrainingConfig config_;
    std::vector<CoefficientPair> versions_;
    size_t best_version_ = 0;
    EloSystem elo_;
    CoefficientFitness fitness_override_;
    LogFn log_;
    // Built once for the whole Trainer when --shared-tt names a size, so the flag that
    // decides whether a probe takes a shard lock is set before any worker exists.
    std::unique_ptr<TranspositionTable> shared_tt_;
    // 丁-2：老师专用的一张表，与上面那张**不是同一张**（指导书 §4 丁-2 注意 1）。
    // 每个 worker 自己的学生引擎共享 shared_tt_，它们的老师共享这一张——
    // 同一份系数、同一个深度口径，所以共享它不改变任何一手的读数，
    // 而 d6 的条目永远不会被随后的 d4 搜索切走。
    std::unique_ptr<TranspositionTable> teacher_tt_;
    int games_played_ = 0;
    size_t samples_collected_ = 0;
    int rounds_completed_ = 0;
};

// v2 §3.2 S2（指导书 §4 丁-6）：开局集按双方胜机筛选。
//
// 一句话：训练赛的一局棋要能**区分两个玩家**才有资格进训练集。一条开局在两局里（候选分别
// 坐黑、坐白）如果都是同一个座位赢，那一对的局分恒等于 0.5——谁强看不出来。留下来的正是
// 「两个座位各赢一局」的那部分。
//
// 为什么不是指导书思路 2 那条判据：那句是「双方胜率都落在 [0.25, 0.75]」，量的是同一座位跑
// K 局的胜率。轮次160 实测（`tools/opening_rule_screen.cpp --repeat 2`：8 条开局 × 两座位 ×
// 2 次重复 = 32 局，出现不同 = 0）：**同一局面对同一座位的重复局逐字相同**——引擎是确定性
// 的，所以每色的 K 局胜率只能取 0 或 1，那条判据永远点不着。指导书 §4 丁-6 注意 2 自己写了
// 怎么办：「筛选判据选错时，**换判据而不是放宽阈值**」。换完的判据就是上面那句，且它只需要
// 每条开局两局（K = 1），正是 注意 1「K 与候选数要小，否则筛选成本超过省下的训练局数」要的。
//
// 起跑线与出口（同一轮实测）：真实一对（缺省系数 vs `data/training/best_params.json`，两者只
// 差 `cand_width_root` 24/15）在 48 条三面开局上是 38 通吃 / 9 留下（18.8 %）/ 1 未分胜负，
// 成本 96 局一次、按 sha 落盘复用。留下的开局数 9 ≥ `Trainer::kMatchOpeningSet`，够用；
// 若某一代筛出 < 8 条，`Trainer` 会退回今天那套固定 8 条轮换并说明原因（思路的杀线后半段）。
//
// 缺省 `candidates = 0` 就是**关**：一条筛选局都不跑，开局仍是 `match_opening_line` 那 8 条。
struct OpeningScreenConfig {
    int candidates = 0;      // 0 = 关
    int stones = 3;          // 每条候选几颗子（第 1 手被 random_opening_line 钉在天元）
    int depth = 4;
    int max_nodes = 1500;    // 判定用节点帽，时限只防死循环
    int max_moves = 60;
    int seed_stride = 1000;  // 第 i 条候选的 seed = seed_stride + i
    std::string path = "data/training/openings.json";
};

struct OpeningReading {
    std::vector<Move> line;
    // 两局都以**座位**计：+1 黑座位胜、-1 白座位胜、0 未分胜负、2 作废。
    int as_candidate_black = 0;  // 候选坐黑（开场方是它）
    int as_candidate_white = 0;  // 候选坐白（开场方是对方）
    int plies = 0;               // 两局手数之和，只做诊断
    int games = 0;               // 真跑完的局数（作废的不算）
    bool valid = false;          // 两局都跑完且都分了胜负
    bool seat_decided = true;    // 同一座位赢下两局 ⇒ 这一对恒为 0.5，该剔
    bool kept = false;

    // 座位不决定结果 = 两个座位各赢一局 = 这一对能区分两个玩家。
    bool discriminates() const { return valid && as_candidate_black != as_candidate_white; }
};

struct OpeningScreenReport {
    OpeningScreenConfig config;
    std::string set_sha;   // 四半套系数 + 档位的指纹：落盘复用的键
    std::vector<OpeningReading> readings;
    int swept_before = 0;  // 筛前被座位决定的对数
    int swept_after = 0;   // 留下的里面还剩几个（判据正确时应为 0）
    int kept = 0;
    int games_played = 0;
    bool reused = false;   // true = 从落盘读回来的，一局没跑

    std::vector<std::vector<Move>> lines() const;
};

class OpeningScreen {
public:
    // 指纹覆盖两个玩家各自的两侧系数与档位。筛选结论是**这一对玩家**的相对强弱决定的，
    // 换任一方的系数或换档，旧结果就不作数——与题卡的 `coeffs_sha` 同一个道理。
    static std::string set_sha(const CoefficientPair& candidate, const CoefficientPair& reference,
                               const OpeningScreenConfig& config);

    // sha 不匹配时返回 false 并在 error 里说清「是哪一套键筛的 vs 现在这套」，
    // 让调用方知道重筛的成本从哪来。
    static bool load(const std::string& path, const std::string& sha, OpeningScreenReport& out,
                     std::string& error);
    static bool save(const OpeningScreenReport& report, std::string& error);

    // 跑一遍：每条候选两局。`error` 只在真正跑不动时非空。
    static OpeningScreenReport run(const CoefficientPair& candidate, const CoefficientPair& reference,
                                   const OpeningScreenConfig& config, std::string& error);

    // 一句人话摘要，进训练日志与 `train_status`。
    static std::string summarize(const OpeningScreenReport& report);
};

}

#endif
