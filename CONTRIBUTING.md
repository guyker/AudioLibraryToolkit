# Contributing

Issues, documentation improvements and code contributions are welcome. Before starting a large change, open an issue
to discuss the intended behavior and avoid duplicated work.

## Report a problem

Include the operating system, build configuration, command or menu action, expected and actual behavior, and relevant
error messages. Do not upload personal music, private paths or identifying metadata; use a small generated fixture
where possible. Session logs and reports can contain paths and metadata, so review them before sharing.

## Build and test

See the [build instructions](README.md#build) for prerequisites, FFmpeg dependencies and platform-specific setup.
The application uses CMake and Ninja; on Windows, use an x64 Visual Studio developer environment:

```text
git submodule update --init --recursive
cmake --preset windows-debug
cmake --build --preset windows-debug --parallel
ctest --preset windows-debug
```

Use `linux-debug` instead on Linux. Tests use disposable generated audio fixtures. The build instructions also list
known FFmpeg-version-related test failures on the Ubuntu CI reference environment.

For behavior changes, consult the [design](AudioLibraryToolkit/DESIGN.md) and
[feature registry](docs/features/index.md), update relevant documentation, and add focused regression coverage.
Keep core C++ portable across Windows, Linux and macOS; isolate platform-specific behavior.

Submit a pull request describing the change, how to reproduce or exercise it, and any relevant limitations. The
project is licensed under [GPL-3.0](LICENSE).
