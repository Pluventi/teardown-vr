cmake_minimum_required(VERSION 3.20)
project(TeardownVR LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# --- OpenXR loader (static) -------------------------------------------------
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/openxr EXCLUDE_FROM_ALL)

# --- MinHook ----------------------------------------------------------------
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# --- Dear ImGui -------------------------------------------------------------
set(IMGUI_DIR third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

# --- The mod DLL (loaded by the game as dsound.dll) -------------------------
add_library(TeardownVR SHARED
    src/main.cpp
    src/proxy.cpp
    src/log.cpp
    src/config.cpp
    src/hooks.cpp
    src/bridge.cpp
    src/d3d_hooks.cpp
    src/render.cpp
    src/vr.cpp
    src/input.cpp
    src/pckeys.cpp
    src/modkeys.cpp
    src/ui.cpp
    src/inventory.cpp
    src/re_tools.cpp
    src/gpu_trace.cpp
    src/engine.cpp
    src/sampler.cpp
    src/taa_vr.cpp
    src/particles_vr.cpp
    src/gpu_watch.cpp
    src/perf.cpp
    src/frameb.cpp
    src/dsound.def)
target_include_directories(TeardownVR PRIVATE src third_party/openxr/include)
target_compile_options(TeardownVR PRIVATE /utf-8 /W3)
# The VR particle, lighting and anti-aliasing shaders are rebuilt from the game's own shaders (src/generated/*.h,
# made by tools/build_stereo_shaders.py from shaders/*.hlsl): they are not part of the public source. Without them
# the mod still builds; those features then keep the game's shaders (stereoParticles / VR taaMode unavailable).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/particles_p0.h")
    set(TDVR_GAME_SHADERS_DEFAULT ON)
else()
    set(TDVR_GAME_SHADERS_DEFAULT OFF)
endif()
option(TDVR_GAME_SHADERS "Build with the game-derived VR shaders (src/generated)" ${TDVR_GAME_SHADERS_DEFAULT})
if(NOT TDVR_GAME_SHADERS)
    target_compile_definitions(TeardownVR PRIVATE TDVR_NO_GAME_SHADERS)
    message(STATUS "TeardownVR: built WITHOUT the game-derived VR shaders (stereo particles and VR anti-aliasing off)")
endif()
target_compile_definitions(TeardownVR PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D12)
target_link_libraries(TeardownVR PRIVATE openxr_loader minhook imgui
    dxguid d3d12 dxgi user32 advapi32 shell32 ole32 pdh)
# d3d12/dxgi/pdh are delay-loaded so our DllMain never forces them in early
# (pdh.dll is only loaded when the GPU watch thread starts, after the first frame).
target_link_options(TeardownVR PRIVATE /DELAYLOAD:d3d12.dll /DELAYLOAD:dxgi.dll /DELAYLOAD:pdh.dll)
target_link_libraries(TeardownVR PRIVATE delayimp)
set_target_properties(TeardownVR PROPERTIES OUTPUT_NAME "dsound")

# --- Test-only mock OpenXR runtime (never shipped) ---------------------------
# Lets the mod's real OpenXR path run without a headset: set
# [Debug] openxrRuntime=<build>\Release\tdvr_mock_openxr.json in TeardownVR.ini.
add_library(tdvr_mock_openxr SHARED EXCLUDE_FROM_ALL tools/mock_openxr/mock_runtime.cpp)
target_include_directories(tdvr_mock_openxr PRIVATE third_party/openxr/include)
target_compile_definitions(tdvr_mock_openxr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(tdvr_mock_openxr PRIVATE /utf-8 /W3)
target_link_libraries(tdvr_mock_openxr PRIVATE d3d12 dxgi)
add_custom_command(TARGET tdvr_mock_openxr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/mock_openxr/tdvr_mock_openxr.json
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/mock_openxr/tdvr_mock_openxr_nohmd.json $<TARGET_FILE_DIR:tdvr_mock_openxr>)
add_executable(tdvr_xr_probe EXCLUDE_FROM_ALL tools/mock_openxr/probe.cpp)
target_include_directories(tdvr_xr_probe PRIVATE third_party/openxr/include)
target_compile_definitions(tdvr_xr_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(tdvr_xr_probe PRIVATE openxr_loader advapi32 shell32 ole32)
