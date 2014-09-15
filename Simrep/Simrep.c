/*++

Copyright (c) 1999 - 2014  Microsoft Corporation

Module Name:

        Simrep.c

Abstract:

The Simulate Reparse Sample demonstrates how to return STATUS_REPARSE
on precreates. This allows the filter to redirect opens down one path 
to another path. The Precreate path is complicated by network query opens 
which come down as Fast IO. Fast IO cannot be redirected with Status Reparse
because reparse only works on IRP based IO. 

Simulating reparse points requires that the filter replace the name in the 
file object. This will cause Driver Verifier to complain that the filter is
leaking pool and will prevent it from being unloaded. To solve this issue
Simrep attempts to use a Windows 7 Function called IoReplaceFileObjectName
which will allow IO Mgr to replace the name for us with the correct pool tag.
However, on downlevel OS Versions Simrep will go ahead and replace the name 
itself.

It is important to note that Simrep only demonstrates how to return 
STATUS_REPARSE, not how to deal with file names on NT. 


Environment:

        Kernel mode

--*/

//
//  Includes
//

#include "Simrep.h"
#include "KernelUserManage.h"

//
//  Enabled warnings
//

#pragma warning(error:4100)         //  Enable-Unreferenced formal parameter
#pragma warning(error:4101)         //  Enable-Unreferenced local variable
#pragma warning(error:4061)         //  Enable-missing enumeration in switch statement
#pragma warning(error:4505)         //  Enable-identify dead functions



//
//  Filter callback routines
//
FLT_OPERATION_REGISTRATION Callbacks[] = 
{
        { IRP_MJ_CREATE,
                FLTFL_OPERATION_REGISTRATION_SKIP_PAGING_IO,
                SimrepPreCreate,
                NULL },

        { IRP_MJ_NETWORK_QUERY_OPEN,
                FLTFL_OPERATION_REGISTRATION_SKIP_PAGING_IO,
                SimrepPreCreate,
                NULL },
        { IRP_MJ_READ,
                FLTFL_OPERATION_REGISTRATION_SKIP_PAGING_IO,
                SimrepPreRead,
                NULL },

        { IRP_MJ_WRITE,
                FLTFL_OPERATION_REGISTRATION_SKIP_PAGING_IO,
                SimrepPreWrite,
                NULL },

        { IRP_MJ_OPERATION_END }
};

//
// Filter registration data structure
//
FLT_REGISTRATION FilterRegistration = 
{
        sizeof( FLT_REGISTRATION ),                                         //  Size
        FLT_REGISTRATION_VERSION,                                           //  Version
        0,                                                                                          //  Flags
        NULL,                                                                                   //  Context
        Callbacks,                                                                          //  Operation callbacks
        SimrepUnload,                                                                   //  Filters unload routine
        SimrepInstanceSetup,                                                        //  InstanceSetup routine
        SimrepInstanceQueryTeardown,                                        //  InstanceQueryTeardown routine
        NULL,                                                                                   //  InstanceTeardownStart routine
        NULL,                                                                                   //  InstanceTeardownComplete routine
        NULL,                                                                                   // GenerateFileNameCallback routine
        NULL,                                                                                   // NormalizeNameComponentCallback routine
        NULL                                                                                    //  NormalizeContextCleanupCallback routine
};

//
//  Global variables
//

Simrep_GLOBAL_DATA Globals;

//
//  Filter driver initialization and unload routines
//
NTSTATUS
DriverEntry (
        __in PDRIVER_OBJECT DriverObject,
        __in PUNICODE_STRING RegistryPath
        )
/*++

Routine Description:

        This is the initialization routine for this filter driver. It registers 
        itself with the filter manager and initializes all its global data structures.

Arguments:

        DriverObject - Pointer to driver object created by the system to
                represent this driver.

        RegistryPath - Unicode string identifying where the parameters for this
                driver are located in the registry.

Return Value:

        Returns STATUS_SUCCESS.

--*/
{
        NTSTATUS                        status = STATUS_UNSUCCESSFUL ;
        UNICODE_STRING          replaceRoutineName;
        PSECURITY_DESCRIPTOR sd;
        OBJECT_ATTRIBUTES       oa;
        UNICODE_STRING           ustrPortName;		//for communication port name

        UNREFERENCED_PARAMETER(RegistryPath) ;

        //
        //  Zero Out Globals
        //

        RtlZeroMemory( &Globals, sizeof( Globals ) );

        // Initialize Kernel user manage
        if (! InitializeKernelUserManage())
        {
                goto DriverEntryCleanup;
        }

#if DBG

        //
        //  Initialize global debug level
        //
        status = SimrepInitializeDebugLevel( RegistryPath );

        if (!NT_SUCCESS(status) )
        {
                goto DriverEntryCleanup;
        }

#endif

        DebugTrace( DEBUG_TRACE_LOAD_UNLOAD,
                                ("[Simrep]: Driver being loaded\n") );

        //
        //  Import function to replace file names.
        //

        RtlInitUnicodeString( &replaceRoutineName, REPLACE_ROUTINE_NAME_STRING );

#pragma warning(push)
#pragma warning(disable:4152) // nonstandard extension, function/data pointer conversion in expression

        // 这个具体可以看Simrep.h头文件
        Globals.ReplaceFileNameFunction = MmGetSystemRoutineAddress( &replaceRoutineName );
        if (Globals.ReplaceFileNameFunction == NULL)
        {
                Globals.ReplaceFileNameFunction = SimrepReplaceFileObjectName;
        }

#pragma warning(pop)

        //
        //  Register with the filter manager
        //

        status = FltRegisterFilter( DriverObject,
                                                                &FilterRegistration,
                                                                &Globals.Filter );

        if (!NT_SUCCESS( status ))
        {
                goto DriverEntryCleanup;
        }

        //
        //  Start filtering I/O
        //

        status = FltStartFiltering( Globals.Filter );

        if (!NT_SUCCESS( status ))
        {
                FltUnregisterFilter( Globals.Filter );
        }

        DebugTrace( DEBUG_TRACE_LOAD_UNLOAD,
                                ("[Simrep]: Driver loaded complete (Status = 0x%08X)\n",
                                status) );

        // 为与应用层通信创建通信端口
        status  = FltBuildDefaultSecurityDescriptor( &sd, FLT_PORT_ALL_ACCESS );

        if (!NT_SUCCESS( status ))
        {
                goto DriverEntryCleanup;
        }

        RtlInitUnicodeString( &ustrPortName, SIMREP_PORT_NAME ) ;

        InitializeObjectAttributes(&oa, 
                                                        &ustrPortName,
                                                        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                                                        NULL,
                                                        sd) ;

        status = FltCreateCommunicationPort( Globals.Filter,
                                                                                &(Globals.ServerPort),
                                                                                &oa,
                                                                                NULL,
                                                                                SimrepMiniConnect,
                                                                                SimrepMiniDisconnect,
                                                                                SimrepMiniMessage,
                                                                                1 );
        FltFreeSecurityDescriptor( sd );

DriverEntryCleanup:

        if (!NT_SUCCESS(status))
        {
                SimrepFreeGlobals();
        }

        return status;
}

#if DBG

NTSTATUS
SimrepInitializeDebugLevel (
        __in PUNICODE_STRING RegistryPath
        )
/*++

Routine Description:

        This routine tries to read the filter DebugLevel parameter from 
        the registry.  This value will be found in the registry location
        indicated by the RegistryPath passed in.

Arguments:

        RegistryPath - The path key passed to the driver during DriverEntry.
                
Return Value:

        None.

--*/
{
        OBJECT_ATTRIBUTES attributes;
        HANDLE driverRegKey;
        BOOLEAN closeHandle = FALSE;
        NTSTATUS status;
        ULONG resultLength;
        UNICODE_STRING valueName;
        UCHAR buffer[sizeof( KEY_VALUE_PARTIAL_INFORMATION ) + sizeof( LONG )];

        Globals.DebugLevel = DEBUG_TRACE_ALL;

        //
        //  Open the desired registry key
        //

        InitializeObjectAttributes( &attributes,
                                                                RegistryPath,
                                                                OBJ_CASE_INSENSITIVE,
                                                                NULL,
                                                                NULL );

        status = ZwOpenKey( &driverRegKey,
                                                KEY_READ,
                                                &attributes );

        if (!NT_SUCCESS( status )) {
                
                goto SimrepInitializeDebugLevelCleanup;
        }

        closeHandle = TRUE;

        //
        // Read the DebugFlags value from the registry.
        //

        RtlInitUnicodeString( &valueName, L"DebugLevel" );
                
        status = ZwQueryValueKey( driverRegKey,
                                                          &valueName,
                                                          KeyValuePartialInformation,
                                                          buffer,
                                                          sizeof(buffer),
                                                          &resultLength );

        if (!NT_SUCCESS( status ))
        {
                goto SimrepInitializeDebugLevelCleanup;
        }

        Globals.DebugLevel = *((PULONG) &(((PKEY_VALUE_PARTIAL_INFORMATION) buffer)->Data));

SimrepInitializeDebugLevelCleanup:

        //
        //  Close the registry entry
        //

        if (closeHandle)
        {
               ZwClose( driverRegKey );
        }

        return status;
}

#endif


void SimrepFreeGlobals()
/*++

Routine Descrition:

        This routine cleans up the global structure on both
        teardown and initialization failure.

Arguments:

Return Value:

        None.

--*/
{
        PAGED_CODE();

        if (NULL != Globals.ServerPort)
        {
                FltCloseCommunicationPort( Globals.ServerPort );
                Globals.ServerPort = NULL ;
        }

        if (NULL != Globals.Filter)
        {
                FltUnregisterFilter( Globals.Filter );
                Globals.Filter = NULL ;
        }
}

NTSTATUS
SimrepUnload (
        FLT_FILTER_UNLOAD_FLAGS Flags
        )
/*++

Routine Description:

        This is the unload routine for this filter driver. This is called 
        when the minifilter is about to be unloaded. Simrep can unload 
        easily because it does not own any IOs. When the filter is unloaded
        existing reparsed creates will continue to work, but new creates will
        not be reparsed. This is fine from the filter's perspective, but could
        result in unexpected bahavior for apps.

Arguments:

        Flags - Indicating if this is a mandatory unload.

Return Value:

        Returns the final status of this operation.

--*/
{
        UNREFERENCED_PARAMETER( Flags );

        PAGED_CODE();

        DebugTrace( DEBUG_TRACE_LOAD_UNLOAD,
                                ("[Simrep]: Unloading driver\n") );
        
        if (NULL != Globals.Filter)
        {
                FltUnregisterFilter( Globals.Filter );
                Globals.Filter = NULL ;
        }

        SimrepFreeGlobals();

        ReleaseKernelUserManage() ;

        return STATUS_SUCCESS;
}


//
//  Instance setup/teardown routines.
//

NTSTATUS
SimrepInstanceSetup (
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __in FLT_INSTANCE_SETUP_FLAGS Flags,
        __in DEVICE_TYPE VolumeDeviceType,
        __in FLT_FILESYSTEM_TYPE VolumeFilesystemType
        )
/*++

Routine Description:

        This routine is called whenever a new instance is created on a volume. This
        gives us a chance to decide if we need to attach to this volume or not.
        Simrep does not attach on automatic attachment, but will attach when asked
        manually.

Arguments:

        FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
                opaque handles to this filter, instance and its associated volume.

        Flags - Flags describing the reason for this attach request.

Return Value:

        STATUS_SUCCESS - attach
        STATUS_FLT_DO_NOT_ATTACH - do not attach

--*/
{
        
        UNREFERENCED_PARAMETER( FltObjects );
        UNREFERENCED_PARAMETER( Flags );
        UNREFERENCED_PARAMETER( VolumeDeviceType );
        UNREFERENCED_PARAMETER( VolumeFilesystemType );

        PAGED_CODE();

        if ( FlagOn( Flags, FLTFL_INSTANCE_SETUP_AUTOMATIC_ATTACHMENT ) ) {

                //
                //  Do not automatically attach to a volume.
                //

                DebugTrace( DEBUG_TRACE_INSTANCES,
                                        ("[Simrep]: Instance setup skipped (Volume = %p, Instance = %p)\n",
                                        FltObjects->Volume,
                                        FltObjects->Instance) );

                return STATUS_FLT_DO_NOT_ATTACH;
        }

        //
        //  Attach on manual attachment.
        //

        DebugTrace( DEBUG_TRACE_INSTANCES, 
                                ("[Simrep]: Instance setup started (Volume = %p, Instance = %p)\n",
                                 FltObjects->Volume, 
                                 FltObjects->Instance) );


        return STATUS_SUCCESS;
}

NTSTATUS
SimrepInstanceQueryTeardown (
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __in FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
        )
/*++

Routine Description:

        This is called when an instance is being manually deleted by a
        call to FltDetachVolume or FilterDetach thereby giving us a
        chance to fail that detach request. Simrep only implements it 
        because otherwise calls to FltDetachVolume or FilterDetach would
        fail to detach.

Arguments:

        FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
                opaque handles to this filter, instance and its associated volume.

        Flags - Indicating where this detach request came from.

Return Value:

        Returns the status of this operation.

--*/
{
        UNREFERENCED_PARAMETER( FltObjects );
        UNREFERENCED_PARAMETER( Flags );

        PAGED_CODE();

        DebugTrace( DEBUG_TRACE_INSTANCES,
                                ("[Simrep]: Instance query teadown ended (Instance = %p)\n",
                                 FltObjects->Instance) );

        return STATUS_SUCCESS;
}

FLT_PREOP_CALLBACK_STATUS
SimrepPreCreate (
        __inout PFLT_CALLBACK_DATA Cbd,
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __out PVOID *CompletionContext
        )
/*++

Routine Description:

        This routine does the work for Simrep sample. SimrepPreCreate is called in 
        the pre-operation path for IRP_MJ_CREATE and IRP_MJ_NETWORK_QUERY_OPEN. 
        The function queries the requested file name for  the create and compares
        it to the mapping path. If the file is down the "old mapping path", the 
        filter checks to see if the request is fast io based. If it is we cannot
        reparse the create because fast io does not support STATUS_REPARSE. 
        Instead we return FLT_PREOP_DISALLOW_FASTIO to force the io to be reissued 
        on the IRP path. If the create is IRP based, then we replace the file 
        object's file name field with a new path based on the "new mapping path".

        This is pageable because it could not be called on the paging path

Arguments:

        Data - Pointer to the filter callbackData that is passed to us.

        FltObjects - Pointer to the FLT_RELATED_OBJECTS data structure containing
                opaque handles to this filter, instance, its associated volume and
                file object.

        CompletionContext - The context for the completion routine for this
                operation.

Return Value:

        The return value is the status of the operation.

--*/
{
        PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
        NTSTATUS status = STATUS_SUCCESS ;
        FLT_PREOP_CALLBACK_STATUS callbackStatus = FLT_PREOP_SUCCESS_NO_CALLBACK ;
        //UNICODE_STRING fileName; //The open name from the end of the volume name to the end.
        //UNICODE_STRING newFileName; //The output file name.
        PUNICODE_STRING pUserName  = NULL ;
        ULONG   uID = 0 ;                                               // 保存用户ID
        ULONG   uAccess = 0 ;
        ULONG   uCreate = 0 ;
        UNICODE_STRING ustrNewFilePath = {0};                   // 用来保存新生成的路径
        UNICODE_STRING ustrVolume1 = {0} ;                       // 卷标，用来判断是否为第一个卷
        UNICODE_STRING ustrFileName = {0} ;                     // 用来比较是否是对windows目录进行访问
        UNICODE_STRING ustrMatchFileName = {0} ;          // 同上
        UNICODE_STRING ustrParentDir = {0} ;                     // 用来保存重定向后的父目录
        UNICODE_STRING ustrExtension = {0} ;                     // 用来判断操作后辍名
        const WCHAR wstrVolume1[] = L"\\Device\\HarddiskVolume1" ;

        UNREFERENCED_PARAMETER(Cbd) ;
        UNREFERENCED_PARAMETER( FltObjects );
        UNREFERENCED_PARAMETER( CompletionContext );

        PAGED_CODE();

        __try
        {
                // 如果SL_OPEN_PAGING_FILE设置，说明是页面文件
                if (FlagOn( Cbd->Iopb->OperationFlags, SL_OPEN_PAGING_FILE ))
                {
                        DbgPrint("SimrepPreCreate SL_OPEN_PAGING_FILE\r\n") ;
                        __leave ;
                }

                // 这是一次卷打开请求
                if (FlagOn( Cbd->Iopb->TargetFileObject->Flags, FO_VOLUME_OPEN )) 
                { 
                        DbgPrint("SimrepPreCreate FO_VOLUME_OPEN\r\n") ;
                        __leave ;
                }

                status = FltGetFileNameInformation( Cbd,
                                                                                FLT_FILE_NAME_OPENED |
                                                                                FLT_FILE_NAME_QUERY_ALWAYS_ALLOW_CACHE_LOOKUP,
                                                                                &nameInfo );
                if (!NT_SUCCESS( status )) 
                {
                        DbgPrint("SimrepPreCreate::FltGetFileNameInformation failed\r\n") ;
                        __leave;
                }

                status = FltParseFileNameInformation( nameInfo );
                if (!NT_SUCCESS( status ))
                {
                        DbgPrint("SimrepPreCreate::FltParseFileNameInformation failed\r\n") ;
                        __leave ;
                }

                uAccess = Cbd->Iopb->Parameters.Create.SecurityContext->DesiredAccess;
                uCreate = (Cbd->Iopb->Parameters.Create.Options >> 24) & 0x000000FF;

                // 如果不是我们感兴趣的，就放它走吧
                if((FILE_CREATE != uCreate) &&
                        (FILE_OPEN_IF != uCreate) &&
                        (FILE_OVERWRITE_IF != uCreate) &&
                        (FILE_SUPERSEDE != uCreate)) 
                {
                        __leave ;
                }

                // 这里我们只处理对于系统盘(C盘)的文件重定向
                RtlInitUnicodeString(&ustrVolume1, wstrVolume1) ;

                // 如果不是系统盘(卷一)的话，就闪人
                if (FALSE == RtlEqualUnicodeString(&(nameInfo->Volume), &ustrVolume1, TRUE)) 
                {
                        DbgPrint("SimrepPreCreate::RtlEqualUnicodeString no equal!\r\n") ;
                        __leave ;
                }

                // 只处理对于c:\ccccc的重定向
                // 先取得除了卷标之外的文件路径
                ustrFileName.Buffer = Add2Ptr(nameInfo->Name.Buffer, nameInfo->Volume.Length) ;
                ustrFileName.Length = nameInfo->Name.Length - nameInfo->Volume.Length;
                ustrFileName.MaximumLength = ustrFileName.Length;

                RtlInitUnicodeString(&ustrMatchFileName, L"\\ccccc") ;

                // 判断是不是\ccccc
                if(! RtlPrefixUnicodeString(&ustrMatchFileName, &ustrFileName, TRUE))
                {
                        DbgPrint("SimrepPreCreate::RtlPrefixUnicodeString return FALSE\r\n") ;
                        __leave ;
                }

                // 只处理.txt后辍
                RtlInitUnicodeString(&ustrExtension, L"txt") ;
                if (FALSE == RtlEqualUnicodeString( &ustrExtension, &(nameInfo->Extension), TRUE))
                {
                        __leave ;
                }

                // 先取得当前进程的pid,再通过pid得到用户的id值，再通过用户id值取得用户名
                uID = GetProcessUID((ULONG)PsGetCurrentProcessId()) ;

#ifdef DBG
                DbgPrint("SimrepPreCreate::GetProcessUID UID is : %p\r\n", uID) ;
#endif

                //// 这里可以判断是不是Administrator用户，UID值为0x1f4，是的话就不重定向了
                //if (0x1f4 == uID)
                //{
                //        __leave ;
                //}
                pUserName = GetUserNameByUID(uID) ;

                if (NULL == pUserName)
                {
                        DbgPrint("SimrepPreCreate::GetUserNameByUID failed\r\n") ;
                        __leave ;
                }

#ifdef DBG
                DbgPrint("Current Process owner user : %wZ\n", pUserName) ;
#endif

                KdBreakPoint() ;
                // 这里生成我们重定向以及创建目录所需要的路径
                if(! SimrepBuildPath(nameInfo, pUserName, &ustrNewFilePath, &ustrParentDir))
                {
                        DbgPrint("SimrepPreCreate::SimrepBuildPath failed!\r\n") ;
                        __leave ;
                }

                DbgPrint("SimrepBuildPath: %wZ!\r\n", ustrNewFilePath) ; 
                

                // 判断路径是否存在
                //if (SimrepPathIsExist(FltObjects->Filter, FltObjects->Instance, &ustrNewFilePath))
                //{
                //        DbgPrint("SimrepBuildPath:  %wZ is Exist!\r\n", ustrNewFilePath) ;
                //}
                //else
                //{
                //        DbgPrint("SimrepBuildPath:  %wZ can't find!\r\n", ustrNewFilePath) ;
                //}

                // 创建目录
                SimrepCreateSandboxDirectory(FltObjects->Filter, FltObjects->Instance,&ustrParentDir) ;

                // 修改FileObject中的路径
                status = Globals.ReplaceFileNameFunction( Cbd->Iopb->TargetFileObject,
                                                                                                ustrNewFilePath.Buffer,
                                                                                                ustrNewFilePath.Length );
        }

        __finally
        {
                if (NULL != nameInfo)
                {
                        FltReleaseFileNameInformation(nameInfo) ;
                        nameInfo = NULL ;
                }
                if (status == STATUS_REPARSE)
                {
                        Cbd->IoStatus.Status = STATUS_REPARSE;
                        Cbd->IoStatus.Information = IO_REPARSE;
                        callbackStatus = FLT_PREOP_COMPLETE;        
                }   
                else if (!NT_SUCCESS( status ))
                {
                        DebugTrace( DEBUG_TRACE_ERROR,
                                ("[SimRep]: SimRepPreCreate -> Failed with status 0x%x \r\n",
                                status) );
                        Cbd->IoStatus.Status = status;
                        callbackStatus = FLT_PREOP_COMPLETE;
                }
                // 这步的检查是为了防止还没有申请内存就已经__leave的情况
                // 那时Buffer为NULL
                if (NULL != ustrNewFilePath.Buffer)
                {
                        SimrepFreeUnicodeString(&ustrNewFilePath) ;
                }
                if (NULL != ustrParentDir.Buffer)
                {
                        SimrepFreeUnicodeString(&ustrParentDir) ;
                }
        }
        return callbackStatus ;
}


FLT_PREOP_CALLBACK_STATUS
        SimrepPreRead (
        __inout PFLT_CALLBACK_DATA Cbd,
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __out PVOID *CompletionContext
        )
{
        PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
        NTSTATUS status;
        FLT_PREOP_CALLBACK_STATUS callbackStatus;

        UNREFERENCED_PARAMETER( FltObjects );
        UNREFERENCED_PARAMETER( CompletionContext );
        DebugTrace( DEBUG_TRACE_ALL_IO,
                ("[SimRep]: SimrepPreRead -> Enter (Cbd = %p, FileObject = %p)\n",
                Cbd,
                FltObjects->FileObject) );

        //
        // Initialize defaults
        //

        status = STATUS_SUCCESS;
        callbackStatus = FLT_PREOP_SUCCESS_NO_CALLBACK; // pass through - default is no post op callback

        __try
        {
                //
                //  Check if this is a paging file as we don't want to redirect
                //  the location of the paging file.
                //

                if (FlagOn( Cbd->Iopb->OperationFlags, SL_OPEN_PAGING_FILE ))
                {
                        DebugTrace( DEBUG_TRACE_ALL_IO,
                                ("[SimRep]: SimrepPreRead -> Ignoring paging file open (Cbd = %p, FileObject = %p)\n",
                                Cbd,
                                FltObjects->FileObject) );

                        __leave ;
                }

                status = FltGetFileNameInformation( Cbd,
                                                                                FLT_FILE_NAME_OPENED |
                                                                                FLT_FILE_NAME_QUERY_ALWAYS_ALLOW_CACHE_LOOKUP,
                                                                                &nameInfo );
                if (!NT_SUCCESS( status ))
                {
                        DebugTrace( DEBUG_TRACE_REPARSE_OPERATIONS | DEBUG_TRACE_ERROR,
                                ("[SimRep]: SimrepPreRead -> Failed to get name information (Cbd = %p, FileObject = %p)\n",
                                Cbd,
                                FltObjects->FileObject) );

                        __leave;
                }


                DebugTrace( DEBUG_TRACE_REPARSE_OPERATIONS,
                        ("[SimRep]: SimrepPreRead -> Processing Read for file %wZ (Cbd = %p, FileObject = %p)\n",
                        &nameInfo->Name,
                        Cbd,
                        FltObjects->FileObject) );

                //
                //  Parse the filename information
                //

                status = FltParseFileNameInformation( nameInfo );
                if (!NT_SUCCESS( status ))
                {
                        DebugTrace( DEBUG_TRACE_REPARSE_OPERATIONS | DEBUG_TRACE_ERROR,
                                ("[SimRep]: SimrepPreRead -> Failed to parse name information for file %wZ (Cbd = %p, FileObject = %p)\n",
                                &nameInfo->Name,
                                Cbd,
                                FltObjects->FileObject) );

                        __leave ;
                }
        }

        __finally
        {
                if (nameInfo != NULL)
                {
                        FltReleaseFileNameInformation( nameInfo );
                }
                if (status == STATUS_REPARSE)
                {
                        Cbd->IoStatus.Status = STATUS_REPARSE;
                        Cbd->IoStatus.Information = IO_REPARSE;
                        callbackStatus = FLT_PREOP_COMPLETE;                
                }   
                else if (!NT_SUCCESS( status ))
                {
                        //
                        //  An error occurred, fail the open
                        //
                        DebugTrace( DEBUG_TRACE_ERROR,
                                ("[SimRep]: SimRepPreRead -> Failed with status 0x%x \n",
                                status) );

                        Cbd->IoStatus.Status = status;
                        callbackStatus = FLT_PREOP_COMPLETE;
                }
        }

        DebugTrace( DEBUG_TRACE_ALL_IO,
                ("[SimRep]: SimRepPreRead -> Exit (Cbd = %p, FileObject = %p)\n",
                Cbd,
                FltObjects->FileObject) );

        return callbackStatus;
}

FLT_PREOP_CALLBACK_STATUS
        SimrepPreWrite (
        __inout PFLT_CALLBACK_DATA Cbd,
        __in PCFLT_RELATED_OBJECTS FltObjects,
        __out PVOID *CompletionContext
        )
{
        PFLT_FILE_NAME_INFORMATION nameInfo = NULL;
        NTSTATUS status;
        FLT_PREOP_CALLBACK_STATUS callbackStatus;

        UNREFERENCED_PARAMETER( FltObjects );
        UNREFERENCED_PARAMETER( CompletionContext );
        DebugTrace( DEBUG_TRACE_ALL_IO,
                ("[SimRep]: SimrepPreWrite -> Enter (Cbd = %p, FileObject = %p)\n",
                Cbd,
                FltObjects->FileObject) );

        //
        // Initialize defaults
        //

        status = STATUS_SUCCESS;
        callbackStatus = FLT_PREOP_SUCCESS_NO_CALLBACK; // pass through - default is no post op callback

        __try
        {
                //
                //  Check if this is a paging file as we don't want to redirect
                //  the location of the paging file.
                //

                if (FlagOn( Cbd->Iopb->OperationFlags, SL_OPEN_PAGING_FILE ))
                {
                        DebugTrace( DEBUG_TRACE_ALL_IO,
                                ("[SimRep]: SimrepPreWrite -> Ignoring paging file open (Cbd = %p, FileObject = %p)\n",
                                Cbd,
                                FltObjects->FileObject) );

                        __leave ;
                }

                status = FltGetFileNameInformation( Cbd,
                        FLT_FILE_NAME_OPENED |
                        FLT_FILE_NAME_QUERY_ALWAYS_ALLOW_CACHE_LOOKUP,
                        &nameInfo );
                if (!NT_SUCCESS( status ))
                {
                        DebugTrace( DEBUG_TRACE_REPARSE_OPERATIONS | DEBUG_TRACE_ERROR,
                                ("[SimRep]: SimrepPreWrite -> Failed to get name information (Cbd = %p, FileObject = %p)\n",
                                Cbd,
                                FltObjects->FileObject) );

                        __leave;
                }


                DebugTrace( DEBUG_TRACE_REPARSE_OPERATIONS,
                        ("[SimRep]: SimrepPreWrite -> Processing Read for file %wZ (Cbd = %p, FileObject = %p)\n",
                        &nameInfo->Name,
                        Cbd,
                        FltObjects->FileObject) );

                //
                //  Parse the filename information
                //

                status = FltParseFileNameInformation( nameInfo );
                if (!NT_SUCCESS( status )) {

                        DebugTrace( DEBUG_TRACE_REPARSE_OPERATIONS | DEBUG_TRACE_ERROR,
                                ("[SimRep]: SimrepPreWrite -> Failed to parse name information for file %wZ (Cbd = %p, FileObject = %p)\n",
                                &nameInfo->Name,
                                Cbd,
                                FltObjects->FileObject) );

                        __leave ;
                }
        }

        __finally
        {
                if (nameInfo != NULL)
                {
                        FltReleaseFileNameInformation( nameInfo );
                }
                if (status == STATUS_REPARSE)
                {
                        Cbd->IoStatus.Status = STATUS_REPARSE;
                        Cbd->IoStatus.Information = IO_REPARSE;
                        callbackStatus = FLT_PREOP_COMPLETE;                
                }   
                else if (!NT_SUCCESS( status ))
                {
                        //
                        //  An error occurred, fail the open
                        //
                        DebugTrace( DEBUG_TRACE_ERROR,
                                ("[SimRep]: SimrepPreWrite -> Failed with status 0x%x \n",
                                status) );

                        Cbd->IoStatus.Status = status;
                        callbackStatus = FLT_PREOP_COMPLETE;
                }
        }

        DebugTrace( DEBUG_TRACE_ALL_IO,
                ("[SimRep]: SimrepPreWrite -> Exit (Cbd = %p, FileObject = %p)\n",
                Cbd,
                FltObjects->FileObject) );

        return callbackStatus;
}

//
//  Support Routines
//

NTSTATUS
SimrepAllocateUnicodeString (
        PUNICODE_STRING String
        )
/*++

Routine Description:

        This routine allocates a unicode string

Arguments:

        String - supplies the size of the string to be allocated in the MaximumLength field 
                         return the unicode string

Return Value:

        STATUS_SUCCESS                                  - success
        STATUS_INSUFFICIENT_RESOURCES   - failure
  
--*/
{

        PAGED_CODE();

        String->Buffer = ExAllocatePoolWithTag( NonPagedPool,
                                                                                        String->MaximumLength,
                                                                                        Simrep_STRING_TAG );

        if (String->Buffer == NULL)
        {
                DebugTrace( DEBUG_TRACE_ERROR,
                                        ("[Simrep]: Failed to allocate unicode string of size 0x%x\n",
                                        String->MaximumLength) );

                return STATUS_INSUFFICIENT_RESOURCES;
        }

        String->Length = 0;

        return STATUS_SUCCESS;
}

VOID
SimrepFreeUnicodeString (
        PUNICODE_STRING String
        )
/*++

Routine Description:

        This routine frees a unicode string

Arguments:

        String - supplies the string to be freed 

Return Value:

        None        

--*/
{
        PAGED_CODE();

        if (String->Buffer)
        {
                ExFreePoolWithTag( String->Buffer,
                                                   Simrep_STRING_TAG );
                String->Buffer = NULL;
        }

        String->Length = String->MaximumLength = 0;
        String->Buffer = NULL;
}

NTSTATUS
SimrepReplaceFileObjectName (
        __in PFILE_OBJECT FileObject,
        __in_bcount(FileNameLength) PWSTR NewFileName,
        __in USHORT FileNameLength
        )
/*++
Routine Description:

        This routine is used to replace a file object's name
        with a provided name. This should only be called if 
        IoReplaceFileObjectName is not on the system.
        If this function is used and verifier is enabled
        the filter will fail to unload due to a false 
        positive on the leaked pool test.

Arguments:

        FileObject - Pointer to file object whose name is to be replaced.

        NewFileName - Pointer to buffer containing the new name.

        FileNameLength - Length of the new name in bytes.

Return Value:

        STATUS_INSUFFICIENT_RESOURCES - No memory to allocate the new buffer.

        STATUS_SUCCESS otherwise.

--*/
{
        PWSTR buffer;
        PUNICODE_STRING fileName;
        USHORT newMaxLength;

        PAGED_CODE();

        fileName = &FileObject->FileName;

        //
        // If the new name fits inside the current buffer we simply copy it over
        // instead of allocating a new buffer (and keep the MaximumLength value
        // the same).
        //
        if (FileNameLength <= fileName->MaximumLength) {

                goto CopyAndReturn;
        }

        //
        // Use an optimal buffer size
        //
        newMaxLength = FileNameLength;

        buffer = ExAllocatePoolWithTag( PagedPool,
                                                                        newMaxLength,
                                                                        Simrep_STRING_TAG );

        if (!buffer) {

                return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (fileName->Buffer != NULL) {

                ExFreePool(fileName->Buffer);
        }

        fileName->Buffer = buffer;
        fileName->MaximumLength = newMaxLength;

CopyAndReturn:

        fileName->Length = FileNameLength;
        RtlZeroMemory(fileName->Buffer, fileName->MaximumLength);
        RtlCopyMemory(fileName->Buffer, NewFileName, FileNameLength);

        return STATUS_SUCCESS;
}

// 生成重定向后的名字
BOOLEAN
        SimrepBuildPath(
        __in PFLT_FILE_NAME_INFORMATION nameInfo,
        __in PUNICODE_STRING pustrUserName,
        __inout PUNICODE_STRING pustrNewFilePath,
        __inout_opt PUNICODE_STRING pustrParentDir)
/*++
Routine Description:

        生成重定向之后的路径与创建目录所需要的路径。
        这里大家可以自己实现将卷标转成盘符

Arguments:

        nameInfo - Pointer to PFLT_FILE_NAME_INFORMATION struct.

        pustrUserName - Pointer to Current user name .

        pustrNewFilePath - Pointer to buffer containing the new name.
                                          这个是不包含卷，但是包含文件名的，用于后面替换FileObject中FileName用的

        pustrParentDir      -  这个是包含卷，但是不包含文件名的,用于后面创建目录用的

Return Value:

        生成重定向后的名件路径成功返回TRUE,失败返回FALSE.

--*/
{
        BOOLEAN bResult = FALSE ;
        const WCHAR wstrSandboxPath[] = L"\\HotDoor\\Sandbox\\" ;
        const USHORT uMaxPath = 260 ;
        const WCHAR wstrVolume1[] = L"\\Device\\HarddiskVolume1" ;

        __try
        {
                if (NULL == pustrNewFilePath
                        || NULL == pustrUserName
                        || NULL == pustrNewFilePath)
                {
                        DbgPrint("SimrepBuildPath parameter can't NULL!\r\n") ;
                        __leave ;
                }

                //if (NULL != pustrNewFilePath->Buffer)
                //{
                //        ExFreePool(pustrNewFilePath->Buffer) ;
                //}

                // 这里开始构造我们重定向后的路径
                pustrNewFilePath->Buffer = ExAllocatePoolWithTag(PagedPool,uMaxPath*2, Simrep_STRING_TAG);
                if (NULL == pustrNewFilePath->Buffer)
                {
                        DbgPrint("SimrepPreCreate::pustrNewFilePath ExAllocatePoolWithTag failed\r\n") ;
                        __leave ;
                }
                pustrNewFilePath->Length = 0 ;
                pustrNewFilePath->MaximumLength = uMaxPath*2 ;

                // 先把卷复制过去
                // 在以后的产品中注意，这个值可能是变动的，比如系统盘在D盘，C盘是别的系统之类的
                //RtlAppendUnicodeToString(pustrNewFilePath, wstrVolume1) ;

                // 再添加转存的路径
                RtlAppendUnicodeToString(pustrNewFilePath, wstrSandboxPath) ;
                // 再添加用户名
                RtlAppendUnicodeStringToString(pustrNewFilePath, pustrUserName) ;

                RtlAppendUnicodeToString(pustrNewFilePath, L"\\Driver\\") ;

                //  这里直接处理C盘的，如果以后用在产品上，要把卷标转成盘符
                // 通过调用QuerySymbolicLink
                RtlAppendUnicodeToString(pustrNewFilePath, L"C") ;

                // 这里再添加原来的路径(不包括卷的)
                // 也可以用Add2Ptr把直接处理nameInfo->name的
                RtlAppendUnicodeStringToString(pustrNewFilePath, &(nameInfo->ParentDir)) ;

                if (NULL != pustrParentDir)
                {
                        if(NULL != pustrParentDir->Buffer)
                        {
                                ExFreePool(pustrParentDir->Buffer) ;
                        }
                        pustrParentDir->Buffer = ExAllocatePoolWithTag(PagedPool, 
                                                                                                                uMaxPath*sizeof(WCHAR), 
                                                                                                                Simrep_STRING_TAG) ;
                        pustrParentDir->Length = 0 ;
                        pustrParentDir->MaximumLength = uMaxPath * sizeof( WCHAR) ;

                        if (NULL != pustrParentDir->Buffer)
                        {
                                RtlAppendUnicodeToString(pustrParentDir, wstrVolume1) ;
                                RtlAppendUnicodeStringToString(pustrParentDir, pustrNewFilePath) ;
                        }
                }
                RtlAppendUnicodeStringToString(pustrNewFilePath, &(nameInfo->FinalComponent)) ;

                bResult = TRUE ;
                //DbgPrint("SimrepBuildPath: %wZ\r\n", pustrNewFilePath) ;
        }

        __finally
        {
                if (! bResult)
                {
                        if (NULL != pustrNewFilePath)
                        {
                                RtlFreeUnicodeString(pustrNewFilePath) ;
                        }
                }
        }
        return bResult ;
}

// 路径是否存在
BOOLEAN
        SimrepPathIsExist(
        __in PFLT_FILTER pFilter,
        __in PFLT_INSTANCE pInstance,
        __in PUNICODE_STRING pustrPath)
/*++
Routine Description:

        判断路径是否存在

Arguments:

        pFilter      -

        pInstance -

        pustrPath - 指向要判断是否存在的路径

Return Value:

        路径存在返回TRUE，不存在返回FALSE.

--*/
{
        BOOLEAN bIsExist = FALSE ;

        NTSTATUS				status;	
        HANDLE					hFile;
        OBJECT_ATTRIBUTES		objAttrib = {0};
        IO_STATUS_BLOCK			ioStatus;

        __try
        {
                KdBreakPoint() ;
                if(NULL == pFilter
                    || NULL == pustrPath)
                {
                        __leave;
                }

                InitializeObjectAttributes(&objAttrib,
                                                                pustrPath,
                                                                OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                                                NULL,
                                                                NULL);

                status = FltCreateFile(pFilter,
                                                        pInstance,    
                                                        &hFile,
                                                        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                                        &objAttrib,
                                                        &ioStatus,
                                                        0,
                                                        FILE_ATTRIBUTE_NORMAL,
                                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                        FILE_OPEN,
                                                        FILE_SYNCHRONOUS_IO_NONALERT,
                                                        NULL,0,0);

                if(NT_SUCCESS(status))
                {
                        FltClose(hFile);
                        bIsExist = TRUE ;
                        __leave ;
                }

                //因为共享冲突打开失败，也返回TRUE
                if(status == STATUS_SHARING_VIOLATION )
                {
                        bIsExist = TRUE ;
                }
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        {

        }

        return bIsExist;
}

BOOLEAN
        SimrepCreateSandboxDirectory(
        __in PFLT_FILTER pFilter,
        __in PFLT_INSTANCE pInstance,
        __in PUNICODE_STRING pustrPath)
/*++
Routine Description:

        创建目录

Arguments:

        pFilter      -

        pInstance   -

        pustrPath - 指向要创建的目录

Return Value:

        创建成功返回TRUE，不失败返回FALSE.

--*/
{
        NTSTATUS						status = STATUS_SUCCESS;
        HANDLE							hFile=NULL;
        OBJECT_ATTRIBUTES			objAttrib = {0};
        IO_STATUS_BLOCK			ioStatus;
        
        if (NULL == pustrPath || NULL == pFilter)
        {
                return FALSE ;
        }

        InitializeObjectAttributes(&objAttrib,
                                                        pustrPath,
                                                        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                                        NULL,
                                                        NULL);
        // 这里要改成for,一级一级的创建目录
        // 你们自己改吧
        status = FltCreateFile(pFilter,
                                                pInstance,    
                                                &hFile,
                                                SYNCHRONIZE,
                                                &objAttrib,
                                                &ioStatus,
                                                0,
                                                FILE_ATTRIBUTE_DIRECTORY,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                FILE_OPEN_IF,
                                                FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                                                NULL,0,0);

        if(NT_SUCCESS(status)
                || status == STATUS_SHARING_VIOLATION /*因为共享冲突打开失败*/)
        {
                if(NULL != hFile)
                {
                        FltClose(hFile);
                }
                return TRUE ;
        }
        return FALSE;
}

NTSTATUS
        SimrepMiniMessage (
        __in PVOID ConnectionCookie,
        __in_bcount_opt(InputBufferSize) PVOID InputBuffer,
        __in ULONG InputBufferSize,
        __out_bcount_part_opt(OutputBufferSize,*ReturnOutputBufferLength) PVOID OutputBuffer,
        __in ULONG OutputBufferSize,
        __out PULONG ReturnOutputBufferLength
        )
{
        NTSTATUS status;
        SIMREPMINI_COMMAND command ;

        PAGED_CODE();

        UNREFERENCED_PARAMETER( ConnectionCookie );
        UNREFERENCED_PARAMETER( OutputBufferSize );
        UNREFERENCED_PARAMETER( OutputBuffer );
        UNREFERENCED_PARAMETER( ReturnOutputBufferLength) ;

        DbgPrint("[Simrep] SimrepMiniMessage\r\n");

        //                      **** PLEASE READ ****    
        //  The INPUT and OUTPUT buffers are raw user mode addresses.  The filter
        //  manager has already done a ProbedForRead (on InputBuffer) and
        //  ProbedForWrite (on OutputBuffer) which guarentees they are valid
        //  addresses based on the access (user mode vs. kernel mode).  The
        //  minifilter does not need to do their own probe.
        //  The filter manager is NOT doing any alignment checking on the pointers.
        //  The minifilter must do this themselves if they care (see below).
        //  The minifilter MUST continue to use a try/except around any access to
        //  these buffers.    

        if ((InputBuffer != NULL) &&
                (InputBufferSize >= (FIELD_OFFSET(COMMAND_MESSAGE,Command) +
                sizeof(SIMREPMINI_COMMAND))))
        {
                        try
                        {
                                //  Probe and capture input message: the message is raw user mode
                                //  buffer, so need to protect with exception handler
                                command = ((PCOMMAND_MESSAGE) InputBuffer)->Command;

                        }
                        except( EXCEPTION_EXECUTE_HANDLER )
                        {
                                return GetExceptionCode();
                        }
                        switch (command)
                        {
                        case ENUM_TEST:
                                {            		
                                        DbgPrint("[Simrep] ENUM_TEST\r\n");
                                        status = STATUS_SUCCESS;         
                                        break;
                                }         		

                        default:
                                DbgPrint("[Simrep] default\r\n");
                                status = STATUS_INVALID_PARAMETER;
                                break;
                        }
        }
        else
        {
                status = STATUS_INVALID_PARAMETER;
        }

        return status;
}

NTSTATUS
        SimrepMiniConnect(
        __in PFLT_PORT ClientPort,
        __in PVOID ServerPortCookie,
        __in_bcount(SizeOfContext) PVOID ConnectionContext,
        __in ULONG SizeOfContext,
        __deref_out_opt PVOID *ConnectionCookie
        )
{
        DbgPrint("[Simrep] SimrepMiniConnect");
        PAGED_CODE();

        UNREFERENCED_PARAMETER( ServerPortCookie );
        UNREFERENCED_PARAMETER( ConnectionContext );
        UNREFERENCED_PARAMETER( SizeOfContext);
        UNREFERENCED_PARAMETER( ConnectionCookie );

        if (NULL != Globals.ClientPort)
        {
                //  Close our handle
                FltCloseClientPort( Globals.Filter, &(Globals.ClientPort));
                Globals.ClientPort = NULL ;
        }

        Globals.ClientPort = ClientPort;

        return STATUS_SUCCESS;
}

VOID
        SimrepMiniDisconnect(
        __in_opt PVOID ConnectionCookie
        )
{
        PAGED_CODE();
        UNREFERENCED_PARAMETER( ConnectionCookie );
        DbgPrint("[Simrep] SimrepMiniDisconnect");

        if (NULL != Globals.ClientPort)
        {
                //  Close our handle
                FltCloseClientPort( Globals.Filter, &(Globals.ClientPort));
                Globals.ClientPort = NULL ;
        }
}