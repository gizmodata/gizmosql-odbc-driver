# Building the GizmoSQL ODBC Driver

Detailed build instructions for all supported platforms.

## Prerequisites

| Dependency | Minimum Version | Notes |
|-----------|-----------------|-------|
| CMake | 3.11+ | Build system |
| C++ compiler | C++11 | GCC, Clang, or MSVC |
| Boost | 1.70+ | Headers and filesystem, system, locale, etc. |
| gRPC | 1.36+ | With C++ bindings |
| Protocol Buffers | 3.x | protoc compiler and libraries |
| OpenSSL | 1.1+ or 3.x | TLS support |
| RapidJSON | | JSON parsing |
| ODBC headers | | iODBC (macOS), iODBC or unixODBC (Linux), Windows SDK (Windows) |

Google Test is fetched automatically by CMake during the build.

## macOS (arm64)

### Install dependencies

```bash
brew install cmake boost libiodbc grpc protobuf openssl@3 rapidjson
```

### Build

```bash
git clone https://github.com/gizmodata/gizmosql-odbc-driver.git
cd gizmosql-odbc-driver

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel $(sysctl -n hw.ncpu)
```

### Output

The built library is at `build/Release/lib/libgizmosql-odbc.dylib` (path may vary by generator).

### Run tests

```bash
ctest --test-dir build --output-on-failure
```

### Notes

- The CMakeLists.txt expects iODBC headers at `/usr/local/Cellar/libiodbc/3.52.16/include`. If your version differs, you may need to adjust or set `ODBC_INCLUDE_DIRS`.
- Apple Silicon (arm64) is the primary supported macOS architecture.

## Linux (x64 / arm64)

### Install dependencies (Debian/Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y \
    cmake \
    build-essential \
    libboost-all-dev \
    libiodbc2-dev \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libssl-dev \
    rapidjson-dev
```

### Install dependencies (RHEL/Fedora)

```bash
sudo dnf install -y \
    cmake \
    gcc-c++ \
    boost-devel \
    libiodbc-devel \
    grpc-devel \
    protobuf-devel \
    openssl-devel \
    rapidjson-devel
```

### Build

```bash
git clone https://github.com/gizmodata/gizmosql-odbc-driver.git
cd gizmosql-odbc-driver

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel $(nproc)
```

### Output

The built library is at `build/Release/lib/libgizmosql-odbc.so`.

### Run tests

```bash
ctest --test-dir build --output-on-failure
```

## Windows (x64 / arm64)

### Prerequisites

- **Visual Studio 2022** with C++ desktop development workload
- **vcpkg** — [installation instructions](https://github.com/microsoft/vcpkg#quick-start-windows)

### Environment setup

Set the following environment variables:

```cmd
set VCPKG_ROOT=C:\path\to\vcpkg
set ARROW_GIT_REPOSITORY=https://github.com/apache/arrow
```

### Install dependencies

```cmd
%VCPKG_ROOT%\vcpkg.exe install --triplet x64-windows --x-install-root=%VCPKG_ROOT%/installed
```

For ARM64:

```cmd
%VCPKG_ROOT%\vcpkg.exe install --triplet arm64-windows --x-install-root=%VCPKG_ROOT%/installed
```

The `vcpkg.json` manifest in the project root defines all required packages: abseil, boost (beast, crc, filesystem, locale, multiprecision, optional, process, system, variant, xpressive), brotli, gflags, openssl, protobuf, zlib, re2, spdlog, grpc, utf8proc, zstd, and rapidjson.

### Build (x64)

```cmd
mkdir build && cd build

cmake .. ^
    -DARROW_GIT_REPOSITORY=%ARROW_GIT_REPOSITORY% ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows ^
    -DVCPKG_MANIFEST_MODE=OFF ^
    -G"Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_BUILD_TYPE=Release

cmake --build . --parallel 8 --config Release
```

### Build (ARM64)

```cmd
mkdir build && cd build

cmake .. ^
    -DARROW_GIT_REPOSITORY=%ARROW_GIT_REPOSITORY% ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=arm64-windows ^
    -DVCPKG_MANIFEST_MODE=OFF ^
    -G"Visual Studio 17 2022" ^
    -A ARM64 ^
    -DCMAKE_BUILD_TYPE=Release

cmake --build . --parallel 8 --config Release
```

### Build scripts

Pre-configured batch scripts are provided for convenience:

```cmd
build_win64.bat   @rem Builds x64
build_win32.bat   @rem Builds x86 (32-bit)
```

### Output

The built DLL is at `build\Release\gizmosql-odbc.dll` (path may vary).

### Notes

- The Arrow Flight SQL library is built from source as part of the CMake build. The `ARROW_GIT_REPOSITORY` variable can point to a local clone for faster builds.
- vcpkg manifest mode is disabled (`-DVCPKG_MANIFEST_MODE=OFF`) because dependencies are installed separately.

## Build options

| CMake Variable | Default | Description |
|---------------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `Debug` | Set to `Release` for optimized builds |
| `ARROW_GIT_REPOSITORY` | Apache GitHub | Arrow source repository URL (can be a local path) |
| `VCPKG_TARGET_TRIPLET` | | Windows only: `x64-windows`, `arm64-windows`, or `x86-windows` |

## Troubleshooting

### iODBC not found on macOS

If CMake cannot find iODBC headers, install via Homebrew and check the include path:

```bash
brew install libiodbc
ls /usr/local/Cellar/libiodbc/*/include
```

### gRPC version mismatch

Ensure your installed gRPC version is 1.36 or later. Older versions may not support all Arrow Flight SQL features.

### vcpkg build failures on Windows

If vcpkg fails to build a dependency, try updating vcpkg:

```cmd
cd %VCPKG_ROOT%
git pull
.\bootstrap-vcpkg.bat
```
