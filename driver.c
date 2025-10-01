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

typedef struct {
    int cpu;
    HANDLE ThreadHandle;
    int Temperature;
    MSR_TEMPERATURE_TARGET_UNION TjMax;
    MSR_THERM_STATUS_UNION ThermStatus;
    ULONG64 Msr808;
    KEVENT ThreadDoneEvent;
} CORE;

CORE Core[64];

VOID OutputToTerminal(const char* str) {
    UNICODE_STRING deviceName;
    OBJECT_ATTRIBUTES objAttrs;
    IO_STATUS_BLOCK ioStatus;
    HANDLE hDevice;

    RtlInitUnicodeString(&deviceName, L"\\DosDevices\\WinMSR_Terminal");
    InitializeObjectAttributes(&objAttrs, &deviceName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    if (NT_SUCCESS(ZwOpenFile(&hDevice, GENERIC_WRITE, &objAttrs, &ioStatus, FILE_SHARE_WRITE, FILE_NON_DIRECTORY_FILE))) {
        ZwWriteFile(hDevice, NULL, NULL, NULL, &ioStatus, (PVOID)str, (ULONG)strlen(str), NULL, NULL);
        ZwClose(hDevice);
    }
}

VOID ThreadEntry(IN PVOID Context) {
    CORE* pCore = (CORE*)Context;

    KAFFINITY coreMask = ((KAFFINITY)1 << pCore->cpu);
    KeSetSystemAffinityThreadEx(coreMask);

    pCore->TjMax.Value = __readmsr(MSR_TEMPERATURE_TARGET);
    pCore->ThermStatus.Value = __readmsr(IA32_THERM_STATUS);
    pCore->Msr808 = __readmsr(MSR_CUSTOM_808);

    if (pCore->ThermStatus.Fields.ReadingValid) {
        pCore->Temperature = pCore->TjMax.Fields.Target - pCore->ThermStatus.Fields.DTS;
    }
    else {
        pCore->Temperature = -1;
    }

    char buffer[128] = { 0 };
    if (pCore->Temperature >= 0) {
        RtlStringCbPrintfA(buffer, sizeof(buffer),
            "Core(%02d): T%dC MSR808: %016llx\n",
            pCore->cpu,
            pCore->Temperature,
            pCore->Msr808);
    }
    else {
        RtlStringCbPrintfA(buffer, sizeof(buffer),
            "Core(%02d): Temperature reading invalid. MSR808: %016llx\n",
            pCore->cpu,
            pCore->Msr808);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, buffer);
    OutputToTerminal(buffer);

    KeSetEvent(&pCore->ThreadDoneEvent, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID MyDriverUnload(_In_ WDFDRIVER Driver) {
    UNREFERENCED_PARAMETER(Driver);
    DbgPrint("WinMSRDriver (KMDF) unloaded.\n");
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
) {
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDFDRIVER hDriver;

    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.EvtDriverUnload = MyDriverUnload;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, &hDriver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Print CPU brand string
    int cpuInfo[4];
    CHAR brandString[49] = { 0 };
    for (int i = 0; i < 3; i++) {
        __cpuid(cpuInfo, 0x80000002 + i);
        RtlCopyMemory(brandString + i * 16, cpuInfo, 16);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "CPU: %s\n", brandString);
    OutputToTerminal(brandString);

    // Create one thread per core
    ULONG coreCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    for (ULONG i = 0; i < coreCount; i++) {
        Core[i].cpu = (int)i;
        KeInitializeEvent(&Core[i].ThreadDoneEvent, NotificationEvent, FALSE);

        status = PsCreateSystemThread(
            &Core[i].ThreadHandle,
            THREAD_ALL_ACCESS,
            NULL,
            NULL,
            NULL,
            ThreadEntry,
            &Core[i]
        );

        if (!NT_SUCCESS(status)) {
            DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
                "Failed to create thread for core %lu: 0x%X\n", i, status);
        }
        else {
            ZwClose(Core[i].ThreadHandle);
        }
    }

    // Wait for threads to complete
    for (ULONG i = 0; i < coreCount; i++) {
        KeWaitForSingleObject(&Core[i].ThreadDoneEvent, Executive, KernelMode, FALSE, NULL);
    }

    return STATUS_SUCCESS;
}
