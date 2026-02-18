cmake_minimum_required(VERSION 3.20)

project(ImprovedReplayMenu LANGUAGES C CXX)

option(REPLAY_BUILD_SHARED "Build replay module as a shared library (DLL)" ON)
option(REPLAY_STATIC_MSVC_RUNTIME "Link MSVC runtime statically (/MT, /MTd)" ON)

# ImGui 1.91.9b (same copy as InGameNetplay) and MinHook 1.3.3, compiled into the DLL.
set(REPLAY_THIRD_PARTY_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_dx9.cpp
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
)

set(REPLAY_SOURCES
    ${REPLAY_THIRD_PARTY_SOURCES}
    src/crash_handler.cpp
    src/dllmain.cpp
    src/version.cpp
    src/logger.cpp
    src/replay/core/patch_utils.cpp
    src/replay/core/text_utils.cpp
    src/replay/core/replay_name_format.cpp
    src/replay/core/replay_file_metadata.cpp
    src/replay/core/replay_browser.cpp
    src/replay/core/settings.cpp
    src/replay/core/i18n.cpp
    src/replay/hooks/replay_flow.cpp
    src/replay/hooks/menu_hooks.cpp
    src/replay/hooks/rewind.cpp
    src/replay/render/draw_surface.cpp
    src/replay/render/software_font.cpp
    src/replay/render/d3d9_overlay.cpp
    src/resources/replay_resources.rc
)

if(REPLAY_BUILD_SHARED)
    add_library(ImprovedReplayMenu SHARED ${REPLAY_SOURCES})
else()
    add_library(ImprovedReplayMenu STATIC ${REPLAY_SOURCES})
endif()

target_include_directories(ImprovedReplayMenu PRIVATE include third_party/imgui third_party/minhook/include)

target_compile_features(ImprovedReplayMenu PRIVATE cxx_std_20)

if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ImprovedReplayMenu must be built as Win32/x86 (32-bit).")
endif()

if(MSVC)
    if(REPLAY_STATIC_MSVC_RUNTIME)
        set_property(
            TARGET ImprovedReplayMenu
            PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    else()
        set_property(
            TARGET ImprovedReplayMenu
            PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        )
    endif()

    # /utf-8: the Japanese UI strings are UTF-8 literals.
    target_compile_options(ImprovedReplayMenu PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>")
    target_compile_definitions(
        ImprovedReplayMenu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        $<$<CONFIG:RelWithDebInfo>:EFZ_REPLAY_LOG_CONSOLE=1>
        $<$<CONFIG:RelWithDebInfo>:EFZ_REPLAY_LOG_FILE=1>
        # Also capture a minidump for the first serious fault seen by the vectored
        # handler (even when it's caught downstream), so swallowed AVs are debuggable.
        $<$<CONFIG:RelWithDebInfo>:EFZ_REPLAY_CRASH_DUMP_VECTORED=1>)
    target_link_libraries(ImprovedReplayMenu PRIVATE dbghelp gdi32 d3d9)
endif()

set_target_properties(ImprovedReplayMenu PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Recompile version.cpp every build so its __DATE__/__TIME__ is the real build time.
add_custom_target(replay_version_stamp ALL
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_SOURCE_DIR}/src/version.cpp")
add_dependencies(ImprovedReplayMenu replay_version_stamp)

include(CTest)
if(BUILD_TESTING)
    # Browser navigation over ASCII, Japanese and mixed-script folder names.
    add_executable(replay_browser_tests
        tests/replay_browser_tests.cpp
        tests/test_log_stub.cpp
        src/replay/core/replay_browser.cpp
        src/replay/core/replay_file_metadata.cpp
        src/replay/core/replay_name_format.cpp
        src/replay/core/text_utils.cpp)
    target_include_directories(replay_browser_tests PRIVATE include)
    target_compile_features(replay_browser_tests PRIVATE cxx_std_20)
    target_compile_definitions(replay_browser_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    add_test(NAME replay_browser_tests COMMAND replay_browser_tests)

    # .rep length: exact from Revival's trailer, else counted HP markers.
    add_executable(replay_length_tests tests/replay_length_tests.cpp src/replay/core/replay_file_metadata.cpp)
    target_include_directories(replay_length_tests PRIVATE include)
    target_compile_features(replay_length_tests PRIVATE cxx_std_20)
    add_test(NAME replay_length_tests COMMAND replay_length_tests)

    # Rewind keys: tap/hold timing and rebinding capture.
    add_executable(key_capture_tests tests/key_capture_tests.cpp)
    target_include_directories(key_capture_tests PRIVATE include)
    target_compile_features(key_capture_tests PRIVATE cxx_std_20)
    add_test(NAME key_capture_tests COMMAND key_capture_tests)
endif()
