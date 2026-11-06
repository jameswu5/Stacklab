cmake_minimum_required(VERSION 3.25)
project(Stacklab CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

add_library(core core/game.cpp)
target_include_directories(core PUBLIC ${CMAKE_SOURCE_DIR})

find_package(SDL3 REQUIRED)
find_package(SDL3_ttf REQUIRED)
add_executable(stacklab app/main.cpp)
target_link_libraries(stacklab PRIVATE core SDL3::SDL3 SDL3_ttf::SDL3_ttf)

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

enable_testing()
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE core)
add_test(NAME core_test COMMAND core_test)

add_executable(replay_parity tests/replay_parity.cpp)
target_link_libraries(replay_parity PRIVATE core nlohmann_json::nlohmann_json)

# Tests rely on assert, which Release builds would otherwise compile out.
target_compile_options(core_test PRIVATE -UNDEBUG)
target_compile_options(replay_parity PRIVATE -UNDEBUG)
