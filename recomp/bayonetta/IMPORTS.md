# Bayonetta: Xbox 360 imports to implement

The 187 system functions `default.xex` imports, i.e. the runtime's to-do list. Generated from the
recompiler output (`private/imports.txt`); tick them off as they are implemented.

## Kernel: threads, sync, time (28)

- [x] `KeAcquireSpinLockAtRaisedIrql`
- [x] `KeBugCheck`
- [x] `KeBugCheckEx`
- [x] `KeDelayExecutionThread`
- [x] `KeEnableFpuExceptions`
- [x] `KeEnterCriticalRegion`
- [x] `KeGetCurrentProcessType`
- [x] `KeLeaveCriticalRegion`
- [x] `KeLockL2`
- [x] `KeQueryBasePriorityThread`
- [x] `KeQueryPerformanceFrequency`
- [x] `KeQuerySystemTime`
- [x] `KeReleaseSpinLockFromRaisedIrql`
- [x] `KeResetEvent`
- [x] `KeSetAffinityThread`
- [x] `KeSetBasePriorityThread`
- [x] `KeSetDisableBoostThread`
- [x] `KeSetEvent`
- [x] `KeTlsAlloc`
- [x] `KeTlsFree`
- [x] `KeTlsGetValue`
- [x] `KeTlsSetValue`
- [x] `KeUnlockL2`
- [x] `KeWaitForMultipleObjects`
- [x] `KeWaitForSingleObject`
- [x] `KfAcquireSpinLock`
- [x] `KfReleaseSpinLock`
- [x] `KiApcNormalRoutineNop`

## Kernel: files, memory, events (25)

- [x] `NtAllocateVirtualMemory`
- [x] `NtClearEvent`
- [x] `NtClose`
- [x] `NtCreateEvent`
- [x] `NtCreateFile`
- [x] `NtCreateSemaphore`
- [x] `NtDuplicateObject`
- [x] `NtFlushBuffersFile`
- [x] `NtFreeVirtualMemory`
- [x] `NtOpenFile`
- [x] `NtQueryDirectoryFile`
- [x] `NtQueryFullAttributesFile`
- [x] `NtQueryInformationFile`
- [x] `NtQueryVirtualMemory`
- [x] `NtQueryVolumeInformationFile`
- [x] `NtReadFile`
- [x] `NtReadFileScatter`
- [x] `NtReleaseSemaphore`
- [x] `NtResumeThread`
- [x] `NtSetEvent`
- [x] `NtSetInformationFile`
- [x] `NtSuspendThread`
- [x] `NtWaitForSingleObjectEx`
- [x] `NtWriteFile`
- [x] `NtYieldExecution`

## Kernel runtime: strings, critical sections, exceptions (21)

- [x] `RtlCompareMemoryUlong`
- [x] `RtlCompareStringN`
- [x] `RtlEnterCriticalSection`
- [x] `RtlFillMemoryUlong`
- [x] `RtlFreeAnsiString`
- [x] `RtlImageXexHeaderField`
- [x] `RtlInitAnsiString`
- [x] `RtlInitUnicodeString`
- [x] `RtlInitializeCriticalSection`
- [x] `RtlInitializeCriticalSectionAndSpinCount`
- [x] `RtlLeaveCriticalSection`
- [x] `RtlMultiByteToUnicodeN`
- [x] `RtlNtStatusToDosError`
- [x] `RtlRaiseException`
- [x] `RtlTimeFieldsToTime`
- [x] `RtlTimeToTimeFields`
- [x] `RtlTryEnterCriticalSection`
- [x] `RtlUnicodeStringToAnsiString`
- [x] `RtlUnicodeToMultiByteN`
- [x] `RtlUnwind`
- [x] `RtlUpcaseUnicodeChar`

## Kernel: memory (6)

- [x] `MmAllocatePhysicalMemoryEx`
- [x] `MmFreePhysicalMemory`
- [x] `MmGetPhysicalAddress`
- [x] `MmMapIoSpace`
- [x] `MmQueryAddressProtect`
- [x] `MmQueryStatistics`

## Kernel: objects (8)

- [x] `ObCreateSymbolicLink`
- [x] `ObDeleteSymbolicLink`
- [x] `ObDereferenceObject`
- [x] `ObIsTitleObject`
- [x] `ObLookupThreadByThreadId`
- [x] `ObOpenObjectByPointer`
- [x] `ObReferenceObject`
- [x] `ObReferenceObjectByHandle`

## Kernel: I/O devices (7)

- [ ] `IoCheckShareAccess`
- [ ] `IoCompleteRequest`
- [ ] `IoCreateDevice`
- [ ] `IoDeleteDevice`
- [ ] `IoInvalidDeviceRequest`
- [ ] `IoRemoveShareAccess`
- [ ] `IoSetShareAccess`

## Kernel: pools, threads, config (6)

- [x] `ExAllocatePoolTypeWithTag`
- [x] `ExCreateThread`
- [x] `ExFreePool`
- [x] `ExGetXConfigSetting`
- [x] `ExRegisterTitleTerminateNotification`
- [ ] `ExTerminateThread`

## Kernel: modules (3)

- [x] `XexCheckExecutablePrivilege`
- [x] `XexGetModuleHandle`
- [x] `XexGetProcedureAddress`

## Kernel: misc (6)

- [x] `DbgBreakPoint`
- [x] `DbgPrint`
- [x] `HalReturnToFirmware`
- [x] `sprintf`
- [x] `_snprintf`
- [x] `_vsnprintf`

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
- [x] `VdPersistDisplay`
- [ ] `VdQueryVideoFlags`
- [x] `VdQueryVideoMode`
- [x] `VdRetrainEDRAM`
- [ ] `VdRetrainEDRAMWorker`
- [ ] `VdSetDisplayMode`
- [ ] `VdSetGraphicsInterruptCallback`
- [ ] `VdSetSystemCommandBufferGpuIdentifierAddress`
- [ ] `VdShutdownEngines`
- [ ] `VdSwap`

## Audio (XAudio render driver, XMA decoder) (7)

- [x] `XAudioGetSpeakerConfig`
- [ ] `XAudioGetVoiceCategoryVolume`
- [x] `XAudioRegisterRenderDriverClient`
- [x] `XAudioSubmitRenderDriverFrame`
- [x] `XAudioUnregisterRenderDriverClient`
- [ ] `XMACreateContext`
- [ ] `XMAReleaseContext`

## XAM: profiles, sign-in, achievements (9)

- [x] `XamShowGamerCardUIForXUID`
- [ ] `XamShowSigninUI`
- [x] `XamUserCreateAchievementEnumerator`
- [x] `XamUserCreateStatsEnumerator`
- [x] `XamUserGetName`
- [x] `XamUserGetSigninInfo`
- [x] `XamUserGetSigninState`
- [x] `XamUserGetXUID`
- [x] `XamUserReadProfileSettings`

## XAM: content/saves (STFS) (8)

- [ ] `StfsControlDevice`
- [ ] `StfsCreateDevice`
- [x] `XamContentClose`
- [x] `XamContentCreateEnumerator`
- [x] `XamContentCreateEx`
- [ ] `XamContentDelete`
- [x] `XamContentGetDeviceData`
- [x] `XamShowDeviceSelectorUI`

## XAM: input (4)

- [x] `XamInputGetCapabilities`
- [x] `XamInputGetKeystrokeEx`
- [x] `XamInputGetState`
- [x] `XamInputSetState`

## XAM: system UI, notifications, loader, misc (26)

- [x] `XGetAVPack`
- [x] `XGetGameRegion`
- [x] `XGetLanguage`
- [x] `XGetVideoMode`
- [x] `XMsgInProcessCall`
- [x] `XMsgStartIORequest`
- [ ] `XMsgStartIORequestEx`
- [x] `XNotifyGetNext`
- [x] `XamAlloc`
- [ ] `XamEnableInactivityProcessing`
- [x] `XamEnumerate`
- [x] `XamFree`
- [ ] `XamGetExecutionId`
- [x] `XamGetSystemVersion`
- [x] `XamLoaderGetLaunchData`
- [x] `XamLoaderGetLaunchDataSize`
- [ ] `XamLoaderLaunchTitle`
- [x] `XamLoaderSetLaunchData`
- [ ] `XamLoaderTerminateTitle`
- [x] `XamNotifyCreateListener`
- [ ] `XamResetInactivity`
- [x] `XamSessionCreateHandle`
- [x] `XamSessionRefObjByHandle`
- [ ] `XamShowDirtyDiscErrorUI`
- [x] `XamShowMessageBoxUI`
- [ ] `XamShowMessageBoxUIEx`

## Crypto (1)

- [ ] `XeCryptSha`

## Network (2)

- [ ] `NetDll_WSACleanup`
- [ ] `NetDll_WSAStartup`
