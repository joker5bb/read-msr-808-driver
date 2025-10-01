# Header includes

```c
#include <ntddk.h>
#include <wdf.h>
#include <intrin.h>
#include <ntstrsafe.h>
```

* **`ntddk.h`** — core kernel APIs and types (WDM). Provides things like `Ke*`, `Zw*`, `DbgPrint*`.
* **`wdf.h`** — KMDF framework API. Required because this is a KMDF driver and you call `WdfDriverCreate`.
* **`intrin.h`** — compiler intrinsics like `__readmsr` and `__cpuid`.
* **`ntstrsafe.h`** — kernel safe-string APIs (e.g., `RtlStringCbPrintfA`) recommended over unsafe `sprintf`.

# MSR and register definitions

```c
#define IA32_THERM_STATUS       0x19C
#define MSR_TEMPERATURE_TARGET  0x1A2
#define MSR_CUSTOM_808          0x808
```

* Constants for MSR (Model-Specific Register) indexes used with `__readmsr`.
* `IA32_THERM_STATUS` and `MSR_TEMPERATURE_TARGET` are standard on many Intel CPUs for thermal reporting.
* `MSR_CUSTOM_808` is a custom/target-specific MSR (0x808); its meaning depends on CPU/platform.

# Bitfield unions for MSR parsing

```c
typedef union {
    ULONG64 Value;
    struct {
        ULONG Reserved1 : 16;
        ULONG Target : 8;
        ULONG Reserved2 : 8;
        ULONG Reserved3 : 32;
    } Fields;
} MSR_TEMPERATURE_TARGET_UNION;
```

* **Purpose:** Interpret the 64‑bit `MSR_TEMPERATURE_TARGET` value.
* `Value` lets you read the whole 64-bit MSR; `Fields.Target` extracts the `TjMax` (temperature target) byte.
* Named inner struct (`Fields`) avoids anonymous struct warnings / errors.

```c
typedef union {
    ULONG64 Value;
    struct {
        ULONG StatusBit : 1;
        /* ... other 1-bit flags ... */
        ULONG Reserved1 : 4;
        ULONG DTS : 8;
        ULONG Reserved2 : 4;
        ULONG Resolution : 5;
        ULONG ReadingValid : 1;
        ULONG Reserved3 : 32;
    } Fields;
} MSR_THERM_STATUS_UNION;
```

* **Purpose:** Interpret `IA32_THERM_STATUS`.
* `DTS` (Digital Thermal Sensor) is an 8-bit delta from TjMax. `ReadingValid` indicates whether DTS is valid.
* `Resolution` and other flags convey additional status (PROCHOT, CriticalTemp, etc.).

**Caveat:** Bitfield layout can be compiler-dependent on signedness/packing. This pattern is commonly used but verify with your platform and compiler settings.

# CORE structure and global array

```c
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
```

* `CORE` holds per-CPU data used by each worker thread.
* `Core[64]` pre-allocates slots for up to 64 CPUs. (You might want to allocate based on `coreCount` instead.)
* `Kevent` is used to signal when a thread finishes its work so the driver can wait on completion.

# OutputToTerminal()

```c
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
```

* **What it does:** Attempts to open a device/file at `\DosDevices\WinMSR_Terminal` and write the ASCII string to it.
* This is a simple way to send text to a user-mode consumer that created that device path (if anything does).
* Uses `ZwOpenFile` and `ZwWriteFile` — kernel API versions of file I/O.

**Caveats & improvements:**

* `strlen` on kernel-mode pointers is okay for ASCII strings you control, but prefer `RtlStringCbLengthA` / safer methods.
* If the device doesn't exist, the `ZwOpenFile` call fails silently.
* This function runs at normal IRQL (calling Zw* is pageable); ensure it's not called at raised IRQL.
* Consider using `WdfFileObject` / WDF APIs if you integrate with KMDF file objects.

# ThreadEntry (worker thread)

```c
VOID ThreadEntry(IN PVOID Context) {
    CORE* pCore = (CORE*)Context;

    KAFFINITY coreMask = ((KAFFINITY)1 << pCore->cpu);
    KeSetSystemAffinityThreadEx(coreMask);

    pCore->TjMax.Value = __readmsr(MSR_TEMPERATURE_TARGET);
    pCore->ThermStatus.Value = __readmsr(IA32_THERM_STATUS);
    pCore->Msr808 = __readmsr(MSR_CUSTOM_808);

    if (pCore->ThermStatus.Fields.ReadingValid) {
        pCore->Temperature = pCore->TjMax.Fields.Target - pCore->ThermStatus.Fields.DTS;
    } else {
        pCore->Temperature = -1;
    }

    char buffer[128] = { 0 };
    if (pCore->Temperature >= 0) {
        RtlStringCbPrintfA(buffer, sizeof(buffer),
            "Core(%02d): T%dC MSR808: %016llx\n",
            pCore->cpu,
            pCore->Temperature,
            pCore->Msr808);
    } else {
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
```

* **Affinity:** `KeSetSystemAffinityThreadEx(coreMask)` pins the current thread to a particular processor. This is necessary because MSR reads (`__readmsr`) are per-CPU — you must execute the MSR read on the CPU whose MSRs you want to read.
* **MSR reads:** `__readmsr(MSR)` returns a 64-bit value. Reading an unsupported MSR can cause a machine check or fault; be careful.
* **Temperature calc:** If `ReadingValid` is set, it computes `TjMax - DTS` → CPU temperature in °C.
* **Formatting:** Uses `RtlStringCbPrintfA` to safely format into `buffer`.
* **Output:** Logs with `DbgPrintEx` and also calls `OutputToTerminal`.
* **Completion:** Signals the event `ThreadDoneEvent` so the creating thread can wait for completion.
* **Termination:** Calls `PsTerminateSystemThread` to end the system thread cleanly.

**Important safety considerations:**

* **MSR faults:** `__readmsr` may cause an exception on unsupported CPUs or if executed at wrong IRQL. Wrap MSR access in `__try/__except` to catch and handle access violations.
* **IRQL:** This thread runs at PASSIVE_LEVEL typically, but `KeSetSystemAffinityThreadEx` does not change IRQL. Ensure kernel calls used are valid at the current IRQL.
* **Affinity mask size:** For systems with >64 processors (or processor groups), your `(1 << cpu)` approach may not be sufficient; on Windows with processor groups, use `KeSetSystemGroupAffinityThread` and consider processor group APIs.
* **Race conditions:** The `Core` array is global and shared. Current code relies on one thread per element and no concurrent writes to the same element — that's okay, but be explicit about ownership in comments.

# Unload callback

```c
VOID MyDriverUnload(_In_ WDFDRIVER Driver) {
    UNREFERENCED_PARAMETER(Driver);
    DbgPrint("WinMSRDriver (KMDF) unloaded.\n");
}
```

* For KMDF, this is the callback set in `WDF_DRIVER_CONFIG` (see `DriverEntry`). WDF calls it on driver unload.
* **Note:** If you started threads or allocated resources, you must ensure they’re stopped and cleaned up here. As written, the driver spawns threads and waits for them to finish before `DriverEntry` returns, so there may be no long-lived threads; still, implement cleanup defensively.

# DriverEntry (KMDF initialization)

```c
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
```

* **WDF bootstrap:** `WdfDriverCreate` registers the driver object with WDF; WDF provides the real entry point glue (`FxDriverEntry`). You supply `DriverEntry`, which calls `WdfDriverCreate`.
* `WDF_NO_EVENT_CALLBACK` means no device add callback is specified (the code doesn't create device objects).
* `config.EvtDriverUnload` sets the unload routine.

```c
    // Print CPU brand string
    int cpuInfo[4];
    CHAR brandString[49] = { 0 };
    for (int i = 0; i < 3; i++) {
        __cpuid(cpuInfo, 0x80000002 + i);
        RtlCopyMemory(brandString + i * 16, cpuInfo, 16);
    }

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "CPU: %s\n", brandString);
    OutputToTerminal(brandString);
```

* **CPU brand string:** Uses `__cpuid` with extended function range to read the vendor / brand string (0x80000002..0x80000004). This is user-friendly logging.
* `RtlCopyMemory` is used to copy raw cpuid output into `brandString`.

```c
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
```

* **Per-core threads:** `KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS)` returns total logical processors across groups. Creates a system thread per core that runs `ThreadEntry`.
* **Events for completion:** Each `CORE` has a `KEVENT` that the worker signals when done; `DriverEntry` waits for all to complete before returning.
* **Handle closing:** After creating a thread, you close the handle (kernel thread keeps running); that's OK if you don't need to manipulate the thread later.
* **Blocking in DriverEntry:** `DriverEntry` waits synchronously for all worker threads. This means the driver initialization will not finish until the worker threads complete — OK for a small diagnostic action but bad if you need the driver to be long-lived. For a persistent driver you should instead create worker threads or WDF work items that run asynchronously and not block DriverEntry.

# Overall behavior summary

* On driver load, WDF is initialized (`WdfDriverCreate`), CPU brand string is read and logged, then the driver spawns one system thread per logical CPU.
* Each per-CPU thread pins itself to its CPU, reads three MSRs (temperature target, thermal status, and custom MSR 0x808), calculates a temperature if valid, logs and writes a string to `\DosDevices\WinMSR_Terminal`, signals completion, and exits.
* `DriverEntry` waits for all threads to finish, then returns `STATUS_SUCCESS`. The driver remains loaded with no device objects; unload routine is present but minimal.

# Important caveats, potential problems, and suggestions

1. **MSR access may fault** — calling `__readmsr` on an unsupported MSR or in certain contexts can trigger exceptions. Wrap MSR reads in a structured exception handler (`__try / __except`) and handle errors gracefully.
2. **IRQL and pageable code** — ensure functions that call pageable routines run at PASSIVE_LEVEL.
3. **Processor group & >64 CPUs** — building a mask with `(1ULL << pCore->cpu)` only supports CPUs up to the mask width. On systems with >64 logical processors or multiple processor groups, use `KeSetSystemGroupAffinityThread`.
4. **KMDF style:** Although this is a KMDF driver (you call `WdfDriverCreate`), you still use WDM primitives (`PsCreateSystemThread`, `ZwOpenFile`, etc.). Consider using KMDF work items and WDF objects to integrate fully with KMDF lifecycle and cleanup.
5. **Driver lifetime:** Right now the driver does all work inside `DriverEntry` and returns success only after threads finish. If you want a driver that remains loaded and responds to IO, create a device object, implement IOCTLs, and make thread creation on-demand.
6. **Resource cleanup:** Make sure any resources are freed on unload; e.g., if you ever keep thread handles, you should terminate/join them or signal shutdown on unload.
7. **Security & permissions:** Writing to `\DosDevices\WinMSR_Terminal` relies on a user-mode component creating/opening that path. Consider exposing a proper device interface (create a device object with `WdfDeviceCreate`) so user-mode clients open `\\.\YourDeviceName` safely with appropriate access controls.
8. **Testing:** Test only in isolated VMs with snapshots. Use WinDbg and Driver Verifier to detect violations.
9. **Use safe string APIs correctly:** `RtlStringCbPrintfA` returns an `NTSTATUS` you can check. Avoid assuming success.

# Suggested improvements (concrete)

* Wrap MSR reads in `__try / __except`:

  ```c
  __try {
      pCore->Msr808 = __readmsr(MSR_CUSTOM_808);
  } __except(EXCEPTION_EXECUTE_HANDLER) {
      pCore->Msr808 = 0;
      // mark error state
  }
  ```
* Replace `PsCreateSystemThread` with KMDF work items if you want proper WDF-managed lifecycle; otherwise ensure you have cleanup logic in `EvtDriverUnload`.
* Use dynamic allocation for `Core` based on `coreCount` rather than a fixed 64-element array.
* Check and use `RtlStringCbPrintfA` return status to ensure buffer was not truncated.
* Consider adding a driver parameter (registry) to control whether to spawn per-core threads or not.

# Build & deployment notes

* Ensure project uses KMDF and links proper libraries (Visual Studio WDK project templates handle this).
* Drivers must be signed for modern Windows or loaded in test-signing mode.
* Don’t run kernel experiments on production hardware.

---
