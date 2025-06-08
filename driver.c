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
    PDEVICE_CONTEXT         deviceContext;

    status = DeviceCreate(Driver,
        DeviceInit,
        &deviceContext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = DeviceConfigure(deviceContext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return status;
}
