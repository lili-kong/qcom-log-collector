#ifndef LOGGER_H
#define LOGGER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
   #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
   #endif
   #include <io.h> // _write için
   #include <windows.h>
#else
   #include <unistd.h> // write, STDOUT_FILENO için
   #include <sys/stat.h> // chmod
#endif



// -----------------------------------------------------------------------------
// LOG DIRECTORY MACROS
// -----------------------------------------------------------------------------

#if defined(_WIN32) || defined(_WIN64)
#define LOG_DIR "C:\\ProgramData\\QFS\\DiagLogger\\DebugLogs\\"
#define PTRACE_DIR "C:\\ProgramData\\QFS\\DiagLogger\\PTraceLogs\\"
#define TMP_DIR "C:\\ProgramData\\QFS\\DiagLogger\\Temp\\"

#else
#define LOG_DIR "/var/tmp/QFS/DiagLogger/DebugLogs/"
#define PTRACE_DIR "/var/tmp/QFS/DiagLogger/PTraceLogs/"
#define TMP_DIR "/var/tmp/QFS/DiagLogger/Temp/"

#endif

// TODO: Fix Signal Handler Undefined Behaviour
// TODO: Fix Atomic/Mutex Data Race

// Project-specific headers
#include "Color.h"
#include "Level.h"
#include "LogEntry.h"
#include "LoggerOptions.h"

namespace KL {

// Logger configuration constants
constexpr size_t MAX_LOG_FILE_SIZE = 100 * 1024 * 1024;  // 100 MB
constexpr size_t MAX_LOG_LINES     = 100000;

/**
 * @class Logger
 * @brief High-performance, thread-safe, asynchronous logging system using the
 * Singleton pattern.
 *
 * This logger writes colored output to the console and rotates log files based
 * on line count. It follows a producer-consumer model: application threads push
 * log entries into a lock-free-style queue while a dedicated background thread
 * consumes them. This design ensures zero blocking on I/O.
 *
 * @note Fully compatible with C++17 (no C++20 features used).
 * @note Zero dynamic allocations in the hot path (timestamp formatting uses
 * stack buffer).
 */
class Logger
{
public:
   /**
    * @brief Returns the singleton instance (Meyers' Singleton - thread-safe
    * since C++11).
    * @return Reference to the global Logger instance.
    */
   static Logger& get_instance()
   {
      static Logger instance;
      return instance;
   }

   // Delete copy constructor and assignment operator
   Logger(const Logger&) = delete;
   Logger& operator=(const Logger&) = delete;

   // -----------------------------------------------------------------
   // Runtime configuration
   // -----------------------------------------------------------------
   void setLevel(Level lvl) noexcept
   {
      m_minLevel.store(lvl, std::memory_order_relaxed);
   }
   Level getLevel() const noexcept
   {
      return m_minLevel.load(std::memory_order_relaxed);
   }

   void setOptions(LogOption optMask) noexcept
   {
      m_options.store(optMask, std::memory_order_relaxed);
   }
   LogOption getOptions() const noexcept
   {
      return m_options.load(std::memory_order_relaxed);
   }

   // Helper used by macros – cheap O(1) test
   bool enabled(Level lvl) const noexcept
   {
      return static_cast<int>(lvl) >= static_cast<int>(getLevel());
   }
   bool optionEnabled(LogOption opt) const noexcept
   {
      return any(getOptions() & opt);
   }

   // Console output positioning (for use with spinners)
   // When > 0, moves cursor up by this many lines before printing to console
   void setConsoleLineOffset(int offset) noexcept
   {
      m_consoleLineOffset.store(offset, std::memory_order_relaxed);
   }

   int getConsoleLineOffset() const noexcept
   {
      return m_consoleLineOffset.load(std::memory_order_acquire);
   }

   // Automatic spinner line management
   // Increments the console line offset by 1 when a spinner is added
   // Uses release semantics to ensure visibility to logging thread
   void addSpinnerLine() noexcept
   {
      m_consoleLineOffset.fetch_add(1, std::memory_order_release);
   }

   // Decrements the console line offset by 1 when a spinner is removed
   // Ensures the offset never goes below 0
   // Uses acquire/release semantics for proper cross-thread synchronization
   void removeSpinnerLine() noexcept
   {
      int current = m_consoleLineOffset.load(std::memory_order_acquire);
      while(current > 0)
      {
         if(m_consoleLineOffset.compare_exchange_weak(
            current, current - 1,
            std::memory_order_release,  // success ordering
            std::memory_order_acquire   // failure ordering
         ))
         {
            break;
         }
         // After failed CAS, 'current' is updated with the actual value
         // Re-check condition explicitly to prevent infinite loop
         if(current <= 0) break;
      }
   }

   // Write raw text to console (for spinners, no logging to file)
   // This is synchronized with the internal console mutex
   void writeRawConsole(const std::string& text, bool useStderr = false)
   {
      // Lock internal console mutex for synchronization
      std::lock_guard<std::mutex> lock(m_consoleMutex);

      if (useStderr) {
         std::cerr << text;
      } else {
         std::cout << text;
      }
   }

   // Atomic console overwrite (for spinners) - write + flush in one operation
   // This ensures the write and flush happen atomically with proper locking
   void consoleOverwriteLine(const std::string& text, bool useStderr = false)
   {
      // Lock internal console mutex for synchronization
      std::lock_guard<std::mutex> lock(m_consoleMutex);

      if (useStderr) {
         std::cerr << text;
         std::cerr.flush();
      } else {
         std::cout << text;
         std::cout.flush();
      }
   }

   /**
    * @brief Initializes the logger and starts the background worker thread.
    *
    * Safe to call multiple times. Should ideally be called once at program
    * startup.
    *
    * @param logFolderPath Directory where debug log files will be stored. Empty =
    * current working directory.
    * @param ptraceFolderPath Directory where ptrace files will be stored. Empty =
    * current working directory.
    * @param maxLinesPerFile Maximum lines per file before rotation (default:
    * 100,000). Set to 0 for unlimited.
    * @param maxBytesPerFile Maximum bytes per file before rotation (default:
    * 0 = unlimited). File rotates when either limit is reached.
    */
   void init(const std::string& logFolderPath = LOG_DIR, const std::string& ptraceFolderPath = PTRACE_DIR, size_t maxLinesPerFile = KL::MAX_LOG_LINES, size_t maxBytesPerFile = 0, std::string appVersion = "")
   {
      std::call_once(mInitFlag, [this, logFolderPath, ptraceFolderPath, maxLinesPerFile, maxBytesPerFile, appVersion]() {
         mMaxLines = maxLinesPerFile;
         mMaxBytes = maxBytesPerFile;
         mAppVersion = appVersion;

         // Resolve debug log directory
         std::error_code ec;
         mLogDirectory = logFolderPath.empty() ? std::filesystem::current_path() : std::filesystem::path(logFolderPath);
         std::filesystem::create_directories(mLogDirectory, ec);
         if(ec)
         {
            std::cerr << "[Logger] Failed to create log directory: " << ec.message() << std::endl;
         }
         else
         {
            set_directory_permissions(mLogDirectory);
         }

         // Resolve ptrace log directory
         mPtraceDirectory = ptraceFolderPath.empty() ? std::filesystem::current_path() : std::filesystem::path(ptraceFolderPath);
         std::filesystem::create_directories(mPtraceDirectory, ec);
         if(ec)
         {
            std::cerr << "[Logger] Failed to create ptrace directory: " << ec.message() << std::endl;
         }
         else
         {
            set_directory_permissions(mPtraceDirectory);
         }

         // Performance: disable stream synchronization with C stdio
         std::ios::sync_with_stdio(false);
         std::cout.tie(nullptr);

#ifdef _WIN32
         // Enable ANSI color support on Windows 10+ consoles
         auto enableVT = []() {
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if(hOut == INVALID_HANDLE_VALUE) return;
            DWORD dwMode = 0;
            if(!GetConsoleMode(hOut, &dwMode)) return;
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
         };
         enableVT();
#endif

         setup_signal_handlers();

         mIsRunning = true;
         mWorkerThread = std::thread(&Logger::process_queue, this);
      });
   }

   /**
    * @brief Queues a log message for asynchronous processing.
    *
    * This is the main logging function. If the logger hasn't been initialized
    * yet, it will automatically initialize with default settings.
    *
    * @param level       Log severity level
    * @param msg         Log message (moved into the queue)
    * @param writeToFile Whether to write this entry to file (default: true)
    */
   void log(
      Level level,
      std::string msg,
      Sink sink = Sink::FileAndConsole,
      const char* file = nullptr,
      int line = 0,
      bool printMsgOnly = false
   )
   {
      if(!enabled(level)) return; // early‑out for speed

      init();

      auto now = std::chrono::system_clock::now();

      {
         std::lock_guard<std::mutex> lock(mMutex);
         auto tid = std::this_thread::get_id();
         mLogEntryQueue
            .emplace(LogEntry{sink, now, level, std::move(msg), std::move(file), line, tid, printMsgOnly = printMsgOnly});
      }

      mCV.notify_one();
   }

   /**
    * @brief Forces immediate flush of all queued logs and shuts down the worker
    * thread.
    *
    * Called automatically from destructor, but can be called manually before
    * program exit if strict ordering or immediate flush is required.
    */
   void flush_and_shutdown()
   {
      shut_down();
   }

   /**
    * @brief Initializes debug file headers that will be written at the start of each debug log file.
    *
    * Call this once before writing any debug logs. The headers will be automatically
    * written when the first debug file is created and after each file rotation.
    *
    * @param headers Vector of header lines to write at the start of each debug file.
    */
   void initialize_debug_headers(const std::vector<std::string>& headers)
   {
      std::lock_guard<std::mutex> lock(mDebugHeadersMutex);
      mDebugHeaders = headers;
      mDebugHeadersInitialized = true;
   }

   /**
    * @brief Initializes ptrace file headers that will be written at the start of each ptrace file.
    *
    * Call this once before writing any ptrace logs. The headers will be automatically
    * written when the first ptrace file is created and after each file rotation.
    *
    * @param headers Vector of header lines to write at the start of each ptrace file.
    */
   void initialize_ptrace_headers(const std::vector<std::string>& headers)
   {
      std::lock_guard<std::mutex> lock(mPtraceHeadersMutex);
      mPtraceHeaders = headers;
      mPtraceHeadersInitialized = true;
   }

private:
   /// Private constructor - initializes member variables
   Logger()
   : mCurrentLineCount(0)
   , mMaxLines(KL::MAX_LOG_LINES)
   , mMaxBytes(0)
   , mCurrentFileSize(0)
   , mPtraceLineCount(0)
   , mPtraceFileSize(0)
   , mIsRunning(false)
   {
   }

   /// Destructor - ensures clean shutdown
   ~Logger()
   {
      shut_down();
   }

   /// Signals worker thread to exit and joins it
   void shut_down()
   {
      {
         std::lock_guard<std::mutex> lock(mMutex);
         mIsRunning = false;
      }

      mCV.notify_all();

      if(mWorkerThread.joinable())
      {
         mWorkerThread.join();
      }

      if(mFileStream.is_open())
      {
         mFileStream.flush();
         mFileStream.close();
      }

      if(mPtraceStream.is_open())
      {
         mPtraceStream.flush();
         mPtraceStream.close();
      }
   }

   void setup_signal_handlers()
   {
      std::signal(SIGSEGV, signal_handler); // Segmentation fault
      std::signal(SIGABRT, signal_handler); // Abort
      std::signal(SIGFPE, signal_handler);  // Floating point exception
      std::signal(SIGILL, signal_handler);  // Illegal instruction
   }

   /// Sets appropriate permissions on a directory after creation
   /// Linux: chmod 0777 (rwxrwxrwx) - only if current user owns the directory
   /// Windows: no-op, uses default inherited permissions
   void set_directory_permissions(const std::filesystem::path& dir)
   {
#ifndef _WIN32
      // Linux/Unix: Only attempt chmod if we own the directory
      struct stat info;
      if(stat(dir.c_str(), &info) == 0)
      {
         if(info.st_uid == getuid())
         {
            if(chmod(dir.c_str(), 0777) != 0)
            {
               std::cerr << "[Logger] Failed to set permissions on " << dir << std::endl;
            }
         }
         // Silently skip if we don't own the directory - permissions were set by owner
      }
#else
      // Windows: Use default inherited permissions
      (void)dir; // Suppress unused parameter warning
#endif
   }

   void emergency_flush()
   {
      if(mFileStream.is_open())
      {
         mFileStream.flush();
      }
      if(mPtraceStream.is_open())
      {
         mPtraceStream.flush();
      }
   }

   static void signal_handler(int signal_num)
   {
      const char* msg = "\nProgram crashed! Flushing logs...\n";

#ifdef _WIN32
      _write(1, msg, (unsigned int)strlen(msg));
#else
      ssize_t __attribute__((unused)) result = write(STDOUT_FILENO, msg, strlen(msg));
#endif

      get_instance().emergency_flush();

      std::signal(signal_num, SIG_DFL);
      std::raise(signal_num);
   }

   /**
    * @brief Formats a time_point into a fixed-size char buffer using snprintf
    * (zero allocation).
    *
    * Format: DD-MM-YYYY HH:MM:SS.mmm
    *
    * @param tp     Time point to format
    * @param buffer Destination buffer (must be at least 64 bytes)
    * @param size   Size of the destination buffer
    */
   void format_timestamp(const std::chrono::system_clock::time_point& tp, char* buffer, size_t size) const
   {
      const auto time_t_val = std::chrono::system_clock::to_time_t(tp);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;

      std::tm tm_val{};
#if defined(_WIN32)
      localtime_s(&tm_val, &time_t_val);
#else
      localtime_r(&time_t_val, &tm_val);
#endif

      std::snprintf(
         buffer,
         size,
         "%02d-%02d-%04d %02d:%02d:%02d.%03d",
         tm_val.tm_mday,
         tm_val.tm_mon + 1,
         tm_val.tm_year + 1900,
         tm_val.tm_hour,
         tm_val.tm_min,
         tm_val.tm_sec,
         static_cast<int>(ms.count())
      );
   }

   /// Converts Level enum to string literal
   constexpr const char* level_to_string(Level level) const noexcept
   {
      switch(level)
      {
         case Level::Info:
            return "INFO";
         case Level::Warn:
            return "WARNING";
         case Level::Error:
            return "ERROR";
         case Level::Debug:
            return "DEBUG";
         default:
            return "UNKNOWN";
      }
   }

   /// Returns ANSI color escape sequence for the given level
   constexpr const char* get_color_code(Level level) const noexcept
   {
      switch(level)
      {
         case Level::Info:
            return "\033[92m"; // Bright Green
         case Level::Warn:
            return "\033[93m"; // Bright Yellow
         case Level::Error:
            return "\033[91m"; // Bright Red
         default:
            return "\033[0m"; // Reset
      }
   }

   /// Background thread main loop - processes queued log entries
   void process_queue()
   {
      std::queue<LogEntry> localQueue;

      char timeBuffer[64]{}; // Stack-allocated timestamp buffer
      std::string lineBuffer;
      lineBuffer.reserve(512); // Pre-allocate for typical log size
      std::string fileBuffer;
      fileBuffer.reserve(512); // Pre-allocate for typical log size

      while(true)
      {
         {
            std::unique_lock<std::mutex> lock(mMutex);
            mCV.wait(lock, [this] { return !mLogEntryQueue.empty() || !mIsRunning; });

            if(!mIsRunning && mLogEntryQueue.empty())
            {
               break;
            }

            std::swap(localQueue,
                      mLogEntryQueue); // Release lock as fast as possible
         }

         while(!localQueue.empty())
         {
            const auto& entry = localQueue.front();
            const Level& level = entry.level;

            // Format timestamp without allocation
            format_timestamp(entry.timeStamp, timeBuffer, sizeof(timeBuffer));

            std::ostringstream tid_ss;
            tid_ss << entry.tid; // stream the thread id to text

            // Build final line (single allocation at most)
            lineBuffer.clear();
            fileBuffer.clear();

            fileBuffer += '[';
            fileBuffer += timeBuffer;
            fileBuffer += "][";
            fileBuffer += level_to_string(level);
            fileBuffer += "]";

            if(!entry.printMsgOnly)
            {
               lineBuffer += "[ ";
               lineBuffer += timeBuffer;
               lineBuffer += ' ';
               lineBuffer += level_to_string(level);
               lineBuffer += " ] ";
            }
            lineBuffer += entry.msg;

            fileBuffer += '\"';
            fileBuffer += entry.msg;
            fileBuffer += '\"';

            fileBuffer += '[';
            fileBuffer += entry.file;
            fileBuffer += ": ";
            fileBuffer += std::to_string(entry.line);
            fileBuffer += "][";
            fileBuffer += tid_ss.str();
            fileBuffer += ']';

            switch(entry.sink)
            {
               case KL::Sink::ConsoleOnly:
                  write_to_console(lineBuffer, level);
                  break;
               case KL::Sink::FileOnly:
                  write_to_file(fileBuffer);
                  break;
               case KL::Sink::FileAndConsole:
                  write_to_file(fileBuffer);
                  write_to_console(lineBuffer, level);
                  break;
               case KL::Sink::PtraceOnly:
                  write_to_ptrace(entry.msg);
                  break;
            }

            localQueue.pop();
         }
      }
   }

   /// Writes a line to the current debug log file, creating a new one if necessary
   void write_to_file(const std::string& msg)
   {
      // Check if rotation is needed - rotate when ANY limit is exceeded
      bool needsRotation = !mFileStream.is_open()
         || (mMaxLines > 0 && mCurrentLineCount >= mMaxLines)
         || (mMaxBytes > 0 && mCurrentFileSize >= mMaxBytes);

      if(needsRotation)
      {
         create_new_file();
      }

      if(mFileStream.is_open())
      {
         mFileStream << msg << '\n';
         ++mCurrentLineCount;
         mCurrentFileSize += msg.length() + 1; // +1 for newline
      }
      // If file still not open → silently drop (disk full, permission, etc.)
      // Critical applications may want to log this to stderr
   }

   /// Writes a line to the current ptrace log file, creating a new one if necessary
   void write_to_ptrace(const std::string& msg)
   {
      // Check if rotation is needed - rotate when ANY limit is exceeded
      bool needsRotation = !mPtraceStream.is_open()
         || (mMaxLines > 0 && mPtraceLineCount >= mMaxLines)
         || (mMaxBytes > 0 && mPtraceFileSize >= mMaxBytes);

      if(needsRotation)
      {
         create_new_ptrace_file();
      }

      if(mPtraceStream.is_open())
      {
         mPtraceStream << msg << '\n';
         ++mPtraceLineCount;
         mPtraceFileSize += msg.length() + 1; // +1 for newline
      }
      // If file still not open → silently drop (disk full, permission, etc.)
   }


   void write_to_console(const std::string& lineBuffer, Level level)
   {
      // Lock internal console mutex for synchronization
      std::lock_guard<std::mutex> lock(m_consoleMutex);

      int offset = getConsoleLineOffset();

      if(Level::Error == level)
      {
         if (offset > 0) {
            // Print above spinners: insert line, move up, print, restore cursor
            // Insert blank line to scroll content up without overwriting
            std::cerr << "\033[s";                    // Save cursor position
            std::cerr << "\033[" << offset << "A";    // Move up N lines
            std::cerr << "\r";                        // Move to beginning of line
            std::cerr << "\033[1L";                   // Insert 1 line (scroll up)
            std::cerr << get_color_code(level) << lineBuffer << Color::RESET << "\033[K"; // Print and clear to end of line
            std::cerr << "\033[u";                    // Restore cursor position
            std::cerr.flush();                        // Explicit flush
         } else {
            std::cerr << get_color_code(level) << lineBuffer << Color::RESET << std::endl;
         }
      }
      else
      {
         if (offset > 0) {
            // Print above spinners: insert line, move up, print, restore cursor
            // Insert blank line to scroll content up without overwriting
            std::cout << "\033[s";                    // Save cursor position
            std::cout << "\033[" << offset << "A";    // Move up N lines
            std::cout << "\r";                        // Move to beginning of line
            std::cout << "\033[1L";                   // Insert 1 line (scroll up)
            std::cout << get_color_code(level) << lineBuffer << Color::RESET << "\033[K"; // Print and clear to end of line
            std::cout << "\033[u";                    // Restore cursor position
            std::cout.flush();                        // Explicit flush
         } else {
            std::cout << get_color_code(level) << lineBuffer << Color::RESET << std::endl;
         }
      }
   }


   /// Closes current file and opens a new one with timestamped name
   void create_new_file()
   {
      if(mFileStream.is_open())
      {
         mFileStream.flush();
         mFileStream.close();
      }

      ++mFileSequence;
      const auto now = std::chrono::system_clock::now();
      const auto time_t_val = std::chrono::system_clock::to_time_t(now);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

      std::tm tm_val{};
#if defined(_WIN32)
      localtime_s(&tm_val, &time_t_val);
#else
      localtime_r(&time_t_val, &tm_val);
#endif

      // Get current process ID
#ifdef _WIN32
      DWORD pid = GetCurrentProcessId();
#else
      pid_t pid = getpid();
#endif

      char filename[128];
      
      if(mFileSequence == 1)
       {
          std::snprintf(
             filename,
             sizeof(filename),
             "qlc_debug_%02d_%02d_%04d_%02d_%02d_%02d_%03d_%s_PID_%d_START_1.log",
             tm_val.tm_mday,
             tm_val.tm_mon + 1,
             tm_val.tm_year + 1900,
             tm_val.tm_hour,
             tm_val.tm_min,
             tm_val.tm_sec,
             static_cast<int>(ms.count()),
             mAppVersion.c_str(),
             static_cast<int>(pid) 
          );
       } else
       {
          std::snprintf(
             filename,
             sizeof(filename),
             "qlc_debug_%02d_%02d_%04d_%02d_%02d_%02d_%03d_%s_PID_%d_%zu.log",
             tm_val.tm_mday,
             tm_val.tm_mon + 1,
             tm_val.tm_year + 1900,
             tm_val.tm_hour,
             tm_val.tm_min,
             tm_val.tm_sec,
             static_cast<int>(ms.count()),
             mAppVersion.c_str(),
             static_cast<int>(pid),
             mFileSequence
          );
         }
      const std::filesystem::path fullPath = mLogDirectory / filename;
      mFileStream.open(fullPath, std::ios::out | std::ios::app);

      if(!mFileStream.is_open())
      {
         std::cerr << "[Logger] CRITICAL: Failed to open log file: " << fullPath << std::endl;
      }
      else
      {
         // Write headers if initialized
         std::lock_guard<std::mutex> headerLock(mDebugHeadersMutex);
         if(mDebugHeadersInitialized)
         {
            for(const auto& header : mDebugHeaders)
            {
               mFileStream << header << '\n';
            }
         }
      }

      mCurrentLineCount = 0;
      mCurrentFileSize = 0;
   }

   /// Closes current ptrace file and opens a new one with timestamped name
   void create_new_ptrace_file()
   {
      if(mPtraceStream.is_open())
      {
         mPtraceStream.flush();
         mPtraceStream.close();
      }

      ++mPtraceFileSequence;
      const auto now = std::chrono::system_clock::now();
      const auto time_t_val = std::chrono::system_clock::to_time_t(now);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

      std::tm tm_val{};
#if defined(_WIN32)
      localtime_s(&tm_val, &time_t_val);
#else
      localtime_r(&time_t_val, &tm_val);
#endif

      // Get current process ID
#ifdef _WIN32
      DWORD pid = GetCurrentProcessId();
#else
      pid_t pid = getpid();
#endif

      char filename[128];

      if(mPtraceFileSequence == 1)
      {
         std::snprintf(
            filename,
            sizeof(filename),
            "qlc_%02d_%02d_%04d_%02d_%02d_%02d_%03d_%s_PID_%d_START_1.ptrace",
            tm_val.tm_mday,
            tm_val.tm_mon + 1,
            tm_val.tm_year + 1900,
            tm_val.tm_hour,
            tm_val.tm_min,
            tm_val.tm_sec,
            static_cast<int>(ms.count()),
            mAppVersion.c_str(),
            static_cast<int>(pid)
         );
      } else
      {
         std::snprintf(
            filename,
            sizeof(filename),
            "qlc_%02d_%02d_%04d_%02d_%02d_%02d_%03d_%s_PID_%d_%zu.ptrace",
            tm_val.tm_mday,
            tm_val.tm_mon + 1,
            tm_val.tm_year + 1900,
            tm_val.tm_hour,
            tm_val.tm_min,
            tm_val.tm_sec,
            static_cast<int>(ms.count()),
            mAppVersion.c_str(),
            static_cast<int>(pid),
            mPtraceFileSequence
         );
      }

      const std::filesystem::path fullPath = mPtraceDirectory / filename;
      mPtraceStream.open(fullPath, std::ios::out | std::ios::app);

      if(!mPtraceStream.is_open())
      {
         std::cerr << "[Logger] CRITICAL: Failed to open ptrace file: " << fullPath << std::endl;
      }
      else
      {
         // Write headers if initialized
         std::lock_guard<std::mutex> headerLock(mPtraceHeadersMutex);
         if(mPtraceHeadersInitialized)
         {
            for(const auto& header : mPtraceHeaders)
            {
               mPtraceStream << header << '\n';
            }
         }
      }

      mPtraceLineCount = 0;
      mPtraceFileSize = 0;
   }

   // Member variables
   std::queue<LogEntry> mLogEntryQueue;
   std::condition_variable mCV;
   std::mutex mMutex;
   std::thread mWorkerThread;

   std::atomic<bool> mIsRunning{false};
   std::once_flag mInitFlag;

   // Debug log file tracking
   std::ofstream mFileStream;
   std::filesystem::path mLogDirectory;
   size_t mMaxLines{KL::MAX_LOG_LINES};
   size_t mCurrentLineCount{0};
   size_t mMaxBytes{0};          // Maximum bytes per file (0 = unlimited)
   size_t mCurrentFileSize{0};   // Current file size in bytes
   size_t mFileSequence{0};

   // Ptrace log file tracking
   std::ofstream mPtraceStream;
   std::filesystem::path mPtraceDirectory;
   size_t mPtraceLineCount{0};
   size_t mPtraceFileSize{0};
   size_t mPtraceFileSequence{0};

   // Debug file headers (written at start of each new debug log file)
   std::mutex mDebugHeadersMutex;
   bool mDebugHeadersInitialized{false};
   std::vector<std::string> mDebugHeaders;

   // Ptrace file headers (written at start of each new ptrace file)
   std::mutex mPtraceHeadersMutex;
   bool mPtraceHeadersInitialized{false};
   std::vector<std::string> mPtraceHeaders;

   // Common settings
   std::atomic<Level> m_minLevel{Level::Info};
   std::atomic<LogOption> m_options{LogOption::None};
   std::atomic<int> m_consoleLineOffset{0};
   std::mutex m_consoleMutex; // Internal mutex for synchronizing console output
   std::string mAppVersion;
};

} // namespace KL

#endif // LOGGER_H