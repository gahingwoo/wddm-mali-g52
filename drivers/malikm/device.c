// SPDX-License-Identifier: GPL-2.0
/*
 * KMDF plumbing for malikm: binds the firmware's GPU0 (ACPI\RKCP7402),
 * exposes \\.\MaliG52 and hands each request to uapi.c in the caller's
 * own thread. Progress and counters go to the device's registry key:
 *   HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters
 */
#include "malikm.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MkEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE MkEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE MkEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY MkEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT MkEvtD0Exit;
EVT_WDF_INTERRUPT_ISR MkEvtIsr;
EVT_WDF_INTERRUPT_DPC MkEvtDpc;
EVT_WDF_IO_IN_CALLER_CONTEXT MkEvtIoInCallerContext;
EVT_WDF_DEVICE_FILE_CREATE MkEvtFileCreate;
EVT_WDF_FILE_CLEANUP MkEvtFileCleanup;

BOOLEAN MkEvtIsr(WDFINTERRUPT Interrupt, ULONG MessageID)
{
    UNREFERENCED_PARAMETER(MessageID);
    PDEVICE_CONTEXT dev = GetDeviceContext(WdfInterruptGetDevice(Interrupt));
    if (!MkIsr(&dev->Gpu))
        return FALSE;
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID MkEvtDpc(WDFINTERRUPT Interrupt, WDFOBJECT Device)
{
    UNREFERENCED_PARAMETER(Interrupt);
    KeSetEvent(&GetDeviceContext((WDFDEVICE)Device)->JobIrqEvent, 0, FALSE);
}

NTSTATUS MkEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT dev = GetDeviceContext(Device);
    NTSTATUS st;

    for (ULONG i = 0; i < WdfCmResourceListGetCount(Translated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Translated, i);
        if (d->Type == CmResourceTypeMemory && dev->Gpu.Regs == NULL) {
            dev->Gpu.RegsLength = d->u.Memory.Length;
            dev->Gpu.Regs = (volatile ULONG *)MmMapIoSpaceEx(d->u.Memory.Start, d->u.Memory.Length,
                                                        PAGE_READWRITE | PAGE_NOCACHE);
        } else if (d->Type == CmResourceTypeInterrupt && dev->InterruptCount < 3) {
            WDF_INTERRUPT_CONFIG cfg;
            WDF_INTERRUPT_CONFIG_INIT(&cfg, MkEvtIsr, MkEvtDpc);
            cfg.InterruptRaw = WdfCmResourceListGetDescriptor(Raw, i);
            cfg.InterruptTranslated = d;
            st = WdfInterruptCreate(Device, &cfg, WDF_NO_OBJECT_ATTRIBUTES,
                                    &dev->Interrupts[dev->InterruptCount]);
            if (!NT_SUCCESS(st))
                return st;
            dev->InterruptCount++;
        }
    }
    if (dev->Gpu.Regs == NULL)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    MkLog(dev, L"Interrupts", dev->InterruptCount);
    return MkMmuInit(&dev->Gpu);
}

NTSTATUS MkEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT dev = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(Translated);

    /* Every file has been cleaned up by now; free what jobs still held. */
    ExAcquireFastMutex(&dev->Lock);
    MkReapZombies(dev);
    ExReleaseFastMutex(&dev->Lock);
    MkMmuFree(&dev->Gpu);
    if (dev->Gpu.Regs != NULL) {
        MmUnmapIoSpace((PVOID)dev->Gpu.Regs, dev->Gpu.RegsLength);
        dev->Gpu.Regs = NULL;
    }
    return STATUS_SUCCESS;
}

/* Interrupts are connected after this returns and disconnected before D0Exit
 * runs, so the ISR only ever sees a powered GPU. */
NTSTATUS MkEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT dev = GetDeviceContext(Device);
    NTSTATUS st;
    UNREFERENCED_PARAMETER(PreviousState);

    st = MkGpuInit(&dev->Gpu);
    if (!NT_SUCCESS(st)) {
        MkLog(dev, L"InitStatus", (ULONG)st);
        return st;
    }
    return MkWorkerStart(dev);
}

NTSTATUS MkEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT dev = GetDeviceContext(Device);
    UNREFERENCED_PARAMETER(TargetState);

    MkWorkerStop(dev);
    MkGpuStop(&dev->Gpu);
    return STATUS_SUCCESS;
}

VOID MkEvtFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject)
{
    PFILE_CONTEXT f = GetFileContext(FileObject);
    UNREFERENCED_PARAMETER(Device);

    RtlZeroMemory(f, sizeof(*f));
    InitializeListHead(&f->Mappings);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID MkEvtFileCleanup(WDFFILEOBJECT FileObject)
{
    MkFileCleanup(GetDeviceContext(WdfFileObjectGetDevice(FileObject)), GetFileContext(FileObject));
}

/* Every request is handled here, synchronously, in the thread that sent it:
 * user mappings have to be made in the calling process, and waits may block. */
VOID MkEvtIoInCallerContext(WDFDEVICE Device, WDFREQUEST Request)
{
    PDEVICE_CONTEXT dev = GetDeviceContext(Device);
    WDF_REQUEST_PARAMETERS params;
    WDFFILEOBJECT fo = WdfRequestGetFileObject(Request);
    PVOID buf = NULL;
    size_t len = 0, outLen;
    NTSTATUS st = STATUS_SUCCESS;
    ULONG_PTR info = 0;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    if (params.Type != WdfRequestTypeDeviceControl || fo == NULL) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    outLen = params.Parameters.DeviceIoControl.OutputBufferLength;
    /* METHOD_BUFFERED: input and output share one system buffer. */
    st = WdfRequestRetrieveInputBuffer(Request, 1, &buf, &len);
    if (!NT_SUCCESS(st)) {
        WdfRequestComplete(Request, st);
        return;
    }
    PFILE_CONTEXT file = GetFileContext(fo);

    switch (params.Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_MALIKM_DRM: {
        MALIKM_DRM_HEADER *h = (MALIKM_DRM_HEADER *)buf;
        if (len < sizeof(*h) || h->Size > len - sizeof(*h) || outLen < sizeof(*h) + h->Size) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        h->Result = MkDrmIoctl(dev, file, h->Nr, (PUCHAR)(h + 1), len - sizeof(*h));
        info = sizeof(*h) + h->Size;
        break;
    }
    case IOCTL_MALIKM_MAP: {
        MALIKM_DRM_HEADER *h = (MALIKM_DRM_HEADER *)buf;
        if (len < sizeof(*h) + sizeof(MALIKM_MAP) || outLen < len) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        h->Result = MkMap(dev, file, (MALIKM_MAP *)(h + 1));
        info = sizeof(*h) + sizeof(MALIKM_MAP);
        break;
    }
    case IOCTL_MALIKM_UNMAP: {
        MALIKM_DRM_HEADER *h = (MALIKM_DRM_HEADER *)buf;
        if (len < sizeof(*h) + sizeof(MALIKM_UNMAP) || outLen < sizeof(*h)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        h->Result = MkUnmap(dev, file, (PVOID)(ULONG_PTR)((MALIKM_UNMAP *)(h + 1))->Address);
        info = sizeof(*h);
        break;
    }
    case IOCTL_MALIKM_VERSION: {
        MALIKM_VERSION *v = (MALIKM_VERSION *)buf;
        if (len < sizeof(*v) || outLen < sizeof(*v)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        RtlZeroMemory(v, sizeof(*v));
        v->Major = MALIKM_DRM_MAJOR;
        v->Minor = MALIKM_DRM_MINOR;
        RtlCopyMemory(v->Name, "panfrost", 9);
        info = sizeof(*v);
        break;
    }
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    WdfRequestCompleteWithInformation(Request, st, NT_SUCCESS(st) ? info : 0);
}

NTSTATUS MkEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_FILEOBJECT_CONFIG fileCfg;
    WDF_OBJECT_ATTRIBUTES attr, fileAttr;
    DECLARE_CONST_UNICODE_STRING(name, MALIKM_DEVICE_NAME);
    DECLARE_CONST_UNICODE_STRING(link, MALIKM_DOS_NAME);
    /* SYSTEM and administrators: all; everyone else: read and write. The
     * D3D application opening the device is an ordinary user process. */
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)");
    WDFDEVICE device;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = MkEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = MkEvtReleaseHardware;
    pnp.EvtDeviceD0Entry = MkEvtD0Entry;
    pnp.EvtDeviceD0Exit = MkEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_FILEOBJECT_CONFIG_INIT(&fileCfg, MkEvtFileCreate, WDF_NO_EVENT_CALLBACK, MkEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttr, FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileCfg, &fileAttr);
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit, MkEvtIoInCallerContext);

    st = WdfDeviceInitAssignName(DeviceInit, &name);
    if (!NT_SUCCESS(st))
        return st;
    st = WdfDeviceInitAssignSDDLString(DeviceInit, &sddl);
    if (!NT_SUCCESS(st))
        return st;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    st = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(st))
        return st;

    PDEVICE_CONTEXT dev = GetDeviceContext(device);
    dev->Device = device;
    dev->Gpu.Log = MkLogHook;
    dev->Gpu.LogOwner = dev;
    ExInitializeFastMutex(&dev->Lock);
    KeInitializeSpinLock(&dev->QueueLock);
    InitializeListHead(&dev->Queue);
    InitializeListHead(&dev->Zombies);
    KeInitializeEvent(&dev->QueueEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&dev->JobIrqEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&dev->ProgressEvent, NotificationEvent, FALSE);

    return WdfDeviceCreateSymbolicLink(device, &link);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, MkEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
