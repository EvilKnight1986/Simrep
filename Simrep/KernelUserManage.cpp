//
//                       _oo0oo_
//                      o8888888o
//                      88" . "88
//                      (| -_- |)
//                      0\  =  /0
//                    ___/`---'\___
//                  .' \\|     |// '.
//                 / \\|||  :  |||// \
//                / _||||| -:- |||||- \
//               |   | \\\  -  /// |   |
//               | \_|  ''\---/''  |_/ |
//               \  .-\__  '-'  ___/-. /
//             ___'. .'  /--.--\  `. .'___
//          ."" '<  `.___\_<|>_/___.' >' "".
//         | | :  `- \`.;`\ _ /`;.`/ - ` : | |
//         \  \ `_.   \_ __\ /__ _/   .-` /  /
//     =====`-.____`.___ \_____/___.-`___.-'=====
//                       `=---='
//
//
//     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
//               佛祖保佑         永无BUG
//
//
//

#include "KernelUserManage.h"

PKERNELUSER g_pKernelUser = NULL ;
PAGED_LOOKASIDE_LIST g_PagedLookasideList ;
/*******************************************************************************
*
*   函 数 名 : InitializeKernelUserManage
*  功能描述 : 初始化内核用户管理器
*  参数列表 : 无
*  说      明 :  此函数需要在DriverEntry中执行 
*  返回结果 :  如果成功，返回TRUE, 失败返回FALSE
*
*******************************************************************************/
BOOLEAN InitializeKernelUserManage(void)
{
        BOOLEAN bResult = FALSE ;
        __try
        {
                // 防止用户多次初始化
                if (NULL != g_pKernelUser)
                {
                        ReleaseKernelUserManage() ;
                }

                // 初始化PagedLookasideList
                ExInitializePagedLookasideList(&g_PagedLookasideList,
                                                                NULL,
                                                                NULL,
                                                                0,
                                                                sizeof(KERNELUSER),
                                                                'HDHD',
                                                                0) ;

                bResult = ReadKernelUser() ;
        }

        __finally
        {
        }

        return bResult ;
}

/*******************************************************************************
*
*   函 数 名 : ReleaseKernelUserManage
*  功能描述 : 释放用户管理器
*  参数列表 : 无
*   说      明 : 
*  返回结果 :  如果成功，返回TRUE, 失败返回FALSE
*
*******************************************************************************/
BOOLEAN ReleaseKernelUserManage(void)
{
        BOOLEAN bResult = FALSE ;
        PKERNELUSER pNode = NULL ;

        __try
        {
                if (NULL == g_pKernelUser)
                {
                        KdPrint(("ReleaseKernelUserManage g_pKernelUser is NULL!\r\n")) ;
                        __leave ;
                }

                // 这里还要把申请的内存全放了
                while (IsListEmpty(&(g_pKernelUser->ListEntry)))
                {
                        pNode = CONTAINING_RECORD(RemoveTailList(&(g_pKernelUser->ListEntry)),
                                                                                                        KERNELUSER,
                                                                                                        ListEntry) ;
                        RtlFreeUnicodeString(&pNode->ustrUserName) ;
                        ExFreeToPagedLookasideList(&g_PagedLookasideList, pNode) ;
                }

                ExFreeToPagedLookasideList(&g_PagedLookasideList, g_pKernelUser) ;
                g_pKernelUser = NULL ;

                ExDeletePagedLookasideList(&g_PagedLookasideList) ;

                bResult = TRUE ;
        }

        __finally
        {
        }
        return bResult ;
}

/*******************************************************************************
*
*   函 数 名 : ReadKernelUser
*  功能描述 : 读取用户名以及对应的SID到用户管理器中
*  参数列表 : 无
*   说      明 : 此函数需要在DriverEntry中执行 
*  返回结果 :  如果成功，返回TRUE, 失败返回FALSE
*
*******************************************************************************/
BOOLEAN ReadKernelUser(void)
{
        BOOLEAN bResult = FALSE ;
        OBJECT_ATTRIBUTES ObjAttributeNames ;
        UNICODE_STRING    ustrObjectNames ;
        HANDLE  hNamesKey = NULL;
        HANDLE hSubKey = NULL;
        PKEY_FULL_INFORMATION pKeyFullInfor = NULL ;
        PKEY_BASIC_INFORMATION pBi = NULL ;
        NTSTATUS ntStatus = STATUS_UNSUCCESSFUL ;

        __try
        {
                RtlInitUnicodeString(&ustrObjectNames, L"\\Registry\\Machine\\SAM\\SAM\\Domains\\Account\\Users\\Names") ;

                InitializeObjectAttributes(&ObjAttributeNames, 
                                                        &ustrObjectNames, 
                                                        OBJ_CASE_INSENSITIVE, 
                                                        NULL, 
                                                        NULL) ;

                // 先打开
                ntStatus = ZwOpenKey(&hNamesKey, KEY_ALL_ACCESS, &ObjAttributeNames) ;
                if (! NT_SUCCESS(ntStatus))
                {
                        DbgPrint(("KernelUserManage::ReadKernelUser ZwOpenKey failed!\r\n")) ;
                        __leave ;
                }

                ULONG uSize ;
                // 第一次先是求大小
                ntStatus = ZwQueryKey(hNamesKey, KeyFullInformation, NULL, 0, &uSize) ;

                pKeyFullInfor = (PKEY_FULL_INFORMATION)ExAllocatePool( PagedPool, uSize) ;
                if (NULL == pKeyFullInfor)
                {
                        DbgPrint(("KernelUserManage::ReadKernelUser ExAllocatePool failed!\r\n")) ;
                        __leave ;
                }
                // 申请内存后就可以用了
                ntStatus = ZwQueryKey(hNamesKey, KeyFullInformation, pKeyFullInfor, uSize, &uSize) ;
                if (! NT_SUCCESS(ntStatus))
                {
                        DbgPrint(("KernelUserManage::ReadKernelUser ZwOpenKey failed!\r\n")) ;
                        __leave ;
                }

                for (ULONG i(0); i < pKeyFullInfor->SubKeys; ++i)
                {
                        ZwEnumerateKey(hNamesKey, i, KeyBasicInformation, NULL, 0, &uSize) ;
                        pBi = (PKEY_BASIC_INFORMATION)ExAllocatePool(PagedPool, uSize) ;
                        ZwEnumerateKey(hNamesKey, i,KeyBasicInformation,pBi,uSize, &uSize) ;

                        PKERNELUSER pNode = (PKERNELUSER)ExAllocateFromPagedLookasideList(&g_PagedLookasideList) ;
                        if (NULL == pNode)
                        {
                                DbgPrint(("KernelUserManage::ReadKernelUser ExAllocateFromPagedLookasideList failed!\r\n")) ;
                                __leave ;
                        }

                        // 构造字符串
                        USHORT uNameLength = (USHORT)pBi->NameLength ;
                        pNode->ustrUserName.Length = uNameLength ;
                        pNode->ustrUserName.MaximumLength = uNameLength ;
                        pNode->ustrUserName.Buffer = (PWCHAR)ExAllocatePool(PagedPool, uNameLength) ;
						
                        if (NULL == pNode->ustrUserName.Buffer)
                        {
                                ExFreeToPagedLookasideList(&g_PagedLookasideList, pNode) ;
                                DbgPrint(("KernelUserManage::ReadKernelUser ExAllocateFromPagedLookasideList failed!\r\n")) ;
                                __leave ;
                        }

                        // 填充0
                        RtlFillMemory(pNode->ustrUserName.Buffer, uNameLength, 0) ;

                        // 将名字copy过去
                        RtlCopyMemory(pNode->ustrUserName.Buffer, pBi->Name, uNameLength) ;

                        KdPrint(("The %d sub item name: %wZ\r\n", i, &pNode->ustrUserName)) ;

                        // 复制完之后没有他什么事了，把内存给放了
                        ExFreePool(pBi) ;
                        pBi = NULL ;

                        // 打开users下面的子键，然后读值，转成数值
                        OBJECT_ATTRIBUTES ObjAttribSub = {0};
                        InitializeObjectAttributes(&ObjAttribSub, 
                                                                &(pNode->ustrUserName), 
                                                                OBJ_CASE_INSENSITIVE,
                                                                hNamesKey, 
                                                                NULL) ;

                        ntStatus = ZwOpenKey(&hSubKey, KEY_ALL_ACCESS, &ObjAttribSub) ;
                        if (! NT_SUCCESS(ntStatus))
                        {
                                ExFreeToPagedLookasideList(&g_PagedLookasideList, pNode) ;
                                DbgPrint(("KernelUserManage::ReadKernelUser ExAllocateFromPagedLookasideList failed!\r\n")) ;
                                __leave ;
                        }

                        // 然后去读值了
                        KEY_VALUE_BASIC_INFORMATION keyVbi = {0} ;  // 这值一般不会太大的

                        // 读默认值unicode_string这样就可以了
                        UNICODE_STRING ustrDefaultString = {0} ;
                        RtlInitUnicodeString(&ustrDefaultString, L"") ;
                        uSize = 0 ;

                        ntStatus = ZwQueryValueKey(hSubKey, 
                                                                        &ustrDefaultString, 
                                                                        KeyValueBasicInformation, 
                                                                        (PVOID)&keyVbi, 
                                                                        sizeof(KEY_VALUE_BASIC_INFORMATION), 
                                                                        &uSize) ;
                        if (NT_SUCCESS(ntStatus))
                        {
                                pNode->UID = keyVbi.Type ;
                        }

                        // 插入到链表当中去
                        if (NULL == g_pKernelUser)
                        {
                                InitializeListHead(&(pNode->ListEntry)) ;
                                g_pKernelUser = pNode ;
                        }
                        else
                        {
                                InsertTailList(&(g_pKernelUser->ListEntry), &(pNode->ListEntry)) ;
                        }
                        bResult = TRUE ;
                }
        }

        __finally
        {
                if (NULL != hSubKey)
                {
                        ZwClose(hSubKey) ;
                }
                if (NULL != hNamesKey)
                {
                        ZwClose(hNamesKey) ;
                }
                if (NULL != pKeyFullInfor)
                {
                        ExFreePool(pKeyFullInfor) ;
                        pKeyFullInfor = NULL ;
                }
                if (NULL != pBi)
                {
                        ExFreePool(pBi) ;
                        pBi = NULL ;
                }
        }

        return bResult ;
}

/*******************************************************************************
*
*   函 数 名 : UserIsExist
*  功能描述 : 通过SID最后一段判断用户是否存在
*  参数列表 : 无
*   说      明 : 
*  返回结果 :  如果存在，返回TRUE, 失败返回FALSE
*
*******************************************************************************/
BOOLEAN UserIsExist(__in ULONG uUID)
{
        BOOLEAN bResult = FALSE ;
        UNREFERENCED_PARAMETER(uUID) ;
        __try
        {
                if (NULL == g_pKernelUser)
                {
                        KdPrint(("KernelUsermanage::UserIsExist g_pKernelUser is NULL!\r\n")) ;
                        __leave ;
                }
                PLIST_ENTRY pList = NULL ;
                PKERNELUSER pNode = g_pKernelUser ;

                do 
                {
                        if (uUID == pNode->UID)
                        {
                                bResult = TRUE ;
                                __leave ;
                        }

                        pList = pNode->ListEntry.Flink ;
                        pNode = (PKERNELUSER)CONTAINING_RECORD(pList, KERNELUSER, ListEntry) ;

                } while (pNode != g_pKernelUser) ;
        }

        __finally
        {
        }

        return bResult ;
}

/*******************************************************************************
*
*   函 数 名 : GetUserNameByUID
*  功能描述 : 通过SID取得用户名
*  参数列表 : 无
*   说      明 : 
*  返回结果 :  如果存在，返回用户名unicode_string用户名, 否则返回NULL
*
*******************************************************************************/
PUNICODE_STRING GetUserNameByUID(__in ULONG uUID)
{

        if (NULL == g_pKernelUser)
        {
                KdPrint(("KernelUserManage::UserIsExist g_pKernelUser is NULL!\r\n")) ;
                return NULL ;
        }

        PLIST_ENTRY pList = NULL ;
        PKERNELUSER pNode = g_pKernelUser ;

        do 
        {
                if (uUID == pNode->UID)
                {
                        return &(pNode->ustrUserName) ;
                }

                pList = pNode->ListEntry.Flink ;
                pNode = (PKERNELUSER)CONTAINING_RECORD(pList, KERNELUSER, ListEntry) ;
        } while (pNode != g_pKernelUser) ;

        return NULL ;
}

/*******************************************************************************
*
*   函 数 名 : GetProcessSID
*  功能描述 : 取得进程所属用户SID
*  参数列表 : uPID       --     进程PID
*                  pustrProcessSID    -- 用来保存进程SID值
*   说      明 : pustrProcessSID需要自己释放，如果获取当前进程SID，dwPID指定为0
*  返回结果 :  成功返回TRUE，失败返回FALSE
*
*******************************************************************************/
BOOLEAN GetProcessSID(__in ULONG uPID, __inout PUNICODE_STRING pustrProcessSID)
{
        BOOLEAN bResult = FALSE ;
        NTSTATUS ntStatus = STATUS_UNSUCCESSFUL ;
        PEPROCESS pEprocess = NULL ;
        //HANDLE         hProcess = NULL ;  
        HANDLE         TokenHandle = NULL;  
        ULONG         ReturnLength;  
        ULONG       uSize; 
        UNICODE_STRING SidString; 
        PTOKEN_USER pTokenInformation = NULL;  
        WCHAR SidStringBuffer[64] ; 

        __try
        {
                if (NULL == pustrProcessSID)
                {
                        KdPrint(("KernelUserManage::GetProcessSID pustrProcessSID can't NULL!\r\n")) ;
                        __leave ;
                }

                if (0 == uPID)
                {
                        uPID = (ULONG)PsGetCurrentProcessId() ;
                }

                // 如果不是获取当前进程的
                ntStatus = PsLookupProcessByProcessId((HANDLE)uPID,  &pEprocess) ;
                if (! NT_SUCCESS(ntStatus))
                {
                        KdPrint(("KernelUserManage::GetProcessSID PsLookupProcessByProcessId failed!\r\n")) ;
                        __leave ;
                }
                // 加载到目标进程，这样才可以取得他的sid
                KeAttachProcess(pEprocess) ;

                //ntStatus = ZwOpenThreadTokenEx (NtCurrentThread(), 
                //                                                        TOKEN_READ, 
                //                                                        TRUE, 
                //                                                        OBJ_KERNEL_HANDLE, 
                //                                                        &TokenHandle); 
                //if (!NT_SUCCESS(ntStatus))
                //{
                //        KdPrint(("KernelUserManage::GetProcessSID ZwOpenThreadTokenEx failed!\r\n")) ;
                //        __leave ;
                //}

               ntStatus = ZwOpenProcessTokenEx (NtCurrentProcess(), 
                                                                        GENERIC_READ, 
                                                                        OBJ_KERNEL_HANDLE, 
                                                                        &TokenHandle); 

                if ( !NT_SUCCESS( ntStatus ))
                {
                        KdPrint(("KernelUserManage::GetProcessSID ZwOpenProcessTokenEx failed!\r\n")) ;
                        __leave; 
                }

                ntStatus = ZwQueryInformationToken( TokenHandle,  
                                                                                TokenUser,  
                                                                                NULL,  
                                                                                0,  
                                                                                &ReturnLength ); 

                uSize = ReturnLength; 
                pTokenInformation = (PTOKEN_USER)ExAllocatePool( NonPagedPool, uSize ); 

                ntStatus = ZwQueryInformationToken( TokenHandle,  
                                                                                TokenUser,  
                                                                                pTokenInformation,  
                                                                                uSize,  
                                                                                &ReturnLength ); 
                if (! NT_SUCCESS(ntStatus))
                {
                        KdPrint(("KernelUserManage::GetProcessSID NtQueryInformationToken failed!\r\n")) ;
                        __leave ;
                }

                RtlZeroMemory( SidStringBuffer, sizeof(SidStringBuffer) );  
                SidString.Buffer = (PWCHAR)SidStringBuffer;  
                SidString.MaximumLength = sizeof( SidStringBuffer );  

                ntStatus = RtlConvertSidToUnicodeString( &SidString,  
                                                                                ((PTOKEN_USER)pTokenInformation)->User.Sid,  
                                                                                FALSE );  

                pustrProcessSID->Length = SidString.Length ;
                pustrProcessSID->MaximumLength = SidString.Length ;
                pustrProcessSID->Buffer = (PWCHAR)ExAllocatePool(PagedPool, SidString.Length) ;
                RtlCopyUnicodeString(pustrProcessSID, &SidString) ;
                
                bResult = TRUE ;

        }

        __finally
        {
                if (NULL != pEprocess)
                {
                        KeDetachProcess() ;
                        ObDereferenceObject(pEprocess) ;
                        pEprocess = NULL ;
                }
                if (NULL != TokenHandle)
                {
                        ZwClose(TokenHandle) ;
                        TokenHandle = NULL ;
                }
                if (NULL != pTokenInformation)
                {
                        ExFreePool(pTokenInformation) ;
                        pTokenInformation = NULL ;
                }
        }

        return bResult ;
}

/*******************************************************************************
*
*   函 数 名 : GetProcessUID
*  功能描述 : 取得进程所属的用户id
*  参数列表 : uPID       --     进程PID
*   说      明 : 失败的话基本是system用户
*  返回结果 :  成功返回UID，失败返回0
*
*******************************************************************************/
// 41位之后的值我们定义为uid，之前的为一些版本，发行机构之类的，跳过
const ULONG uStartIndex = 42 ;          
ULONG GetProcessUID(__in ULONG uPID)
{
        ULONG uResult = 0 ;
        UNICODE_STRING ustrSID = {0} ;

        __try
        {
                if (! GetProcessSID(uPID, &ustrSID))
                {
                        KdPrint(("KernelUserManage::GetProcessUID GetProcessUID failed!\r\n")) ;
                        __leave ;
                }

                // 再判断sid的长度
                if (ustrSID.Length <= 82)
                {
                        KdPrint(("KernelUserManage::GetProcessUID ustrSID.Length too small!\r\n")) ;
                        RtlFreeUnicodeString(&ustrSID) ;
                        __leave ;
                }

                for (ULONG i(0); (i + uStartIndex) * 2 < ustrSID.Length; i++)
                {
                        uResult = uResult * 10 + ustrSID.Buffer[i + uStartIndex] - L'0' ;
                }
        }

        __finally
        {
                if(NULL != ustrSID.Buffer)
                {
                        RtlFreeUnicodeString(&ustrSID) ;
                }
        }
        
        return uResult ;
}

// 取得当前进程所属用户名
/*******************************************************************************
*
*   函 数 名 : GetCurrentProcessUserName
*  功能描述 : 取得当前进程所属用户名id
*  参数列表 : 无
*   说      明 : 
*  返回结果 :  成功返回UNICODE_STRING用户名指针，失败返回NULL
*
*******************************************************************************/
PUNICODE_STRING GetCurrentProcessUserName(VOID)
{
        return GetUserNameByUID(GetProcessUID((ULONG)PsGetCurrentProcessId())) ;
}
