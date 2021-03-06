/*

A盾的ring3与ring0通信文件，利用NtReadFile来进行通信

*/

#include "Control.h"

/*

获取EPROCESS进程的文件大小
参数：进程PEPROCESS

*/
ULONG GetCallerFileSize(__in PEPROCESS Eprocess)
{
	WCHAR CallerFilePath[260] = {0};
	ULONG ulSizeRet = 0;
	UNICODE_STRING UnicodeCallerFile;
	// 初始化文件路径
	OBJECT_ATTRIBUTES obj_attrib;
	NTSTATUS status;
	IO_STATUS_BLOCK Io_Status_Block;
	ULONG ulHighPart;
	ULONG ulLowPart;
	HANDLE hFile;

	if (g_bDebugOn)
		KdPrint(("GetCallerFile:%08x\r\n",Eprocess));

	memset(CallerFilePath,0,sizeof(CallerFilePath));
	if (GetProcessFullImagePath(Eprocess,(WCHAR*)(&CallerFilePath)))
	{
		if (g_bDebugOn)
			KdPrint(("GetCallerFile:%ws\r\n",CallerFilePath));

		RtlInitUnicodeString(&UnicodeCallerFile,CallerFilePath);
		InitializeObjectAttributes(
			&obj_attrib,
			&UnicodeCallerFile,
			OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
			NULL, 
			NULL
			);
		status = IoCreateFile(
			&hFile,
			GENERIC_READ,  //以只读的方式打开，不然会提示错误32
			&obj_attrib,
			&Io_Status_Block,
			0,
			FILE_ATTRIBUTE_NORMAL,
			0,
			FILE_OPEN_IF,
			0,
			NULL,
			0,
			0,
			NULL,
			IO_NO_PARAMETER_CHECKING
			);
		if (NT_SUCCESS(status))
		{
			ulLowPart = CsGetFileSize(hFile,&ulHighPart);
			if (ulLowPart != -1)
			{
				if (g_bDebugOn)
					KdPrint(("FileSize:%d\r\n",ulLowPart));

				ulSizeRet = ulLowPart;
			}
			ZwClose(hFile);
		}
	}
	return ulLowPart;
}
/*

通信函数

*/
NTSTATUS __stdcall NewNtReadFile(__in      HANDLE FileHandle,__in_opt  HANDLE Event,__in_opt  PIO_APC_ROUTINE ApcRoutine,__in_opt  PVOID ApcContext,__out     PIO_STATUS_BLOCK IoStatusBlock,__out     PVOID Buffer,__in      ULONG Length,__in_opt  PLARGE_INTEGER ByteOffset,__in_opt  PULONG Key)
{
	NTSTATUS status;
	ULONG ulSize;
	ULONG ulKeServiceDescriptorTable;
	int i=0,x=0;
	BOOL bInit = FALSE;
	WIN_VER_DETAIL WinVer;
	HANDLE HFileHandle;
	WCHAR lpwzKey[256];
	WCHAR lpwzModule[256];
	char *lpszProName = NULL;
	BOOL bIsNormalServices = FALSE;
	ULONG g_Offset_Eprocess_ProcessId;
	PVOID KernelBuffer;
	ULONG ulCsrssTemp;
	ULONG ulRealDispatch;
	CHAR lpszModule[256];
	ZWREADFILE OldZwReadFile;
	BOOL bIsMyCommand = FALSE;
	KIRQL oldIrql;
	ULONG ulReLoadSelectModuleBase = 0;

	OldZwReadFile = (ZWREADFILE)g_pOriginalServiceDescriptorTable->ServiceTable[ZwReadFileIndex];

	//IRQL有可能过高哦
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
	{
		goto _FunctionRet;
	}
	/*

	如果没有找到要挂载的进程explorer，就枚举进程并挂载
	挂载成功接着reload win32K
	然后初始化保护自身进程代码
	*/
	if (!IsExitProcess(AttachGuiEProcess))
	{
		lpszProName = (char *)PsGetProcessImageFileName(g_fnRPsGetCurrentProcess());
		if (_strnicmp(lpszProName,"csrss.exe",strlen("csrss.exe")) == 0)
		{
			//获取csrss的eprocess，这里不能获取其他的gui，不然在KeInsertQueueApc那里就会卡住，造成进程无法退出等等问题
			AttachGuiEProcess = g_fnRPsGetCurrentProcess();

			//保护自己
			if (!bProtect)
			{
				//挂载，然后reload win32K
				if (ReloadWin32K() == STATUS_SUCCESS)
				{
					KdPrint(("Init Win32K module success\r\n"));
					bInitWin32K = TRUE; //success
				}
				ProtectCode();
				bProtect = TRUE;
			}
		}
	}
	/*

	这里是ring3打开对话框的时候，InitSuccess要为FALSE，并且A盾进程要存在，就可疑暂停下保护
	暂停的时候，直接跳过SAFE_SYSTEM命令

	*/
	if ((int)FileHandle == RESUME_PROTECT && g_bIsInitSuccess == FALSE && IsExitProcess(g_protectEProcess))
	{
		goto _ResumeProtect;
	}
	/*

	判断是否是A盾进程的命令

	*/
	if (g_bIsInitSuccess == TRUE && IsExitProcess(g_protectEProcess))
	{
			if (g_fnRPsGetCurrentProcess() == g_protectEProcess)
			{
				bIsMyCommand = TRUE;
			}
	}
	/*

	不是A盾自身的命令，那么就只检查是否是SAFE_SYSTEM，如果不是，直接返回
	如果是SAFE_SYSTEM，说明是A盾或者其他运行了准备初始化
	如果是SAFE_SYSTEM，但是ProtectEProcess却存在，说明是双开，直接返回
	*/
	if (!bIsMyCommand)
	{
		//只要不是SAFE_SYSTEM的命令，一律返回；
		if ((int)FileHandle != SAFE_SYSTEM)
		{
			goto _FunctionRet;
		}
		//如果是SAFE_SYSTEM命令，并且进程还在的时候，也返回
		if ((int)FileHandle == SAFE_SYSTEM)
		{
			if (IsExitProcess(g_protectEProcess))
			{
				goto _FunctionRet;
			}
		}
	}
_ResumeProtect:
	if (Buffer != NULL &&
		Length > 0)
	{
		__try{
			ProbeForRead( Buffer, Length, sizeof( UCHAR ) );
			ProbeForWrite( Buffer, Length, sizeof( UCHAR ) );
		}__except(EXCEPTION_EXECUTE_HANDLER){
			return STATUS_UNSUCCESSFUL;
		}
	}
	if ((int)FileHandle == START_IO_TIMER)
	{
		if (g_bDebugOn)
			KdPrint(("start io time:%08x\n",Length));

		if (MmIsAddressValidEx((PDEVICE_OBJECT)Length)){
			IoTimerControl((PDEVICE_OBJECT)Length,TRUE);
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == STOP_IO_TIMER)
	{
		if (g_bDebugOn)
			KdPrint(("stop io time:%08x\n",Length));

		if (MmIsAddressValidEx((PDEVICE_OBJECT)Length)){
			IoTimerControl((PDEVICE_OBJECT)Length,FALSE);
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LIST_IO_TIMER)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		IoTimer = (PMyIoTimer)g_fnRExAllocatePool(NonPagedPool,sizeof(MyIoTimer)*256);
		if (!IoTimer)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(IoTimer,0,sizeof(MyIoTimer)*256);
		QueryIoTimer(IoTimer);
		if (Length > sizeof(MyIoTimer)*256)
		{
			for (i=0;i<(int)IoTimer->ulCount;i++)
			{
				if (g_bDebugOn) 
					KdPrint(("DeviceObject:%08x\nTimerRoutine:%08x\r\nModule:%s\r\nstatus:%d\r\n\r\n",
					IoTimer->MyTimer[i].DeviceObject,
					IoTimer->MyTimer[i].IoTimerRoutineAddress,
					IoTimer->MyTimer[i].lpszModule,
					IoTimer->MyTimer[i].ulStatus));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,IoTimer,sizeof(MyIoTimer)*256);
			Length = sizeof(MyIoTimer)*256;
		}
		g_fnRExFreePool(IoTimer);
		return STATUS_UNSUCCESSFUL;
	}
	/*

	恢复所选先模块的inline hook需要的初始化模块的基址

	*/
	if ((int)FileHandle == INIT_SET_SELECT_INLINE_HOOK_1)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			ulInitRealModuleBase = Length;
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	恢复所选模块的inline hook需要的初始化函数的真实地址

	*/
	if ((int)FileHandle == INIT_SET_SELECT_INLINE_HOOK)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			ulInitRealFuncBase = Length;
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	这里是恢复inlinehook和anti inlinehook的命令
	首先做参数检查
	当条件都满足之后，进行该模块的重载
	重载之后就开始分别判断命令

	*/
	if ((int)FileHandle == SET_SELECT_INLINE_HOOK ||
		(int)FileHandle == ANTI_SELECT_INLINE_HOOK)
	{
		if (MmIsAddressValidEx(Buffer) &&
			Length*2 < sizeof(lpwzModule) &&
			ulInitRealFuncBase &&
			ulInitRealModuleBase)
		{
			memset(lpwzModule,0,sizeof(lpwzModule));
			memcpy(lpwzModule,Buffer,Length*2);

			if (g_bDebugOn)
				KdPrint(("func:%08x module:%08x path:%ws\n",ulInitRealFuncBase,ulInitRealModuleBase,lpwzModule));

			//重载当前模块
			//c:\\windows\\system32\\drivers\\tcpip.sys
			//不能用IsFileInSystem函数来检查，\\??\\c:\\windows\\system32\\drivers\\tcpip.sys才符合检查函数的路径
			if (PeLoad(
				lpwzModule,
				(BYTE**)(&ulReLoadSelectModuleBase),
				g_pDriverObject,
				ulInitRealModuleBase
				))
			{
				if (g_bDebugOn)
					KdPrint(("reload success:%08x\n",ulReLoadSelectModuleBase));

				if ((int)FileHandle == SET_SELECT_INLINE_HOOK){
					RestoreInlineHook(ulInitRealFuncBase,ulInitRealModuleBase,ulReLoadSelectModuleBase);
				}
				if ((int)FileHandle == ANTI_SELECT_INLINE_HOOK){
					AntiInlineHook(ulInitRealFuncBase,ulInitRealModuleBase,ulReLoadSelectModuleBase);
				}
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	扫描所选函数的inlinhook，传入一个结构用于保护扫描到得钩子

	*/
	if ((int)FileHandle == LIST_SELECT_MODULE_INLINE_HOOK)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		SelectModuleInlineHookInfo = (PINLINEHOOKINFO)g_fnRExAllocatePool(NonPagedPool,g_nSystemKernelModuleSize+260);
		if (SelectModuleInlineHookInfo)
		{
			memset(SelectModuleInlineHookInfo,0,g_nSystemKernelModuleSize+260);
			KernelHookCheck(SelectModuleInlineHookInfo,SelectModule);
			if (Length > g_nSystemKernelModuleSize+260)
			{
				for (i=0;i<(int)SelectModuleInlineHookInfo->ulCount;i++)
				{
					if (g_bDebugOn)
						KdPrint(("[%d]SelectModuleHook\r\n"
						"被挂钩地址:%08x\r\n"
						"原始地址:%08x\r\n"
						"挂钩函数:%s\r\n"
						"hook跳转地址:%08x\r\n"
						"所在模块:%s\r\n"
						"模块基址:%08x\r\n"
						"模块大小:%x\r\n",
						i,
						SelectModuleInlineHookInfo->InlineHook[i].ulMemoryFunctionBase,
						SelectModuleInlineHookInfo->InlineHook[i].ulRealFunctionBase,
						SelectModuleInlineHookInfo->InlineHook[i].lpszFunction,
						SelectModuleInlineHookInfo->InlineHook[i].ulMemoryHookBase,
						SelectModuleInlineHookInfo->InlineHook[i].lpszHookModuleImage,
						SelectModuleInlineHookInfo->InlineHook[i].ulHookModuleBase,
						SelectModuleInlineHookInfo->InlineHook[i].ulHookModuleSize
						));
				}
				status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				g_fnRmemcpy(Buffer,SelectModuleInlineHookInfo,g_nSystemKernelModuleSize+260);
				Length = g_nSystemKernelModuleSize+260;
			}
			g_fnRExFreePool(SelectModuleInlineHookInfo);
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	恢复所选函数的hook之前，必须要初始化当前模块的PDB，从PDB符号里提取函数地址，函数名
	如果当前操作的是win32K，则需要挂靠到GUI线程

	*/
	if ((int)FileHandle == INIT_SELECT_MODULE_INLINE_HOOK)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRKeAttachProcess),L"KeAttachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&RKeDetachProcess),L"KeDetachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy &&
			g_fnRKeAttachProcess &&
			RKeDetachProcess)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		if (g_bDebugOn)
			KdPrint(("INIT_SELECT_MODULE_INLINE_HOOK\n"));

		if (MmIsAddressValidEx(Buffer) &&
			g_nSystemKernelModuleSize+1024 > Length &&
			Length > g_nSystemKernelModuleSize)
		{
			if (SelectModuleFuncInfo)
				g_fnRExFreePool(SelectModuleFuncInfo);

			SelectModuleFuncInfo = g_fnRExAllocatePool(PagedPool,g_nSystemKernelModuleSize+1024);    //分配足够大的缓冲
			if (!SelectModuleFuncInfo){
				return STATUS_UNSUCCESSFUL;
			}
			memset(SelectModuleFuncInfo,0,g_nSystemKernelModuleSize+1024);
			//从ring3得到内核信息
			g_fnRmemcpy(SelectModuleFuncInfo,Buffer,Length);

			if (g_bDebugOn)
				KdPrint(("copy memory\n"));

			if (SelectModuleFuncInfo->ulCount > 10){

				if (g_bDebugOn)
					KdPrint(("reload path:0x%08X:%ws\n",SelectModuleFuncInfo->ulModuleBase,SelectModuleFuncInfo->szModulePath));
				//如果是win32K，挂载
				if (SelectModuleFuncInfo->ulModuleBase == ulWin32kBase){
					g_fnRKeAttachProcess(AttachGuiEProcess);
				}
				//重载当前模块
				if (PeLoad(
					SelectModuleFuncInfo->szModulePath,
					(BYTE**)(&ulReLoadSelectModuleBase),
					g_pDriverObject,
					SelectModuleFuncInfo->ulModuleBase
					))
				{
					for (i=0;i<(int)SelectModuleFuncInfo->ulCount;i++)
					{
						//计算重载后的地址并保存，如果当前地址无效，则赋值0
						if (wcslen(SelectModuleFuncInfo->ntosFuncInfo[i].FuncName) &&
							MmIsAddressValidEx((PVOID)ulReLoadSelectModuleBase) &&
							MmIsAddressValidEx((PVOID)SelectModuleFuncInfo->ntosFuncInfo[i].ulAddress)){
							SelectModuleFuncInfo->ntosFuncInfo[i].ulReloadAddress = SelectModuleFuncInfo->ntosFuncInfo[i].ulAddress - SelectModuleFuncInfo->ulModuleBase + (ULONG)ulReLoadSelectModuleBase;
						}else{
							SelectModuleFuncInfo->ntosFuncInfo[i].ulReloadAddress = 0;
							SelectModuleFuncInfo->ntosFuncInfo[i].ulAddress = 0;
							//KdPrint(("%ws : 0x%X\n",SelectModuleFuncInfo->NtosFuncInfo[i].FuncName,SelectModuleFuncInfo->NtosFuncInfo[i].ulAddress));
						}
					}
				}
				if (SelectModuleFuncInfo->ulModuleBase == ulWin32kBase){
					RKeDetachProcess();
				}
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	手动蓝屏

	*/
	if ((int)FileHandle == KERNEL_BSOD)
	{
		oldIrql = KeRaiseIrqlToDpcLevel();
		PsGetProcessImageFileName(PsGetCurrentProcess());

		return STATUS_UNSUCCESSFUL;
	}
	/*

	内核数据查看器要查看的大小

	*/
	if ((int)FileHandle == INIT_KERNEL_DATA_SIZE)
	{
		if (Length > 0x10)
		{
			ulLookupSize = Length;
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	内核数据查看器要查看的起始地址

	*/
	if ((int)FileHandle == INIT_KERNEL_DATA_BASE)
	{
		if (Length > 0x123456 &&
			MmIsAddressValidEx((PVOID)Length))
		{
			LookupBase =(PVOID) Length;
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	开始读取内核数据，并传送到ring3

	*/
	if ((int)FileHandle == LIST_KERNEL_DATA)
	{
		if (!MmIsAddressRangeValid(LookupBase,ulLookupSize)){
			return STATUS_UNSUCCESSFUL;
		}
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		LookupKernelData = (PLOOKUP_KERNEL_DATA)g_fnRExAllocatePool(PagedPool,g_nSystemKernelModuleSize);    //分配足够大的缓冲
		if (LookupKernelData == NULL) 
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(LookupKernelData,0,g_nSystemKernelModuleSize);
		LookupKernelDataInfo(LookupBase,ulLookupSize,LookupKernelData);
		if (Length > g_nSystemKernelModuleSize)
		{
			for (i=0;i<(int)LookupKernelData->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("0x%08x %08x %08x %08x %08x\n",
					LookupKernelData->KernelData[i].ulAddress,
					LookupKernelData->KernelData[i].ulStack1,
					LookupKernelData->KernelData[i].ulStack2,
					LookupKernelData->KernelData[i].ulStack3,
					LookupKernelData->KernelData[i].ulStack4));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,LookupKernelData,g_nSystemKernelModuleSize);
			Length = g_nSystemKernelModuleSize;
		}
		g_fnRExFreePool(LookupKernelData);
		LookupBase = 0;
		ulLookupSize = 0;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_THREAD_STACK)
	{
		if (Length > 0x123456 &&
			MmIsAddressValidEx((PVOID)Length) &&
			!PsIsThreadTerminating((PETHREAD)Length))
		{
			ulThread = Length;
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LIST_THREAD_STACK)
	{
		if (!ulThread)
			return STATUS_UNSUCCESSFUL;

		if (Length > 0x123456)
		{
			ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
			ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
			ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
			if (g_fnRExAllocatePool &&
				g_fnRExFreePool &&
				g_fnRmemcpy)
			{
				bInit = TRUE;
			}
			if (!bInit)
				return STATUS_UNSUCCESSFUL;

			ThreadStack = g_fnRExAllocatePool(PagedPool,g_nSystemKernelModuleSize);    //分配足够大的缓冲
			if (ThreadStack == NULL) 
			{
				return STATUS_UNSUCCESSFUL;
			}
			memset(ThreadStack,0,g_nSystemKernelModuleSize);
			ReadThreadStack((PETHREAD)ulThread,ThreadStack);
			if (Length > g_nSystemKernelModuleSize)
			{
				for (i=0;i<(int)ThreadStack->ulCount;i++)
				{
					if (g_bDebugOn)
						KdPrint(("0x%08x %08x %08x %08x %08x\n",
						ThreadStack->StackInfo[i].ulAddress,
						ThreadStack->StackInfo[i].ulStack1,
						ThreadStack->StackInfo[i].ulStack2,
						ThreadStack->StackInfo[i].ulStack3,
						ThreadStack->StackInfo[i].ulStack4));
				}
				status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				g_fnRmemcpy(Buffer,ThreadStack,g_nSystemKernelModuleSize);
				Length = g_nSystemKernelModuleSize;
			}
			g_fnRExFreePool(ThreadStack);
			ulThread = 0;
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	如果当前的机器是联网的，则初始化ntkrnlpa扫描未导出函数
	如果当前的机器不联网，则默认扫描导出函数

	*/
	if ((int)FileHandle == INIT_PDB_KERNEL_INFO)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		//已经取到了，直接返回了。
		if (bKrnlPDBSuccess){
			if (g_bDebugOn)
				KdPrint(("doen success\n"));
			return STATUS_UNSUCCESSFUL;
		}
		if (g_bDebugOn)
			KdPrint(("enter\n"));

		if (MmIsAddressValidEx(Buffer) &&
			g_nSystemKernelModuleSize+1024 > Length &&
			Length > g_nSystemKernelModuleSize)
		{
			PDBNtosFuncAddressInfo = g_fnRExAllocatePool(PagedPool,g_nSystemKernelModuleSize+1024);    //分配足够大的缓冲
			if (PDBNtosFuncAddressInfo == NULL) 
			{
				if (g_bDebugOn)
					KdPrint(("pdb failed\n"));
				return STATUS_UNSUCCESSFUL;
			}
			memset(PDBNtosFuncAddressInfo,0,g_nSystemKernelModuleSize+1024);

			//从ring3得到内核信息
			g_fnRmemcpy(PDBNtosFuncAddressInfo,Buffer,Length);

			if (g_bDebugOn)
				KdPrint(("copy memory\n"));

			for (i=0;i<(int)PDBNtosFuncAddressInfo->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("%ws : 0x%X\n",PDBNtosFuncAddressInfo->ntosFuncInfo[i].FuncName,PDBNtosFuncAddressInfo->ntosFuncInfo[i].ulAddress));

				//计算重载后的地址并保存
				if (wcslen(PDBNtosFuncAddressInfo->ntosFuncInfo[i].FuncName) &&
					MmIsAddressValidEx((PVOID)PDBNtosFuncAddressInfo->ntosFuncInfo[i].ulAddress)){
					PDBNtosFuncAddressInfo->ntosFuncInfo[i].ulReloadAddress = PDBNtosFuncAddressInfo->ntosFuncInfo[i].ulAddress - g_pOldSystemKernelModuleBase + (ULONG)g_pNewSystemKernelModuleBase;
				}else{
					PDBNtosFuncAddressInfo->ntosFuncInfo[i].ulAddress = 0;
					PDBNtosFuncAddressInfo->ntosFuncInfo[i].ulReloadAddress = 0;
				}
			}
			if (g_bDebugOn)
				KdPrint(("copy memory ok\n"));

			if (PDBNtosFuncAddressInfo->ulCount > 188 &&
				MmIsAddressValidEx((PVOID)PDBNtosFuncAddressInfo->ntosFuncInfo[188].ulAddress)){
				bKrnlPDBSuccess = TRUE;
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	暂停进程

	*/
	if ((int)FileHandle == SUSPEND_PROCESS)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			if (SuspendProcess((PEPROCESS)Length) == STATUS_SUCCESS)
			{
				if (g_bDebugOn)
					KdPrint(("Suspend process:%08x",Length));
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	恢复进程运行

	*/
	if ((int)FileHandle == RESUME_PROCESS)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			if (ResumeProcess((PEPROCESS)Length) == STATUS_SUCCESS)
			{
				if (g_bDebugOn)
					KdPrint(("ResumeThread process:%08x",Length));
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	暂停线程

	*/
	if ((int)FileHandle == SUSPEND_THREAD)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			if (SuspendThread((PETHREAD)Length) == STATUS_SUCCESS)
			{
				if (g_bDebugOn)
					KdPrint(("Suspend Thread:%08x",Length));
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	恢复线程运行

	*/
	if ((int)FileHandle == RESUME_THREAD)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			if (ResumeThread((PETHREAD)Length) == STATUS_SUCCESS)
			{
				if (g_bDebugOn)
					KdPrint(("ResumeThread Thread:%08x",Length));
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == DLL_FUCK)
	{
		bDisDllFuck = TRUE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == DIS_DLL_FUCK)
	{
		bDisDllFuck = FALSE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_WINDOWS_HOOK)
	{
		bDisSetWindowsHook = TRUE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == DIS_SET_WINDOWS_HOOK)
	{
		bDisSetWindowsHook = FALSE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == KERNEL_THREAD)
	{
		bDisKernelThread = TRUE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == DIS_KERNEL_THREAD)
	{
		bDisKernelThread = FALSE;
		return STATUS_UNSUCCESSFUL;
	}

	if ((int)FileHandle == RESET_SRV)
	{
		bDisResetSrv = TRUE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == DIS_RESET_SRV)
	{
		bDisResetSrv = FALSE;
		return STATUS_UNSUCCESSFUL;
	}

	if ((int)FileHandle == PROTECT_PROCESS)
	{
		bProtectProcess = TRUE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == UNPROTECT_PROCESS)
	{
		bProtectProcess = FALSE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_KILL_SYSTEM_NOTIFY)
	{
		IntNotify = Length;
		return STATUS_UNSUCCESSFUL;
	}
	/*

	工作队列线程的枚举（通过硬编码的方式定位KTHREAD的kernelstack堆栈）

	*/
	if ((int)FileHandle == LIST_WORKQUEUE)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		WorkQueueThread = g_fnRExAllocatePool(PagedPool,sizeof(WORKQUEUE)*788);    //分配足够大的缓冲
		if (WorkQueueThread == NULL) 
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(WorkQueueThread,0,sizeof(WORKQUEUE)*788);
		QueryWorkQueue(WorkQueueThread);
		if (g_bDebugOn)
			KdPrint(("%d Length:%08x :%08x\r\n",WorkQueueThread->ulCount,Length,sizeof(WORKQUEUE)*788));

		if (Length >  sizeof(WORKQUEUE)*788)
		{
			for (i=0;i<(int)WorkQueueThread->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("[%d]工作队列线程\r\n"
					"EHTREAD：%08X\r\n"
					"类型：%d\r\n"
					"函数入口：%08X\r\n"
					"函数入口所在模块：%s\r\n",
					i,
					WorkQueueThread->WorkQueueInfo[i].ulEthread,
					WorkQueueThread->WorkQueueInfo[i].ulBasePriority,
					WorkQueueThread->WorkQueueInfo[i].ulWorkerRoutine,
					WorkQueueThread->WorkQueueInfo[i].lpszModule));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,WorkQueueThread,sizeof(WORKQUEUE)*788);
			Length =  sizeof(WORKQUEUE)*788;
		}
		g_fnRExFreePool(WorkQueueThread);
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举启动项

	*/
	if ((int)FileHandle == LIST_START_UP)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		StartupInfo = g_fnRExAllocatePool(PagedPool,sizeof(STARTUP_INFO)*788);    //分配足够大的缓冲
		if (StartupInfo == NULL) 
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(StartupInfo,0,sizeof(STARTUP_INFO)*788);
		QueryStartup(StartupInfo);

		if (g_bDebugOn)
			KdPrint(("Length:%08x :%08x\r\n",Length,sizeof(STARTUP_INFO)*788));

		if (Length >  sizeof(STARTUP_INFO)*788)
		{
			for (i=0;i<(int)StartupInfo->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("[%d]启动项\r\n"
					"名称：%ws\r\n"
					"注册表路径：%ws\r\n"
					"键值：%ws\r\n\r\n",
					i,
					StartupInfo->Startup[i].lpwzName,
					StartupInfo->Startup[i].lpwzKeyPath,
					StartupInfo->Startup[i].lpwzKeyValue));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,StartupInfo,sizeof(STARTUP_INFO)*788);
			Length =  sizeof(STARTUP_INFO)*788;
		}
		g_fnRExFreePool(StartupInfo);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == KILL_SYSTEM_NOTIFY)
	{
		if (g_bDebugOn)
			KdPrint(("Length:0x%08X IntNotify:%d\r\n",Length,IntNotify));

		if (MmIsAddressValidEx((PVOID)Length)){
			RemoveNotifyRoutine(Length,IntNotify);
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举系统回调

	*/
	if ((int)FileHandle == LIST_SYSTEM_NOTIFY)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		SystemNotify = g_fnRExAllocatePool(PagedPool,sizeof(SYSTEM_NOTIFY)*1024);    //分配足够大的缓冲
		if (SystemNotify == NULL) 
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(SystemNotify,0,sizeof(SYSTEM_NOTIFY)*1024);
		QuerySystemNotify(g_pDriverObject,SystemNotify);

		if (g_bDebugOn)
			KdPrint(("Length:%08x :%08x\r\n",Length,sizeof(SYSTEM_NOTIFY)*1024));

		if (Length >  sizeof(SYSTEM_NOTIFY)*1024)
		{
			for (i=0;i<(int)SystemNotify->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("[%d]回调类型:%ws\r\n"
					"回调入口:%08X\r\n"
					"所在模块:%s\r\n"
					"对象:%ws\r\n\r\n",
					i,
					SystemNotify->NotifyInfo[i].lpwzType,
					SystemNotify->NotifyInfo[i].ulNotifyBase,
					SystemNotify->NotifyInfo[i].lpszModule,
					SystemNotify->NotifyInfo[i].lpwzObject));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,SystemNotify,sizeof(SYSTEM_NOTIFY)*1024);
			Length =  sizeof(SYSTEM_NOTIFY)*1024;
		}
		g_fnRExFreePool(SystemNotify);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == KILL_DPC_TIMER)
	{
		if (Length > 0x123456 &&
			MmIsAddressValidEx((PKTIMER)Length))
		{
			if (g_bDebugOn)
				KdPrint(("Timer:0x%08X",Length));

			KillDcpTimer((PKTIMER)Length);
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举DPC定时器

	*/
	if ((int)FileHandle == LIST_DPC_TIMER)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		DpcTimer = g_fnRExAllocatePool(PagedPool,sizeof(MyDpcTimer)*MAX_DPCTIMER_COUNT);    //分配足够大的缓冲
		if (DpcTimer == NULL) 
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(DpcTimer,0,sizeof(MyDpcTimer)*MAX_DPCTIMER_COUNT);
		WinVer = GetWindowsVersion();
		switch (WinVer)
		{
		case WINDOWS_VERSION_2K3_SP1_SP2:
		case WINDOWS_VERSION_XP:
		case WINDOWS_VERSION_7_7000:
			GetDpcTimerInformation_XP_2K3_WIN7000(DpcTimer);
			break;
		case WINDOWS_VERSION_7_7600_UP:
			GetDpcTimerInformation_WIN7600_UP(DpcTimer);
			break;
		}
		
		if (Length >  sizeof(MyDpcTimer)*MAX_DPCTIMER_COUNT)
		{
			for (i=0;i<(int)DpcTimer->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("[%d]定时器对象:%08x\r\n"
					"触发周期:%d\r\n"
					"函数入口:%08x\r\n"
					"函数入口所在模块:%s\r\n"
					"DPC结构地址:%08x\r\n",
					i,
					DpcTimer->MyTimer[i].TimerAddress,
					DpcTimer->MyTimer[i].Period,
					DpcTimer->MyTimer[i].DpcRoutineAddress,
					DpcTimer->MyTimer[i].lpszModule,
					DpcTimer->MyTimer[i].DpcAddress));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,DpcTimer,sizeof(MyDpcTimer)*MAX_DPCTIMER_COUNT);
			Length =  sizeof(MyDpcTimer)*MAX_DPCTIMER_COUNT;
		}
		g_fnRExFreePool(DpcTimer);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_KERNEL_FILTER_DRIVER)
	{
		if (Length > 0 &&
			MmIsAddressRangeValid(Buffer,Length))
		{
			memset(lpwzFilter,0,sizeof(lpwzFilter));
			wcsncat(lpwzFilter,Buffer,Length);
			if (g_bDebugOn)
				KdPrint(("lpwzFilter:%ws",lpwzFilter));
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == DELETE_KERNEL_FILTER_DRIVER)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			ulDeviceObject = Length;
			if (g_bDebugOn)
				KdPrint(("ulDeviceObject:%08X",ulDeviceObject));

			ClearFilters(lpwzFilter,ulDeviceObject);
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	清空内核线程数据，清空之前要暂停下，避免资源清空的同时被其他模块访问就会BSOD
	其实也可以用自选锁的方式来搞，不过这里直接赋值FALSE，一切从简

	*/
	if ((int)FileHandle == CLEAR_KERNEL_THREAD)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		g_bIsInitSuccess = FALSE;   //暂停所有

		if (KernelThread)
			g_fnRExFreePool(KernelThread);

		ThreadCount = 0;
		KernelThread = (PKERNEL_THREAD_INFO)g_fnRExAllocatePool(NonPagedPool,sizeof(KERNEL_THREAD_INFO)*256);
		if (!KernelThread)
		{
			if (g_bDebugOn)
				KdPrint(("KernelThread failed"));
			return STATUS_UNSUCCESSFUL;
		}
		memset(KernelThread,0,sizeof(KERNEL_THREAD_INFO)*256);

		g_bIsInitSuccess = TRUE;   //恢复

		return STATUS_UNSUCCESSFUL;
	}
	/*

	内核线程是通过Hook PsCreateSystemThread来监视线程的创建并保存到结构，当ring3需要的时候，直接传送结构即可

	*/
	if ((int)FileHandle == LIST_KERNEL_THREAD)
	{
		if (Length >  sizeof(KERNEL_THREAD_INFO)*256)
		{
			if (g_bDebugOn)
				KdPrint(("Length:%08x-%08x",Length,sizeof(KERNEL_THREAD_INFO)*256));
			for (i=0;i<(int)ThreadCount;i++)
			{
				if (MmIsAddressValidEx((PVOID)KernelThread->KernelThreadInfo[i].ThreadStart))
				{
					if (g_bDebugOn)
						KdPrint(("ThreadStart:%08x",KernelThread->KernelThreadInfo[i].ThreadStart));

					memset(lpszModule,0,sizeof(lpszModule));
					if (!IsAddressInSystem(
						KernelThread->KernelThreadInfo[i].ThreadStart,
						&ulThreadModuleBase,
						&ulThreadModuleSize,
						lpszModule))
					{
						KernelThread->KernelThreadInfo[i].ulHideType = 1;  //隐藏线程
					}
					if (g_bDebugOn)
						KdPrint(("Hided:%08x:%s",KernelThread->KernelThreadInfo[i].ThreadStart,lpszModule));
				}else
				{
					KernelThread->KernelThreadInfo[i].ulStatus = 1;   //线程退出
				}
			}
			KernelThread->ulCount = ThreadCount;
			if (g_bDebugOn)
				KdPrint(("ThreadCount:%d",KernelThread->ulCount));

			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,KernelThread,sizeof(KERNEL_THREAD_INFO)*256);
			Length =  sizeof(KERNEL_THREAD_INFO)*256;
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_PROCESS_THREAD)
	{
		if (MmIsAddressValidEx((PVOID)Length))
		{
			TempEProcess = (PEPROCESS)Length;
			KdPrint(("TempEProcess:%08x success",TempEProcess));
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == KILL_SYSTEM_THREAD)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			if (KillThread((PETHREAD)Length))
			{
				if (g_bDebugOn)
					KdPrint(("Kill ETHREAD:%08x success",Length));
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举系统线程 or 进程线程

	*/
	if ((int)FileHandle == LIST_SYSTEM_THREAD ||
		(int)FileHandle == LIST_PROCESS_THREAD)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		SystemThread = (PSYSTEM_THREAD_INFO)g_fnRExAllocatePool(NonPagedPool,sizeof(SYSTEM_THREAD_INFO)*256);
		if (!SystemThread)
		{
			if (g_bDebugOn)
				KdPrint(("SystemThread failed"));
			return STATUS_UNSUCCESSFUL;
		}
		memset(SystemThread,0,sizeof(SYSTEM_THREAD_INFO)*256);
		if ((int)FileHandle == LIST_PROCESS_THREAD)
		{
			QuerySystemThread(SystemThread,TempEProcess);
		}else
			QuerySystemThread(SystemThread,g_systemEProcess);

		if (Length >  sizeof(SYSTEM_THREAD_INFO)*256)
		{
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,SystemThread,sizeof(SYSTEM_THREAD_INFO)*256);
			Length =  sizeof(SYSTEM_THREAD_INFO)*256;
		}
		g_fnRExFreePool(SystemThread);
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举过滤驱动

	*/
	if ((int)FileHandle == LIST_KERNEL_FILTER_DRIVER)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		KernelFilterDriver = (PKERNEL_FILTERDRIVER)g_fnRExAllocatePool(NonPagedPool,sizeof(KERNEL_FILTERDRIVER)*256);
		if (!KernelFilterDriver)
		{
			if (g_bDebugOn)
				KdPrint(("KernelFilterDriver failed"));
			return STATUS_UNSUCCESSFUL;
		}
		memset(KernelFilterDriver,0,sizeof(KERNEL_FILTERDRIVER)*256);
		if (KernelFilterDriverEnum(KernelFilterDriver) == STATUS_SUCCESS)
		{
			if (g_bDebugOn)
				KdPrint(("KernelFilterDriverEnum STATUS_SUCCESS"));
			if (Length >  sizeof(KERNEL_FILTERDRIVER)*256)
			{
				for (i=0;i<(int)KernelFilterDriver->ulCount;i++)
				{
					if (g_bDebugOn)
						KdPrint(("[%d]过滤驱动\r\n"
						"类型:%08X\r\n" 
						"驱动对象名:%ws\r\n"
						"驱动路径:%ws\r\n"
						"设备地址:%08X\r\n"
						"宿主驱动对象名:%ws\r\n\r\n",
						i,
						KernelFilterDriver->KernelFilterDriverInfo[i].ulObjType,
						KernelFilterDriver->KernelFilterDriverInfo[i].FileName,
						KernelFilterDriver->KernelFilterDriverInfo[i].FilePath,
						KernelFilterDriver->KernelFilterDriverInfo[i].ulAttachDevice,
						KernelFilterDriver->KernelFilterDriverInfo[i].HostFileName));
				}
				status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				g_fnRmemcpy(Buffer,KernelFilterDriver, sizeof(KERNEL_FILTERDRIVER)*256);
				Length =  sizeof(KERNEL_FILTERDRIVER)*256;
			}
		}
		g_fnRExFreePool(KernelFilterDriver);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == PROTECT_360SAFE)
	{
		bIsProtect360 = TRUE;
		//Fix360Hook(bIsProtect360);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == UNPROTECT_360SAFE)
	{
		bIsProtect360 = FALSE;
		//Fix360Hook(bIsProtect360);
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举object hook

	*/
	if ((int)FileHandle == LIST_OBJECT_HOOK)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		ObjectHookInfo = (POBJECTHOOKINFO)g_fnRExAllocatePool(NonPagedPool, sizeof(OBJECTHOOKINFO)*256);
		if (!ObjectHookInfo)
		{
			//KdPrint(("ObjectHookInfo failed"));
			return STATUS_UNSUCCESSFUL;
		}
		memset(ObjectHookInfo,0, sizeof(OBJECTHOOKINFO)*256);
		IoFileObjectTypeHookInfo(ObjectHookInfo);
		IoDeviceObjectTypeHookInfo(ObjectHookInfo);
		IoDriverObjectTypeHookInfo(ObjectHookInfo);
		CmpKeyObjectTypeHookInfo(ObjectHookInfo);

		if (g_bDebugOn)
			KdPrint(("Length:%08x-ObjectHookInfo:%08x",Length, sizeof(OBJECTHOOKINFO)*256));

		if (Length >  sizeof(OBJECTHOOKINFO)*256)
		{
			for (i=0;i<(int)ObjectHookInfo->ulCount;i++)
			{
				if (g_bDebugOn)
				   KdPrint(("[%d]ObjectHook\r\n"
					"当前函数地址:%08X\r\n"
					"原始函数地址:%08X\r\n"
					"函数名:%s\r\n"
					"所在模块:%s\r\n"
					"ObjectType地址:%08X\r\n"
					"hook类型:%d\r\n"
					"objectType类型:%s\r\n",
					i,
					ObjectHookInfo->ObjectHook[i].ulMemoryHookBase,
					ObjectHookInfo->ObjectHook[i].ulMemoryFunctionBase,
					ObjectHookInfo->ObjectHook[i].lpszFunction,
					ObjectHookInfo->ObjectHook[i].lpszHookModuleImage,
					ObjectHookInfo->ObjectHook[i].ulObjectTypeBase,
					ObjectHookInfo->ObjectHook[i].ulHookType,
					ObjectHookInfo->ObjectHook[i].lpszObjectTypeName
					));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,ObjectHookInfo, sizeof(OBJECTHOOKINFO)*256);
			Length =  sizeof(OBJECTHOOKINFO)*256;
		}
		g_fnRExFreePool(ObjectHookInfo);
	}
	if ((int)FileHandle == SET_SHADOWSSDT_INLINE_HOOK)
	{
		//传入服务号
		if (Length > 0 ||
			Length == 0)
		{
			if (IsExitProcess(AttachGuiEProcess))
			{
				ReLoadNtosCALL((PVOID)(&g_fnRKeAttachProcess),L"KeAttachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
				ReLoadNtosCALL((PVOID)(&RKeDetachProcess),L"KeDetachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
				if (g_fnRKeAttachProcess &&
					RKeDetachProcess)
				{
					g_fnRKeAttachProcess(AttachGuiEProcess);
					RestoreShadowInlineHook(Length);
					RKeDetachProcess();
				}
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_ONE_SHADOWSSDT)
	{
		//传入服务号
		if (Length > 0 ||
			Length == 0)
		{
			if (IsExitProcess(AttachGuiEProcess))
			{
				ReLoadNtosCALL((PVOID)(&g_fnRKeAttachProcess),L"KeAttachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
				ReLoadNtosCALL((PVOID)(&RKeDetachProcess),L"KeDetachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
				if (g_fnRKeAttachProcess &&
					RKeDetachProcess)
				{
					g_fnRKeAttachProcess(AttachGuiEProcess);
					RestoreAllShadowSSDTFunction(Length);
					RKeDetachProcess();
				}
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_ALL_SHADOWSSDT)
	{
		if (IsExitProcess(AttachGuiEProcess))
		{
			ReLoadNtosCALL((PVOID)(&g_fnRKeAttachProcess),L"KeAttachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
			ReLoadNtosCALL((PVOID)(&RKeDetachProcess),L"KeDetachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
			if (g_fnRKeAttachProcess &&
				RKeDetachProcess)
			{
				g_fnRKeAttachProcess(AttachGuiEProcess);
				RestoreAllShadowSSDTFunction(8888);
				RKeDetachProcess();
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举ShadowSSDT

	*/
	if ((int)FileHandle == LIST_SHADOWSSDT ||
		(int)FileHandle == LIST_SHADOWSSDT_ALL)
	{
		if ((int)FileHandle == LIST_SHADOWSSDT_ALL)
		{
			//KdPrint(("Print SSDT All"));
			bShadowSSDTAll = TRUE;
		}
		if ((int)FileHandle == LIST_SHADOWSSDT)
		{
			//KdPrint(("Print SSDT"));
			bShadowSSDTAll = FALSE;
		}
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		ShadowSSDTInfo = (PSHADOWSSDTINFO)g_fnRExAllocatePool(NonPagedPool,sizeof(SHADOWSSDTINFO)*900);
		if (!ShadowSSDTInfo)
		{
			if (g_bDebugOn)
				KdPrint(("ShadowSSDTInfo failed:%08x\r\n",sizeof(SHADOWSSDTINFO)*900));
			return STATUS_UNSUCCESSFUL;
		}
		memset(ShadowSSDTInfo,0,sizeof(SHADOWSSDTINFO)*900);
		if (IsExitProcess(AttachGuiEProcess))
		{
			ReLoadNtosCALL((PVOID)(&g_fnRKeAttachProcess),L"KeAttachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
			ReLoadNtosCALL((PVOID)(&RKeDetachProcess),L"KeDetachProcess",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
			if (g_fnRKeAttachProcess &&
				RKeDetachProcess)
			{
				g_fnRKeAttachProcess(AttachGuiEProcess);
				ShadowSSDTHookCheck(ShadowSSDTInfo);
				RKeDetachProcess();

				if (g_bDebugOn)
					KdPrint(("Length:%08x-ShadowSSDTInfo:%08x\r\n",Length,sizeof(SHADOWSSDTINFO)*900));

				if (Length > sizeof(SHADOWSSDTINFO)*900)
				{
					for (i=0;i<(int)ShadowSSDTInfo->ulCount;i++)
					{
						if (g_bDebugOn)
							KdPrint(("[%d]发现ShadowSSDT hook\r\n"
							"服务号:%d\r\n"
							"当前地址:%08x\r\n"
							"函数名字:%s\r\n"
							"当前hook模块:%s\r\n"
							"当前模块地址:%08x\r\n"
							"当前模块大小:%d KB\r\n"
							"Hook类型:%d\r\n\r\n",
							i,
							ShadowSSDTInfo->SSDT[i].ulNumber,
							ShadowSSDTInfo->SSDT[i].ulMemoryFunctionBase,
							ShadowSSDTInfo->SSDT[i].lpszFunction,
							ShadowSSDTInfo->SSDT[i].lpszHookModuleImage,
							ShadowSSDTInfo->SSDT[i].ulHookModuleBase,
							ShadowSSDTInfo->SSDT[i].ulHookModuleSize/1024,
							ShadowSSDTInfo->SSDT[i].IntHookType));
					}
					status = OldZwReadFile(
						FileHandle,
						Event,
						ApcRoutine,
						ApcContext,
						IoStatusBlock,
						Buffer,
						Length,
						ByteOffset,
						Key
						);
					g_fnRmemcpy(Buffer,ShadowSSDTInfo,sizeof(SHADOWSSDTINFO)*900);
					Length = sizeof(SHADOWSSDTINFO)*900;
				}
			}
		}
		bShadowSSDTAll = FALSE;
		g_fnRExFreePool(ShadowSSDTInfo);
		return STATUS_UNSUCCESSFUL;
	}
	//强制重启
	if ((int)FileHandle == SHUT_DOWN_SYSTEM)
	{
		KeBugCheck(POWER_FAILURE_SIMULATE);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LOAD_DRIVER)
	{
		if (bDisLoadDriver == FALSE)
		{
			bDisLoadDriver = TRUE;
			EnableDriverLoading();   //允许加载驱动
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == DIS_LOAD_DRIVER)
	{
		if (bDisLoadDriver == TRUE)
		{
			bDisLoadDriver = FALSE;
			DisableDriverLoading();    //禁止加载驱动
		}
		return STATUS_UNSUCCESSFUL;
	}
	//---------------------------------------------
	if ((int)FileHandle == WRITE_FILE)
	{
		bDisWriteFile = TRUE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == DIS_WRITE_FILE)
	{
		bDisWriteFile = FALSE;
		return STATUS_UNSUCCESSFUL;
	}
	//----------------------------------------------------
	if ((int)FileHandle == CREATE_PROCESS)
	{
		bDisCreateProcess = TRUE;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == DIS_CREATE_PROCESS)
	{
		bDisCreateProcess = FALSE;
		return STATUS_UNSUCCESSFUL;
	}
	/*

	DUMP内核模块到文件

	*/
	if ((int)FileHandle == DUMP_KERNEL_MODULE_MEMORY)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		if (Buffer != NULL &&
			Length > 0)
		{
			//KdPrint(("savefile:%ws",Buffer));

			KernelBuffer = g_fnRExAllocatePool(NonPagedPool,ulDumpKernelSize+0x100); //申请一样的内存
			if (KernelBuffer)
			{
				memset(KernelBuffer,0,ulDumpKernelSize);
				if (MmIsAddressValidEx((PVOID)ulDumpKernelBase))
				{
					if (DumpMemory((PVOID)ulDumpKernelBase,KernelBuffer,ulDumpKernelSize) == STATUS_SUCCESS)
					{
						DebugWriteToFile(Buffer,KernelBuffer,ulDumpKernelSize);

						if (g_bDebugOn)
							KdPrint(("DumpKernel success"));
					}
				}
				g_fnRExFreePool(KernelBuffer);
			}
		}
	}
	//size
	if ((int)FileHandle == INIT_DUMP_KERNEL_MODULE_MEMORY_1)
	{
		if (Length > 0x10 &&
			Length < 0xfffffff)
		{
			ulDumpKernelSize = Length;

			if (g_bDebugOn)
				KdPrint(("ulDumpKernelBase:%08x\nulDumpKernelSize:%x",ulDumpKernelBase,ulDumpKernelSize));
		}
		return STATUS_UNSUCCESSFUL;
	}
	//Base
	if ((int)FileHandle == INIT_DUMP_KERNEL_MODULE_MEMORY)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			ulDumpKernelBase = Length;  //初始化
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	清空防御日志，清空之前暂停，避免资源被其他线程访问

	*/
	if ((int)FileHandle == CLEAR_LIST_LOG)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		g_bIsInitSuccess = FALSE;   // 暂停所有

		if (g_pLogDefenseInfo)
			g_fnRExFreePool(g_pLogDefenseInfo);

		ulLogCount = 0;
		g_pLogDefenseInfo = (PLOGDEFENSE)g_fnRExAllocatePool(NonPagedPool,sizeof(LOGDEFENSE)*1024);
		if (!g_pLogDefenseInfo)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(g_pLogDefenseInfo,0,sizeof(LOGDEFENSE)*1024);

		g_bIsInitSuccess = TRUE;   //恢复

		return STATUS_UNSUCCESSFUL;
	}
	/*

	当ring3需要的时候，直接传送结构

	*/
	if ((int)FileHandle == LIST_LOG)
	{
		WinVer = GetWindowsVersion();
		switch(WinVer)
		{
		case WINDOWS_VERSION_XP:
			g_Offset_Eprocess_ProcessId = 0x84;
			break;
		case WINDOWS_VERSION_7_7000:
		case WINDOWS_VERSION_7_7600_UP:
			g_Offset_Eprocess_ProcessId = 0xb4;
			break;
		case WINDOWS_VERSION_2K3_SP1_SP2:
			g_Offset_Eprocess_ProcessId = 0x94;
			break;
		}
		
		if (g_bDebugOn)
			KdPrint(("Length:%x %x",Length,sizeof(LOGDEFENSE)*1024));

		if (Length > sizeof(LOGDEFENSE)*1024)
		{
			__try
			{
				for (i=0;i<(int)ulLogCount;i++)
				{
					if (g_pLogDefenseInfo->LogDefense[i].ulPID)
					{
						//这里LogDefenseInfo->LogDefense[i].EProcess是驱动的基址，所有单独处理
						if (g_pLogDefenseInfo->LogDefense[i].Type == 6)
						{
							if (!MmIsAddressValidEx((PVOID)g_pLogDefenseInfo->LogDefense[i].EProcess)){
								g_pLogDefenseInfo->LogDefense[i].EProcess = 0;
							}
							g_pLogDefenseInfo->ulCount = ulLogCount;
							continue;
						}
						if (!IsExitProcess((PEPROCESS)g_pLogDefenseInfo->LogDefense[i].EProcess)){
							memset(g_pLogDefenseInfo->LogDefense[i].lpszProName,0,sizeof(g_pLogDefenseInfo->LogDefense[i].lpszProName));
							strcat(g_pLogDefenseInfo->LogDefense[i].lpszProName,"Unknown");
							g_pLogDefenseInfo->LogDefense[i].ulPID = 0;

						}else{
							memset(g_pLogDefenseInfo->LogDefense[i].lpszProName,0,sizeof(g_pLogDefenseInfo->LogDefense[i].lpszProName));
							lpszProName = (CHAR *)PsGetProcessImageFileName((PEPROCESS)g_pLogDefenseInfo->LogDefense[i].EProcess);
							if (lpszProName){
								strcat(g_pLogDefenseInfo->LogDefense[i].lpszProName,lpszProName);
							}
							lpszProName = NULL;
							g_pLogDefenseInfo->LogDefense[i].ulInheritedFromProcessId = GetInheritedProcessPid((PEPROCESS)g_pLogDefenseInfo->LogDefense[i].EProcess);
						}
					    g_pLogDefenseInfo->ulCount = ulLogCount;

					}
				}
				if (g_bDebugOn)
					KdPrint(("ulLogCount:%d",g_pLogDefenseInfo->ulCount));

			}__except(EXCEPTION_EXECUTE_HANDLER){
				if (g_bDebugOn)
					KdPrint(("[EXCEPTION_EXECUTE_HANDLER]ulLogCount:%d,%d:%ws",g_pLogDefenseInfo->ulCount,ulLogCount,g_pLogDefenseInfo->LogDefense[i].lpwzCreateProcess));

				status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				g_fnRmemcpy(Buffer,g_pLogDefenseInfo,sizeof(LOGDEFENSE)*1024);
				Length = sizeof(LOGDEFENSE)*1024;
				return STATUS_UNSUCCESSFUL;
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,g_pLogDefenseInfo,sizeof(LOGDEFENSE)*1024);
			Length = sizeof(LOGDEFENSE)*1024;
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == CHANG_SERVICES_TYPE_1 ||
		(int)FileHandle == CHANG_SERVICES_TYPE_2 ||
		(int)FileHandle == CHANG_SERVICES_TYPE_3)
	{
		if (MmIsAddressRangeValid(Buffer,Length) &&
			Length > 0)
		{
			memset(lpwzKey,0,sizeof(lpwzKey));
			wcscat(lpwzKey,L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\");
			wcscat(lpwzKey,Buffer);

			switch ((ULONG)FileHandle)
			{
			case CHANG_SERVICES_TYPE_1:  //手动
				Safe_CreateValueKey(lpwzKey,REG_DWORD,L"Start",(PWCHAR)0x3);
				break;
			case CHANG_SERVICES_TYPE_2:  //自动
				Safe_CreateValueKey(lpwzKey,REG_DWORD,L"Start",(PWCHAR)0x2);
				break;
			case CHANG_SERVICES_TYPE_3:  //禁用
				Safe_CreateValueKey(lpwzKey,REG_DWORD,L"Start",(PWCHAR)0x4);
				break;
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	深度服务扫描，其实就是注册一个驱动自启动，然后再注册表刚刚初始化好的时候枚举注册表
	因为驱动启动的早，而木马没有启动，所以就可以枚举到木马的注册表

	*/
	if ((int)FileHandle == LIST_DEPTH_SERVICES)
	{
		if (DepthServicesRegistry == NULL)
		{
			if (g_bDebugOn)
				KdPrint(("DepthServicesRegistry is NULL"));
			return STATUS_UNSUCCESSFUL;
		}
		if (!MmIsAddressValidEx(DepthServicesRegistry))
		{
			if (g_bDebugOn)
				KdPrint(("MmIsAddressValidEx!!!"));
			return STATUS_UNSUCCESSFUL;
		}
		
		if (g_bDebugOn)
			KdPrint(("Length:%08x--DepthServicesRegistry:%08x",Length,sizeof(SERVICESREGISTRY)*1024));

		if (Length > sizeof(SERVICESREGISTRY)*1024)
		{
			if (DepthServicesRegistry->ulCount)
			{
				for (i=0;i<(int)DepthServicesRegistry->ulCount;i++)
				{
					if (g_bDebugOn)
						KdPrint(("[%d]深度服务查看\r\n"
						"服务名:%ws\r\n"
						"映像路径:%ws\r\n"
						"动态链接库:%ws\r\n\r\n",
						i,
						DepthServicesRegistry->SrvReg[i].lpwzSrvName,
						DepthServicesRegistry->SrvReg[i].lpwzImageName,
						DepthServicesRegistry->SrvReg[i].lpwzDLLPath
						));
				}
				status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				g_fnRmemcpy(Buffer,DepthServicesRegistry,sizeof(SERVICESREGISTRY)*1024);
				Length = sizeof(SERVICESREGISTRY)*1024;
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	普通方式的服务枚举

	*/
	if ((int)FileHandle == LIST_SERVICES)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		ServicesRegistry = (PSERVICESREGISTRY)g_fnRExAllocatePool(NonPagedPool,sizeof(SERVICESREGISTRY)*1024);
		if (!ServicesRegistry)
		{
			if (g_bDebugOn)
				KdPrint(("RExAllocatePool !!"));
			return STATUS_UNSUCCESSFUL;
		}
		if (g_bDebugOn)
			KdPrint(("search !!"));
		memset(ServicesRegistry,0,sizeof(SERVICESREGISTRY)*1024);
		if (QueryServicesRegistry(ServicesRegistry) == STATUS_SUCCESS)
		{
			if (g_bDebugOn)
				KdPrint(("Length:%08x-ServicesRegistry:%08x",Length,sizeof(SERVICESREGISTRY)*1024));
			if (Length > sizeof(SERVICESREGISTRY)*1024)
			{
				if (g_bDebugOn)
					KdPrint(("Length !!"));

				for (i=0;i<(int)ServicesRegistry->ulCount;i++)
				{
					if (DepthServicesRegistry)
					{
						for (x=0;x<(int)DepthServicesRegistry->ulCount;x++)
						{
							bIsNormalServices = FALSE;

							if (_wcsnicmp(ServicesRegistry->SrvReg[i].lpwzSrvName,DepthServicesRegistry->SrvReg[x].lpwzSrvName,wcslen(DepthServicesRegistry->SrvReg[x].lpwzSrvName)) == 0)
							{
								bIsNormalServices = TRUE;
								break;
							}
						}
						//服务不正常
						if (!bIsNormalServices)
						{
							wcscat(ServicesRegistry->SrvReg[i].lpwzSrvName,L"(最新创建)");
						}
					}
					if (g_bDebugOn)
						KdPrint(("[%d]服务查看\r\n"
						"服务名:%ws\r\n"
						"映像路径:%ws\r\n"
						"动态链接库:%ws\r\n\r\n",
						i,
						ServicesRegistry->SrvReg[i].lpwzSrvName,
						ServicesRegistry->SrvReg[i].lpwzImageName,
						ServicesRegistry->SrvReg[i].lpwzDLLPath
						));
				}
				status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				g_fnRmemcpy(Buffer,ServicesRegistry,sizeof(SERVICESREGISTRY)*1024);
				Length = sizeof(SERVICESREGISTRY)*1024;
			}
		}
		g_fnRExFreePool(ServicesRegistry);
		return STATUS_UNSUCCESSFUL;
	}
// 	//暂停保护
	if ((int)FileHandle == SUSPEND_PROTECT)
	{
		g_bIsInitSuccess = FALSE;   //恢复标记
		return STATUS_UNSUCCESSFUL;
	}
	//恢复保护
	if ((int)FileHandle == RESUME_PROTECT)
	{
		g_bIsInitSuccess = TRUE;   //恢复标记
		return STATUS_UNSUCCESSFUL;
	}
	//随便赋值，让进程顺利exit~！
	if ((int)FileHandle == EXIT_PROCESS)
	{
		g_bIsInitSuccess = FALSE;   //恢复标记
		g_protectEProcess = (PEPROCESS)0x12345678;

		return STATUS_UNSUCCESSFUL;
	}
	/*

	读取驱动模块

	*/
	if ((int)FileHandle == LIST_SYS_MODULE)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		SysModuleInfo = (PSYSINFO)g_fnRExAllocatePool(NonPagedPool,sizeof(SYSINFO)*260);
		if (!SysModuleInfo)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(SysModuleInfo,0,sizeof(SYSINFO)*260);
		EnumKernelModule(g_pDriverObject,SysModuleInfo);
		if (Length > sizeof(SYSINFO)*260)
		{
			for (i=0;i<(int)SysModuleInfo->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("[%d]SysModule\r\n"
						"对象:%08x\r\n"
						"基址:%08x\r\n"
						"大小:%x\r\n"
						"驱动名:%ws\r\n"
						"驱动路径:%ws\r\n"
						"服务:%ws\r\n"
						"隐藏类型:%d\r\n",
						i,
						SysModuleInfo->SysInfo[i].DriverObject,
						SysModuleInfo->SysInfo[i].ulSysBase,
						SysModuleInfo->SysInfo[i].SizeOfImage,
						SysModuleInfo->SysInfo[i].lpwzBaseSysName,
						SysModuleInfo->SysInfo[i].lpwzFullSysName,
						SysModuleInfo->SysInfo[i].lpwzServiceName,
						SysModuleInfo->SysInfo[i].IntHideType
						));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,SysModuleInfo,sizeof(SYSINFO)*260);
			Length = sizeof(SYSINFO)*260;
		}
		g_fnRExFreePool(SysModuleInfo);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == NO_KERNEL_SAFE_MODULE)
	{
		g_bKernelSafeModule = FALSE;  //关闭内核安全模式
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == KERNEL_SAFE_MODULE)
	{
		g_bKernelSafeModule = TRUE;  //开启内核安全模式，让系统不存在任何的hook
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_EAT_HOOK)
	{
		ulInitRealModuleBase = Length;

		KdPrint(("num:%d module:%x func:%x\r\n",ulNumber,ulInitRealModuleBase,ulInitRealFuncBase));

		if (MmIsAddressValidEx((PVOID)ulInitRealModuleBase) &&
			MmIsAddressValidEx((PVOID)ulInitRealFuncBase))
		{
			KdPrint(("111num:%d module:%x func:%x\r\n",ulNumber,ulInitRealModuleBase,ulInitRealFuncBase));
			ReSetEatHook(ulNumber,ulInitRealModuleBase,ulInitRealFuncBase);
			ulNumber = 0;
			ulInitRealModuleBase = 0;
			ulInitRealFuncBase = 0;
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_EAT_REAL_ADDRESS)
	{
		ulInitRealFuncBase = Length;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_EAT_NUMBER)
	{
		ulNumber = Length;
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == ANTI_INLINEHOOK)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0)
		{
			AntiInlineHook(Length,g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举ntkrnlpa的inline hook（联网下则下载符号扫描未导出函数，反之扫描导出函数）

	*/
	if ((int)FileHandle == LIST_INLINEHOOK)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		InlineHookInfo = (PINLINEHOOKINFO)g_fnRExAllocatePool(NonPagedPool,sizeof(INLINEHOOKINFO)*256);
		if (!InlineHookInfo)
		{
			if (g_bDebugOn)
				KdPrint(("InlineHookInfo failed\r\n"));
			return STATUS_UNSUCCESSFUL;
		}
		memset(InlineHookInfo,0,sizeof(INLINEHOOKINFO)*256);
		KernelHookCheck(InlineHookInfo,NtosModule);

		//KdPrint(("%08x---%08x\r\n",Length,sizeof(INLINEHOOKINFO)*256));
		if (Length > sizeof(INLINEHOOKINFO)*256)
		{
			for (i=0;i<(int)InlineHookInfo->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("[%d]KernelHook\r\n"
					"被挂钩地址:%08x\r\n"
					"原始地址:%08x\r\n"
					"挂钩函数:%s\r\n"
					"hook跳转地址:%08x\r\n"
					"所在模块:%s\r\n"
					"模块基址:%08x\r\n"
					"模块大小:%x\r\n",
					i,
					InlineHookInfo->InlineHook[i].ulMemoryFunctionBase,
					InlineHookInfo->InlineHook[i].ulRealFunctionBase,
					InlineHookInfo->InlineHook[i].lpszFunction,
					InlineHookInfo->InlineHook[i].ulMemoryHookBase,
					InlineHookInfo->InlineHook[i].lpszHookModuleImage,
					InlineHookInfo->InlineHook[i].ulHookModuleBase,
					InlineHookInfo->InlineHook[i].ulHookModuleSize
					));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,InlineHookInfo,sizeof(INLINEHOOKINFO)*256);
			Length = sizeof(INLINEHOOKINFO)*256;
		}
		g_fnRExFreePool(InlineHookInfo);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_PROCESS_LIST_PROCESS_MODULE)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			ulInitEProcess = Length;  //初始化
			if (g_bDebugOn)
				KdPrint(("InitEprocess:%08x\n",ulInitEProcess));
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举进程DLL模块

	*/
	if ((int)FileHandle == LIST_PROCESS_MODULE)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		if (!MmIsAddressValidEx((PVOID)ulInitEProcess))
		{
			return STATUS_UNSUCCESSFUL;
		}
		PDll = (PDLLINFO)g_fnRExAllocatePool(NonPagedPool,sizeof(DLLINFO)*512);
		if (!PDll)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(PDll,0,sizeof(DLLINFO)*512);
		
		EunmProcessModule(ulInitEProcess,PDll);

		ulInitEProcess = 0;  //恢复为NULL
		if (Length > sizeof(DLLINFO)*512)
		{
			for (i=0;i<(int)PDll->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("[%d]Dll模块\r\n"
					"Path:%ws\r\n"
					"Base:%08X\r\n\r\n",
					i,
					PDll->DllInfo[i].lpwzDllModule,
					PDll->DllInfo[i].ulBase
					));
			}
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,PDll,sizeof(DLLINFO)*512);
			Length = sizeof(DLLINFO)*512;
		}
		g_fnRExFreePool(PDll);
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举进程模块

	*/
	if ((int)FileHandle == LIST_PROCESS)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		g_pNormalProcessInfo = (PPROCESSINFO)g_fnRExAllocatePool(NonPagedPool,sizeof(PROCESSINFO)*256);
		if (!g_pNormalProcessInfo)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(g_pNormalProcessInfo,0,sizeof(PROCESSINFO)*256);

		bPaused = TRUE;  //暂停下读取内存，免得造成同步问题

		GetNormalProcessList(g_pNormalProcessInfo,g_pHideProcessInfo);
		if (g_bDebugOn)
			KdPrint(("Length:%08x-NormalProcessInfo:%08x",Length,sizeof(PROCESSINFO)*256));
		if (Length > sizeof(PROCESSINFO)*256)
		{
			for (i=0;i<(int)g_pNormalProcessInfo->ulCount;i++)
			{
				if (g_bDebugOn)
					KdPrint(("[%d]进程查看\r\n"
						"隐藏状态:%d\r\n"
						"pid:%d\r\n"
						"父进程:%d\r\n"
						"内核访问状态:%d\r\n"
						"PEPROCESS:%08x\r\n"
						"进程全路径:%ws\r\n\r\n",
						i,
						g_pNormalProcessInfo->ProcessInfo[i].IntHideType,
						g_pNormalProcessInfo->ProcessInfo[i].ulPid,
						g_pNormalProcessInfo->ProcessInfo[i].ulInheritedFromProcessId,
						g_pNormalProcessInfo->ProcessInfo[i].ulKernelOpen,
						g_pNormalProcessInfo->ProcessInfo[i].EProcess,
						g_pNormalProcessInfo->ProcessInfo[i].lpwzFullProcessPath
						));
			}
			status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				g_fnRmemcpy(Buffer,g_pNormalProcessInfo,sizeof(PROCESSINFO)*256);
				Length = sizeof(PROCESSINFO)*256;
		}
		g_fnRExFreePool(g_pNormalProcessInfo);

		//查看过后，就要清零一次
		//因为bPaused控制，所以就不担心同步问题
		//其实也可以用自选锁
		memset(g_pHideProcessInfo,0,(sizeof(PROCESSINFO)+sizeof(SAFESYSTEM_PROCESS_INFORMATION))*120);
		bPaused = FALSE;   //恢复隐藏进程的读取

		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == KILL_PROCESS_BY_PID)
	{
		//传入服务号
		if (Length > 0)
		{
			g_bKernelSafeModule = TRUE;

			KillPro(Length);

			g_bKernelSafeModule = FALSE;
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_SET_ATAPI_HOOK)
	{
		if ((PVOID)Length >= NULL && Length <= 0x1c)
		{
			ulNumber = Length;  //初始化
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_ATAPI_HOOK)
	{
		if (Length > 0x123456)
		{
			ulRealDispatch = Length;

			if (g_bDebugOn)
				KdPrint(("Init ulRealDispatch:[%d]%X\n",ulNumber,ulRealDispatch));

			if ((PVOID)ulNumber >= NULL && ulNumber <= 0x1c)
			{
				SetAtapiHook(ulNumber,ulRealDispatch);
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LIST_ATAPI_HOOK)
	{
		//开始填充
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		AtapiDispatchBakUp = (PATAPIDISPATCHBAKUP)g_fnRExAllocatePool(NonPagedPool,sizeof(ATAPIDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
		if (!AtapiDispatchBakUp)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(AtapiDispatchBakUp,0,sizeof(ATAPIDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);

		ReLoadAtapi(g_pDriverObject,AtapiDispatchBakUp,1);  //kbdclass hook

		if (Length > sizeof(ATAPIDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION)
		{
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,AtapiDispatchBakUp,sizeof(ATAPIDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
			Length = sizeof(ATAPIDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION;
		}
		g_fnRExFreePool(AtapiDispatchBakUp);

		return STATUS_UNSUCCESSFUL;
	}
	/////////////////
	if ((int)FileHandle == INIT_SET_MOUCLASS_HOOK)
	{
		if ((PVOID)Length >= NULL && Length <= IRP_MJ_MAXIMUM_FUNCTION)
		{
			ulNumber = Length;  //初始化
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_MOUCLASS_HOOK)
	{
		if (Length > 0x123456)
		{
			ulRealDispatch = Length;

			if (g_bDebugOn)
				KdPrint(("Init ulRealDispatch:[%d]%X\n",ulNumber,ulRealDispatch));

			if ((PVOID)ulNumber >= NULL && ulNumber <= IRP_MJ_MAXIMUM_FUNCTION)
			{
				SetMouclassHook(ulNumber,ulRealDispatch);
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LIST_MOUCLASS_HOOK)
	{
		//开始填充
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		MouclassDispatchBakUp = (PMOUCLASSDISPATCHBAKUP)g_fnRExAllocatePool(NonPagedPool,sizeof(MOUCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
		if (!MouclassDispatchBakUp)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(MouclassDispatchBakUp,0,sizeof(MOUCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);

		ReLoadMouclass(g_pDriverObject,MouclassDispatchBakUp,1);  //kbdclass hook

		if (Length > sizeof(MOUCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION)
		{
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,MouclassDispatchBakUp,sizeof(MOUCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
			Length = sizeof(MOUCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION;
		}
		g_fnRExFreePool(MouclassDispatchBakUp);

		return STATUS_UNSUCCESSFUL;
	}
	///////
	if ((int)FileHandle == INIT_SET_KBDCLASS_HOOK)
	{
		if ((PVOID)Length >= NULL && Length <= IRP_MJ_MAXIMUM_FUNCTION)
		{
			ulNumber = Length;  //初始化
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_KBDCLASS_HOOK)
	{
		if (Length > 0x123456)
		{
			ulRealDispatch = Length;

			if (g_bDebugOn)
				KdPrint(("Init ulRealDispatch:[%d]%X\n",ulNumber,ulRealDispatch));

			if ((PVOID)ulNumber >= NULL && ulNumber <= IRP_MJ_MAXIMUM_FUNCTION)
			{
				SetKbdclassHook(ulNumber,ulRealDispatch);
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LIST_KBDCLASS_HOOK)
	{
		//开始填充
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		KbdclassDispatchBakUp = (PKBDCLASSDISPATCHBAKUP)g_fnRExAllocatePool(NonPagedPool,sizeof(KBDCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
		if (!KbdclassDispatchBakUp)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(KbdclassDispatchBakUp,0,sizeof(KBDCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);

		ReLoadKbdclass(g_pDriverObject,KbdclassDispatchBakUp,1);  //kbdclass hook

		if (Length > sizeof(KBDCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION)
		{
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,KbdclassDispatchBakUp,sizeof(KBDCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
			Length = sizeof(KBDCLASSDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION;
		}
		g_fnRExFreePool(KbdclassDispatchBakUp);

		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_SET_FSD_HOOK)
	{
		if ((PVOID)Length >= NULL && Length <= IRP_MJ_MAXIMUM_FUNCTION)
		{
			ulNumber = Length;  //初始化
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_FSD_HOOK)
	{
		if (Length > 0x123456)
		{
			ulRealDispatch = Length;

			if (g_bDebugOn)
				KdPrint(("Init ulRealDispatch:[%d]%X\n",ulNumber,ulRealDispatch));

			if ((PVOID)ulNumber >= NULL && ulNumber <= IRP_MJ_MAXIMUM_FUNCTION)
			{
				SetFsdHook(ulNumber,ulRealDispatch);
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LIST_FSD_HOOK)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		NtfsDispatchBakUp = (PNTFSDISPATCHBAKUP)g_fnRExAllocatePool(NonPagedPool,sizeof(NTFSDISPATCHBAKU)*IRP_MJ_MAXIMUM_FUNCTION);
		if (!NtfsDispatchBakUp)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(NtfsDispatchBakUp,0,sizeof(NTFSDISPATCHBAKU)*IRP_MJ_MAXIMUM_FUNCTION);

		ReLoadNtfs(g_pDriverObject,NtfsDispatchBakUp,1);  //fsd hook

		if (Length > sizeof(NTFSDISPATCHBAKU)*IRP_MJ_MAXIMUM_FUNCTION)
		{
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,NtfsDispatchBakUp,sizeof(NTFSDISPATCHBAKU)*IRP_MJ_MAXIMUM_FUNCTION);
			Length = sizeof(NTFSDISPATCHBAKU)*IRP_MJ_MAXIMUM_FUNCTION;
		}
		g_fnRExFreePool(NtfsDispatchBakUp);

		return STATUS_UNSUCCESSFUL;
	}
	//ONLY_DELETE_FILE是删除360文件，因此不需要reload
	if ((int)FileHandle == DELETE_FILE ||
		(int)FileHandle == ONLY_DELETE_FILE)
	{
		if (MmIsAddressValidEx(Buffer) &&
			Length > 0)
		{
			g_bKernelSafeModule = TRUE;

			if ((int)FileHandle == DELETE_FILE)
			{
				ReLoadNtfs(g_pDriverObject,0,0);  //reload ntfs 恢复真实地址
				ReLoadAtapi(g_pDriverObject,0,0); //reload atapi 
			}
			if (IsFileInSystem(Buffer))
			{
				HFileHandle = SkillIoOpenFile(
					Buffer,   //删除dll文件
					FILE_READ_ATTRIBUTES,
					FILE_SHARE_DELETE);
				if (HFileHandle!=NULL)
				{
					SKillDeleteFile(HFileHandle);
					ZwClose(HFileHandle);
				}
			}
			if ((int)FileHandle == DELETE_FILE)
			{
				ReLoadNtfsFree();  //恢复
				ReLoadAtapiFree();
			}
			g_bKernelSafeModule = FALSE;
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_SET_TCPIP_HOOK)
	{
		if ((PVOID)Length >= NULL && Length <= IRP_MJ_MAXIMUM_FUNCTION)
		{
			ulNumber = Length;  //初始化
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_TCPIP_HOOK)
	{
		if (Length > 0x123456)
		{
			ulRealDispatch = Length;

			if (g_bDebugOn)
				KdPrint(("Init ulRealDispatch:[%d]%X\n",ulNumber,ulRealDispatch));

			if ((PVOID)ulNumber >= NULL && ulNumber <= IRP_MJ_MAXIMUM_FUNCTION)
			{
				SetTcpHook(ulNumber,ulRealDispatch);
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LIST_TCPIP_HOOK)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		TcpDispatchBakUp = (PTCPDISPATCHBAKUP)g_fnRExAllocatePool(NonPagedPool,sizeof(TCPDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
		if (!TcpDispatchBakUp)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(TcpDispatchBakUp,0,sizeof(TCPDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);

		ReLoadTcpip(g_pDriverObject,TcpDispatchBakUp,1);
		if (Length > sizeof(TCPDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION)
		{
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,TcpDispatchBakUp,sizeof(TCPDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
			Length = sizeof(TCPDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION;
		}
		ReLoadTcpipFree(); //释放

		g_fnRExFreePool(TcpDispatchBakUp);

		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == INIT_SET_NSIPROXY_HOOK)
	{
		if ((PVOID)Length >= NULL && Length <= IRP_MJ_MAXIMUM_FUNCTION)
		{
			ulNumber = Length;  //初始化
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_NSIPROXY_HOOK)
	{
		if (Length > 0x123456)
		{
			ulRealDispatch = Length;

			if (g_bDebugOn)
				KdPrint(("Init ulRealDispatch:[%d]%X\n",ulNumber,ulRealDispatch));

			if ((PVOID)ulNumber >= NULL && ulNumber <= IRP_MJ_MAXIMUM_FUNCTION)
			{
				SetNsiproxyHook(ulNumber,ulRealDispatch);
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LIST_NSIPROXY_HOOK)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		NsiproxyDispatchBakUp = (PNSIPROXYDISPATCHBAKUP)g_fnRExAllocatePool(NonPagedPool,sizeof(NSIPROXYDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
		if (!NsiproxyDispatchBakUp)
		{
			return STATUS_UNSUCCESSFUL;
		}
		memset(NsiproxyDispatchBakUp,0,sizeof(NSIPROXYDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
		ReLoadNsiproxy(g_pDriverObject,NsiproxyDispatchBakUp,1);

		if (Length > sizeof(NSIPROXYDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION)
		{
			status = OldZwReadFile(
				FileHandle,
				Event,
				ApcRoutine,
				ApcContext,
				IoStatusBlock,
				Buffer,
				Length,
				ByteOffset,
				Key
				);
			g_fnRmemcpy(Buffer,NsiproxyDispatchBakUp,sizeof(NSIPROXYDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION);
			Length = sizeof(NSIPROXYDISPATCHBAKUP)*IRP_MJ_MAXIMUM_FUNCTION;
		}
		g_fnRExFreePool(NsiproxyDispatchBakUp);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == LIST_TCPUDP)
	{
		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

		ReLoadTcpip(g_pDriverObject,0,0);  //先reload
		ReLoadNsiproxy(g_pDriverObject,0,0);  //先reload

		TCPUDPInfo = (PTCPUDPINFO)g_fnRExAllocatePool(NonPagedPool,sizeof(TCPUDPINFO)*256);
		if (TCPUDPInfo)
		{
			memset(TCPUDPInfo,0,sizeof(TCPUDPINFO)*256);

			WinVer = GetWindowsVersion();
			if (WinVer == WINDOWS_VERSION_XP ||
				WinVer == WINDOWS_VERSION_2K3_SP1_SP2)
			{
				PrintTcpIp(TCPUDPInfo);
			}
			else if (WinVer == WINDOWS_VERSION_7_7000 || 
				     WinVer == WINDOWS_VERSION_7_7600_UP)
			{
				PrintTcpIpInWin7(TCPUDPInfo);
			}
			if (Length > sizeof(TCPUDPINFO)*256)
			{
				for (i = 0; i<(int)TCPUDPInfo->ulCount ;i++)
				{
					if (g_bDebugOn)
						KdPrint(("[%d]网络连接\r\n"
						"协议:%d\r\n"
						"连接状态:%d\r\n"
						"本地地址:%08x\r\n"
						"本地端口:%d\r\n"
						"进程pid:%d\r\n"
						"进程路径:%ws\r\n"
						"远程地址:%08x\r\n"
						"远程端口:%d\r\n\r\n",
						i,
						TCPUDPInfo->TCPUDP[i].ulType,
						TCPUDPInfo->TCPUDP[i].ulConnectType,
						TCPUDPInfo->TCPUDP[i].ulLocalAddress,
						TCPUDPInfo->TCPUDP[i].ulLocalPort,
						TCPUDPInfo->TCPUDP[i].ulPid,
						TCPUDPInfo->TCPUDP[i].lpwzFullPath,
						TCPUDPInfo->TCPUDP[i].ulRemoteAddress,
						TCPUDPInfo->TCPUDP[i].ulRemotePort));
				}
				status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				g_fnRmemcpy(Buffer,TCPUDPInfo,sizeof(TCPUDPINFO)*256);
				Length = sizeof(TCPUDPINFO)*256;
			}
			g_fnRExFreePool(TCPUDPInfo);
		}
		ReLoadTcpipFree(); //释放
		ReLoadNsiproxyFree(); //释放

		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_INLINE_HOOK)
	{
		if (MmIsAddressValidEx((PVOID)Length) &&
			Length > 0x123456)
		{
			if (g_bDebugOn)
				KdPrint(("Set Inline hook:%08x\n",Length));

			RestoreInlineHook(Length,g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_ONE_SSDT)
	{
		//传入服务号
		if (Length > 0 ||
			Length == 0)
		{
			RestoreAllSSDTFunction(Length);
		}
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == SET_ALL_SSDT)
	{
		RestoreAllSSDTFunction(8888);
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == UNPROTECT_DRIVER_FILE)
	{
		bProtectDriverFile = FALSE;   //取消保护
		KdPrint(("UnProtect Driver File\r\n"));
		return STATUS_UNSUCCESSFUL;
	}
	if ((int)FileHandle == PROTECT_DRIVER_FILE)
	{
		bProtectDriverFile = TRUE;     //保护
		KdPrint(("Protect Driver File\r\n"));
		return STATUS_UNSUCCESSFUL;
	}
	/*

	这个命令很关键，它是区别A盾的命令，只要写好一点就可以避免冒充

	*/
	if ((int)FileHandle == SAFE_SYSTEM)
	{
		if (g_bDebugOn)
			KdPrint(("ProtectCode:%08x\r\n",SAFE_SYSTEM));

		if (Length == 8 &&
			MmIsAddressValidEx(Buffer))
		{
			if (g_bDebugOn)
				KdPrint(("Buffer:%s %d\r\n",Buffer,Length));

			if (_strnicmp(Buffer,"Safe",4) ==0)
			{
				//验证Caller的大小
// 				if (GetCallerFileSize(RPsGetCurrentProcess()) != ulCallerFileSize){
// 					//文件体积大小不对，很明显不是A盾
// 					status = OldZwReadFile(
// 						FileHandle,
// 						Event,
// 						ApcRoutine,
// 						ApcContext,
// 						IoStatusBlock,
// 						Buffer,
// 						Length,
// 						ByteOffset,
// 						Key
// 						);
// 					memcpy(Buffer,"call",strlen("call"));
// 					Length = 8;
// 					return STATUS_UNSUCCESSFUL;
// 				}
				g_protectEProcess = PsGetCurrentProcess();   //自己的进程啊
				ProtectProcessId = PsGetCurrentProcessId();  //Shadow SSDT 需要用的

				if (g_bDebugOn)
					KdPrint(("ProtectCode:%08x\r\n",g_protectEProcess));

				status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				memcpy(Buffer,"hehe",strlen("hehe"));
				Length = 8;
				g_bIsInitSuccess = TRUE;
			}
		}
		return STATUS_UNSUCCESSFUL;
	}
	/*

	枚举SSDT

	*/
	if ((int)FileHandle == LIST_SSDT ||
		(int)FileHandle == LIST_SSDT_ALL)
	{
		if ((int)FileHandle == LIST_SSDT_ALL)
		{
			KdPrint(("Print SSDT All"));
			bSSDTAll = TRUE;
		}
		if ((int)FileHandle == LIST_SSDT)
		{
			KdPrint(("Print SSDT"));
		}

		ReLoadNtosCALL((PVOID)(&g_fnRmemcpy),L"memcpy",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExAllocatePool),L"ExAllocatePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		ReLoadNtosCALL((PVOID)(&g_fnRExFreePool),L"ExFreePool",g_pOldSystemKernelModuleBase,(ULONG)g_pNewSystemKernelModuleBase);
		if (g_fnRExAllocatePool &&
			g_fnRExFreePool &&
			g_fnRmemcpy)
		{
			bInit = TRUE;
		}
		if (!bInit)
			return STATUS_UNSUCCESSFUL;

// 		ulKeServiceDescriptorTable = GetSystemRoutineAddress(1,L"KeServiceDescriptorTable");
// 		ulSize = ((PSERVICE_DESCRIPTOR_TABLE)ulKeServiceDescriptorTable)->TableSize;
		g_pSSDTInfo = (PSSDTINFO)g_fnRExAllocatePool(NonPagedPool,sizeof(SSDTINFO)*800);
		if (g_pSSDTInfo)
		{
			memset(g_pSSDTInfo,0,sizeof(SSDTINFO)*800);
			PrintSSDT(g_pSSDTInfo);
			if (Length > sizeof(SSDTINFO)*800)
			{
				for (i = 0; i< (int)g_pSSDTInfo->ulCount ;i++)
				{
					if (g_bDebugOn)
						KdPrint(("[%d]发现SSDT hook\r\n"
						"服务号:%d\r\n"
						"当前地址:%08x\r\n"
						"原始地址:%08x\r\n"
						"函数名字:%s\r\n"
						"当前hook模块:%s\r\n"
						"当前模块地址:%08x\r\n"
						"当前模块大小:%d KB\r\n"
						"Hook类型:%d\r\n\r\n",
						i,
						g_pSSDTInfo->SSDT[i].ulNumber,
						g_pSSDTInfo->SSDT[i].ulMemoryFunctionBase,
						g_pSSDTInfo->SSDT[i].ulRealFunctionBase,
						g_pSSDTInfo->SSDT[i].lpszFunction,
						g_pSSDTInfo->SSDT[i].lpszHookModuleImage,
						g_pSSDTInfo->SSDT[i].ulHookModuleBase,
						g_pSSDTInfo->SSDT[i].ulHookModuleSize/1024,
						g_pSSDTInfo->SSDT[i].IntHookType));
				}
				status = OldZwReadFile(
					FileHandle,
					Event,
					ApcRoutine,
					ApcContext,
					IoStatusBlock,
					Buffer,
					Length,
					ByteOffset,
					Key
					);
				g_fnRmemcpy(Buffer,g_pSSDTInfo,sizeof(SSDTINFO)*800);
				Length = sizeof(SSDTINFO)*800;
			}
			g_fnRExFreePool(g_pSSDTInfo);
		}
		bSSDTAll = FALSE;
		return status;
	}
_FunctionRet:
	return OldZwReadFile(
		FileHandle,
		Event,
		ApcRoutine,
		ApcContext,
		IoStatusBlock,
		Buffer,
		Length,
		ByteOffset,
		Key
		);
}
__declspec(naked) NTSTATUS NtReadFileHookZone(,...)
{
	_asm
	{
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		_emit 0x90;
		jmp [NtReadFileRet];
	}
}
/*

内核下的Sleep
参数：要sleep的毫秒

*/
VOID WaitMicroSecond(__in LONG MicroSeconds)
{
	KEVENT KEnentTemp;
	LARGE_INTEGER waitTime;

	KeInitializeEvent(
		&KEnentTemp, 
		SynchronizationEvent, 
		FALSE
		);
	waitTime = RtlConvertLongToLargeInteger(-10 * MicroSeconds);

	KeWaitForSingleObject(
		&KEnentTemp,
		Executive,
		KernelMode,
		FALSE, 
		&waitTime
		);
}
/*

负责Hook NtReadFile和恢复
因为之前要获取csrss.exe的EPROCESS，ssdt hook ZwReadFile是获取不到得
必须要inline hook NtReadFile

*/

VOID ResetMyControl()
{
	BOOL bRet = FALSE;
	KIRQL oldIrql = (KIRQL)0; 

	while (1)
	{
		if (!bRet)
		{
			bRet = HookFunctionHeader((DWORD)NewNtReadFile,L"ZwReadFile",TRUE,0,(PVOID)NtReadFileHookZone,&NtReadFilePatchCodeLen,&NtReadFileRet);
			if (g_bDebugOn)
				KdPrint(("inline hook ZwReadFile success"));
		}
		//取到pid之后
		if (IsExitProcess(AttachGuiEProcess))
		{
			//线程里的IRQL太高了，要降低下
			if (KeGetCurrentIrql() <= DISPATCH_LEVEL &&
				KeGetCurrentIrql() > PASSIVE_LEVEL)
			{
				if (!oldIrql)
					oldIrql = KeRaiseIrqlToDpcLevel(); //注意自旋锁 
			}
			UnHookFunctionHeader(L"ZwReadFile",TRUE,0,(PVOID)NtReadFileHookZone,NtReadFilePatchCodeLen);  //恢复一下

			if (oldIrql)
				KeLowerIrql(oldIrql);

			KdPrint(("Init Protect Thread success\r\n"));
			
			/*

			深度扫描，驱动自启动不能调用PsTerminateSystemThread结束线程
			应该让线程自己返回

			*/
			break;
		}
		WaitMicroSecond(88);
	}
}

NTSTATUS __stdcall NewZwTerminateProcess(
	IN HANDLE  ProcessHandle,
	IN NTSTATUS  ExitStatus
	)
{
	PEPROCESS EProcess;
	NTSTATUS status;
	ZWTERMINATEPROCESS OldZwTerminateProcess;
	KPROCESSOR_MODE PreviousMode;

	//KdPrint(("bIsProtect360:%d",bIsProtect360));
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
	{
		goto _FunctionRet;
	}
	//如果退出了
	if (!g_bIsInitSuccess)
		goto _FunctionRet;

	//默认第一层简单保护下
// 	if (!bProtectProcess)
// 		goto _FunctionRet;

	if (ProcessHandle &&
		ARGUMENT_PRESENT(ProcessHandle))
	{
		status = ObReferenceObjectByHandle(
			ProcessHandle,
			PROCESS_ALL_ACCESS,
			0,
			KernelMode,
			(PVOID*)&EProcess,
			NULL
			);
		if (NT_SUCCESS(status))
		{
			ObDereferenceObject(EProcess);

			//保护自身
			if (EProcess == g_protectEProcess &&
				PsGetCurrentProcess() != g_protectEProcess)
			{
				return STATUS_ACCESS_DENIED;
			}
		}
	}
_FunctionRet:
	OldZwTerminateProcess = (ZWTERMINATEPROCESS)g_pOriginalServiceDescriptorTable->ServiceTable[ZwTerminateProcessIndex];
	return OldZwTerminateProcess(
		ProcessHandle,
		ExitStatus
		);
}
/*

初始化通信控制

*/
BOOL InitControl()
{
	UNICODE_STRING UnicdeFunction;
	HANDLE ThreadHandle;
	PEPROCESS EProcess;

 	if (SystemCallEntryTableHook(
		(PUNICODE_STRING)("ZwReadFile"),
		&ZwReadFileIndex,
		(DWORD)NewNtReadFile) == TRUE)
	{
		if (g_bDebugOn)
			KdPrint(("Init Control Thread success 1\r\n"));
	}
	if (bKernelBooting)
	{
		//重启就要初始化
		g_bIsInitSuccess = TRUE;
		KdPrint(("kernel booting\r\n"));
	}
	if (SystemCallEntryTableHook(
		(PUNICODE_STRING)("ZwTerminateProcess"),
		&ZwTerminateProcessIndex,
		(DWORD)NewZwTerminateProcess) == TRUE)
	{
		if (g_bDebugOn)
			KdPrint(("Create Control Thread success 2\r\n"));
	}
	InitZwSetValueKey();  //注册表
 	InitNetworkDefence();

 	InitWriteFile();

	//去掉object hook保护文件，暂时不需要吧
	//InstallFileObejctHook();
	InitKernelThreadData();   //kernel thread hook

	if (PsCreateSystemThread(
		&ThreadHandle,
		0,
		NULL,
		NULL,
		NULL,
		(PKSTART_ROUTINE)ResetMyControl,
		NULL) == STATUS_SUCCESS)
	{
		ZwClose(ThreadHandle);
		if (g_bDebugOn)
			KdPrint(("Create Control Thread success 2\r\n"));
	}
	return TRUE;
}