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

# ── UMF Runtime (DLL injected into target) ──
add_library(umf_runtime SHARED
    # Util
    src/util/umf_log.c
    src/util/umf_platform.c

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

    # Sandbox
    src/sandbox/umf_lua_sandbox.c

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
    ntdll          # NtQuerySystemInformation
    dbghelp        # Symbol resolution (future)
)

# Link Windows libs
target_link_libraries(umf_runtime PRIVATE
    kernel32 user32 advapi32 psapi
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
)

target_include_directories(umf_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tests
)

target_link_libraries(umf_test PRIVATE umf_runtime)

# ── Test plugin DLL (loaded + unloaded by the watchdog test) ──
add_library(umf_test_plugin SHARED tests/plugin/test_plugin.c)
add_dependencies(umf_test umf_test_plugin)

# ── Install ──
install(TARGETS umf_runtime RUNTIME DESTINATION bin)
install(DIRECTORY include/umf DESTINATION include)
