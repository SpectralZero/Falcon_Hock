cmake_minimum_required(VERSION 3.20)
project(UMF VERSION 0.1.0 LANGUAGES C CXX)

# ── Standards ──
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Windows-specific ──
if(NOT WIN32)
    message(FATAL_ERROR "UMF is Windows-only")
endif()
add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)

# ── Dependencies (vcpkg) ──
find_package(Zydis CONFIG REQUIRED)
find_package(Lua REQUIRED)
find_package(imgui CONFIG REQUIRED)

# ── UMF Runtime (DLL injected into target) ──
add_library(umf_runtime SHARED
    # Util
    src/util/umf_log.c
    src/util/umf_platform.c
    src/util/umf_launch.c
    src/util/umf_crash.c
    src/util/umf_arch.c

    # Mitigations
    src/mitigations/umf_mitigations.c

    # Core hooks (prepare/apply engine + Zydis relocator)
    src/core/hooks/umf_trampoline_pool.c
    src/core/hooks/umf_trampoline_builder.c
    src/core/hooks/umf_unwind.c
    src/core/hooks/umf_hook_registry.c
    src/core/hooks/umf_hook_batch.c
    src/core/hooks/umf_thread_freezer.c
    src/core/hooks/umf_dll_watchdog.c
    src/core/hooks/umf_iat.c
    src/core/hooks/umf_vtable.c
    src/core/hooks/umf_eat.c
    src/core/hooks/umf_hwbp.c
    src/core/hooks/umf_xfg.c

    # Hook profiler (additive instrumentation)
    src/core/hooks/umf_profiler.c

    # Sandbox
    src/sandbox/umf_lua_sandbox.c
    src/sandbox/umf_lua_hook.c
    src/sandbox/umf_lua_reload.c

    # Mods
    src/mods/umf_manifest.c
    src/mods/umf_mod_loader.c

    # Overlay (C++ / Dear ImGui DX11)
    src/overlay/umf_overlay_dx11.cpp

    # Overlay (GDI layered-window fallback)
    src/overlay/umf_overlay_gdi.c

    # Memory tools
    src/mem/umf_aob.c
    src/mem/umf_mem.c
    src/mem/umf_scan.c
    src/mem/umf_ct.c

    # IPC (Studio transport)
    src/ipc/umf_ipc.c

    # Discovery (RTTI/vtable scanner + symbol resolver)
    src/discovery/umf_discovery.c

    # Proxy / forwarder DLL generator
    src/proxy/umf_proxy.c

    # Entry point
    src/umf_init.c
)

target_include_directories(umf_runtime PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LUA_INCLUDE_DIR}
)

target_link_libraries(umf_runtime PRIVATE
    Zydis::Zydis
    ${LUA_LIBRARIES}
    imgui::imgui
    d3d11 dxgi
    ntdll          # NtQuerySystemInformation
    dbghelp        # Symbol resolution (future)
)

# Link Windows libs
target_link_libraries(umf_runtime PRIVATE
    kernel32 user32 advapi32 psapi gdi32
)

# Export the UMF API
target_compile_definitions(umf_runtime PRIVATE UMF_EXPORTS)

# ── Test Harness (standalone exe for unit testing) ──
add_executable(umf_test
    tests/test_main.c
    tests/test_mitigations.c
    tests/test_trampoline_pool.c
    tests/test_log.c
    tests/test_hook_inline.c
    tests/test_dll_watchdog.c
    tests/test_iat.c
    tests/test_vtable.c
    tests/test_eat.c
    tests/test_hwbp.c
    tests/test_mod_loader.c
    tests/test_lua_hook.c
    tests/test_xfg.c
    tests/test_overlay_dx11.cpp
    tests/test_aob.c
    tests/test_mem.c
    tests/test_ipc.c
    tests/test_scan.c
    tests/test_ct.c
    tests/test_manifest.c
    tests/test_launch.c
    tests/test_discovery.cpp
    tests/test_proxy.c
    tests/test_gdi_overlay.c
    tests/test_crash.c
    tests/test_lua_reload.c
    tests/test_profiler.c
    tests/test_arch.c
)

target_include_directories(umf_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
    ${LUA_INCLUDE_DIR}
)

target_link_libraries(umf_test PRIVATE umf_runtime ${LUA_LIBRARIES} d3d11 dxgi)

# ── Test plugin DLL (loaded + unloaded by the watchdog test) ──
add_library(umf_test_plugin SHARED tests/plugin/test_plugin.c)
add_dependencies(umf_test umf_test_plugin)

# ── Native test mod DLL (loaded by the mod-loader test) ──
add_library(umf_test_mod_native SHARED tests/mod_native/native_mod.c)
target_include_directories(umf_test_mod_native PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(umf_test_mod_native PRIVATE umf_runtime)
add_dependencies(umf_test umf_test_mod_native)

# ── Launcher (elevated injector front-end; requests UAC at run time) ──
add_executable(umf_launcher WIN32 src/launcher/umf_launcher.c)
target_include_directories(umf_launcher PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(umf_launcher PRIVATE umf_runtime legacy_stdio_definitions)
if(MSVC)
    target_link_options(umf_launcher PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()
add_dependencies(umf_launcher umf_runtime)

# ── Sample self-host (brings the runtime + IPC up in-process for Studio) ──
add_executable(umf_host samples/umf_host/umf_host.c)
target_include_directories(umf_host PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(umf_host PRIVATE umf_runtime)
add_dependencies(umf_host umf_runtime)

# ── Install ──
install(TARGETS umf_runtime umf_launcher RUNTIME DESTINATION bin)
install(DIRECTORY include/umf DESTINATION include)
