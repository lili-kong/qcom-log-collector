// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef QD_UTILS_H
#define QD_UTILS_H

#include <pthread.h>
#include "version.h"
#include "../../common/utils.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief                   Value of boolean variable
 */
typedef enum
{
    QCDEV_FALSE  = 0,      /**< Variable is false. Must be 0 */
    QCDEV_TRUE   = 1       /**< Variable is true. Must be 1 */
} QCDEV_Bool;

/**
 * Cleanup handler for integer containing a saved thread cancellation state to
 * be restored.
 *
 * @param   oldCancelState  Pointer to saved cancel state
 *
 * @returns                 Nothing
 */
void utils_restore_cancellation_state(int* oldCancelState);


/**
 * Lock a mutex and unlock it on return.
 *
 * Failures are logged to detect coding errors but are ignored since they are
 * unlikely to happen for any other reason
 *
 * @param   Lock            Mutex to lock
 *
 * @returns                 Nothing
 *
 * @note                    Depends on gcc extension allowing cleanup
 *                          handlers for automatic variables
 */
#define QCDEV_LOCK_MUTEX_AND_UNLOCK_ON_RETURN(Lock)                         \
            __attribute__((__cleanup__(utils_unlock_mutex)))                \
            struct utils_mutex_holder lock ## _holder = { Lock };           \
            do {                                                            \
                if (QCDEV_FAILED_RC(                                        \
                        pthread_mutex_lock(lock ## _holder.mutex))) {}      \
            } while (QCDEV_FALSE);


#define QCDEV_LOGGING
/* Define ENABLELOCALLOGGING to implemet local logging*/
// #define ENABLELOCALLOGGING

using namespace Utils;
typedef QLogLevel QCDEV_LogLevel;

/**
 * Based on the log level corresponding messages will be printed
 */
extern int log_level;

#ifdef QCDEV_LOGGING

/**
 * @brief   To print the log messages
 *
 * @param   lvl     Log level
 * @param   file    File name
 * @param   func    Function name
 * @param   fmt     Format
 *
 * @returns         Nothing
 */
//void libqcdev_log(int lvl, char *ver_num, char *file, int line, const char *func, char *fmt,...);

/**
 * To set log levels
*/
static inline void QCDEV_Log(QLogLevel Level,
                                     const char* ver_num,
                                     const char* file,
                                     int         line,
                                     const char* func,
                                     const char* fmt, ...)
{
    char msgBuf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msgBuf, sizeof(msgBuf), fmt, args);
    va_end(args);

    char fullBuf[2048];
    if (ver_num && ver_num[0] != '\0')
        snprintf(fullBuf, sizeof(fullBuf), "[v%s][%s:%d][%s] %s",
                 ver_num, file, line, func, msgBuf);
    else
        snprintf(fullBuf, sizeof(fullBuf), "[%s:%d][%s] %s",
                 file, line, func, msgBuf);

    QCD_Printf(Level, "%s", fullBuf);
}

/**
 * To log data messages
 */
#define QCDEV_LOG_DATA(args...) \
    QCDEV_Log(Data,      "", __FILE__, __LINE__, __FUNCTION__, ##args)

/**
 * To log debug messages
 */
#define QCDEV_LOG_DBG(args...) \
    QCDEV_Log(Debug,     "", __FILE__, __LINE__, __FUNCTION__, ##args)

/**
 * To log info messages
 */
#define QCDEV_LOG_INFO(args...) \
    QCDEV_Log(Info,      "", __FILE__, __LINE__, __FUNCTION__, ##args)

/**
 * To log warning messages
 */
#define QCDEV_LOG_WARN(args...) \
    QCDEV_Log(Warning,   "", __FILE__, __LINE__, __FUNCTION__, ##args)

/**
 * To log error messages — includes library version number
 */
#define QCDEV_LOG_ERR(args...) \
    QCDEV_Log(Error, DEV_DIS_LIB_VERSION, \
              __FILE__, __LINE__, __FUNCTION__, ##args)

/**
 * To log exception messages
 */
#define QCDEV_LOG_EXCEPTION(args...) \
    QCDEV_Log(Exception, "", __FILE__, __LINE__, __FUNCTION__, ##args)

/**
 * To log fatal error messages
 */
#define QCDEV_LOG_FATAL(args...) \
    QCDEV_Log(Fatal,     "", __FILE__, __LINE__, __FUNCTION__, ##args)


#else

/**
 * To log data messages
 */
#define QCDEV_LOG_DATA(args...)     do {} while (0)

/**
 * To log debug messages
 */
#define QCDEV_LOG_DBG(args...)     do {} while (0)

/**
 * To log info messages
 */
#define QCDEV_LOG_INFO(args...)   do {} while (0)

/**
 * To log warning messages
 */
#define QCDEV_LOG_WARN(args...)    do {} while (0)

/**
 * To log error messages
 */
#define QCDEV_LOG_ERR(args...)     do {} while (0)

/**
 * To log exception messages
 */
#define QCDEV_LOG_EXCEPTION(args...)     do {} while (0)

/**
 * To log fatal error messages
 */
#define QCDEV_LOG_FATAL(args...)     do {} while (0)


#endif

#ifdef __cplusplus
}
#endif
#endif  /* QDUTILS_H */

