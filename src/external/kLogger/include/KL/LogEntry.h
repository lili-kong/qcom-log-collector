#ifndef LOGENTRY_H
#define LOGENTRY_H

#include "Level.h"

#include <chrono> // For std::chrono::syttem_clock::time_point
#include <string> // For std::string
#include <thread>

namespace KL {
enum class Sink
{
   ConsoleOnly,
   FileAndConsole,
   FileOnly,
   PtraceOnly
};

struct LogEntry
{
   Sink sink;
   std::chrono::system_clock::time_point timeStamp;
   Level level;
   std::string msg;
   const char* file;
   int line;
   std::thread::id tid;
   bool printMsgOnly;
};

} // namespace KL


#endif //! LOGENTRY_H