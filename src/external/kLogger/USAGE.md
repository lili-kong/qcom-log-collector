# kLogger Usage Guide

Simple, clean logging for C++17 projects.

## Quick Start

```cpp
#include "KL/kLogger.h"

int main() {
    // Initialize (folder, max lines per file, max bytes, app version)
    KL::Logger::get_instance().init("logs", 10000, 0, "v1.0");
    
    // Set log level
    KL::Logger::get_instance().setLevel(KL::Level::Info);
    
    // Log messages
    CLOG_INFO("Application started");
    CLOG_WARNING("Warning message");
    CLOG_ERROR("Error message");
    
    // Shutdown
    KL::Logger::get_instance().flush_and_shutdown();
    return 0;
}
```

## Log Levels

```cpp
KL::Level::Data       // Lowest (data dumps)
KL::Level::Debug      // Debug information
KL::Level::Info       // Normal information (default)
KL::Level::Warn       // Warnings
KL::Level::Error      // Errors
KL::Level::Exception  // Exceptions
KL::Level::Fatal      // Fatal errors
```

## Logging Macros

### Console Only (CLOG_*)
```cpp
CLOG_INFO("Message");      // Green
CLOG_WARNING("Message");   // Yellow
CLOG_ERROR("Message");     // Red
CLOG_DEBUG("Message");     // Only if debug enabled
```

### File Only (FLOG_*)
```cpp
FLOG_INFO("Message");
FLOG_WARNING("Message");
FLOG_ERROR("Message");
FLOG_DEBUG("Message");
```

### Both Console and File (CFLOG_*)
```cpp
CFLOG_INFO("Message", false);      // false = full format
CFLOG_WARNING("Message", false);
CFLOG_ERROR("Message", false);
CFLOG_DEBUG("Message", false);

CFLOG_INFO("Message", true);       // true = message only (no timestamp)
```

## Initialization

```cpp
KL::Logger::get_instance().init(
    "logs",      // Log directory (empty = current dir)
    10000,       // Max lines per file (0 = unlimited)
    0,           // Max bytes per file (0 = unlimited)
    "v1.0"       // App version (appears in filename)
);
```

## Configuration

```cpp
// Set minimum log level
KL::Logger::get_instance().setLevel(KL::Level::Debug);

// Enable debug logging to file
KL::Logger::get_instance().setOptions(KL::LogOption::DebugToFile);

// Get current level
KL::Level level = KL::Logger::get_instance().getLevel();

// Check if level is enabled
if (KL::Logger::get_instance().enabled(KL::Level::Debug)) {
    // Debug is enabled
}
```

## Advanced Features

### Spinner Support

```cpp
// Add a spinner line (logger will print above it)
KL::Logger::get_instance().addSpinnerLine();

// Your spinner code here...

// Remove spinner line when done
KL::Logger::get_instance().removeSpinnerLine();
```

### Raw Console Output

```cpp
// Write directly to console (useful for progress bars)
KL::Logger::get_instance().writeRawConsole("Progress: 50%");

// Overwrite current line (with flush)
KL::Logger::get_instance().consoleOverwriteLine("\rProgress: 75%");
```

## File Naming

Log files are automatically named with timestamp and process ID:
```
diaglogger_05-05-2026-14-30-45-123_v1.0_PID_12345_START_1.log
diaglogger_05-05-2026-14-35-20-456_v1.0_PID_12345_2.log
```

Format: `diaglogger_DD-MM-YYYY-HH-MM-SS-mmm_VERSION_PID_PROCESSID_SEQUENCE.log`

## Log Format

### Console Format
```
[ DD-MM-YYYY HH:MM:SS.mmm LEVEL ] Message
```

Example:
```
[ 05-05-2026 14:30:45.123 INFO ] Application started
```

### File Format
```
[DD-MM-YYYY HH:MM:SS.mmm][LEVEL]"Message"[file:line][thread_id]
```

Example:
```
[05-05-2026 14:30:45.123][INFO]"Application started"[main.cpp: 42][1234]
```

## Integration

### CMake
```cmake
target_include_directories(your_target PRIVATE 
    ${CMAKE_SOURCE_DIR}/src/external/kLogger/include
)
```

### Visual Studio
```
Project Properties → C/C++ → Additional Include Directories
Add: $(SolutionDir)src\external\kLogger\include
```

### Manual Compilation
```bash
# Windows (MinGW)
g++ -std=c++17 -Ipath/to/kLogger/include your_app.cpp -o your_app.exe -static

# Linux
g++ -std=c++17 -Ipath/to/kLogger/include your_app.cpp -o your_app -pthread
```

## Best Practices

1. **Initialize once** at program startup
2. **Use appropriate log levels**: Info for normal flow, Debug for diagnostics
3. **Log to file** for production (FLOG_* macros)
4. **Log to console** for development (CLOG_* macros)
5. **Log to both** for important events (CFLOG_* macros)
6. **Shutdown cleanly** with `flush_and_shutdown()` before exit

## Architecture

- **Singleton pattern**: One logger instance per application
- **Producer-consumer**: Background thread handles I/O
- **Non-blocking**: Logging never blocks your code
- **Thread-safe**: Use from any thread
- **Automatic rotation**: Files rotate based on lines/size
- **Crash-safe**: Signal handlers flush logs before crash

## Example

See [example.cpp](example.cpp) for a complete working example.
