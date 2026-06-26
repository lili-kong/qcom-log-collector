// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#include "utils.h"
#include "qdver.h"

#include <iostream>
using namespace std;

Utils::QCD_LOGGING_CALLBACK  fLogger = NULL;

#ifdef TOOLS_TARGET_WINDOWS
#else
/**
 * Cleanup handler for mutex holder
 *
 * @param   holder          Pointer to mutex holder
 *
 * @returns                 Nothing
 */
void utils_unlock_mutex(struct utils_mutex_holder* holder)
{
   if (QCDEV_FAILED_RC(pthread_mutex_unlock(holder->mutex))) {}

   return;
}
#endif

VOID Utils::QCD_Printf(QLogLevel Level, const PCHAR Format, ...)
{
#define DBG_MSG_MAX_SZ 1024
#define MSG_LEN_MAX (DBG_MSG_MAX_SZ - 50)
   va_list arguments;
   CHAR   msgBuffer[DBG_MSG_MAX_SZ], * pBuf;

   pBuf = (PCHAR)msgBuffer;

   // log data
   va_start(arguments, Format);
   vsnprintf((PCHAR)pBuf, MSG_LEN_MAX, Format, arguments);
   va_end(arguments);

   if (fLogger != NULL)
   {
      fLogger(Level, (PCHAR)msgBuffer);
   }
   else
   {
#ifdef TOOLS_TARGET_WINDOWS
      OutputDebugStringA((PCHAR)msgBuffer);
#else
      cout << msgBuffer << std::endl;
#endif
   }
}  // QCD_Printf

VOID Utils::setLoggerCallback(QCD_LOGGING_CALLBACK Cb)
{
   fLogger = Cb;
}  // setLoggerCallback
