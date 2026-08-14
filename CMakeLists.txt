# PlayOS Init — PID 1 Process Supervisor
#
# C99, musl-compatible, host-buildable for development and testing.
#
# Cross-compilation: use the Buildroot toolchain file:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$BR2_EXTERNAL/toolchain.cmake
#
# Host build:
#   cmake -B build && cmake --build build

cmake_minimum_required(VERSION 3.16)
project(playos-init C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ── Source files ────────────────────────────────────────────────────

set(PLAYOS_INIT_SOURCES
    src/main.c
    src/init.c
    src/mount.c
    src/logging.c
    src/supervisor.c
    src/child_process.c
    src/recovery.c
    src/shutdown.c
    src/ipc_handler.c
    # IPC framing from playos-runtime (bundled copy)
    ipc/ipc_framing.c
    ipc/ipc_server.c
    ipc/ipc_client.c
    ipc/lifecycle_fd.c
)

set(PLAYOS_INIT_HEADERS
    include/playos-init/init.h
    include/playos-init/mount.h
    include/playos-init/supervisor.h
    include/playos-init/recovery.h
    include/playos-init/shutdown.h
)

# ── Executable ──────────────────────────────────────────────────────

add_executable(playos-init ${PLAYOS_INIT_SOURCES} ${PLAYOS_INIT_HEADERS})
target_include_directories(playos-init PRIVATE include ipc)

# ── Compositor placeholder ─────────────────────────────────────────

add_executable(playos-compositor-placeholder tests/playos-compositor-placeholder.c)
target_compile_options(playos-compositor-placeholder PRIVATE -Wall -Wextra)
target_link_options(playos-compositor-placeholder PRIVATE -static)

# ── Game stub (for lifecycle testing) ──────────────────────────────

add_executable(playos-game-stub tests/playos-game-stub.c)
target_compile_options(playos-game-stub PRIVATE -Wall -Wextra)
target_link_options(playos-game-stub PRIVATE -static)

# ── IPC test client (for QEMU integration tests) ───────────────────

add_executable(ipc-test-client
    tests/ipc_test_client.c
    ipc/ipc_framing.c
    ipc/ipc_client.c
    ipc/lifecycle_fd.c
)
target_include_directories(ipc-test-client PRIVATE include ipc)
target_compile_options(ipc-test-client PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_options(ipc-test-client PRIVATE -static)

# ── Compiler flags ──────────────────────────────────────────────────

target_compile_options(playos-init PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -fno-stack-protector        # PID 1 in initramfs: no libssp
)

# ── Linker flags ────────────────────────────────────────────────────

target_link_options(playos-init PRIVATE
    -static                      # initramfs has no shared libs
    -Wl,-z,now                  # full RELRO
    -Wl,-z,noexecstack
)

# ── Install ─────────────────────────────────────────────────────────

install(TARGETS playos-init RUNTIME DESTINATION /)
install(TARGETS playos-compositor-placeholder RUNTIME DESTINATION /usr/bin)
install(TARGETS playos-game-stub RUNTIME DESTINATION /usr/bin)
install(TARGETS ipc-test-client RUNTIME DESTINATION /usr/bin)

# ── Tests ───────────────────────────────────────────────────────────

option(BUILD_TESTS "Build host tests" ON)

if(BUILD_TESTS)
    enable_testing()

    # State initialization test
    add_executable(test_init_state
        tests/host/test_init_state.c
        src/init.c
    )
    target_include_directories(test_init_state PRIVATE include)
    target_compile_options(test_init_state PRIVATE -Wall -Wextra)
    add_test(NAME init_state COMMAND test_init_state)
endif()
