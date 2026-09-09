# teez-worker

Test script runner for [teez](https://github.com/Coditary/teez): executes `*.teez.lua` files with HTTP and assertion helpers.

## Build

Requires [teez-core](https://github.com/Coditary/teez-core) as a sibling directory, via `-DTEEZ_CORE_DIR=...`, or fetched automatically from GitHub when missing (`TEEZ_FETCH_CORE_IF_MISSING=ON`, default).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Usage

```bash
./build/teez-worker /path/to/test.teez.lua
```

Or via `teez run` when the worker plugin is discovered.
