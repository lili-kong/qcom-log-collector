# Qualcomm Log Collector (QLC)

## Overview
Qualcomm Log Collector (QLC) is a tool for collecting diagnostic logs from Qualcomm devices via the diag protocol. This project supports multiple platforms including Windows (ARM64 and x64) and Linux.

## Build Instructions for Windows

### Windows One Time Setup
1. Download Visual Studio Build Tools:
   - Visit [https://aka.ms/vs/stable/vs_BuildTools.exe](https://aka.ms/vs/stable/vs_BuildTools.exe)
   NOTE: Only Visual Studio Build Tools are required.

2. Run the installer and select "Individual components"

3. Install required components based on your target architecture:
   - For x64 builds, select:
     - MSVC build tools for x64/x86 (latest)
     - C++ CMake Tools for Windows
     - Windows 11 SDK (10.0.22621.0)
     - MSVC v142 - VS 2019 C++ x64/x86 build tools

   - For ARM64 builds, select:
     - MSVC build tools for ARM64/ARM64EC (latest)
     - C++ CMake Tools for Windows
     - Windows 11 SDK (10.0.22621.0)
     - MSVC v142 - VS 2019 C++ ARM64 build tools

4. Verify installation:
   ```cmd
   cmake --version
   ```
   You should see the CMake version and the MSVC compiler information.

   If CMake is not found, ensure you've added it to your system PATH. You can add it by:
   1. Open System Properties (Win + Pause/Break)
   2. Click "Advanced system settings"
   3. Click "Environment Variables"
   4. Under "System variables", find and select "Path"
   5. Click "Edit"
   6. Click "New"
   7. Add the CMake bin directory path
   8. Click "OK" to save
   9. Restart any open command prompts for the changes to take effect

#### Building on Windows (x64/ARM64)
1. Open a Command Prompt
2. Navigate to the project directory:
   ```cmd
   cd path\to\project
   ```
3. Create and navigate to a build directory:
   ```cmd
   mkdir build
   cd build
   ```
4. Configure with CMake:
   - For x64:
     ```cmd
     cmake .. -A x64
     ```
   - For ARM64:
     ```cmd
     cmake .. -A ARM64
     ```
5. Build the project:
   ```cmd
   cmake --build . --config Release
   ```

### Build Output ###

#### Windows (x64)

`Build/Windows/x64/Release/bin/`

#### Windows (ARM64)

`Build/Windows/ARM64/Release/bin/`


## Build Instructions for Linux

### Linux One Time Setup
1. Install required development packages:
   ```bash
   sudo apt update
   sudo apt install \
     cmake \
     libusb-1.0-0-dev \
     libudev-dev \
     pkg-config \
     build-essential
   ```

2. Verify installation:
   ```bash
   cmake --version
   gcc --version
   ```

### Building on Linux
1. Navigate to the project directory:
   ```bash
   cd path/to/project
   ```
2. Create and navigate to a build directory:
   ```bash
   mkdir build
   cd build
   ```
3. Configure and build:
   ```bash
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build .
   ```

### Build Output

`Build/Linux/x86_64/Release/bin/`

## Project Structure
```
.
├── CMakeLists.txt          # Main CMake configuration file
├── src/                    # Source code directory
│   ├── diag/              # Diag protocol commands and parsing
│   ├── hdlc/              # HDLC framing and encoding
│   ├── logger/            # Diag logger implementation
│   ├── ptrace/            # Ptrace writer for log output
│   ├── qlc/               # CLI interface and QLC writer
│   ├── transport/         # Device transport layer
│   ├── external/          # External dependencies
│   │   └── kLogger/       # Logging library
│   └── qds/              # Qualcomm Device Service
│       ├── common/        # Common utilities
│       ├── libusb/        # USB library interface
│       ├── qds_lnx/       # Linux-specific implementations
│       └── qds_win/       # Windows-specific implementations
├── tests/                  # Unit tests
├── doc/                    # Documentation
└── Build/                  # Build output directory
```

## C API

QLC provides a C-compatible shared-library API for external consumers and
language bindings. See [doc/C_API.md](doc/C_API.md) for the ABI, lifecycle,
linking, and buffer-ownership rules.

## Troubleshooting

### Common Issues

#### Windows
1. "CMake not found"
   - Ensure CMake is installed and added to PATH
   - Restart the Command Prompt

2. "MSVC compiler not found"
   - Verify Visual Studio Build Tools installation

3. Missing Windows SDK
   - Rerun the Visual Studio installer
   - Select and install the Windows 11 SDK (10.0.22621.0)

#### Linux
1. Missing development packages
   - Ensure all required packages are installed (see Linux One Time Setup section)
   - If you see errors about missing libraries, install the missing package

2. Build errors related to C++17
   - Ensure your compiler supports C++17
   - On older systems, you may need to install or upgrade GCC/G++


## Getting in Contact

* [Report an Issue on GitHub](../../issues)
* [Open a Discussion on GitHub](../../discussions)

## License
Qualcomm Log Collector is licensed under the [BSD-3-clause License](https://spdx.org/licenses/BSD-3-Clause.html). See [LICENSE.txt](LICENSE.txt) for the full license text.
