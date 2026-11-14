cmake_minimum_required(VERSION 3.25)
project(Stacklab CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

add_library(core core/game.cpp core/fumen.cpp)
target_include_directories(core PUBLIC ${CMAKE_SOURCE_DIR})

add_library(ai ai/move_search.cpp ai/finesse.cpp ai/pc_solver.cpp ai/bot.cpp ai/lst.cpp)
target_link_libraries(ai PUBLIC core)

find_package(SDL3 REQUIRED)
find_package(SDL3_ttf REQUIRED)
add_executable(stacklab app/main.cpp app/play.cpp app/pc_coach.cpp app/versus.cpp app/ui.cpp app/settings.cpp modes/drill.cpp)
target_link_libraries(stacklab PRIVATE ai SDL3::SDL3 SDL3_ttf::SDL3_ttf nlohmann_json::nlohmann_json objc)
add_custom_command(TARGET stacklab POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:stacklab>/assets
  COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:stacklab>/data  # so files deleted from data/ go too
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:stacklab>/data)

include(FetchContent)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

enable_testing()
add_executable(core_test tests/core_test.cpp)
target_link_libraries(core_test PRIVATE core)
add_test(NAME core_test COMMAND core_test)

add_executable(fumen_test tests/fumen_test.cpp)
target_link_libraries(fumen_test PRIVATE core)
add_test(NAME fumen_test COMMAND fumen_test)

add_executable(ai_test tests/ai_test.cpp)
target_link_libraries(ai_test PRIVATE ai)
add_test(NAME ai_test COMMAND ai_test)

add_executable(pc_test tests/pc_test.cpp)
target_link_libraries(pc_test PRIVATE ai)
add_test(NAME pc_test COMMAND pc_test)

add_executable(bot_test tests/bot_test.cpp)
target_link_libraries(bot_test PRIVATE ai)
add_test(NAME bot_test COMMAND bot_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})  # it reads data/bot

add_executable(lst_test tests/lst_test.cpp)
target_link_libraries(lst_test PRIVATE ai)
add_test(NAME lst_test COMMAND lst_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})  # it reads data/bot

add_executable(bot_match tools/bot_match.cpp)
target_link_libraries(bot_match PRIVATE ai)

add_executable(app_test tests/app_test.cpp app/play.cpp app/pc_coach.cpp app/versus.cpp app/settings.cpp modes/drill.cpp)
target_link_libraries(app_test PRIVATE ai SDL3::SDL3 nlohmann_json::nlohmann_json)
add_test(NAME app_test COMMAND app_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})  # it reads tests/fixtures/drills

add_executable(ui_shot tests/ui_shot.cpp app/ui.cpp app/versus.cpp app/settings.cpp)
target_link_libraries(ui_shot PRIVATE ai SDL3::SDL3 SDL3_ttf::SDL3_ttf nlohmann_json::nlohmann_json)

# Not a test: prints frame times in the PC coach with the solver busy.
add_executable(frame_bench tests/frame_bench.cpp app/play.cpp app/pc_coach.cpp app/versus.cpp app/ui.cpp app/settings.cpp modes/drill.cpp)
target_link_libraries(frame_bench PRIVATE ai SDL3::SDL3 SDL3_ttf::SDL3_ttf nlohmann_json::nlohmann_json)

add_executable(replay_parity tests/replay_parity.cpp)
target_link_libraries(replay_parity PRIVATE core nlohmann_json::nlohmann_json)

file(GLOB PARITY_FIXTURES CONFIGURE_DEPENDS tests/fixtures/*.json)
foreach(fixture ${PARITY_FIXTURES})
  get_filename_component(name ${fixture} NAME_WE)
  add_test(NAME parity_${name} COMMAND replay_parity ${fixture})
endforeach()

# Real replays stay local because they include usernames. Skipped if there are none.
set(REPLAY_DIR ${CMAKE_SOURCE_DIR}/replays CACHE PATH "Folder of TETR.IO replays")
add_test(NAME parity_replays COMMAND replay_parity ${REPLAY_DIR})
set_tests_properties(parity_replays PROPERTIES SKIP_RETURN_CODE 77)

# The runner itself: synthetic replays load, and wrong results are caught.
set(SELFCHECK ${CMAKE_SOURCE_DIR}/tests/fixtures/selfcheck)
add_test(NAME parity_loader COMMAND replay_parity ${SELFCHECK}/replays_ok)
add_test(NAME parity_catches_fixture_mismatch COMMAND replay_parity ${SELFCHECK}/wrong_board.json)
add_test(NAME parity_catches_replay_mismatch COMMAND replay_parity ${SELFCHECK}/replays_bad)
set_tests_properties(parity_catches_fixture_mismatch parity_catches_replay_mismatch
  PROPERTIES PASS_REGULAR_EXPRESSION "board differs")

# Tests rely on assert, which Release builds would otherwise compile out.
target_compile_options(core_test PRIVATE -UNDEBUG)
target_compile_options(fumen_test PRIVATE -UNDEBUG)
target_compile_options(ai_test PRIVATE -UNDEBUG)
target_compile_options(pc_test PRIVATE -UNDEBUG)
target_compile_options(bot_test PRIVATE -UNDEBUG)
target_compile_options(lst_test PRIVATE -UNDEBUG)
target_compile_options(app_test PRIVATE -UNDEBUG)
target_compile_options(replay_parity PRIVATE -UNDEBUG)
