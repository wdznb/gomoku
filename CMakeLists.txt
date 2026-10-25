cmake_minimum_required(VERSION 3.14)
project(gomoku VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        json
        URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.2.tar.gz
    )
    FetchContent_MakeAvailable(json)
endif()

find_package(Boost REQUIRED)
find_package(Threads REQUIRED)

# Engine as a unit: everything below the server. Both the trainer and the test
# targets build against this list, so a new core source is compiled everywhere.
set(GOMOKU_CORE_SOURCES
    src/core/board.cpp
    src/core/pattern.cpp
    src/core/evaluator.cpp
    src/core/transforms.cpp
    src/core/position_setup.cpp
    src/search/greedy.cpp
    src/search/vcf.cpp
    src/search/vct.cpp
    src/search/move_generator.cpp
    src/search/alphabeta.cpp
    src/search/search_engine.cpp
    src/search/controller.cpp
    src/search/time_manager.cpp
    src/optimization/transposition.cpp
    src/optimization/opening_book.cpp
)

# 复盘与题卡的解析器：段 A/B/C 与卡片格式。训练流要用它，服务端甲-3 的复盘面板也要用
# 它，所以单列一份，免得 server 为了两个类把整条训练流水线一起编进去。
set(GOMOKU_TACTICS_SOURCES
    src/training/replay_analyzer.cpp
    src/training/tactics_card.cpp
)

set(GOMOKU_TRAINING_SOURCES
    src/training/params.cpp
    src/training/game_record.cpp
    src/training/elo.cpp
    src/training/self_play.cpp
    src/training/data_collector.cpp
    ${GOMOKU_TACTICS_SOURCES}
    src/training/genetic.cpp
    src/training/hill_climbing.cpp
    src/training/trainer.cpp
    src/training/run_spool.cpp src/training/worker_mode.cpp
)
set(GOMOKU_SOURCES
    src/main.cpp
    ${GOMOKU_CORE_SOURCES}
    # Only the parameter file reader: the server plays with trained coefficients,
    # it does not train them.
    src/training/params.cpp
    # Sessions persist in the training record format, so the server owns that
    # reader/writer rather than keeping a second schema for the same moves.
    src/training/game_record.cpp
    # 复盘面板（甲-3）：段 A/B/C 的解析器，服务端自己跑，不走训练流水线。
    ${GOMOKU_TACTICS_SOURCES}
    src/server/server.cpp src/server/param_schema.cpp
    src/server/game_manager.cpp
    src/server/position_json.cpp
    src/server/session_store.cpp
)

add_executable(gomoku_server ${GOMOKU_SOURCES})

target_include_directories(gomoku_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(gomoku_server PRIVATE
    ${Boost_LIBRARIES}
    Threads::Threads
    nlohmann_json::nlohmann_json
)

if(UNIX)
    target_link_libraries(gomoku_server PRIVATE pthread)
endif()

if(MSVC)
    target_compile_options(gomoku_server PRIVATE /W4)
else()
    target_compile_options(gomoku_server PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

add_custom_command(TARGET gomoku_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/web $<TARGET_FILE_DIR:gomoku_server>/web
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:gomoku_server>/data
)

# Phase 3 trainer: self-play, parameter optimisation and data collection. Needs
# the engine but not the server, so it stays free of Boost.
add_executable(gomoku_train
    src/training/main_trainer.cpp
    ${GOMOKU_CORE_SOURCES}
    ${GOMOKU_TRAINING_SOURCES}
)

target_include_directories(gomoku_train PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gomoku_train PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
)

if(UNIX)
    target_link_libraries(gomoku_train PRIVATE pthread)
endif()

if(MSVC)
    target_compile_options(gomoku_train PRIVATE /W4)
else()
    target_compile_options(gomoku_train PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

add_custom_command(TARGET gomoku_train POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:gomoku_train>/data
)

# Replay analysis (design doc ENGINE_REDESIGN_V2.md 6.10). Stage A only for now:
# it reads records and screens plies. The engine is linked as one unit anyway,
# because stage B's fixed-budget re-search will need it and a hand-picked list
# here is the mistake test_search already records.
add_executable(gomoku_analyze
    tools/analyze_main.cpp
    ${GOMOKU_CORE_SOURCES}
    ${GOMOKU_TRAINING_SOURCES}
)

target_include_directories(gomoku_analyze PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gomoku_analyze PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
)

if(UNIX)
    target_link_libraries(gomoku_analyze PRIVATE pthread)
endif()

if(MSVC)
    target_compile_options(gomoku_analyze PRIVATE /W4)
else()
    target_compile_options(gomoku_analyze PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 位置项 / 贴边项的差分表（v2 §7 回归保障 3）。它不是测试目标，是一台仪器：跑一次
# 写 data/diff/position_term.csv，18,900 行逐格比对，任何一格不一致就非零退出。
add_executable(position_term_table
    tools/position_term_table.cpp
    ${GOMOKU_CORE_SOURCES}
)
target_include_directories(position_term_table PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(position_term_table PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(position_term_table PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(position_term_table PRIVATE /W4)
else()
    target_compile_options(position_term_table PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

enable_testing()

add_executable(test_board
    tests/test_board.cpp
    src/core/board.cpp
)
target_include_directories(test_board PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_board COMMAND test_board)

add_executable(test_pattern
    tests/test_pattern.cpp
    src/core/board.cpp
    src/core/pattern.cpp
)
target_include_directories(test_pattern PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_pattern COMMAND test_pattern)

add_executable(test_evaluator
    tests/test_evaluator.cpp
    src/core/board.cpp
    src/core/pattern.cpp
    src/core/evaluator.cpp
)
target_include_directories(test_evaluator PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_evaluator COMMAND test_evaluator)

add_executable(test_search
    tests/test_search.cpp
    # The engine as one unit, like the other engine-facing targets: a hand-copied
    # list here meant a new src/search source could be compiled into the server
    # and silently absent from the suite that tests it.
    ${GOMOKU_CORE_SOURCES}
    src/server/game_manager.cpp
)
target_include_directories(test_search PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_search PRIVATE
    GOMOKU_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_search PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
add_test(NAME test_search COMMAND test_search)

# The position loader spans three layers (core parsing, JSON/documents, the
# GameManager slot a loaded board lands in), so it links the whole engine rather
# than a hand-picked list.
add_executable(test_position
    tests/test_position.cpp
    ${GOMOKU_CORE_SOURCES}
    src/server/game_manager.cpp
    src/server/position_json.cpp
)
target_include_directories(test_position PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_position PRIVATE
    GOMOKU_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_position PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(MSVC)
    target_compile_options(test_position PRIVATE /W4)
else()
    target_compile_options(test_position PRIVATE -Wall -Wextra -Wno-template-body -Wno-template-id-cdtor)
endif()
add_test(NAME test_position COMMAND test_position)

# The session store is file I/O over one record schema, so it is tested against a
# real temporary directory rather than a mock.
add_executable(test_session
    tests/test_session.cpp
    src/server/session_store.cpp
    src/training/game_record.cpp
)
target_include_directories(test_session PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_session PRIVATE nlohmann_json::nlohmann_json)
if(MSVC)
    target_compile_options(test_session PRIVATE /W4)
else()
    target_compile_options(test_session PRIVATE -Wall -Wextra -Wno-template-body -Wno-template-id-cdtor)
endif()
add_test(NAME test_session COMMAND test_session)

# The server's session lifecycle - re-armed exports, zero-stone cleanup, retried
# flushes - lives behind private handlers, so this target drives GameServer
# directly with the white-box macro rather than opening sockets.
add_executable(test_server
    tests/test_server.cpp
    ${GOMOKU_CORE_SOURCES}
    src/training/params.cpp
    src/training/game_record.cpp
    # 复盘面板（甲-3）：段 A/B/C 的解析器，服务端自己跑，不走训练流水线。
    ${GOMOKU_TACTICS_SOURCES}
    src/server/server.cpp src/server/param_schema.cpp
    src/server/game_manager.cpp
    src/server/position_json.cpp
    src/server/session_store.cpp
)
target_include_directories(test_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Boost_INCLUDE_DIRS}
)
target_link_libraries(test_server PRIVATE
    ${Boost_LIBRARIES}
    Threads::Threads
    nlohmann_json::nlohmann_json
)
if(UNIX)
    target_link_libraries(test_server PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(test_server PRIVATE /W4)
else()
    target_compile_options(test_server PRIVATE -Wall -Wextra -Wno-template-body -Wno-template-id-cdtor)
endif()
add_test(NAME test_server COMMAND test_server)

add_executable(test_training
    tests/test_training.cpp
    ${GOMOKU_CORE_SOURCES}
    ${GOMOKU_TRAINING_SOURCES}
)
target_include_directories(test_training PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_training PRIVATE
    GOMOKU_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_training PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(MSVC)
    target_compile_options(test_training PRIVATE /W4)
else()
    target_compile_options(test_training PRIVATE -Wall -Wextra -Wno-template-body -Wno-template-id-cdtor)
endif()
add_test(NAME test_training COMMAND test_training)

# Every suite gets a process-level timeout. A ctest run without TIMEOUT hangs forever on a dead loop, and a
# hung gate is worse than a red one: the reader never sees a failure, only "still running". The values are
# about twice the measured cost, so a busy machine cannot kill a real green run and a real hang still dies
# inside a couple of minutes rather than inside ten. (Implementation guide §9.1.)
# 乙-2 的常驻选点臂。单独一个套件：d8 两臂在 -O2 上各 6 s 与 10 s，塞进 test_search
# （实测 470 s）只会让人开始跳过 ctest。
add_executable(test_pinned
    tests/test_pinned.cpp
    ${GOMOKU_CORE_SOURCES}
)
target_include_directories(test_pinned PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_pinned PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(test_pinned PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(test_pinned PRIVATE /W4)
else()
    target_compile_options(test_pinned PRIVATE -Wall -Wextra -Wno-template-body -Wno-template-id-cdtor)
endif()
add_test(NAME test_pinned COMMAND test_pinned)

set_tests_properties(test_board     PROPERTIES TIMEOUT 120)
set_tests_properties(test_pattern   PROPERTIES TIMEOUT 120)
set_tests_properties(test_evaluator PROPERTIES TIMEOUT 120)   # 丁-3 后实测 5 s（两条 18,900 组普查臂）
set_tests_properties(test_search    PROPERTIES TIMEOUT 900)   # 实测 470 s
set_tests_properties(test_position  PROPERTIES TIMEOUT 120)
set_tests_properties(test_session   PROPERTIES TIMEOUT 60)
set_tests_properties(test_server    PROPERTIES TIMEOUT 180)
set_tests_properties(test_training  PROPERTIES TIMEOUT 300)   # 轮次189 实测 53.5 s（本轮那条同版 4 场的用例值 ~29 s，~7 s/局；三条普查臂在 test_evaluator，不在这里）
set_tests_properties(test_pinned    PROPERTIES TIMEOUT 900)   # 六臂定深，d8 那两臂最慢

# Every suite relies on plain assert(); a Release/RelWithDebInfo -DNDEBUG
# would compile every assert away while ctest still reports green, so undo it
# for the test targets only.
foreach(gomoku_test_target test_board test_pattern test_evaluator test_search
                           test_position test_session test_server test_training
                           test_pinned)
    if(MSVC)
        target_compile_options(${gomoku_test_target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${gomoku_test_target} PRIVATE -UNDEBUG)
    endif()
endforeach()

# 开局规则的验收仪器（v2 §3.2 S1 / 指导书 §4 丁-5）。它不是测试，是一台量「一方通吃比例」
# 的仪器：四个臂跑同一批三面开局，报座位通吃与玩家通吃两个数。放在文件末尾是为了不去动
# 上面那些被 REVIEW_LOG 按行号指认的目标行（`:166-180`、`:313-327`、`:331/336`、`:337`、`:339`）。
add_executable(opening_rule_screen
    tools/opening_rule_screen.cpp
    ${GOMOKU_CORE_SOURCES}
    ${GOMOKU_TRAINING_SOURCES}
)
target_include_directories(opening_rule_screen PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(opening_rule_screen PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(opening_rule_screen PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(opening_rule_screen PRIVATE /W4)
else()
    target_compile_options(opening_rule_screen PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 进程模型的成本仪器（v2 §4.7 第 1 条 / 指导书 §4 戊-1）。同一批普查棋盘、同一个节点帽，
# 单进程与 2 / 4 份并发各报一次 nps；绑核由进程自己（--pin）或由外面的 taskset 做，两条都要能响。
# 继续追加在文件末尾：上面那些目标行被 REVIEW_LOG 按行号指认（`:166-180`、`:313-327`、
# `:331/336`、`:337`、`:339`、`:352-369`）。
add_executable(worker_nps_probe
    tools/worker_nps_probe.cpp
    ${GOMOKU_CORE_SOURCES}
)
target_include_directories(worker_nps_probe PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(worker_nps_probe PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(worker_nps_probe PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(worker_nps_probe PRIVATE /W4)
else()
    target_compile_options(worker_nps_probe PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 进程模型的 supervisor（v2 §4.1 / 指导书 §4 戊-1 思路 1）。它只要 run_spool 那一层契约
# （任务文件 + spool 信封 + tail 判决），训练本身在 worker 进程里，所以这里不链训练器。
# 继续追加在文件末尾：上面那些目标行被 REVIEW_LOG 按行号指认（`:166-180`、`:313-327`、
# `:331/336`、`:337`、`:339`、`:352-369`、`:375-388`）。
add_executable(gomoku_broker
    tools/broker_main.cpp
    src/training/run_spool.cpp
)
target_include_directories(gomoku_broker PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(gomoku_broker PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(gomoku_broker PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(gomoku_broker PRIVATE /W4)
else()
    target_compile_options(gomoku_broker PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 一次人类 think 的墙钟分布仪器（v2 §4.4 的验收档 / 指导书 §4 戊-1 验收第 3 条）。
# 继续追加在文件末尾：上面那些目标行被 REVIEW_LOG 按行号指认。
add_executable(think_latency_probe
    tools/think_latency_probe.cpp
    ${GOMOKU_CORE_SOURCES}
)
target_include_directories(think_latency_probe PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(think_latency_probe PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(think_latency_probe PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(think_latency_probe PRIVATE /W4)
else()
    target_compile_options(think_latency_probe PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 整树赛夹具（v2 §每阶段回归保障 第 2 条 / ENGINE_REDESIGN :555 的方案 C）：
# 两个 gomoku_server 进程经 WS 对打，换坐、固定开局集、作废局不计分。
# 纯客户端，不链引擎 —— 它要量的正是「两个各自跑完整生命周期引擎的进程」，
# 链进来就成了进程内近似。继续追加在文件末尾（上面那些目标行被 REVIEW_LOG 按行号指认）。
add_executable(gomoku_tree_match
    tools/tree_match.cpp
)
target_link_libraries(gomoku_tree_match PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(gomoku_tree_match PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(gomoku_tree_match PRIVATE /W4)
else()
    target_compile_options(gomoku_tree_match PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 「新根降一层」探针（轮次169 的读数出处）：读的是引擎已有的旋钮，不改任何产品代码，
# 所以它只链 core 源集，跟 think_latency_probe 一个形状。继续追加在文件末尾。
add_executable(new_root_depth_probe
    tools/new_root_depth_probe.cpp
    ${GOMOKU_CORE_SOURCES}
)
target_include_directories(new_root_depth_probe PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(new_root_depth_probe PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(new_root_depth_probe PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(new_root_depth_probe PRIVATE /W4)
else()
    target_compile_options(new_root_depth_probe PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 整树赛开局集的采集器（轮次171）：从 gomoku_train --records 写的对局记录里按固定手数取中盘快照，
# 因为赛具的有效样本单位是「开局×座位」这一对，开局集不宽 ⇒ 力量就上不去。
# 继续追加在文件末尾（上面那些目标行被 REVIEW_LOG 按行号指认）。
add_executable(match_openings_harvest
    tools/match_openings_harvest.cpp
    ${GOMOKU_CORE_SOURCES}
)
target_include_directories(match_openings_harvest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(match_openings_harvest PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(match_openings_harvest PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(match_openings_harvest PRIVATE /W4)
else()
    target_compile_options(match_openings_harvest PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 色别不对称探针（轮次172 的读数出处）：量禁手对黑方候选的实际挤压、黑白在同一盘面上的
# 真实分支宽度、以及自对弈记录按色别的胜负构成。只读，走真的 MoveGenerator，不跑搜索。
# 继续追加在文件末尾（上面那些目标行被 REVIEW_LOG 按行号指认）。
add_executable(side_asymmetry_probe
    tools/side_asymmetry_probe.cpp
    ${GOMOKU_CORE_SOURCES}
)
target_include_directories(side_asymmetry_probe PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(side_asymmetry_probe PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(side_asymmetry_probe PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(side_asymmetry_probe PRIVATE /W4)
else()
    target_compile_options(side_asymmetry_probe PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 基因可达性探针（轮次174）：一次评估函数的逐基因差分，回答「这一格改了对评分有没有影响」。
# 轮次173 那次「晋升」的就是这样一格（黑 double_three），所以这条是训练线可信度的地基检查。
# 继续追加在文件末尾（上面那些目标行被 REVIEW_LOG 按行号指认）。
add_executable(gene_reach_probe
    tools/gene_reach_probe.cpp
    ${GOMOKU_CORE_SOURCES}
)
target_include_directories(gene_reach_probe PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(gene_reach_probe PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(gene_reach_probe PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(gene_reach_probe PRIVATE /W4)
else()
    target_compile_options(gene_reach_probe PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# VCT 的切片烧法探针（轮次181 的读数出处）：线上墙钟档里 VCT 实际花掉多少、
# 先撞哪个限、以及它那句「我赢」独立复核成不成立。继续追加在文件末尾。
add_executable(vct_slice_probe
    tools/vct_slice_probe.cpp
    ${GOMOKU_CORE_SOURCES}
)
target_include_directories(vct_slice_probe PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(vct_slice_probe PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
if(UNIX)
    target_link_libraries(vct_slice_probe PRIVATE pthread)
endif()
if(MSVC)
    target_compile_options(vct_slice_probe PRIVATE /W4)
else()
    target_compile_options(vct_slice_probe PRIVATE -Wall -Wextra -O2 -Wno-template-body -Wno-template-id-cdtor)
endif()

# 轮次188：赛具的配对报表（判定用它算 n_inf 与 SE）自己的钉值用例。它跑工具本体、不连服务器，
# 所以要挂在 ctest 里 —— 一把没人测过的尺子，早晚量出假数。（放在文件末尾：前面的行号被账引用着。）
add_test(NAME tree_match_pairs COMMAND gomoku_tree_match --selfcheck-pairs)
set_tests_properties(tree_match_pairs PROPERTIES TIMEOUT 60)
