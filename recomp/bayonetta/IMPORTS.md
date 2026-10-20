# Bayonetta: Xbox 360 imports to implement

The 185 system functions `default.xex` imports, i.e. the runtime's to-do list. Generated from the
recompiler output (`private/imports.txt`); tick them off as they are implemented.

## Kernel: threads, sync, time (28)

- [ ] `KeAcquireSpinLockAtRaisedIrql`
- [ ] `KeBugCheck`
- [ ] `KeBugCheckEx`
- [ ] `KeDelayExecutionThread`
- [ ] `KeEnableFpuExceptions`
- [ ] `KeEnterCriticalRegion`
- [ ] `KeGetCurrentProcessType`
- [ ] `KeLeaveCriticalRegion`
- [ ] `KeLockL2`
- [ ] `KeQueryBasePriorityThread`
- [ ] `KeQueryPerformanceFrequency`
- [ ] `KeQuerySystemTime`
- [ ] `KeReleaseSpinLockFromRaisedIrql`
- [ ] `KeResetEvent`
- [ ] `KeSetAffinityThread`
- [ ] `KeSetBasePriorityThread`
- [ ] `KeSetDisableBoostThread`
- [ ] `KeSetEvent`
- [ ] `KeTlsAlloc`
- [ ] `KeTlsFree`
- [ ] `KeTlsGetValue`
- [ ] `KeTlsSetValue`
- [ ] `KeUnlockL2`
- [ ] `KeWaitForMultipleObjects`
- [ ] `KeWaitForSingleObject`
- [ ] `KfAcquireSpinLock`
- [ ] `KfReleaseSpinLock`
- [ ] `KiApcNormalRoutineNop`

## Kernel: files, memory, events (25)

- [ ] `NtAllocateVirtualMemory`
- [ ] `NtClearEvent`
- [ ] `NtClose`
- [ ] `NtCreateEvent`
- [ ] `NtCreateFile`
- [ ] `NtCreateSemaphore`
- [ ] `NtDuplicateObject`
- [ ] `NtFlushBuffersFile`
- [ ] `NtFreeVirtualMemory`
- [ ] `NtOpenFile`
- [ ] `NtQueryDirectoryFile`
- [ ] `NtQueryFullAttributesFile`
- [ ] `NtQueryInformationFile`
- [ ] `NtQueryVirtualMemory`
- [ ] `NtQueryVolumeInformationFile`
- [ ] `NtReadFile`
- [ ] `NtReadFileScatter`
- [ ] `NtReleaseSemaphore`
- [ ] `NtResumeThread`
- [ ] `NtSetEvent`
- [ ] `NtSetInformationFile`
- [ ] `NtSuspendThread`
- [ ] `NtWaitForSingleObjectEx`
- [ ] `NtWriteFile`
- [ ] `NtYieldExecution`

## Kernel runtime: strings, critical sections, exceptions (21)

- [ ] `RtlCompareMemoryUlong`
- [ ] `RtlCompareStringN`
- [ ] `RtlEnterCriticalSection`
- [ ] `RtlFillMemoryUlong`
- [ ] `RtlFreeAnsiString`
- [ ] `RtlImageXexHeaderField`
- [ ] `RtlInitAnsiString`
- [ ] `RtlInitUnicodeString`
- [ ] `RtlInitializeCriticalSection`
- [ ] `RtlInitializeCriticalSectionAndSpinCount`
- [ ] `RtlLeaveCriticalSection`
- [ ] `RtlMultiByteToUnicodeN`
- [ ] `RtlNtStatusToDosError`
- [ ] `RtlRaiseException`
- [ ] `RtlTimeFieldsToTime`
- [ ] `RtlTimeToTimeFields`
- [ ] `RtlTryEnterCriticalSection`
- [ ] `RtlUnicodeStringToAnsiString`
- [ ] `RtlUnicodeToMultiByteN`
- [ ] `RtlUnwind`
- [ ] `RtlUpcaseUnicodeChar`

## Kernel: memory (6)

- [ ] `MmAllocatePhysicalMemoryEx`
- [ ] `MmFreePhysicalMemory`
- [ ] `MmGetPhysicalAddress`
- [ ] `MmMapIoSpace`
- [ ] `MmQueryAddressProtect`
- [ ] `MmQueryStatistics`

## Kernel: objects (8)

- [ ] `ObCreateSymbolicLink`
- [ ] `ObDeleteSymbolicLink`
- [ ] `ObDereferenceObject`
- [ ] `ObIsTitleObject`
- [ ] `ObLookupThreadByThreadId`
- [ ] `ObOpenObjectByPointer`
- [ ] `ObReferenceObject`
- [ ] `ObReferenceObjectByHandle`

## Kernel: I/O devices (7)

- [ ] `IoCheckShareAccess`
- [ ] `IoCompleteRequest`
- [ ] `IoCreateDevice`
- [ ] `IoDeleteDevice`
- [ ] `IoInvalidDeviceRequest`
- [ ] `IoRemoveShareAccess`
- [ ] `IoSetShareAccess`

## Kernel: pools, threads, config (6)

- [ ] `ExAllocatePoolTypeWithTag`
- [ ] `ExCreateThread`
- [ ] `ExFreePool`
- [ ] `ExGetXConfigSetting`
- [ ] `ExRegisterTitleTerminateNotification`
- [ ] `ExTerminateThread`

## Kernel: modules (3)

- [ ] `XexCheckExecutablePrivilege`
- [ ] `XexGetModuleHandle`
- [ ] `XexGetProcedureAddress`

## Kernel: misc (4)

- [ ] `DbgBreakPoint`
- [ ] `DbgPrint`
- [ ] `HalReturnToFirmware`
- [ ] `sprintf`

## Video (Xenos GPU display/ring buffer) -> Metal (20)

- [ ] `VdCallGraphicsNotificationRoutines`
- [ ] `VdEnableDisableClockGating`
- [ ] `VdEnableRingBufferRPtrWriteBack`
- [ ] `VdGetCurrentDisplayGamma`
- [ ] `VdGetCurrentDisplayInformation`
- [ ] `VdGetSystemCommandBuffer`
- [ ] `VdInitializeEngines`
- [ ] `VdInitializeRingBuffer`
- [ ] `VdInitializeScalerCommandBuffer`
- [ ] `VdIsHSIOTrainingSucceeded`
- [ ] `VdPersistDisplay`
- [ ] `VdQueryVideoFlags`
- [ ] `VdQueryVideoMode`
- [ ] `VdRetrainEDRAM`
- [ ] `VdRetrainEDRAMWorker`
- [ ] `VdSetDisplayMode`
- [ ] `VdSetGraphicsInterruptCallback`
- [ ] `VdSetSystemCommandBufferGpuIdentifierAddress`
- [ ] `VdShutdownEngines`
- [ ] `VdSwap`

## Audio (XAudio render driver, XMA decoder) (7)

- [ ] `XAudioGetSpeakerConfig`
- [ ] `XAudioGetVoiceCategoryVolume`
- [ ] `XAudioRegisterRenderDriverClient`
- [ ] `XAudioSubmitRenderDriverFrame`
- [ ] `XAudioUnregisterRenderDriverClient`
- [ ] `XMACreateContext`
- [ ] `XMAReleaseContext`

## XAM: profiles, sign-in, achievements (9)

- [ ] `XamShowGamerCardUIForXUID`
- [ ] `XamShowSigninUI`
- [ ] `XamUserCreateAchievementEnumerator`
- [ ] `XamUserCreateStatsEnumerator`
- [ ] `XamUserGetName`
- [ ] `XamUserGetSigninInfo`
- [ ] `XamUserGetSigninState`
- [ ] `XamUserGetXUID`
- [ ] `XamUserReadProfileSettings`

## XAM: content/saves (STFS) (8)

- [ ] `StfsControlDevice`
- [ ] `StfsCreateDevice`
- [ ] `XamContentClose`
- [ ] `XamContentCreateEnumerator`
- [ ] `XamContentCreateEx`
- [ ] `XamContentDelete`
- [ ] `XamContentGetDeviceData`
- [ ] `XamShowDeviceSelectorUI`

## XAM: input (4)

- [ ] `XamInputGetCapabilities`
- [ ] `XamInputGetKeystrokeEx`
- [ ] `XamInputGetState`
- [ ] `XamInputSetState`

## XAM: system UI, notifications, loader, misc (26)

- [ ] `XGetAVPack`
- [ ] `XGetGameRegion`
- [ ] `XGetLanguage`
- [ ] `XGetVideoMode`
- [ ] `XMsgInProcessCall`
- [ ] `XMsgStartIORequest`
- [ ] `XMsgStartIORequestEx`
- [ ] `XNotifyGetNext`
- [ ] `XamAlloc`
- [ ] `XamEnableInactivityProcessing`
- [ ] `XamEnumerate`
- [ ] `XamFree`
- [ ] `XamGetExecutionId`
- [ ] `XamGetSystemVersion`
- [ ] `XamLoaderGetLaunchData`
- [ ] `XamLoaderGetLaunchDataSize`
- [ ] `XamLoaderLaunchTitle`
- [ ] `XamLoaderSetLaunchData`
- [ ] `XamLoaderTerminateTitle`
- [ ] `XamNotifyCreateListener`
- [ ] `XamResetInactivity`
- [ ] `XamSessionCreateHandle`
- [ ] `XamSessionRefObjByHandle`
- [ ] `XamShowDirtyDiscErrorUI`
- [ ] `XamShowMessageBoxUI`
- [ ] `XamShowMessageBoxUIEx`

## Crypto (1)

- [ ] `XeCryptSha`

## Network (2)

- [ ] `NetDll_WSACleanup`
- [ ] `NetDll_WSAStartup`
