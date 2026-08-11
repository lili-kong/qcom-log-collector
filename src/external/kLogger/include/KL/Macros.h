#ifndef MACROS_H
#define MACROS_H

#include "Logger.h" // Logger sınıfının tanımını içerdiğinden emin olun

/**
 * @file LogMacros.h
 * @brief Helper macros for the KL::Logger class.
 *
 * LOG_ prefix: Writes only to the terminal (Console).
 * FLOG_ prefix: Writes only to the log file.
 * CFLOG_ prefix: Writes to the console and to the log file.
 */

// -----------------------------------------------------------------------------
// CONSOLE ONLY LOGGING MACROS (Sink = ConsoleOnly)
// -----------------------------------------------------------------------------

/**
 * @brief Logs an INFO message to the console only.
 * @param msg The message string (std::string compatible).
 */
#define CLOG_INFO(msg) KL::Logger::get_instance().log(KL::Level::Info, msg, KL::Sink::ConsoleOnly, __FILE__, __LINE__, false)

/**
 * @brief Logs a Warning message to the console only.
 * @param msg The message string (std::string compatible).
 */
#define CLOG_WARNING(msg)                                                                                 \
   KL::Logger::get_instance().log(KL::Level::Warn, msg, KL::Sink::ConsoleOnly, __FILE__, __LINE__,false)

/**
 * @brief Logs an Error message to the console only.
 * @param msg The message string (std::string compatible).
 */
#define CLOG_ERROR(msg) KL::Logger::get_instance().log(KL::Level::Error, msg, KL::Sink::ConsoleOnly, __FILE__, __LINE__, false)

#define CLOG_DEBUG(msg)                                                                                                \
   do                                                                                                                  \
   {                                                                                                                   \
      if(KL::Logger::get_instance().enabled(KL::Level::Debug) &&                                                       \
         KL::Logger::get_instance().optionEnabled(KL::LogOption::DebugToFile))                                         \
      {                                                                                                                \
         KL::Logger::get_instance().log(KL::Level::Debug, msg, KL::Sink::ConsoleOnly, __FILE__, __LINE__, false);      \
      }                                                                                                                \
   } while(0)
// -----------------------------------------------------------------------------
// FILE LOGGING MACROS (Sink = FileOnly)
// -----------------------------------------------------------------------------

/**
 * @brief Logs an INFO message to the log file.
 * @param msg The message string (std::string compatible).
 */
#define FLOG_INFO(msg) KL::Logger::get_instance().log(KL::Level::Info, msg, KL::Sink::FileOnly, __FILE__, __LINE__,false)

/**
 * @brief Logs a Warning message to the log file.
 * @param msg The message string (std::string compatible).
 */
#define FLOG_WARNING(msg) KL::Logger::get_instance().log(KL::Level::Warn, msg, KL::Sink::FileOnly, __FILE__, __LINE__, false)

/**
 * @brief Logs an Error message to the log file.
 * @param msg The message string (std::string compatible).
 */
#define FLOG_ERROR(msg) KL::Logger::get_instance().log(KL::Level::Error, msg, KL::Sink::FileOnly, __FILE__, __LINE__, false)


/**
 * @brief Logs a Debug message to the log file.
 * @param msg The message string (std::string compatible).
 */

#define FLOG_DEBUG(msg)                                                                                                \
   do                                                                                                                  \
   {                                                                                                                   \
      if(KL::Logger::get_instance().enabled(KL::Level::Debug) &&                                                       \
         KL::Logger::get_instance().optionEnabled(KL::LogOption::DebugToFile))                                         \
      {                                                                                                                \
         KL::Logger::get_instance().log(KL::Level::Debug, msg, KL::Sink::FileOnly, __FILE__, __LINE__, false);         \
      }                                                                                                                \
   } while(0)
// -----------------------------------------------------------------------------
// CONSOLE AND FILE LOGGING MACROS (Sink = FileAndConsole)
// -----------------------------------------------------------------------------

/**
 * @brief Logs an INFO message to the console and log file.
 * @param msg The message string (std::string compatible, true).
 */
#define CFLOG_INFO(msg, printMsgOnly)                                                                                  \
   KL::Logger::get_instance().log(KL::Level::Info, msg, KL::Sink::FileAndConsole, __FILE__, __LINE__, printMsgOnly)

/**
 * @brief Logs a Warning message to the console and log file.
 * @param msg The message string (std::string compatible, true).
 */
#define CFLOG_WARNING(msg, printMsgOnly)                                                                               \
   KL::Logger::get_instance().log(KL::Level::Warn, msg, KL::Sink::FileAndConsole, __FILE__, __LINE__, printMsgOnly)

/**
 * @brief Logs an Error message to the console and log file.
 * @param msg The message string (std::string compatible, true).
 */
#define CFLOG_ERROR(msg, printMsgOnly)                                                                                 \
   KL::Logger::get_instance().log(KL::Level::Error, msg, KL::Sink::FileAndConsole, __FILE__, __LINE__, printMsgOnly)


/**
 * @brief Logs an Debug message to the console and log file.
 * @param msg The message string (std::string compatible, true).
 */
#define CFLOG_DEBUG(msg, printMsgOnly)                                                                                    \
   do                                                                                                                     \
   {                                                                                                                      \
      if(KL::Logger::get_instance().enabled(KL::Level::Debug) &&                                                          \
         KL::Logger::get_instance().optionEnabled(KL::LogOption::DebugToFile))                                            \
      {                                                                                                                   \
         KL::Logger::get_instance().log(KL::Level::Debug, msg, KL::Sink::FileAndConsole, __FILE__,__LINE__, printMsgOnly); \
      }                                                                                                                   \
   } while(0)

// -----------------------------------------------------------------------------
// PTRACE LOGGING MACROS (Sink = PtraceOnly)
// -----------------------------------------------------------------------------

/**
 * @brief Logs a raw ptrace message to the ptrace file only.
 * @param msg The message string (std::string compatible) - typically CSV formatted data.
 *
 * This macro writes directly to the ptrace log file without any formatting.
 * The message is written as-is, should be input with formatted msg.
 */
#define PTRACE_LOG(msg) KL::Logger::get_instance().log(KL::Level::Info, msg, KL::Sink::PtraceOnly, __FILE__, __LINE__, false)

#endif // MACROS_H