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

set(GOMOKU_SOURCES
    src/main.cpp
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
)
target_include_directories(test_search PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_search PRIVATE Threads::Threads)
add_test(NAME test_search COMMAND test_search)
