set_project("T7MP-MOD")
set_languages("cxx17")
set_arch("x64")
add_rules("mode.debug", "mode.release")
target("d3d11")
    set_kind("shared")
    set_prefixname("")
    set_runtimes("MT")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "UNICODE", "_UNICODE")
    add_includedirs("vendor/json", "vendor/imgui-1.91.8", "vendor/minhook-1.3.4/include")
    add_files(
        "proxy.cpp",
        "overlay.cpp",
        "camo.cpp",
        "game_console.cpp",
        "console.cpp",
        "native.cpp",
        "native_progression.cpp",
        "discord_presence.cpp",
        "build_profiles.cpp",
        "beta_startup.cpp",
        "auth.cpp",
        "window_mode.cpp",
        "diagnostics.cpp",
        "crash_capture.cpp",
        "weapon_audio.cpp",
        "animation_import.cpp",
        "model_import.cpp",
        "native_zone.cpp", "map_dump.cpp",
        "weaponzone.cpp",
        "weapondef.cpp",
        "script.cpp",
        "patches.cpp",
        "bots.cpp",
        "bot_setup_ui.cpp",
        "lua_runtime.cpp",
        "lua_log.cpp",
        "utils/hook.cpp",
        "forward.asm"
    )
    add_files(
        "vendor/imgui-1.91.8/imgui.cpp",
        "vendor/imgui-1.91.8/imgui_draw.cpp",
        "vendor/imgui-1.91.8/imgui_tables.cpp",
        "vendor/imgui-1.91.8/imgui_widgets.cpp"
    )
    add_files(
        "vendor/imgui-1.91.8/backends/imgui_impl_win32.cpp",
        "vendor/imgui-1.91.8/backends/imgui_impl_dx11.cpp"
    )
    add_files(
        "vendor/minhook-1.3.4/src/*.c",
        "vendor/minhook-1.3.4/src/hde/hde64.c"
    )
    add_ldflags("/DEF:" .. path.join(os.projectdir(), "exports.def"), {force = true})
    add_syslinks("dxgi", "d3dcompiler", "user32", "gdi32", "dwmapi", "imm32", "winmm", "windowscodecs", "ole32")
