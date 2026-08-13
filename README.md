# PlayOS Init

`playos-init` is the PID 1 supervisor for PlayOS. It is not intended to be a general-purpose init system; its job is to boot the system, keep core PlayOS services alive, and orchestrate the startup and lifecycle of the shell, compositor, and games.

## What it does

- Initializes as PID 1 in the initramfs / early boot environment
- Mounts virtual filesystems such as `/proc` and `/sys`
- Discovers and mounts the data partition
- Creates required runtime directories
- Starts the PlayOS IPC server on `/run/playos/control.sock`
- Spawns and supervises the compositor
- Starts `playos-shell` once the compositor is ready
- Launches games and other child processes requested by the shell over IPC
- Reaps child processes and handles recovery / shutdown flows

## Repository layout

- `src/` — init, mount, logging, supervisor, recovery, shutdown, and IPC handling logic
- `include/playos-init/` — public headers
- `ipc/` — bundled IPC framing/server/client code
- `tests/` — compositor/game stubs, IPC test client, and host tests
- `CMakeLists.txt` — build configuration

## Build

### Host build

```bash
cmake -B build
cmake --build build
```

### Cross-compilation

Use the Buildroot toolchain file when building for the target environment:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$BR2_EXTERNAL/toolchain.cmake
cmake --build build
```

## Tests

If enabled, host tests can be built and run through CMake:

```bash
cmake -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build
```

## Runtime notes

- The binary is built statically for initramfs use.
- PID 1 must not exit normally.
- Logging is initialized after the runtime filesystem is available.
- If the data partition is missing, the system enters recovery/provisioning flow.

## Related components

This repository includes small test helpers used for validation and integration testing:

- `playos-compositor-placeholder`
- `playos-game-stub`
- `ipc-test-client`

## License

Add licensing information here if/when it becomes available.
