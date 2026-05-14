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
    DECLSPEC_ALIGN(8) UCHAR infoBuf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)infoBuf;
    ULONG retLen = 0;

    Trace(TRACE_LEVEL_ERROR,
        "vSerious: ReadCompatMode entry, ServiceKeyPath=%wZ", ServiceKeyPath);

    fullPath.Buffer = pathBuf;
    fullPath.Length = 0;
    fullPath.MaximumLength = sizeof(pathBuf);
    status = RtlAppendUnicodeStringToString(&fullPath, ServiceKeyPath);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "vSerious: append ServiceKeyPath failed 0x%x", status);
        return;
    }
    status = RtlAppendUnicodeToString(&fullPath, L"\\Parameters");
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "vSerious: append \\Parameters failed 0x%x", status);
        return;
    }

    Trace(TRACE_LEVEL_ERROR, "vSerious: opening %wZ", &fullPath);

    InitializeObjectAttributes(&oa, &fullPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenKey(&key, KEY_QUERY_VALUE, &oa);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "vSerious: ZwOpenKey(%wZ) failed 0x%x", &fullPath, status);
        return;
    }

    RtlInitUnicodeString(&valueName, L"CompatMode");
    status = ZwQueryValueKey(key, &valueName, KeyValuePartialInformation,
        info, sizeof(infoBuf), &retLen);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "vSerious: ZwQueryValueKey(CompatMode) failed 0x%x retLen=%u", status, retLen);
    }
    else if (info->Type != REG_DWORD) {
        Trace(TRACE_LEVEL_ERROR, "vSerious: CompatMode wrong type %u (expected REG_DWORD=%u)",
            info->Type, REG_DWORD);
    }
    else if (info->DataLength != sizeof(ULONG)) {
        Trace(TRACE_LEVEL_ERROR, "vSerious: CompatMode wrong size %u", info->DataLength);
    }
    else {
        ULONG value = *(PULONG)info->Data;
        g_CompatMode = (value != 0);
        Trace(TRACE_LEVEL_ERROR, "vSerious: CompatMode value=%u -> g_CompatMode=%u",
            value, g_CompatMode);
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
