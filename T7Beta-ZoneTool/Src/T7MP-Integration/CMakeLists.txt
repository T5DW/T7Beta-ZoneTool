cmake_minimum_required(VERSION 3.24)
project(T7MP_MOD LANGUAGES C CXX ASM_MASM)
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
 message(FATAL_ERROR "Use MSVC x64")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(imgui_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui-1.91.8")
add_subdirectory(vendor/minhook-1.3.4)
add_library(d3d11 SHARED
    proxy.cpp
    overlay.cpp
    camo.cpp
    game_console.cpp
    console.cpp
    native.cpp
    native_progression.cpp
    discord_presence.cpp
    build_profiles.cpp
    beta_startup.cpp
    auth.cpp
    window_mode.cpp
    diagnostics.cpp
    crash_capture.cpp
    weapon_audio.cpp
    animation_import.cpp
    model_import.cpp
    native_zone.cpp
    map_dump.cpp
    weaponzone.cpp
    weapondef.cpp
    script.cpp
    patches.cpp
    bots.cpp
    bot_setup_ui.cpp
    lua_runtime.cpp
    lua_log.cpp
    utils/hook.cpp
    forward.asm
    exports.def
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(d3d11 PRIVATE vendor/json ${imgui_SOURCE_DIR})
target_compile_definitions(d3d11 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(d3d11 PRIVATE minhook dxgi d3dcompiler user32 gdi32 dwmapi imm32 winmm windowscodecs ole32)
set_target_properties(d3d11 PROPERTIES PREFIX "" OUTPUT_NAME "d3d11")

set(T7MP_LUA_TEST_ROOT "" CACHE PATH "Lua test fixture root containing tools/lua/hksc.exe and CoDLuaDecompiler.exe")
option(T7MP_BUILD_TESTS "Build standalone helper and mocked command tests" OFF)
if(T7MP_BUILD_TESTS)
 enable_testing()
 add_executable(camo_test tests/camo_test.cpp utils/hook.cpp)
 target_link_libraries(camo_test PRIVATE minhook d3dcompiler windowscodecs ole32)
 target_compile_definitions(camo_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
 target_compile_options(camo_test PRIVATE /UNDEBUG)
 add_test(NAME camo_test COMMAND camo_test "${CMAKE_CURRENT_SOURCE_DIR}")
 foreach(test_name hook_test bots_test lua_test native_progression_test discord_test build_profiles_test auth_test diagnostics_test)
  add_executable(${test_name} tests/${test_name}.cpp utils/hook.cpp)
  if(test_name STREQUAL "lua_test")
   target_sources(lua_test PRIVATE lua_log.cpp)
  endif()
  target_link_libraries(${test_name} PRIVATE minhook winmm)
  target_compile_definitions(${test_name} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(${test_name} PRIVATE /UNDEBUG)
  if(test_name STREQUAL "native_progression_test")
   add_test(NAME native_progression_test COMMAND native_progression_test "${CMAKE_CURRENT_BINARY_DIR}/native-progression-fixture")
  elseif(NOT test_name STREQUAL "lua_test")
   add_test(NAME ${test_name} COMMAND ${test_name})
  elseif(T7MP_LUA_TEST_ROOT)
   add_test(NAME lua_test COMMAND lua_test "${T7MP_LUA_TEST_ROOT}")
  endif()
 endforeach()
endif()
