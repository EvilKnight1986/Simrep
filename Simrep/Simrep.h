#ifndef __Simrep__H
#define __Simrep__H

#ifdef __cplusplus
extern "C" {
#endif

#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>

//  Memory Pool Tags
#define Simrep_STRING_TAG       'Evil'

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers")

// Constants
#define REPLACE_ROUTINE_NAME_STRING     L"IoReplaceFileObjectName"
#define SIMREP_PORT_NAME                                L"\\SimrepMiniPort"


//
//  Starting with windows 7, the IO Manager provides IoReplaceFileObjectName, 
//  but old versions of Windows will not have this function. Rather than just 
//  writing our own function, and forfeiting future windows functionality, we can
//  use MmGetRoutineAddr, which will allow us to dynamically import IoReplaceFileObjectName
//  if it exists. If not it allows us to implement the function ourselves.
//

typedef
        NTSTATUS
        (* PReplaceFileObjectName ) (
        __in PFILE_OBJECT FileObject,
        __in_bcount(FileNameLength) PWSTR NewFileName,
        __in USHORT FileNameLength
        );


typedef struct _Simrep_GLOBAL_DATA
{
        // Handle to minifilter returned from FltRegisterFilter()
        PFLT_FILTER Filter;

        //  Pointer to the function we will use to 
        //  replace file names.
        PReplaceFileObjectName ReplaceFileNameFunction;

        // 内核与应用层通信端口使用
        PFLT_PORT 	ServerPort ;
        PFLT_PORT     ClientPort ;
#if DBG

        // Field to control nature of debug output        
        ULONG DebugLevel;
#endif
} Simrep_GLOBAL_DATA, *PSimrep_GLOBAL_DATA;

//  Defines the commands between the utility and the filter
typedef enum _SIMREPMINI_COMMAND
{
        ENUM_TEST = 0
} SIMREPMINI_COMMAND;	

//  Defines the command structure between the utility and the filter.
typedef struct _COMMAND_MESSAGE
{
        SIMREPMINI_COMMAND 	Command;  
} COMMAND_MESSAGE, *PCOMMAND_MESSAGE;

//  Debug helper functions
#if DBG

#define DEBUG_TRACE_ERROR                                                           0x00000001  // Errors - whenever we return a failure code
#define DEBUG_TRACE_LOAD_UNLOAD                                                 0x00000002  // Loading/unloading of the filter
#define DEBUG_TRACE_INSTANCES                                                   0x00000004  // Attach / detach of instances

#define DEBUG_TRACE_REPARSE_OPERATIONS                                  0x00000008  // Operations that are performed to determine if we should return STATUS_REPARSE
#define DEBUG_TRACE_REPARSED_OPERATIONS                                 0x00000010  // Operations that return STATUS_REPARSE
#define DEBUG_TRACE_REPARSED_REISSUE                                        0X00000020  // Operations that need to be reissued with an IRP.

#define DEBUG_TRACE_ALL_IO                                                          0x00000040  // All IO operations tracked by this filter

#define DEBUG_TRACE_ALL                                                                 0xFFFFFFFF  // All flags

#define DebugTrace(Level, Data)                                         \
        if ((Level) & Globals.DebugLevel) {                         \
        DbgPrint Data;                                                          \
        }

#else

#define DebugTrace(Level, Data)                         {NOTHING;}

#endif


/*************************************************************************
    Prototypes
*************************************************************************/
DRIVER_INITIALIZE DriverEntry;
NTSTATUS
DriverEntry (
    __in PDRIVER_OBJECT DriverObject,
    __in PUNICODE_STRING RegistryPath
    );

#if DBG

NTSTATUS
        SimrepInitializeDebugLevel (
        __in PUNICODE_STRING RegistryPath
        );

#endif

void SimrepFreeGlobals();

NTSTATUS
        SimrepUnload (
        FLT_FILTER_UNLOAD_FLAGS Flags
        );


NTSTATUS
        SimrepInstanceSetup (
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __in FLT_INSTANCE_SETUP_FLAGS Flags,
        __in DEVICE_TYPE VolumeDeviceType,
        __in FLT_FILESYSTEM_TYPE VolumeFilesystemType
        );

NTSTATUS
        SimrepInstanceQueryTeardown (
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __in FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
        );

//
//  Functions that track operations on the volume
//

FLT_PREOP_CALLBACK_STATUS
        SimrepPreCreate (
        __inout PFLT_CALLBACK_DATA Cbd,
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __out PVOID *CompletionContext
        );

FLT_PREOP_CALLBACK_STATUS
        SimrepPreRead (
        __inout PFLT_CALLBACK_DATA Cbd,
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __out PVOID *CompletionContext
        );

FLT_PREOP_CALLBACK_STATUS
        SimrepPreWrite (
        __inout PFLT_CALLBACK_DATA Cbd,
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __out PVOID *CompletionContext
        );

//
//  Functions that provide string allocation support
//

NTSTATUS
        SimrepAllocateUnicodeString (
        PUNICODE_STRING String
        );

VOID
        SimrepFreeUnicodeString (
        PUNICODE_STRING String
        );

NTSTATUS
        SimrepReplaceFileObjectName (
        __in PFILE_OBJECT FileObject,
        __in_bcount(FileNameLength) PWSTR NewFileName,
        __in USHORT FileNameLength
        );

// 生成重定向后的名字
BOOLEAN
        SimrepBuildPath(
        __in PFLT_FILE_NAME_INFORMATION nameInfo,
        __in PUNICODE_STRING pustrUserName,
        __inout PUNICODE_STRING pustrNewFilePath,
        __inout_opt PUNICODE_STRING pustrParentDir) ;

// 路径是否存在
BOOLEAN
        SimrepPathIsExist(
        __in PFLT_FILTER pFilter,
        __in PFLT_INSTANCE pInstance,
        __in PUNICODE_STRING pustrPath) ;

// 创建目录
BOOLEAN
        SimrepCreateSandboxDirectory(
        __in PFLT_FILTER pFilter,
        __in PFLT_INSTANCE pInstance,
        __in PUNICODE_STRING pustrPath) ;

NTSTATUS
        SimrepMiniMessage (
        __in PVOID ConnectionCookie,
        __in_bcount_opt(InputBufferSize) PVOID InputBuffer,
        __in ULONG InputBufferSize,
        __out_bcount_part_opt(OutputBufferSize,*ReturnOutputBufferLength) PVOID OutputBuffer,
        __in ULONG OutputBufferSize,
        __out PULONG ReturnOutputBufferLength
        );

NTSTATUS
        SimrepMiniConnect(
        __in PFLT_PORT ClientPort,
        __in PVOID ServerPortCookie,
        __in_bcount(SizeOfContext) PVOID ConnectionContext,
        __in ULONG SizeOfContext,
        __deref_out_opt PVOID *ConnectionCookie
        );

VOID
        SimrepMiniDisconnect(
        __in_opt PVOID ConnectionCookie
        );

//
//  Assign text sections for each routine.
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT,   DriverEntry)
#ifdef DBG
#pragma alloc_text(INIT,   SimrepInitializeDebugLevel)
#endif
#pragma alloc_text(PAGE, SimrepFreeGlobals)
#pragma alloc_text(PAGE, SimrepUnload)
#pragma alloc_text(PAGE, SimrepInstanceSetup)
#pragma alloc_text(PAGE, SimrepInstanceQueryTeardown)
#pragma alloc_text(PAGE, SimrepPreCreate)
#pragma alloc_text(PAGE, SimrepPreRead)
#pragma alloc_text(PAGE, SimrepPreWrite)
#pragma alloc_text(PAGE, SimrepAllocateUnicodeString)
#pragma alloc_text(PAGE, SimrepFreeUnicodeString)
#pragma alloc_text(PAGE, SimrepReplaceFileObjectName)
#pragma alloc_text(PAGE, SimrepBuildPath)
#pragma alloc_text(PAGE, SimrepPathIsExist)
#pragma alloc_text(PAGE, SimrepCreateSandboxDirectory)
#pragma alloc_text(PAGE, SimrepMiniMessage)
#pragma alloc_text(PAGE, SimrepMiniConnect)
#pragma alloc_text(PAGE, SimrepMiniDisconnect)
#endif

#ifdef __cplusplus
}
#endif

#endif