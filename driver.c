#include "internal.h"

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status = STATUS_SUCCESS;

    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, vSeriousEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDriverCreate failed 0x%x", status);
    }

    return status;
}

NTSTATUS
vSeriousEvtDeviceAdd(
    _In_  WDFDRIVER         Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS                status;
    PCONTROLLER_CONTEXT     controllerContext;

    status = ControllerCreate(Driver, DeviceInit, &controllerContext);

    return status;
}

VOID
vSeriousEvtDeviceCleanup(
    _In_  WDFOBJECT         Object
)
{
    WDFDEVICE               device = (WDFDEVICE)Object;
    PCONTROLLER_CONTEXT     controllerContext = GetControllerContext(device);

    if (controllerContext->COMDevice != NULL) {
        DeviceUnplug(controllerContext);
    }

    return;
}