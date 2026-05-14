#include "internal.h"

BOOLEAN g_CompatMode = FALSE;

static VOID
ReadCompatMode(_In_ PUNICODE_STRING ServiceKeyPath)
{
    NTSTATUS status;
    HANDLE key = NULL;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING valueName;
    WCHAR pathBuf[256];
    UNICODE_STRING fullPath;
    UCHAR infoBuf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)infoBuf;
    ULONG retLen = 0;

    fullPath.Buffer = pathBuf;
    fullPath.Length = 0;
    fullPath.MaximumLength = sizeof(pathBuf);
    if (!NT_SUCCESS(RtlAppendUnicodeStringToString(&fullPath, ServiceKeyPath))) return;
    if (!NT_SUCCESS(RtlAppendUnicodeToString(&fullPath, L"\\Parameters"))) return;

    InitializeObjectAttributes(&oa, &fullPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenKey(&key, KEY_READ, &oa);
    if (!NT_SUCCESS(status)) return;

    RtlInitUnicodeString(&valueName, L"CompatMode");
    status = ZwQueryValueKey(key, &valueName, KeyValuePartialInformation,
        info, sizeof(infoBuf), &retLen);
    if (NT_SUCCESS(status) && info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
        g_CompatMode = (*(PULONG)info->Data) != 0;
    }
    ZwClose(key);
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status = STATUS_SUCCESS;

    ReadCompatMode(RegistryPath);
    Trace(TRACE_LEVEL_INFO, "vSerious CompatMode = %u", g_CompatMode);

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
    PCONTROLLER_CONTEXT controllerContext;

    return ControllerCreate(Driver, DeviceInit, &controllerContext);
}
