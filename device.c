#include "internal.h"
#include <ntstrsafe.h>

NTSTATUS
vSeriousEvtChildListCreateDevice(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    _In_ PWDFDEVICE_INIT ChildInit
)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    PDEVICE_CONTEXT deviceContext;
    PVSERIOUS_PDO_IDENTIFICATION_DESCRIPTION desc;
    PCWSTR comName;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDFKEY key;
    LPGUID guid = (LPGUID)&GUID_DEVINTERFACE_COMPORT;
    DECLARE_UNICODE_STRING_SIZE(symbolicLink, SYMBOLIC_LINK_NAME_LENGTH);
    DECLARE_UNICODE_STRING_SIZE(hardwareId, 64);
    UNICODE_STRING comNameString;

    UNREFERENCED_PARAMETER(ChildList);

    desc = CONTAINING_RECORD(IdentificationDescription,
        VSERIOUS_PDO_IDENTIFICATION_DESCRIPTION,
        Header);
    comName = desc->ComName;

    if (comName[0] == L'\0') {
        return STATUS_INVALID_PARAMETER;
    }

    RtlInitUnicodeString(&comNameString, comName);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    WdfDeviceInitSetPnpPowerEventCallbacks(ChildInit, &pnpCallbacks);

    // Hardware ID: vSerious\COM5
    status = RtlAppendUnicodeToString(&hardwareId, L"vSerious\\");
    if (!NT_SUCCESS(status)) return status;
    status = RtlAppendUnicodeStringToString(&hardwareId, &comNameString);
    if (!NT_SUCCESS(status)) return status;

    status = WdfPdoInitAssignDeviceID(ChildInit, &hardwareId);
    if (!NT_SUCCESS(status)) return status;

    status = WdfPdoInitAssignInstanceID(ChildInit, &comNameString);
    if (!NT_SUCCESS(status)) return status;

    status = WdfPdoInitAddHardwareID(ChildInit, &hardwareId);
    if (!NT_SUCCESS(status)) return status;

    status = WdfPdoInitAddCompatibleID(ChildInit, &hardwareId);
    if (!NT_SUCCESS(status)) return status;

    DECLARE_CONST_UNICODE_STRING(deviceDesc, L"vSerious Virtual COM Port");
    status = WdfPdoInitAddDeviceText(ChildInit, &deviceDesc, &deviceDesc, 0x409);
    if (!NT_SUCCESS(status)) return status;

    WdfPdoInitSetDefaultLocale(ChildInit, 0x409);

    WdfDeviceInitSetDeviceType(ChildInit, FILE_DEVICE_SERIAL_PORT);
    WdfDeviceInitSetExclusive(ChildInit, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    deviceAttributes.EvtCleanupCallback = vSeriousPdoEvtDeviceCleanup;

    status = WdfDeviceCreate(&ChildInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceCreate failed 0x%x", status);
        // Do NOT call WdfDeviceInitFree on ChildInit here — when this callback
        // returns failure the framework owns ChildInit. Calling Free ourselves
        // is a double-free.
        return status;
    }

    deviceContext = GetDeviceContext(device);
    deviceContext->Device = device;
    status = RtlStringCchCopyW(deviceContext->ComName,
        ARRAYSIZE(deviceContext->ComName),
        comName);
    if (!NT_SUCCESS(status)) return status;

    // Build "\DosDevices\COMx"
    status = RtlAppendUnicodeToString(&symbolicLink, SYMBOLIC_LINK_NAME_PREFIX);
    if (!NT_SUCCESS(status)) return status;
    status = RtlAppendUnicodeStringToString(&symbolicLink, &comNameString);
    if (!NT_SUCCESS(status)) return status;

    // Defensive: if a previous unclean shutdown left a stale link in the
    // object manager namespace, drop it before recreating. Ignore the result.
    (void)IoDeleteSymbolicLink(&symbolicLink);

    status = WdfDeviceCreateSymbolicLink(device, &symbolicLink);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceCreateSymbolicLink failed 0x%x", status);
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, guid, NULL);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceCreateDeviceInterface failed 0x%x", status);
        return status;
    }

    status = DeviceGetPdoName(deviceContext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // PortName under PLUGPLAY_REGKEY_DEVICE — what serenum/serial.sys reads
    // to know which COM letter to expose for this PDO.
    status = WdfDeviceOpenRegistryKey(device,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceOpenRegistryKey failed 0x%x", status);
        return status;
    }

    DECLARE_CONST_UNICODE_STRING(portNameLabel, REG_VALUENAME_PORTNAME);
    status = WdfRegistryAssignUnicodeString(key, &portNameLabel, &comNameString);
    WdfRegistryClose(key);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfRegistryAssignUnicodeString failed 0x%x", status);
        return status;
    }

    // SERIALCOMM device-map — what user-mode COM enumeration looks at.
    status = DeviceWriteSerialCommMap(device, deviceContext->PdoName, deviceContext->ComName);
    if (NT_SUCCESS(status)) {
        deviceContext->CreatedLegacyHardwareKey = TRUE;
    }
    else {
        Trace(TRACE_LEVEL_ERROR, "ERROR: DeviceWriteSerialCommMap failed 0x%x", status);
        deviceContext->CreatedLegacyHardwareKey = FALSE;
        // Non-fatal: the device may still work for direct \\.\COMx opens.
    }

    status = QueueCreateDevice(deviceContext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}

VOID
vSeriousPdoEvtDeviceCleanup(
    _In_ WDFOBJECT Object
)
{
    WDFDEVICE device = (WDFDEVICE)Object;
    PDEVICE_CONTEXT deviceContext = GetDeviceContext(device);

    if (deviceContext->CreatedLegacyHardwareKey && deviceContext->PdoName != NULL) {
        (void)DeviceDeleteSerialCommMap(device, deviceContext->PdoName);
        deviceContext->CreatedLegacyHardwareKey = FALSE;
    }
}

NTSTATUS
DeviceGetPdoName(
    _In_  PDEVICE_CONTEXT DeviceContext
)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFMEMORY memory;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = DeviceContext->Device;

    status = WdfDeviceAllocAndQueryProperty(
        DeviceContext->Device,
        DevicePropertyPhysicalDeviceObjectName,
        NonPagedPoolNx,
        &attributes,
        &memory);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "Failed to query PDO name");
        return status;
    }

    DeviceContext->PdoNameMemory = memory;
    DeviceContext->PdoName = (PWCHAR)WdfMemoryGetBuffer(memory, NULL);

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceWriteSerialCommMap(
    _In_  WDFDEVICE Device,
    _In_  PWSTR     PdoName,
    _In_  PWSTR     ComName
)
{
    NTSTATUS status;
    WDFKEY mapKey;
    UNICODE_STRING valueName;
    UNICODE_STRING valueData;
    DECLARE_CONST_UNICODE_STRING(serialComm, SERIAL_DEVICE_MAP);

    status = WdfDeviceOpenDevicemapKey(Device,
        &serialComm,
        KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &mapKey);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "Failed to open SERIALCOMM devicemap key 0x%x", status);
        return status;
    }

    RtlInitUnicodeString(&valueName, PdoName);
    RtlInitUnicodeString(&valueData, ComName);

    status = WdfRegistryAssignUnicodeString(mapKey, &valueName, &valueData);
    WdfRegistryClose(mapKey);

    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "Failed to write SERIALCOMM value 0x%x", status);
    }

    return status;
}

NTSTATUS
DeviceDeleteSerialCommMap(
    _In_  WDFDEVICE Device,
    _In_  PWSTR     PdoName
)
{
    NTSTATUS status;
    WDFKEY mapKey;
    UNICODE_STRING valueName;
    DECLARE_CONST_UNICODE_STRING(serialComm, SERIAL_DEVICE_MAP);

    status = WdfDeviceOpenDevicemapKey(Device,
        &serialComm,
        KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &mapKey);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&valueName, PdoName);
    status = WdfRegistryRemoveValue(mapKey, &valueName);
    WdfRegistryClose(mapKey);

    return status;
}

ULONG
GetBaudRate(
    _In_  PDEVICE_CONTEXT   DeviceContext
)
{
    return DeviceContext->BaudRate;
}

VOID
SetBaudRate(
    _In_  PDEVICE_CONTEXT   DeviceContext,
    _In_  ULONG             BaudRate
)
{
    DeviceContext->BaudRate = BaudRate;
}

ULONG*
GetModemControlRegisterPtr(
    _In_  PDEVICE_CONTEXT   DeviceContext
)
{
    return &DeviceContext->ModemControlRegister;
}

ULONG*
GetFifoControlRegisterPtr(
    _In_  PDEVICE_CONTEXT   DeviceContext
)
{
    return &DeviceContext->FifoControlRegister;
}

ULONG*
GetLineControlRegisterPtr(
    _In_  PDEVICE_CONTEXT   DeviceContext
)
{
    return &DeviceContext->LineControlRegister;
}

VOID
SetValidDataMask(
    _In_  PDEVICE_CONTEXT   DeviceContext,
    _In_  UCHAR             Mask
)
{
    DeviceContext->ValidDataMask = Mask;
}

VOID
SetTimeouts(
    _In_  PDEVICE_CONTEXT   DeviceContext,
    _In_  SERIAL_TIMEOUTS   Timeouts
)
{
    DeviceContext->Timeouts = Timeouts;
}

VOID
GetTimeouts(
    _In_  PDEVICE_CONTEXT   DeviceContext,
    _Out_ SERIAL_TIMEOUTS* Timeouts
)
{
    *Timeouts = DeviceContext->Timeouts;
}
