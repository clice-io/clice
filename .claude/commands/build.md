Build the project. Accepts an optional argument for build type: `Debug` or `RelWithDebInfo` (default).

Available build commands:

- CMake configure only: `pixi run cmake-config [type]`
- CMake build only (skip configure): `pixi run cmake-build [type]`
- Full build (configure + build): `pixi run build [type]`
- Build a specific target: `pixi run cmake-build [type]` then `cmake --build build/[type] --target [target]`

Common targets: `clice`, `unit_tests`

Example usage:

- `/build` — full build RelWithDebInfo
- `/build Debug` — full build Debug
