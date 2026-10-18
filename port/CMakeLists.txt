cmake_minimum_required(VERSION 3.20)

# Clang-CL's resource-compiler mode does not preserve the version strings from the
# launcher's generated VERSIONINFO resource on this project. Prefer the Windows SDK
# resource compiler before project() initializes the RC language.
if(CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_RC_COMPILER)
    file(GLOB TTT2_RC_CANDIDATES
        "$ENV{ProgramFiles}/Windows Kits/10/bin/*/x64/rc.exe"
        "$ENV{ProgramFiles}/../Program Files (x86)/Windows Kits/10/bin/*/x64/rc.exe")
    list(SORT TTT2_RC_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
    if(TTT2_RC_CANDIDATES)
        list(GET TTT2_RC_CANDIDATES 0 TTT2_RC_COMPILER)
        set(CMAKE_RC_COMPILER "${TTT2_RC_COMPILER}" CACHE FILEPATH "Windows SDK resource compiler")
    endif()
endif()
if(CMAKE_HOST_WIN32 AND CMAKE_RC_COMPILER MATCHES "Windows Kits/10/bin/([0-9.]+)/x64/rc.exe$")
    set(TTT2_WINDOWS_KIT_VERSION "${CMAKE_MATCH_1}")
    get_filename_component(TTT2_RC_X64_DIR "${CMAKE_RC_COMPILER}" DIRECTORY)
    get_filename_component(TTT2_RC_VERSION_DIR "${TTT2_RC_X64_DIR}" DIRECTORY)
    get_filename_component(TTT2_WINDOWS_KITS_ROOT "${TTT2_RC_VERSION_DIR}/../.." ABSOLUTE)
    set(TTT2_RC_INCLUDE_FLAGS
        "/I\"${TTT2_WINDOWS_KITS_ROOT}/Include/${TTT2_WINDOWS_KIT_VERSION}/um\" /I\"${TTT2_WINDOWS_KITS_ROOT}/Include/${TTT2_WINDOWS_KIT_VERSION}/shared\"")
    set(CMAKE_RC_FLAGS_INIT "${CMAKE_RC_FLAGS_INIT} ${TTT2_RC_INCLUDE_FLAGS}")
    if(NOT CMAKE_RC_FLAGS MATCHES "Windows Kits/10/Include/[0-9.]+/um")
        string(APPEND CMAKE_RC_FLAGS " ${TTT2_RC_INCLUDE_FLAGS}")
    endif()
endif()

project(tekken_recomp CXX)
enable_language(C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(TTT2_LOCAL_SAVES "Enable persistent local save containers" ON)
option(BUILD_RELAY_DAEMON "Build standalone UDP relay daemon" ON)
if(BUILD_RELAY_DAEMON)
    add_subdirectory(tools/relay_daemon)
endif()

# Standalone verification of the rollback coordinator. This uses a synthetic deterministic
# state fixture and does not link or alter guest session/runtime hooks.
add_executable(ttt2_test_rollback
    tools/test_rollback_loop.cpp
    src/runtime/kernel/net_rollback.cpp)
target_include_directories(ttt2_test_rollback PRIVATE src/runtime/kernel)
add_executable(ttt2_test_netplay_input
    tools/test_netplay_input.cpp
    src/runtime/kernel/netplay_input.cpp)
target_include_directories(ttt2_test_netplay_input PRIVATE src/runtime/kernel)
if(WIN32)
    add_executable(ttt2_test_save_store
        tools/test_save_store.cpp
        src/runtime/kernel/save_store.cpp)
    target_include_directories(ttt2_test_save_store PRIVATE src/runtime/kernel)
    target_compile_definitions(ttt2_test_save_store PRIVATE TTT2_LOCAL_SAVES=1 _CRT_SECURE_NO_WARNINGS)
endif()
add_library(ttt2_net_support STATIC
    src/runtime/net/net_adapter.cpp
    src/runtime/net/net_telemetry.cpp
    src/runtime/net/packet_window.cpp
    src/runtime/net/virtual_lan.cpp)
target_include_directories(ttt2_net_support PUBLIC src/runtime/net)
target_compile_definitions(ttt2_net_support PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(ttt2_net_support PUBLIC ws2_32 iphlpapi)

add_library(ttt2_enet STATIC
    third_party/enet/callbacks.c
    third_party/enet/compress.c
    third_party/enet/host.c
    third_party/enet/list.c
    third_party/enet/packet.c
    third_party/enet/peer.c
    third_party/enet/protocol.c
    third_party/enet/win32.c)
target_include_directories(ttt2_enet PUBLIC third_party/enet/include)
target_compile_definitions(ttt2_enet PRIVATE ENET_BUILDING_LIB=1)
if(WIN32)
    target_link_libraries(ttt2_enet PUBLIC ws2_32 winmm)
endif()
if(WIN32)
    add_executable(ttt2_test_xnet_route
        tools/test_xnet_route.cpp
        src/runtime/kernel/net_transport.cpp)
    target_include_directories(ttt2_test_xnet_route PRIVATE src/runtime/kernel)
    target_compile_definitions(ttt2_test_xnet_route PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(ttt2_test_xnet_route PRIVATE ws2_32 ttt2_enet ttt2_net_support)
    add_executable(ttt2_test_ranked_e2e
        tools/test_ranked_e2e.cpp
        src/runtime/kernel/net_transport.cpp
        src/runtime/kernel/ranked_adapter.cpp)
    target_include_directories(ttt2_test_ranked_e2e PRIVATE src/runtime/kernel)
    target_compile_definitions(ttt2_test_ranked_e2e PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(ttt2_test_ranked_e2e PRIVATE ws2_32 winhttp ttt2_enet ttt2_net_support)
endif()
add_executable(ttt2_test_net_support tools/test_net_support.cpp)
target_include_directories(ttt2_test_net_support PRIVATE src/runtime)
target_link_libraries(ttt2_test_net_support PRIVATE ttt2_net_support gdi32 user32)
target_compile_definitions(ttt2_test_net_support PRIVATE _CRT_SECURE_NO_WARNINGS)
add_executable(ttt2_test_virtual_lan tools/test_virtual_lan.cpp src/runtime/kernel/net_transport.cpp src/runtime/kernel/matchmaking.cpp)
target_include_directories(ttt2_test_virtual_lan PRIVATE src/runtime/kernel)
target_link_libraries(ttt2_test_virtual_lan PRIVATE ttt2_enet ttt2_net_support)
target_compile_definitions(ttt2_test_virtual_lan PRIVATE _CRT_SECURE_NO_WARNINGS)
include(CTest)
if(BUILD_TESTING)
    add_test(NAME net_support COMMAND ttt2_test_net_support "${CMAKE_BINARY_DIR}/netplay-hud-preview.bmp")
    add_test(NAME rollback_loop COMMAND ttt2_test_rollback)
    add_test(NAME netplay_input COMMAND ttt2_test_netplay_input)
    if(WIN32)
        add_test(NAME save_store COMMAND ttt2_test_save_store)
        add_test(NAME xnet_route COMMAND ttt2_test_xnet_route)
        set_tests_properties(xnet_route PROPERTIES TIMEOUT 30 ENVIRONMENT "TTT2_NET=loopback")
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME virtual_lan COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/run_virtual_lan_test.py" "$<TARGET_FILE:ttt2_test_virtual_lan>")
        set_tests_properties(virtual_lan PROPERTIES TIMEOUT 40)
        add_test(NAME ranked_e2e COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/run_ranked_e2e.py" "$<TARGET_FILE:ttt2_test_ranked_e2e>")
        set_tests_properties(ranked_e2e PROPERTIES TIMEOUT 60 ENVIRONMENT "TTT2_NET=ipv4")
        if(BUILD_RELAY_DAEMON)
            add_test(NAME relay_daemon COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test_relay_daemon.py" "$<TARGET_FILE:ttt2_relay_daemon>")
            set_tests_properties(relay_daemon PROPERTIES TIMEOUT 20)
        endif()
    endif()
    if(UNIX AND BUILD_RELAY_DAEMON)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME relay_daemon COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/test_relay_daemon.py" "$<TARGET_FILE:ttt2_relay_daemon>")
        set_tests_properties(relay_daemon PROPERTIES TIMEOUT 20)
    endif()
endif()
if(MSVC)
    target_compile_options(ttt2_test_rollback PRIVATE /W4 /WX)
    target_compile_options(ttt2_test_netplay_input PRIVATE /W4 /WX)
    if(WIN32)
        target_compile_options(ttt2_test_save_store PRIVATE /W4 /WX)
        target_compile_options(ttt2_test_xnet_route PRIVATE /W4 /WX)
        target_compile_options(ttt2_test_ranked_e2e PRIVATE /W4 /WX)
    endif()
else()
    target_compile_options(ttt2_test_rollback PRIVATE -Wall -Wextra -Werror)
    target_compile_options(ttt2_test_netplay_input PRIVATE -Wall -Wextra -Werror)
    if(WIN32)
        target_compile_options(ttt2_test_save_store PRIVATE -Wall -Wextra -Werror)
        target_compile_options(ttt2_test_xnet_route PRIVATE -Wall -Wextra -Werror)
        target_compile_options(ttt2_test_ranked_e2e PRIVATE -Wall -Wextra -Werror)
    endif()
endif()

# Recompiler output + XenonRecomp runtime headers (generated / third-party, not in repo).
set(PPC_DIR      "${CMAKE_SOURCE_DIR}/../re/ppc"                       CACHE PATH "generated PPC C++")
set(XENON_UTILS  "${CMAKE_SOURCE_DIR}/../tools/XenonRecomp/XenonUtils" CACHE PATH "ppc_context.h")
set(SIMDE_DIR    "${CMAKE_SOURCE_DIR}/../tools/XenonRecomp/thirdparty/simde" CACHE PATH "simde")
set(TTT2_PPC_OPT_LEVEL "0" CACHE STRING "Optimization level for recompiled PPC code (0, 1, or 2)")
set_property(CACHE TTT2_PPC_OPT_LEVEL PROPERTY STRINGS 0 1 2)
if(NOT TTT2_PPC_OPT_LEVEL MATCHES "^[012]$")
    message(FATAL_ERROR "TTT2_PPC_OPT_LEVEL must be 0, 1, or 2")
endif()

file(GLOB PPC_SOURCES CONFIGURE_DEPENDS
    "${PPC_DIR}/ppc_recomp.*.cpp"
    "${PPC_DIR}/ppc_func_mapping.cpp")

# The recompiled game code, as a static library (Gate 1: it must compile & link).
add_library(tekken_ppc STATIC ${PPC_SOURCES})
target_include_directories(tekken_ppc PRIVATE "${PPC_DIR}" "${XENON_UTILS}" "${SIMDE_DIR}")
# Keep the quick bring-up build at O0; use a separate build directory for performance runs.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl parses bare -O1 as MSVC /O1 (size optimization). Forward the actual
    # Clang level, and undo Debug's /Ob0 explicitly: /Ob2 alone does not clear it.
    if(TTT2_PPC_OPT_LEVEL STREQUAL "0")
        target_compile_options(tekken_ppc PRIVATE -O0)
    else()
        target_compile_options(tekken_ppc PRIVATE "/clang:-O${TTT2_PPC_OPT_LEVEL}" /clang:-finline)
    endif()
else()
    target_compile_options(tekken_ppc PRIVATE "-O${TTT2_PPC_OPT_LEVEL}")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(tekken_ppc PRIVATE -Wno-everything)
else()
    target_compile_options(tekken_ppc PRIVATE /W0)
endif()
# Host ISA for the recompiled code. With no -march, clang-cl targets baseline x86-64
# (SSE2), so SIMDe emulates every SSSE3/SSE4.1 op the VMX128 translation uses in scalar
# loops - including the pshufb byte swap in every lvx/stvx (191,722 sites) and vsldoi
# (alignr). Measured 2026-10-10: the WMV decoder's hottest function was ~2,800 x86
# instructions, mostly scalar lane loops. x86-64-v2 (SSSE3, SSE4.1/4.2, POPCNT) is the
# Windows 11 24H2 minimum, so it adds no new hardware requirement. The only result-visible
# difference from the portable path: simde_mm_dp_ps (vmsum3/4fp128) becomes native DPPS,
# which sums products pairwise rather than sequentially (last-bit differences possible).
# -ffp-contract=off keeps separate PPC multiply/add instructions unfused on any target.
set(TTT2_PPC_MARCH "x86-64-v2" CACHE STRING "clang -march for recompiled PPC code (empty = compiler default)")
if(TTT2_PPC_MARCH)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(tekken_ppc PRIVATE "/clang:-march=${TTT2_PPC_MARCH}" /clang:-ffp-contract=off)
    else()
        target_compile_options(tekken_ppc PRIVATE "-march=${TTT2_PPC_MARCH}" -ffp-contract=off)
    endif()
endif()

# XMA audio decoding: a minimal static FFmpeg (C) built from the local download of its source.
add_subdirectory(third_party/ffmpeg_xma)

# The runtime executable: minimal host (memory, function table, entry) + kernel import stubs.
file(GLOB RUNTIME_KERNEL "${CMAKE_SOURCE_DIR}/src/runtime/kernel/*.cpp")
add_executable(tekken_recomp
    "${CMAKE_SOURCE_DIR}/src/runtime/main.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/runtime_math.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/guest_time.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/audio_output.cpp"
    "${CMAKE_SOURCE_DIR}/src/runtime/kernel_stubs.gen.cpp"
    ${RUNTIME_KERNEL})
target_include_directories(tekken_recomp PRIVATE
    "${PPC_DIR}" "${XENON_UTILS}" "${SIMDE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/runtime" "${CMAKE_SOURCE_DIR}/src/runtime/kernel")
target_compile_definitions(tekken_recomp PRIVATE TTT2_LOCAL_SAVES=$<BOOL:${TTT2_LOCAL_SAVES}>)
target_compile_definitions(tekken_recomp PRIVATE TTT2_DRAW_CAPTURE_DIAGNOSTICS=1)
target_compile_definitions(tekken_recomp PRIVATE TTT2_D3D11_MOVIE_BACKEND=1)
# Hardware renderer (gpu_hw_d3d11.h); still opt-in at run time with TTT2_GPU_HW=1 / compare.
target_compile_definitions(tekken_recomp PRIVATE TTT2_GPU_HW_BACKEND=1)
target_link_libraries(tekken_recomp PRIVATE d3d11 dxgi d3dcompiler pdh psapi)
# The runtime is ~10 hand-written files, so optimising it costs seconds of build time - and it
# carries the software rasterizer, which interprets a pixel shader PER PIXEL and shades about
# 2.5 M pixels a frame. At -O0 that was slow enough that the opening video's white fade-in
# (alpha stepping down ~0.005 per rendered frame, so ~200 frames) never finished inside a run.
# The recompiled PPC library has its own configurable optimization level above.
# (C++ only: the target also compiles game.rc, and rc.exe rejects compiler flags.)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(tekken_recomp PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-O2;-Wno-everything>")
else()
    target_compile_options(tekken_recomp PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/O2;/W0>")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # Allow optimized renderer/runtime helpers to inline even in the Debug configuration.
    target_compile_options(tekken_recomp PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/clang:-finline>")
endif()
target_link_libraries(tekken_recomp PRIVATE tekken_ppc xaudio2 ole32 winmm ttt2_ffmpeg bcrypt ws2_32 winhttp ttt2_enet ttt2_net_support)
# Steam online (kernel/steam_net.cpp, port/ONLINE_PLAN.md): built when the Steamworks SDK sits in
# tools/steamworks_sdk_165 (user-supplied, not part of the repository). steam_api64.dll is
# delay-loaded and copied next to the executable; without it (or without a running Steam
# client) the game falls back to the loopback transport.
set(TTT2_STEAMWORKS "${CMAKE_SOURCE_DIR}/../tools/steamworks_sdk_165/sdk" CACHE PATH "Steamworks SDK (sdk folder)")
set(TTT2_HAVE_STEAMWORKS OFF)
if(EXISTS "${TTT2_STEAMWORKS}/public/steam/steam_api.h" AND EXISTS "${TTT2_STEAMWORKS}/redistributable_bin/win64/steam_api64.lib")
    set(TTT2_HAVE_STEAMWORKS ON)
    target_include_directories(tekken_recomp PRIVATE "${TTT2_STEAMWORKS}/public")
    target_compile_definitions(tekken_recomp PRIVATE TTT2_HAVE_STEAM=1)
    target_link_libraries(tekken_recomp PRIVATE "${TTT2_STEAMWORKS}/redistributable_bin/win64/steam_api64.lib" delayimp)
    target_link_options(tekken_recomp PRIVATE "/DELAYLOAD:steam_api64.dll")
    add_custom_command(TARGET tekken_recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TTT2_STEAMWORKS}/redistributable_bin/win64/steam_api64.dll"
                "$<TARGET_FILE_DIR:tekken_recomp>/steam_api64.dll")
    message(STATUS "Steamworks SDK: ${TTT2_STEAMWORKS} (Steam online enabled)")
else()
    target_compile_definitions(tekken_recomp PRIVATE TTT2_HAVE_STEAM=0)
    message(STATUS "Steamworks SDK not found - Steam online disabled")
endif()
# Emit a linker map so faulting PC RVAs can be resolved to guest functions during bring-up.
target_link_options(tekken_recomp PRIVATE "/MAP:tekken_recomp.map")

# --- Version, icon -------------------------------------------------------------------------
# One version for the game executable's VERSIONINFO, the installer and Add or remove programs.
set(TTT2_VERSION "2.3.0")
string(REPLACE "." "," TTT2_VERSION_COMMAS "${TTT2_VERSION},0")
# Original TTT2 Community Edition mark, shared by the installer and installed game executable.
set(TTT2_SETUP_ICON "${CMAKE_SOURCE_DIR}/src/setup/ttt2.ico")
configure_file(src/runtime/game.rc.in "${CMAKE_BINARY_DIR}/game.rc" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/game.rc" PROPERTIES OBJECT_DEPENDS "${TTT2_SETUP_ICON}")
target_sources(tekken_recomp PRIVATE "${CMAKE_BINARY_DIR}/game.rc")

# --- Installer (ttt2_setup) ------------------------------------------------------------------
# Modeled on the Tom & Jerry port's tj_setup. It ships NO game data: the player supplies their
# own disc image + Title Update 3 and the installer builds game\ from them (src/setup/*). /MT so
# the installer itself needs no VC++ redist. Build: tools\build_setup.cmd (after build_port.cmd).
#
# The shipped game exe is tekken_recomp.exe with its PE subsystem switched to WINDOWS (no console
# window; installed mode logs to %LOCALAPPDATA%\TTT2Recomp\log.txt, see main.cpp InstalledLayout).
# The dev exe stays a console program so run_port.cmd / the test scripts keep their stderr.
set(TTT2_PAYLOAD_DIR "${CMAKE_BINARY_DIR}/payload")
set(TTT2_GAME_EXE "${TTT2_PAYLOAD_DIR}/Tekken Tag Tournament 2.exe")
set(TTT2_GAME_RESOURCE "${TTT2_GAME_EXE}.lzms")
set(TTT2_LAUNCHER_EXE "${TTT2_PAYLOAD_DIR}/TTT2 Launcher.exe")
configure_file(src/launcher/launcher.rc.in "${CMAKE_BINARY_DIR}/launcher.rc" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/launcher.rc" PROPERTIES OBJECT_DEPENDS "${TTT2_SETUP_ICON}")
add_executable(ttt2_launcher WIN32 src/launcher/launcher.cpp src/runtime/net/net_adapter.cpp "${CMAKE_BINARY_DIR}/launcher.rc")
target_link_libraries(ttt2_launcher PRIVATE comctl32 shell32 ole32 uxtheme dwmapi winhttp crypt32 bcrypt ws2_32 iphlpapi)
target_compile_definitions(ttt2_launcher PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    TTT2_VERSION_STR="${TTT2_VERSION}")
if(MSVC)
    target_compile_options(ttt2_launcher PRIVATE /O2)
else()
    target_compile_options(ttt2_launcher PRIVATE -O2)
endif()
set_property(TARGET ttt2_launcher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
set_target_properties(ttt2_launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/launcher")
add_custom_command(TARGET ttt2_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TTT2_PAYLOAD_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ttt2_launcher>" "${TTT2_LAUNCHER_EXE}"
    VERBATIM)
set(TTT2_README_TXT "${CMAKE_SOURCE_DIR}/tools/dist_README.txt")
set(TTT2_STEAM_DLL "${TTT2_STEAMWORKS}/redistributable_bin/win64/steam_api64.dll")
set(TTT2_STEAM_RESOURCE_LINE "")
set(TTT2_STEAM_RESOURCE_DEPENDENCY "")
if(TTT2_HAVE_STEAMWORKS AND EXISTS "${TTT2_STEAM_DLL}")
    set(TTT2_STEAM_RESOURCE_LINE "IDR_STEAM_API          RCDATA  \"@TTT2_STEAM_DLL@\"")
    set(TTT2_STEAM_RESOURCE_DEPENDENCY "${TTT2_STEAM_DLL}")
endif()
set(XENON_THIRDPARTY "${XENON_UTILS}/../thirdparty")
find_program(TTT2_EDITBIN editbin)
if(NOT TTT2_EDITBIN AND CMAKE_CXX_COMPILER)
    get_filename_component(TTT2_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(TTT2_EDITBIN editbin HINTS "${TTT2_COMPILER_BIN}")
endif()
if(CMAKE_GENERATOR_INSTANCE)
    set(TTT2_VS_ROOT "${CMAKE_GENERATOR_INSTANCE}")
elseif(DEFINED ENV{VSINSTALLDIR})
    set(TTT2_VS_ROOT "$ENV{VSINSTALLDIR}")
elseif(CMAKE_CXX_COMPILER_AR)
    # For Ninja builds, derive the VS root from the bundled LLVM archive tool.
    get_filename_component(TTT2_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER_AR}" DIRECTORY)
    get_filename_component(TTT2_VS_ROOT "${TTT2_TOOLCHAIN_BIN}/../../../../.." ABSOLUTE)
endif()
if(TTT2_VS_ROOT)
    file(GLOB TTT2_CRT_DIRS "${TTT2_VS_ROOT}/VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
    if(NOT TTT2_EDITBIN)
        file(GLOB TTT2_EDITBIN_CANDIDATES
            "${TTT2_VS_ROOT}/VC/Tools/MSVC/*/bin/Hostx64/x64/editbin.exe")
        list(SORT TTT2_EDITBIN_CANDIDATES)
        list(REVERSE TTT2_EDITBIN_CANDIDATES)
        if(TTT2_EDITBIN_CANDIDATES)
            list(GET TTT2_EDITBIN_CANDIDATES 0 TTT2_EDITBIN)
        endif()
    endif()
endif()
list(SORT TTT2_CRT_DIRS)
list(REVERSE TTT2_CRT_DIRS)
if(TTT2_CRT_DIRS AND TTT2_EDITBIN AND EXISTS "${XENON_UTILS}/xex_patcher.cpp")
    list(GET TTT2_CRT_DIRS 0 TTT2_CRT_DIR)
    # Stage the runtime DLLs beside the other installer inputs. The Windows resource
    # compiler mishandles absolute Visual Studio paths containing spaces on some hosts.
    set(CRT_MSVCP140        "${TTT2_PAYLOAD_DIR}/msvcp140.dll")
    set(CRT_MSVCP140_ATOMIC "${TTT2_PAYLOAD_DIR}/msvcp140_atomic_wait.dll")
    set(CRT_VCRUNTIME140    "${TTT2_PAYLOAD_DIR}/vcruntime140.dll")
    set(CRT_VCRUNTIME140_1  "${TTT2_PAYLOAD_DIR}/vcruntime140_1.dll")
    add_custom_command(OUTPUT
            "${CRT_MSVCP140}" "${CRT_MSVCP140_ATOMIC}"
            "${CRT_VCRUNTIME140}" "${CRT_VCRUNTIME140_1}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TTT2_PAYLOAD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TTT2_CRT_DIR}/msvcp140.dll" "${CRT_MSVCP140}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TTT2_CRT_DIR}/msvcp140_atomic_wait.dll" "${CRT_MSVCP140_ATOMIC}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TTT2_CRT_DIR}/vcruntime140.dll" "${CRT_VCRUNTIME140}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TTT2_CRT_DIR}/vcruntime140_1.dll" "${CRT_VCRUNTIME140_1}"
        DEPENDS
            "${TTT2_CRT_DIR}/msvcp140.dll"
            "${TTT2_CRT_DIR}/msvcp140_atomic_wait.dll"
            "${TTT2_CRT_DIR}/vcruntime140.dll"
            "${TTT2_CRT_DIR}/vcruntime140_1.dll"
        VERBATIM)
    add_custom_target(ttt2_crt_payload DEPENDS
        "${CRT_MSVCP140}" "${CRT_MSVCP140_ATOMIC}"
        "${CRT_VCRUNTIME140}" "${CRT_VCRUNTIME140_1}")

    add_custom_command(OUTPUT "${TTT2_GAME_EXE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TTT2_PAYLOAD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:tekken_recomp>" "${TTT2_GAME_EXE}"
        COMMAND "${TTT2_EDITBIN}" /nologo /SUBSYSTEM:WINDOWS "${TTT2_GAME_EXE}"
        DEPENDS tekken_recomp
        COMMENT "Game executable for the installer (GUI subsystem)"
        VERBATIM)
    add_custom_target(ttt2_game_exe DEPENDS "${TTT2_GAME_EXE}")

    # Keep the downloaded setup small without changing the installed executable. LZMS uses one
    # large block for the best ratio on this 200+ MiB payload; the API is inbox on Windows 8+.
    add_library(ttt2_payload_compression STATIC src/setup/payload_compression.cpp)
    target_include_directories(ttt2_payload_compression PUBLIC src)
    target_link_libraries(ttt2_payload_compression PUBLIC cabinet)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(ttt2_payload_compression PRIVATE -O2 -Wno-everything)
    else()
        target_compile_options(ttt2_payload_compression PRIVATE /O2 /W0)
    endif()
    set_property(TARGET ttt2_payload_compression PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

    add_executable(ttt2_compress_game_payload tools/compress_game_payload.cpp)
    target_link_libraries(ttt2_compress_game_payload PRIVATE ttt2_payload_compression)
    target_compile_definitions(ttt2_compress_game_payload PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(ttt2_compress_game_payload PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/O2>")
    set_property(TARGET ttt2_compress_game_payload PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

    add_custom_command(OUTPUT "${TTT2_GAME_RESOURCE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TTT2_PAYLOAD_DIR}"
        COMMAND $<TARGET_FILE:ttt2_compress_game_payload> "${TTT2_GAME_EXE}" "${TTT2_GAME_RESOURCE}"
        DEPENDS "${TTT2_GAME_EXE}" ttt2_compress_game_payload
        COMMENT "Compressing and verifying the installer game payload (LZMS)"
        VERBATIM)
    add_custom_target(ttt2_packed_game DEPENDS "${TTT2_GAME_RESOURCE}")

    if(BUILD_TESTING)
        add_executable(ttt2_test_payload_compression tools/test_payload_compression.cpp)
        target_link_libraries(ttt2_test_payload_compression PRIVATE ttt2_payload_compression)
        target_compile_definitions(ttt2_test_payload_compression PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(ttt2_test_payload_compression PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/O2>")
        set_property(TARGET ttt2_test_payload_compression PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        add_test(NAME payload_compression_roundtrip
            COMMAND ttt2_test_payload_compression "${TTT2_GAME_EXE}" "${TTT2_GAME_RESOURCE}")
    endif()

    # Disc / title-update readers + XenonUtils' XEX delta patcher (MIT) and its dependencies.
    add_library(ttt2_setup_core STATIC
        src/setup/xdvdfs.cpp
        src/setup/stfs.cpp
        src/setup/game_data.cpp
        "${XENON_UTILS}/xex_patcher.cpp"
        "${XENON_UTILS}/memory_mapped_file.cpp"
        "${XENON_THIRDPARTY}/libmspack/libmspack/mspack/lzxd.c"
        "${XENON_THIRDPARTY}/tiny-AES-c/aes.c")
    target_include_directories(ttt2_setup_core PUBLIC src PRIVATE
        "${XENON_UTILS}"
        "${XENON_THIRDPARTY}/libmspack/libmspack/mspack"
        "${XENON_THIRDPARTY}/tiny-AES-c"
        "${XENON_THIRDPARTY}/TinySHA1")
    target_compile_definitions(ttt2_setup_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(ttt2_setup_core PRIVATE -O2 -Wno-everything)
    else()
        target_compile_options(ttt2_setup_core PRIVATE /O2 /W0)
    endif()
    set_property(TARGET ttt2_setup_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

    # ttt2_setup: the shipped installer (requireAdministrator). ttt2_setup_probe: the same
    # program as asInvoker, so `/silent <iso> <tu> <folder>` can be scripted into a test folder.
    foreach(variant ttt2_setup ttt2_setup_probe)
        if(variant STREQUAL "ttt2_setup")
            set(TTT2_UAC_LEVEL "requireAdministrator")
        else()
            set(TTT2_UAC_LEVEL "asInvoker")
        endif()
        set(TTT2_SETUP_MANIFEST "${CMAKE_BINARY_DIR}/${variant}.manifest")
        configure_file(src/setup/setup.manifest.in "${TTT2_SETUP_MANIFEST}" @ONLY)
        configure_file(src/setup/payload.rc.in "${CMAKE_BINARY_DIR}/${variant}.rc" @ONLY)
        # The .rc pulls these in by PATH: without OBJECT_DEPENDS a rebuilt game exe would not
        # relink the installer, which would silently ship the previous build (T&J lesson).
        set_source_files_properties("${CMAKE_BINARY_DIR}/${variant}.rc" PROPERTIES OBJECT_DEPENDS
            "${TTT2_GAME_EXE};${TTT2_GAME_RESOURCE};${TTT2_LAUNCHER_EXE};${TTT2_SETUP_ICON};${TTT2_README_TXT};${TTT2_STEAM_RESOURCE_DEPENDENCY};${TTT2_SETUP_MANIFEST}")
        add_executable(${variant} WIN32
            src/setup/setup_main.cpp
            src/setup/payload.cpp
            "${CMAKE_BINARY_DIR}/${variant}.rc")
        add_dependencies(${variant} ttt2_game_exe)
        add_dependencies(${variant} ttt2_packed_game)
        add_dependencies(${variant} ttt2_launcher)
        add_dependencies(${variant} ttt2_crt_payload)
        target_link_libraries(${variant} PRIVATE ttt2_setup_core ttt2_payload_compression comctl32 shell32 ole32 shlwapi advapi32 uuid bcrypt uxtheme dwmapi)
        target_compile_definitions(${variant} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                                   TTT2_VERSION_STR="${TTT2_VERSION}"
                                   TTT2_INSTALLER_HAS_STEAM=$<BOOL:${TTT2_HAVE_STEAMWORKS}>)
        target_compile_options(${variant} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-O2>")
        target_link_options(${variant} PRIVATE /MANIFEST:NO)   # the manifest is in the .rc
        set_property(TARGET ${variant} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
        set_target_properties(${variant} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/setup")
    endforeach()
    message(STATUS "installer: ttt2_setup available (CRT ${TTT2_CRT_DIR})")
else()
    message(STATUS "installer: unavailable (needs the x64 VC++ redist, editbin and XenonUtils)")
endif()
