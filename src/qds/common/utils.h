// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD 3-Clause Clear License
#ifndef UTILS_H
#define UTILS_H

#ifdef TOOLS_TARGET_WINDOWS
#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <conio.h>
#include <string.h>
#include <winioctl.h>
#else
#include "windows_compatible.h"     /**< Local header to have compatibility */
#include <pthread.h>
#include <sys/types.h>
#endif

#include <stdarg.h>
#include <stdio.h>

#ifdef TOOLS_TARGET_WINDOWS
#else
#define EnterCriticalSection(mutex)         pthread_mutex_lock(mutex)
#define LeaveCriticalSection(mutex)         pthread_mutex_unlock(mutex)
#define InitializeCriticalSection(mutex)    pthread_mutex_init(mutex, NULL)
#define InitializeEventSection(cond)        pthread_cond_init(cond, NULL)

#define InterlockedIncrement(data) __atomic_fetch_add(data, 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(data) __atomic_fetch_sub(data, 1, __ATOMIC_SEQ_CST)

#define CONTAINING_RECORD(address, type, field) (\
        (type *)((char*)(address) -(unsigned long)(&((type *)0)->field)))

#pragma pack(1)

typedef struct _LIST_ENTRY {
   struct _LIST_ENTRY* Flink;
   struct _LIST_ENTRY* Blink;
} LIST_ENTRY, * PLIST_ENTRY, PRLIST_ENTRY;

typedef struct _SINGLE_LIST_ENTRY {
   struct _SINGLE_LIST_ENTRY* Next;
} SINGLE_LIST_ENTRY, * PSINGLE_LIST_ENTRY;

typedef pthread_mutex_t CRITICAL_SECTION;
 /**
  * Holds a pointer to a mutex to be released at function exit
  */
struct utils_mutex_holder
{
   pthread_mutex_t* mutex;
};
#endif

VOID FORCEINLINE
InitializeListHead(IN PLIST_ENTRY ListHead)
{
    ListHead->Flink = ListHead->Blink = ListHead;
}

#define IsListEmpty(ListHead) \
    ((ListHead)->Flink == (ListHead))

VOID FORCEINLINE
RemoveEntryList(IN PLIST_ENTRY Entry)
{
    PLIST_ENTRY Blink;
    PLIST_ENTRY Flink;

    Flink = Entry->Flink;
    Blink = Entry->Blink;
    Blink->Flink = Flink;
    Flink->Blink = Blink;
}

PLIST_ENTRY FORCEINLINE
RemoveHeadList(IN PLIST_ENTRY ListHead)
{
    PLIST_ENTRY Flink;
    PLIST_ENTRY Entry;

    Entry = ListHead->Flink;
    Flink = Entry->Flink;
    ListHead->Flink = Flink;
    Flink->Blink = ListHead;
    return Entry;
}

PLIST_ENTRY FORCEINLINE
RemoveTailList(IN PLIST_ENTRY ListHead)
{
    PLIST_ENTRY Blink;
    PLIST_ENTRY Entry;

    Entry = ListHead->Blink;
    Blink = Entry->Blink;
    ListHead->Blink = Blink;
    Blink->Flink = ListHead;
    return Entry;
}

VOID FORCEINLINE
InsertTailList(IN PLIST_ENTRY ListHead, IN PLIST_ENTRY Entry)
{
    PLIST_ENTRY Blink;

    Blink = ListHead->Blink;
    Entry->Flink = ListHead;
    Entry->Blink = Blink;
    Blink->Flink = Entry;
    ListHead->Blink = Entry;
}

VOID FORCEINLINE
InsertHeadList(IN PLIST_ENTRY ListHead, IN PLIST_ENTRY Entry)
{
    PLIST_ENTRY Flink;

    Flink = ListHead->Flink;
    Entry->Flink = Flink;
    Entry->Blink = ListHead;
    Flink->Blink = Entry;
    ListHead->Flink = Entry;
}

namespace Utils
{
   // Logging callback
   typedef VOID(_stdcall* QCD_LOGGING_CALLBACK)(int Level, PCHAR Message); // NULL-terminated ANSI string

   // APIs for Logging used by all modules
   enum QLogLevel
   {
      Data = 10,
      Debug = 20,
      Info = 30,
      Warn = 40,
      Error = 50,
      Exception = 60,
      Fatal = 70,
   };

   VOID QCD_Printf(QLogLevel Level, const PCHAR Format, ...);
   VOID setLoggerCallback(QCD_LOGGING_CALLBACK Cb);
}

#ifdef TOOLS_TARGET_WINDOWS
#else
/**
 * Call a function and log an error (with the value returned by the function)
 * if the call fails (returns value != 0).
 *
 * Intended to be used as an 'if' condition
 *
 * @param   function        Function to be called, including arguments, e.g.,
 *                          "foobar(1, 2, 3)"
 *
 * @returns                 True if the function returned a non-zero value,
 *                          false otherwise
 *
 * @note                    Depends on gcc extension allowing statements and
 *                          declarations in expressions
 */
#define QCDEV_FAILED_RC(function)                                       \
            ({int rc = function;                                        \
              if (rc != 0)                                              \
                  QCD_Printf(Utils::Error, "Call to " #function                    \
                                 " failed, rc=%d\n", rc); (rc != 0);})

/**
 * Cleanup handler for mutex holder
 *
 * @param   holder          Pointer to mutex holder
 *
 * @returns                 Nothing
 */
void utils_unlock_mutex(struct utils_mutex_holder* holder);
#endif

#endif // UTILS_H
