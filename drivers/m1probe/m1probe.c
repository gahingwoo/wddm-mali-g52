// SPDX-License-Identifier: GPL-2.0
/*
 * m1probe: milestone M1 on Windows. Binds the firmware's GPU0 (ACPI\RKCP7402)
 * and makes the Mali-G52 run one WRITE_VALUE job, the same sequence as
 * tools/m1/m1_raw.c. The firmware has already set the GPU clock and powered
 * PD_GPU; this driver checks that before touching a GPU register, because a
 * GPU access while the domain's bus is idle is an SError.
 *
 * Results go to the device's "Device Parameters" registry key:
 *   HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters
 * Result = 1 means the GPU wrote the expected value.
 */
#include <ntddk.h>
#include <wdf.h>

#define PMU_PA            0x27380000ULL
#define PMU_ACK0          0x120
#define PMU_STATUS0       0x230

#define GPU_ID            0x000
#define GPU_INT_RAWSTAT   0x020
#define GPU_INT_CLEAR     0x024
#define GPU_CMD           0x030
#define GPU_CMD_SOFT_RESET 1
#define GPU_IRQ_RESET_COMPLETED 0x100
#define SHADER_PRESENT    0x100
#define SHADER_READY      0x140
#define TILER_READY       0x150
#define L2_READY          0x160
#define SHADER_PWRON      0x180
#define TILER_PWRON       0x190
#define L2_PWRON          0x1a0
#define JOB_INT_RAWSTAT   0x1000
#define JOB_INT_CLEAR     0x1004
#define JS0               0x1800
#define JS_STATUS         0x24
#define JS_HEAD_NEXT_LO   0x40
#define JS_HEAD_NEXT_HI   0x44
#define JS_AFFINITY_NEXT_LO 0x50
#define JS_AFFINITY_NEXT_HI 0x54
#define JS_CONFIG_NEXT    0x58
#define JS_COMMAND_NEXT   0x60
#define MMU_INT_RAWSTAT   0x2000
#define MMU_INT_CLEAR     0x2004
#define AS0               0x2400
#define AS_TRANSTAB_LO    0x00
#define AS_TRANSTAB_HI    0x04
#define AS_MEMATTR_LO     0x08
#define AS_MEMATTR_HI     0x0c
#define AS_COMMAND        0x18
#define AS_FAULTSTATUS    0x1c
#define AS_STATUS         0x28
#define AS_TRANSCFG_LO    0x30
#define AS_TRANSCFG_HI    0x34
#define AS_CMD_UPDATE     1

/* AS 0, thread priority 8, clean+invalidate caches at start and end. */
#define JS_CONFIG         ((8u << 16) | (3u << 8) | (3u << 12))

#define GPU_VA            0x2000000ULL
#define TARGET_OFFSET     0x100
#define MAGIC             0xC0FFEE42u
#define NPAGES            5

typedef struct _DEVICE_CONTEXT {
    volatile ULONG *Gpu;
    SIZE_T GpuLength;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD M1EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE M1EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE M1EvtReleaseHardware;

#define RD(b, o)    READ_REGISTER_ULONG((PULONG)((PUCHAR)(b) + (o)))
#define WR(b, o, v) WRITE_REGISTER_ULONG((PULONG)((PUCHAR)(b) + (o)), (v))

static void Log(WDFDEVICE Device, PCWSTR Name, ULONG Value)
{
    WDFKEY key;
    UNICODE_STRING name;

    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "m1probe: %ws = 0x%08x\n", Name, Value);
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&name, Name);
        (void)WdfRegistryAssignULong(key, &name, Value);
        WdfRegistryClose(key);
    }
}

/* Poll (reg & mask) == want for up to 100 ms. */
static BOOLEAN Poll(volatile ULONG *Base, ULONG Off, ULONG Mask, ULONG Want)
{
    for (ULONG i = 0; i < 10000; i++) {
        if ((RD(Base, Off) & Mask) == Want)
            return TRUE;
        KeStallExecutionProcessor(10);
    }
    return FALSE;
}

/* Step reached, so a failure can be located from the registry alone. */
enum { STEP_PMU = 1, STEP_ID, STEP_RESET, STEP_POWER, STEP_ALLOC, STEP_AS, STEP_JOB, STEP_DONE };

static void RunM1(WDFDEVICE Device, volatile ULONG *Gpu)
{
    PHYSICAL_ADDRESS pa, low, high, skip;
    volatile ULONG *pmu;
    ULONG v;

    Log(Device, L"Result", 0);

    /* 1. The firmware must have powered PD_GPU and released bus idle. */
    Log(Device, L"Step", STEP_PMU);
    pa.QuadPart = PMU_PA;
    pmu = (volatile ULONG *)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    if (pmu == NULL)
        return;
    Log(Device, L"PmuStatus0", RD(pmu, PMU_STATUS0));
    Log(Device, L"PmuAck0", RD(pmu, PMU_ACK0));
    v = ((RD(pmu, PMU_STATUS0) >> 25) & 1) | (RD(pmu, PMU_ACK0) & 1);
    MmUnmapIoSpace((PVOID)pmu, 0x1000);
    if (v != 0)
        return;   /* domain off or bus idle: touching the GPU would SError */

    /* 2. Identify. */
    Log(Device, L"Step", STEP_ID);
    Log(Device, L"GpuId", RD(Gpu, GPU_ID));
    if ((RD(Gpu, GPU_ID) >> 16) != 0x7402)
        return;

    /* 3. Soft reset. */
    Log(Device, L"Step", STEP_RESET);
    WR(Gpu, GPU_INT_CLEAR, 0xffffffff);
    WR(Gpu, GPU_CMD, GPU_CMD_SOFT_RESET);
    if (!Poll(Gpu, GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED))
        return;
    WR(Gpu, GPU_INT_CLEAR, 0xffffffff);

    /* 4. L2, tiler, shader cores. */
    Log(Device, L"Step", STEP_POWER);
    ULONG cores = RD(Gpu, SHADER_PRESENT);
    WR(Gpu, L2_PWRON, 1);
    if (!Poll(Gpu, L2_READY, 1, 1))
        return;
    WR(Gpu, TILER_PWRON, 1);
    WR(Gpu, SHADER_PWRON, cores);
    if (!Poll(Gpu, TILER_READY, 1, 1) || !Poll(Gpu, SHADER_READY, cores, cores))
        return;
    Log(Device, L"ShaderReady", RD(Gpu, SHADER_READY));

    /* 5. Page tables and the job in uncached, physically contiguous memory,
     * so the CPU side needs no cache maintenance. Page 0..3: L0..L3, 4: data. */
    Log(Device, L"Step", STEP_ALLOC);
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFFFULL;   /* 40-bit physical addresses */
    skip.QuadPart = 0;
    PUCHAR mem = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(NPAGES * PAGE_SIZE, low, high, skip, MmNonCached);
    if (mem == NULL)
        return;
    RtlZeroMemory(mem, NPAGES * PAGE_SIZE);
    ULONG64 base = MmGetPhysicalAddress(mem).QuadPart;
    Log(Device, L"MemPhysLo", (ULONG)base);
    Log(Device, L"MemPhysHi", (ULONG)(base >> 32));

    volatile ULONG64 *l0 = (volatile ULONG64 *)(mem + 0 * PAGE_SIZE);
    volatile ULONG64 *l1 = (volatile ULONG64 *)(mem + 1 * PAGE_SIZE);
    volatile ULONG64 *l2 = (volatile ULONG64 *)(mem + 2 * PAGE_SIZE);
    volatile ULONG64 *l3 = (volatile ULONG64 *)(mem + 3 * PAGE_SIZE);
    volatile ULONG *data = (volatile ULONG *)(mem + 4 * PAGE_SIZE);

    /* Mali LPAE: tables type 3; leaves type 1 even at L3, no AF,
     * read 0x40 | write 0x80 | ATTRINDX 1 (0x8D) 0x4 | outer shareable 0x200. */
    l0[(GPU_VA >> 39) & 0x1ff] = (base + 1 * PAGE_SIZE) | 3;
    l1[(GPU_VA >> 30) & 0x1ff] = (base + 2 * PAGE_SIZE) | 3;
    l2[(GPU_VA >> 21) & 0x1ff] = (base + 3 * PAGE_SIZE) | 3;
    l3[(GPU_VA >> 12) & 0x1ff] = (base + 4 * PAGE_SIZE) | 0x2C5;

    ULONG64 target = GPU_VA + TARGET_OFFSET;
    data[4] = 1u | (2u << 1) | (1u << 16);   /* is_64b, WRITE_VALUE, index 1 */
    data[8] = (ULONG)target;
    data[9] = (ULONG)(target >> 32);
    data[10] = 6;                            /* immediate 32 */
    data[12] = MAGIC;
    KeMemoryBarrier();

    /* 6. Address space 0. */
    Log(Device, L"Step", STEP_AS);
    WR(Gpu, MMU_INT_CLEAR, 0xffffffff);
    WR(Gpu, AS0 + AS_TRANSTAB_LO, (ULONG)(base | 0x7));
    WR(Gpu, AS0 + AS_TRANSTAB_HI, (ULONG)(base >> 32));
    WR(Gpu, AS0 + AS_MEMATTR_LO, 0x00888d88);
    WR(Gpu, AS0 + AS_MEMATTR_HI, 0);
    WR(Gpu, AS0 + AS_TRANSCFG_LO, 0);
    WR(Gpu, AS0 + AS_TRANSCFG_HI, 0);
    WR(Gpu, AS0 + AS_COMMAND, AS_CMD_UPDATE);
    if (Poll(Gpu, AS0 + AS_STATUS, 1, 0)) {
        /* 7. Job slot 0. */
        Log(Device, L"Step", STEP_JOB);
        WR(Gpu, JOB_INT_CLEAR, 0xffffffff);
        WR(Gpu, JS0 + JS_HEAD_NEXT_LO, (ULONG)GPU_VA);
        WR(Gpu, JS0 + JS_HEAD_NEXT_HI, (ULONG)(GPU_VA >> 32));
        WR(Gpu, JS0 + JS_AFFINITY_NEXT_LO, cores);
        WR(Gpu, JS0 + JS_AFFINITY_NEXT_HI, 0);
        WR(Gpu, JS0 + JS_CONFIG_NEXT, JS_CONFIG);
        WR(Gpu, JS0 + JS_COMMAND_NEXT, 1);   /* START */

        for (ULONG i = 0; i < 50000 && !(RD(Gpu, JOB_INT_RAWSTAT) & 0x10001); i++)
            KeStallExecutionProcessor(10);

        Log(Device, L"JobIntRaw", RD(Gpu, JOB_INT_RAWSTAT));
        Log(Device, L"Js0Status", RD(Gpu, JS0 + JS_STATUS));
        Log(Device, L"MmuIntRaw", RD(Gpu, MMU_INT_RAWSTAT));
        Log(Device, L"As0FaultStatus", RD(Gpu, AS0 + AS_FAULTSTATUS));
        Log(Device, L"JobException", data[0]);
        Log(Device, L"Target", data[TARGET_OFFSET / 4]);
        Log(Device, L"Step", STEP_DONE);
        Log(Device, L"Result", data[TARGET_OFFSET / 4] == MAGIC ? 1 : 2);
    }

    MmFreeContiguousMemorySpecifyCache(mem, NPAGES * PAGE_SIZE, MmNonCached);
}

NTSTATUS M1EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = GetContext(Device);
    UNREFERENCED_PARAMETER(Raw);

    for (ULONG i = 0; i < WdfCmResourceListGetCount(Translated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);
        if (d->Type == CmResourceTypeMemory && ctx->Gpu == NULL) {
            ctx->GpuLength = d->u.Memory.Length;
            ctx->Gpu = (volatile ULONG *)MmMapIoSpaceEx(d->u.Memory.Start, d->u.Memory.Length,
                                                        PAGE_READWRITE | PAGE_NOCACHE);
        }
    }
    if (ctx->Gpu == NULL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    RunM1(Device, ctx->Gpu);
    return STATUS_SUCCESS;
}

NTSTATUS M1EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = GetContext(Device);
    UNREFERENCED_PARAMETER(Translated);

    if (ctx->Gpu != NULL) {
        MmUnmapIoSpace((PVOID)ctx->Gpu, ctx->GpuLength);
        ctx->Gpu = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS M1EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDFDEVICE device;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = M1EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = M1EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    return WdfDeviceCreate(&DeviceInit, &attr, &device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, M1EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
