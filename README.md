---

## 🧠 HIGH-LEVEL OVERVIEW

This is a **KMDF driver** that:

* Runs once on load (`DriverEntry`)
* Reads **thermal MSRs** (including TjMax and thermal status) for each **logical processor**
* Spawns one thread per core, pins the thread to that core, reads the MSRs
* Computes **core temperature** = `TjMax - DTS`
* Logs the MSR values and temperature using `DbgPrintEx`
* Cleans up memory and handles on unload

---

## 📦 INCLUDED HEADERS

```c
#include <ntddk.h>
#include <wdf.h>
#include <intrin.h>
#include <ntstrsafe.h>
```

### ✅ Purpose:

* `ntddk.h` – Core NT driver definitions (e.g., `__readmsr`, `DbgPrintEx`, kernel types)
* `wdf.h` – KMDF driver functions/macros
* `intrin.h` – CPU intrinsics (`__readmsr`, `__cpuid`)
* `ntstrsafe.h` – Safe string formatting (`RtlStringCbPrintfA`)

---

## 🏷️ DEFINES: MSR Addresses

```c
#define IA32_THERM_STATUS       0x19C
#define MSR_TEMPERATURE_TARGET  0x1A2
#define MSR_CUSTOM_808          0x808
```

### 🔍 Purpose:

* `IA32_THERM_STATUS` (0x19C): MSR reporting thermal state (DTS value, PROCHOT, CriticalTemp)
* `MSR_TEMPERATURE_TARGET` (0x1A2): Contains `TjMax`, the max safe temp (in °C)
* `MSR_CUSTOM_808` (0x808): A custom/experimental MSR – can be vendor-defined or reserved (read-only)

---

## 🧱 STRUCTURES: MSR Bitfields

### 1️⃣ TjMax MSR (0x1A2)

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

### 🔍 Purpose:

* `Target` contains **TjMax** (e.g., 100°C). Temperature is computed as:

  ```
  Temp = TjMax - DTS
  ```

---

### 2️⃣ Therm Status MSR (0x19C)

```c
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
```

### 🔍 Purpose:

* Gives real-time thermal data:

  * `DTS`: Delta to TjMax (difference between current temp and TjMax)
  * `ReadingValid`: Set if temperature reading is valid
  * Status bits: PROCHOT, CriticalTemp, Thresholds, etc.

---

## 🧵 PER-CORE THREAD STRUCTURE

```c
typedef struct _CORE {
    int CpuIndex;
    HANDLE ThreadHandle;
    KEVENT ThreadDoneEvent;
    int Temperature;
    MSR_TEMPERATURE_TARGET_UNION TjMax;
    MSR_THERM_STATUS_UNION ThermStatus;
    ULONG64 Msr808;
} CORE, * PCORE;
```

### 🔍 Purpose:

* `CpuIndex`: Logical processor index
* `ThreadHandle`: Handle to the system thread for this core
* `ThreadDoneEvent`: Used to wait for thread completion
* `Temperature`: Final temperature computed
* `TjMax`, `ThermStatus`, `Msr808`: Raw MSR readings

---

## 🔧 GLOBALS

```c
PCORE CoreArray = NULL;
ULONG CoreCount = 0;
```

* Array of `CORE` structs — one per logical CPU.
* `CoreCount`: Total number of logical processors.

---

## 🧵 THREAD FUNCTION: `ThreadEntry`

```c
VOID ThreadEntry(IN PVOID Context)
```

### 🔍 Purpose:

This runs per CPU thread:

1. Cast `Context` to `PCORE`

2. Pins thread to specific CPU using:

   ```c
   KeSetSystemAffinityThreadEx(1 << CpuIndex);
   ```

3. Reads 3 MSRs:

   * `MSR_TEMPERATURE_TARGET`
   * `IA32_THERM_STATUS`
   * `MSR_CUSTOM_808`

4. If reading is valid (`ReadingValid`):

   ```c
   Temperature = TjMax - DTS
   ```

   Else:

   ```c
   Temperature = -1
   ```

5. Logs all info using `DbgPrintEx` and `RtlStringCbPrintfA`

6. Signals `ThreadDoneEvent`

7. Terminates the thread with `PsTerminateSystemThread`

---

## 🔄 CLEANUP: `MyDriverUnload`

```c
VOID MyDriverUnload(_In_ WDFDRIVER Driver)
```

### 🔍 Purpose:

* On driver unload:

  * Waits for all threads to finish
  * Closes their handles
  * Frees memory (`ExFreePoolWithTag`)
  * Logs unload message

---

## 🚀 DRIVER ENTRY: `DriverEntry`

```c
NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
```

This is the **starting point** when the driver loads.

---

### 🪛 Step-by-Step Breakdown:

#### ✅ 1. Initialize KMDF driver config

```c
WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
config.EvtDriverUnload = MyDriverUnload;
```

* Sets up the unload callback (`MyDriverUnload`)
* No device events (`WDF_NO_EVENT_CALLBACK`)

#### ✅ 2. Create WDF driver

```c
WdfDriverCreate(...)
```

* Registers the driver with WDF
* If it fails, logs and exits

---

#### ✅ 3. Get CPU Brand String

```c
for (int i = 0; i < 3; i++) {
    __cpuid(cpuInfo, 0x80000002 + i);
    RtlCopyMemory(brandString + i * 16, cpuInfo, 16);
}
```

* Reads the **processor brand name** (3 x 16-byte blocks = 48 bytes)
* Logs brand to debugger

---

#### ✅ 4. Get number of active processors

```c
CoreCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
```

* Gets logical processor count across all groups (SMT cores included)

---

#### ✅ 5. Allocate array of `CORE`

```c
CoreArray = (PCORE)ExAllocatePoolWithTag(...);
```

* Allocates per-core structures from non-paged pool (required in kernel mode)

---

#### ✅ 6. Create per-core threads

```c
for (ULONG i = 0; i < CoreCount; i++) {
    KeInitializeEvent(...);
    PsCreateSystemThread(...);
}
```

* Initializes event per core
* Creates a system thread that runs `ThreadEntry` for that core

---

#### ✅ 7. Wait for all threads

```c
KeWaitForSingleObject(...);
ZwClose(...);
```

* Waits until each thread signals completion
* Closes thread handle

---

#### ✅ 8. Final log

```c
DbgPrintEx(..., "All core temperature readings completed.");
```

---

## 🧪 DEBUG LOGGING

All logs use:

```c
DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "...");
```

* Only visible in tools like **WinDbg** or **DebugView**
* Will show:

  ```
  CPU Brand: Intel(R) Core(TM) i9-13900K CPU @ 3.00GHz
  Core(00): Temp=55°C, MSR808=...
  ...
  WinMSRDriver: All core temperature readings completed.
  ```

---

## 🧯 THREAD SAFETY & PERFORMANCE

* Threads are **affinity-bound** and isolated
* Uses events to safely track thread completions
* Does not share writable state between threads → no need for locks
* Runs once on driver load → minimal resource impact

---

## 📦 BUILD REQUIREMENTS

To compile this:

### ✔ Requires:

* **Visual Studio with WDK**
* Set project to:

  * Configuration Type: **Driver**
  * Target Platform: **x64**
  * Platform Toolset: `WindowsKernelModeDriver10.0`
* Add `.inf` file if loading
