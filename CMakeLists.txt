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
    src/server/server.cpp
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
    src/server/server.cpp
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
set_tests_properties(test_board     PROPERTIES TIMEOUT 120)
set_tests_properties(test_pattern   PROPERTIES TIMEOUT 120)
set_tests_properties(test_evaluator PROPERTIES TIMEOUT 120)
set_tests_properties(test_search    PROPERTIES TIMEOUT 900)   # 实测 470 s
set_tests_properties(test_position  PROPERTIES TIMEOUT 120)
set_tests_properties(test_session   PROPERTIES TIMEOUT 60)
set_tests_properties(test_server    PROPERTIES TIMEOUT 180)
set_tests_properties(test_training  PROPERTIES TIMEOUT 300)   # 实测 13 s，memcheck 下 166 s

# All eight suites rely on plain assert(); a Release/RelWithDebInfo -DNDEBUG
# would compile every assert away while ctest still reports green, so undo it
# for the test targets only.
foreach(gomoku_test_target test_board test_pattern test_evaluator test_search
                           test_position test_session test_server test_training)
    if(MSVC)
        target_compile_options(${gomoku_test_target} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${gomoku_test_target} PRIVATE -UNDEBUG)
    endif()
endforeach()
