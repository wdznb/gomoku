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
    src/search/greedy.cpp
    src/search/vcf.cpp
    src/search/vct.cpp
    src/search/alphabeta.cpp
    src/search/search_engine.cpp
    src/search/controller.cpp
    src/optimization/transposition.cpp
    src/optimization/opening_book.cpp
)

set(GOMOKU_TRAINING_SOURCES
    src/training/params.cpp
    src/training/transforms.cpp
    src/training/game_record.cpp
    src/training/elo.cpp
    src/training/self_play.cpp
    src/training/data_collector.cpp
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
    src/server/server.cpp
    src/server/game_manager.cpp
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
    src/core/board.cpp
    src/core/pattern.cpp
    src/core/evaluator.cpp
    src/search/greedy.cpp
    src/search/vcf.cpp
    src/search/vct.cpp
    src/search/alphabeta.cpp
    src/search/search_engine.cpp
    src/search/controller.cpp
    src/optimization/transposition.cpp
    src/optimization/opening_book.cpp
    src/server/game_manager.cpp
)
target_include_directories(test_search PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(test_search PRIVATE
    GOMOKU_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_search PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
add_test(NAME test_search COMMAND test_search)

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
