// kLogger Example - Demonstrates all logging macros and initialization
// Build: g++ -std=c++17 -I ../include example.cpp -o example.exe -static (Windows)
//        g++ -std=c++17 -I ../include example.cpp -o example -lpthread (Linux)

#include "../include/KL/kLogger.h"
#include <iostream>
#include <sstream>

int main()
{
    std::cout << "=== kLogger Example ===" << std::endl;
    std::cout << std::endl;

    // =========================================================================
    // 1. Initialize kLogger (similar to main.cpp)
    // =========================================================================
    try
    {
        // init(logFolderPath, ptraceFolderPath, maxLinesPerFile, maxBytesPerFile, appVersion)
        KL::Logger::get_instance().init(
            LOG_DIR,          // Debug log directory (from Macros.h)
            PTRACE_DIR,       // Ptrace log directory (from Macros.h)
            0,                // maxLines = 0 (unlimited)
            KL::MAX_LOG_FILE_SIZE,  // maxBytes = 100MB
            "v1.0.0"          // App version
        );

        // Set minimum log level to Debug to see all messages
        KL::Logger::get_instance().setLevel(KL::Level::Debug);

        // Enable debug logging option
        KL::Logger::get_instance().setOptions(KL::LogOption::DebugToFile);

        std::cout << "[INIT] kLogger initialized successfully!" << std::endl;
        std::cout << "[INIT] Debug logs: " << LOG_DIR << std::endl;
        std::cout << "[INIT] Ptrace logs: " << PTRACE_DIR << std::endl;
        std::cout << std::endl;
    }
    catch(const std::exception& e)
    {
        std::cerr << "[ERROR] Failed to initialize kLogger: " << e.what() << std::endl;
        return 1;
    }

    // =========================================================================
    // 2. Initialize Debug File Headers (optional)
    // =========================================================================
    KL::Logger::get_instance().initialize_debug_headers({
        "=== kLogger Debug Log ===",
        "Application: kLogger Example",
        "Version: v1.0.0",
        "Format: [timestamp][level]\"message\"[file: line][thread_id]",
        "==========================================="
    });

    // =========================================================================
    // 3. Initialize Ptrace File Headers
    // =========================================================================
    std::ostringstream meta_ss;
    meta_ss << "12345" << ","           // PID
            << "kLogger-example" << "," // App name
            << "v1.0.0" << ","          // Version
            << "localhost" << ","       // Hostname
            << "Windows";               // OS

    KL::Logger::get_instance().initialize_ptrace_headers({
        "pid,appName,appVersion,hostname,osFamily",
        meta_ss.str(),
        "",
        "datetime,message,datatype,datalen,data"
    });

    // =========================================================================
    // 4. Test CONSOLE ONLY Macros (CLOG_*)
    // =========================================================================
    std::cout << "--- Testing CLOG_* (Console Only) ---" << std::endl;

    CLOG_INFO("This is an INFO message - console only");
    CLOG_WARNING("This is a WARNING message - console only");
    CLOG_ERROR("This is an ERROR message - console only");
    CLOG_DEBUG("This is a DEBUG message - console only");

    std::cout << std::endl;

    // =========================================================================
    // 5. Test FILE ONLY Macros (FLOG_*)
    // =========================================================================
    std::cout << "--- Testing FLOG_* (File Only) ---" << std::endl;
    std::cout << "(Check ./logs/ directory for output)" << std::endl;

    FLOG_INFO("This is an INFO message - file only");
    FLOG_WARNING("This is a WARNING message - file only");
    FLOG_ERROR("This is an ERROR message - file only");
    FLOG_DEBUG("This is a DEBUG message - file only");

    std::cout << std::endl;

    // =========================================================================
    // 6. Test CONSOLE AND FILE Macros (CFLOG_*)
    // =========================================================================
    std::cout << "--- Testing CFLOG_* (Console + File) ---" << std::endl;

    // printMsgOnly = false: Shows full formatted output with timestamp/level
    CFLOG_INFO("INFO to both console and file (full format)", false);
    CFLOG_WARNING("WARNING to both console and file (full format)", false);
    CFLOG_ERROR("ERROR to both console and file (full format)", false);

    // printMsgOnly = true: Shows message only (no timestamp/level prefix)
    CFLOG_INFO("INFO message only (no prefix)", true);

    std::cout << std::endl;

    // =========================================================================
    // 7. Test PTRACE Macro (PTRACE_LOG)
    // =========================================================================
    std::cout << "--- Testing PTRACE_LOG (Ptrace File Only) ---" << std::endl;
    std::cout << "(Check ./ptrace_logs/ directory for output)" << std::endl;

    // Simulate ptrace CSV records
    PTRACE_LOG("\"2026/05/19 10:30:45.123\",\"Send to Device size: 64\",\"Tx\",\"64\",\"7E 00 01 02 03 7E\"");
    PTRACE_LOG("\"2026/05/19 10:30:45.456\",\"# Receive from Device Size: 128\",\"Rx\",\"128\",\"7E 00 0A 0B 0C 7E\"");
    PTRACE_LOG("\"2026/05/19 10:30:45.789\",\"Send to Device size: 32\",\"Tx\",\"32\",\"7E 00 FF FE FD 7E\"");

    std::cout << std::endl;

    // =========================================================================
    // 8. Test String Concatenation in Logs
    // =========================================================================
    std::cout << "--- Testing String Concatenation ---" << std::endl;

    int count = 42;
    std::string name = "TestUser";

    CLOG_INFO("Processing item " + std::to_string(count) + " for user: " + name);
    FLOG_INFO("Logged " + std::to_string(count) + " records to file");

    std::cout << std::endl;

    // =========================================================================
    // 9. Test Multiple Rapid Logs (async queue test)
    // =========================================================================
    std::cout << "--- Testing Rapid Logging (100 messages) ---" << std::endl;

    for(int i = 0; i < 100; ++i)
    {
        FLOG_INFO("Rapid log message #" + std::to_string(i));
    }

    std::cout << "Queued 100 messages to file" << std::endl;
    std::cout << std::endl;

    // =========================================================================
    // 10. Shutdown - IMPORTANT: Flush queue before exit
    // =========================================================================
    std::cout << "--- Shutting Down ---" << std::endl;
    std::cout << "Flushing log queue..." << std::endl;

    KL::Logger::get_instance().flush_and_shutdown();

    std::cout << "Done! Check " << LOG_DIR << " and " << PTRACE_DIR << " for output files." << std::endl;

    return 0;
}
