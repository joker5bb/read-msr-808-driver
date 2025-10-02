#include <ntddk.h>
#include <wdf.h>
#include <intrin.h>
#include <ntstrsafe.h>

#define IA32_THERM_STATUS       0x19C
#define MSR_TEMPERATURE_TARGET  0x1A2
#define MSR_CUSTOM_808          0x808

typedef union {
    ULONG64 Value;
    struct {
        ULONG Reserved1 : 16;
        ULONG Target : 8;
        ULONG Reserved2 : 8;
        ULONG Reserved3 : 32;
    } Fields;
} MSR_TEMPERATURE_TARGET_UNION;

typedef union {
    ULONG64 Value;
    struct {
        ULONG StatusBit : 1;
        ULONG StatusLog : 1;
        ULONG PROCHOT : 1;
        ULONG PROCHOTLog : 1;
        ULONG CriticalTemp : 1;
        ULONG CriticalTempLog : 1;
        ULONG Threshold1 : 1;
        ULONG Threshold1Log : 1;
        ULONG Threshold2 : 1;
        ULONG Threshold2Log : 1;
        ULONG PowerLimit : 1;
        ULONG PowerLimitLog : 1;
        ULONG Reserved1 : 4;
        ULONG DTS : 8;
        ULONG Reserved2 : 4;
        ULONG Resolution : 5;
        ULONG ReadingValid : 1;
        ULONG Reserved3 : 32;
    } Fields;
} MSR_THERM_STATUS_UNION;

typedef struct _CORE {
    int CpuIndex;
    HANDLE ThreadHandle;
    KEVENT ThreadDoneEvent;
    int Temperature;
    MSR_TEMPERATURE_TARGET_UNION TjMax;
    MSR_THERM_STATUS_UNION ThermStatus;
    ULONG64 Msr808;
} CORE, *PCORE;

PCORE CoreArray = NULL;
ULONG CoreCount = 0;

// Forward declarations
VOID ThreadEntry(IN PVOID Context);
VOID MyDriverUnload(_In_ WDFDRIVER Driver);

VOID ThreadEntry(IN PVOID Context)
{
    PCORE pCore = (PCORE)Context;
    NTSTATUS status = STATUS_SUCCESS;

    // Set affinity for this thread to specific core
    KAFFINITY oldAffinity = KeSetSystemAffinityThreadEx(((KAFFINITY)1) << pCore->CpuIndex);

    __try {
        // Read MSRs
        pCore->TjMax.Value = __readmsr(MSR_TEMPERATURE_TARGET);
        pCore->ThermStatus.Value = __readmsr(IA32_THERM_STATUS);
        pCore->Msr808 = __readmsr(MSR_CUSTOM_808);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "Core(%d): Exception reading MSRs.\n", pCore->CpuIndex);
        pCore->Temperature = -1;
        KeRevertToUserAffinityThread();
        KeSetEvent(&pCore->ThreadDoneEvent, IO_NO_INCREMENT, FALSE);
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
    }

    KeRevertToUserAffinityThread();

    if (pCore->ThermStatus.Fields.ReadingValid) {
        pCore->Temperature = pCore->TjMax.Fields.Target - pCore->ThermStatus.Fields.DTS;
    }
    else {
        pCore->Temperature = -1;
    }

    char buffer[256] = { 0 };
    if (pCore->Temperature >= 0) {
        RtlStringCbPrintfA(buffer, sizeof(buffer),
            "Core(%02d): Temp=%d°C, MSR808=0x%016llX\n"
            "  ThermStatus: StatusBit=%d, PROCHOT=%d, CriticalTemp=%d, Threshold1=%d, Threshold2=%d, PowerLimit=%d\n"
            "  DTS=%d, Resolution=%d, ReadingValid=%d\n",
            pCore->CpuIndex,
            pCore->Temperature,
            pCore->Msr808,
            pCore->ThermStatus.Fields.StatusBit,
            pCore->ThermStatus.Fields.PROCHOT,
            pCore->ThermStatus.Fields.CriticalTemp,
            pCore->ThermStatus.Fields.Threshold1,
            pCore->ThermStatus.Fields.Threshold2,
            pCore->ThermStatus.Fields.PowerLimit,
            pCore->ThermStatus.Fields.DTS,
            pCore->ThermStatus.Fields.Resolution,
            pCore->ThermStatus.Fields.ReadingValid);
    }
    else {
        RtlStringCbPrintfA(buffer, sizeof(buffer),
            "Core(%02d): Temperature reading invalid, MSR808=0x%016llX\n",
            pCore->CpuIndex,
            pCore->Msr808);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "%s", buffer);

    KeSetEvent(&pCore->ThreadDoneEvent, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID MyDriverUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);

    if (CoreArray != NULL)
    {
        // Wait for any running threads to finish
        for (ULONG i = 0; i < CoreCount; i++)
        {
            if (CoreArray[i].ThreadHandle)
            {
                KeWaitForSingleObject(&CoreArray[i].ThreadDoneEvent, Executive, KernelMode, FALSE, NULL);
                ZwClose(CoreArray[i].ThreadHandle);
                CoreArray[i].ThreadHandle = NULL;
            }
        }
        ExFreePoolWithTag(CoreArray, 'corE');
        CoreArray = NULL;
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "WinMSRDriver (KMDF) unloaded.\n");
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDFDRIVER hDriver;

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.EvtDriverUnload = MyDriverUnload;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &hDriver);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "Failed to create WDF driver: 0x%X\n", status);
        return status;
    }

    // Get CPU brand string (null-terminated)
    int cpuInfo[4];
    CHAR brandString[49] = { 0 };
    for (int i = 0; i < 3; i++) {
        __cpuid(cpuInfo, 0x80000002 + i);
        RtlCopyMemory(brandString + i * 16, cpuInfo, 16);
    }
    brandString[48] = '\0'; // Ensure null-termination

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "CPU Brand: %s\n", brandString);

    // Query active processor count (total logical cores)
    CoreCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (CoreCount == 0) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "No active processors found.\n");
        return STATUS_UNSUCCESSFUL;
    }

    // Allocate array dynamically
    CoreArray = (PCORE)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(CORE) * CoreCount, 'corE');
    if (CoreArray == NULL) {
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "Failed to allocate memory for CoreArray.\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(CoreArray, sizeof(CORE) * CoreCount);

    // Create a system thread per CPU core to read MSRs
    for (ULONG i = 0; i < CoreCount; i++)
    {
        CoreArray[i].CpuIndex = (int)i;
        KeInitializeEvent(&CoreArray[i].ThreadDoneEvent, NotificationEvent, FALSE);

        status = PsCreateSystemThread(
            &CoreArray[i].ThreadHandle,
            THREAD_ALL_ACCESS,
            NULL,
            NULL,
            NULL,
            ThreadEntry,
            &CoreArray[i]);

        if (!NT_SUCCESS(status)) {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                "Failed to create thread for core %lu: 0x%X\n", i, status);
            CoreArray[i].ThreadHandle = NULL;
            KeSetEvent(&CoreArray[i].ThreadDoneEvent, IO_NO_INCREMENT, FALSE);
        }
    }

    // Wait for all threads to finish
    for (ULONG i = 0; i < CoreCount; i++) {
        if (CoreArray[i].ThreadHandle != NULL) {
            KeWaitForSingleObject(&CoreArray[i].ThreadDoneEvent, Executive, KernelMode, FALSE, NULL);
            ZwClose(CoreArray[i].ThreadHandle);
            CoreArray[i].ThreadHandle = NULL;
        }
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "WinMSRDriver: All core temperature readings completed.\n");

    return STATUS_SUCCESS;
}
