#include "internal.h"

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, vSeriousEvtDeviceAdd);

    return STATUS_SUCCESS;
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

    DeviceUnplug(controllerContext);

    return;
}