#include "internal.h"
#include <ntstrsafe.h>

// ObQueryNameString lives in ntifs.h, which conflicts with KMDF's header
// chain. Forward-declare it here so we can read the WDM device-object name
// without pulling in the IFS kit.
NTKERNELAPI NTSTATUS NTAPI
ObQueryNameString(
    _In_ PVOID Object,
    _Out_writes_bytes_opt_(Length) POBJECT_NAME_INFORMATION ObjectNameInfo,
    _In_ ULONG Length,
    _Out_ PULONG ReturnLength
);

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
    LPGUID guid = (LPGUID)&GUID_DEVINTERFACE_COMPORT;
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

    // Declare the PDO as a raw device under the Ports class. We have no INF
    // matching "vSerious\COMx" and the bus driver itself owns all I/O on this
    // PDO — RawDevice makes that ownership explicit to KMDF and PnP, avoiding
    // "no function driver" WDF_VIOLATIONs.
    status = WdfPdoInitAssignRawDevice(ChildInit, &GUID_DEVCLASS_PORTS);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfPdoInitAssignRawDevice failed 0x%x", status);
        return status;
    }

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
    // Force passive-level execution so the cleanup callback (which calls
    // RtlDeleteRegistryValue, a PASSIVE_LEVEL-only API) is safe.
    deviceAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    deviceAttributes.EvtCleanupCallback = vSeriousPdoEvtDeviceCleanup;

    status = WdfDeviceCreate(&ChildInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceCreate failed 0x%x", status);
        // WdfDeviceCreate itself frees ChildInit on its own failure.
        return status;
    }

    // From here on, returning a failure status is enough — KMDF auto-deletes
    // the partially-built PDO when EvtChildListCreateDevice returns !NT_SUCCESS.
    // Calling WdfObjectDelete ourselves trips a WDF Verifier break.

    deviceContext = GetDeviceContext(device);
    deviceContext->Device = device;
    status = RtlStringCchCopyW(deviceContext->ComName,
        ARRAYSIZE(deviceContext->ComName),
        comName);
    if (!NT_SUCCESS(status)) goto Fail;

    status = WdfDeviceCreateDeviceInterface(device, guid, NULL);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceCreateDeviceInterface failed 0x%x", status);
        goto Fail;
    }

    status = DeviceGetPdoName(deviceContext);
    if (!NT_SUCCESS(status)) {
        goto Fail;
    }

    // Without serial.sys attached (we're a raw PDO), nothing creates
    // \DosDevices\COMx for us, so user-mode CreateFile(\\.\COMx) would fail
    // with ERROR_FILE_NOT_FOUND. WdfDeviceCreateSymbolicLink refuses to
    // create links on PDOs (STATUS_INVALID_DEVICE_STATE), so use the raw
    // Io API and unwind it ourselves in EvtDeviceCleanup.
    {
        DECLARE_UNICODE_STRING_SIZE(symbolicLink, 32);
        UNICODE_STRING prefix;
        UNICODE_STRING pdoNameString;
        RtlInitUnicodeString(&prefix, SYMBOLIC_LINK_NAME_PREFIX);
        status = RtlAppendUnicodeStringToString(&symbolicLink, &prefix);
        if (!NT_SUCCESS(status)) goto Fail;
        status = RtlAppendUnicodeStringToString(&symbolicLink, &comNameString);
        if (!NT_SUCCESS(status)) goto Fail;
        RtlInitUnicodeString(&pdoNameString, deviceContext->PdoName);
        status = IoCreateSymbolicLink(&symbolicLink, &pdoNameString);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR, "ERROR: IoCreateSymbolicLink failed 0x%x", status);
            goto Fail;
        }
        deviceContext->CreatedSymbolicLink = TRUE;
    }

    // PortName under PLUGPLAY_REGKEY_DEVICE is what serial.sys reads to pick
    // the COM letter — we have no serial.sys attached, and the key doesn't
    // exist yet inside EvtChildListCreateDevice anyway (WdfDeviceOpenRegistryKey
    // returns STATUS_INVALID_DEVICE_STATE). User-mode discovers the port via
    // the SERIALCOMM device-map entry and the \DosDevices\COMx symlink, so
    // skipping PortName is fine.

    // SERIALCOMM device-map — what user-mode COM enumeration looks at.
    status = DeviceWriteSerialCommMap(device, deviceContext->PdoName, deviceContext->ComName);
    if (NT_SUCCESS(status)) {
        deviceContext->CreatedLegacyHardwareKey = TRUE;
    }
    else {
        Trace(TRACE_LEVEL_ERROR, "ERROR: DeviceWriteSerialCommMap failed 0x%x", status);
        deviceContext->CreatedLegacyHardwareKey = FALSE;
        // Non-fatal: device may still work for direct \\.\COMx opens.
    }

    status = QueueCreateDevice(deviceContext);
    if (!NT_SUCCESS(status)) {
        goto Fail;
    }

    return STATUS_SUCCESS;

Fail:
    return status;
}

VOID
vSeriousPdoEvtDeviceCleanup(
    _In_ WDFOBJECT Object
)
{
    WDFDEVICE device = (WDFDEVICE)Object;
    PDEVICE_CONTEXT deviceContext = GetDeviceContext(device);

    if (deviceContext->CreatedSymbolicLink) {
        DECLARE_UNICODE_STRING_SIZE(symbolicLink, 32);
        UNICODE_STRING prefix;
        UNICODE_STRING comNameString;
        RtlInitUnicodeString(&prefix, SYMBOLIC_LINK_NAME_PREFIX);
        RtlInitUnicodeString(&comNameString, deviceContext->ComName);
        if (NT_SUCCESS(RtlAppendUnicodeStringToString(&symbolicLink, &prefix)) &&
            NT_SUCCESS(RtlAppendUnicodeStringToString(&symbolicLink, &comNameString))) {
            (void)IoDeleteSymbolicLink(&symbolicLink);
        }
        deviceContext->CreatedSymbolicLink = FALSE;
    }

    if (deviceContext->CreatedLegacyHardwareKey) {
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
    PDEVICE_OBJECT wdmDevice;
    ULONG returnLength = 0;
    UCHAR scratchBuffer[256];
    POBJECT_NAME_INFORMATION nameInfo = (POBJECT_NAME_INFORMATION)scratchBuffer;

    // DevicePropertyPhysicalDeviceObjectName fails with STATUS_INVALID_DEVICE_STATE
    // inside EvtChildListCreateDevice — that property is meant for FDOs/filters
    // querying their underlying PDO. Read the WDM object name directly instead.
    wdmDevice = WdfDeviceWdmGetDeviceObject(DeviceContext->Device);

    status = ObQueryNameString(wdmDevice, nameInfo, sizeof(scratchBuffer), &returnLength);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ObQueryNameString failed 0x%x", status);
        return status;
    }

    if (nameInfo->Name.Length >= sizeof(DeviceContext->PdoName)) {
        Trace(TRACE_LEVEL_ERROR, "PdoName too large: %u bytes", nameInfo->Name.Length);
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(DeviceContext->PdoName, nameInfo->Name.Buffer, nameInfo->Name.Length);
    DeviceContext->PdoName[nameInfo->Name.Length / sizeof(WCHAR)] = L'\0';
    return STATUS_SUCCESS;
}

NTSTATUS
DeviceWriteSerialCommMap(
    _In_  WDFDEVICE Device,
    _In_  PWSTR     PdoName,
    _In_  PWSTR     ComName
)
{
    UNREFERENCED_PARAMETER(Device);

    // RTL_REGISTRY_DEVICEMAP rooted at HKLM\HARDWARE\DEVICEMAP — used here
    // (rather than WdfDeviceOpenDevicemapKey) because the KMDF helper requires
    // version 1.13, but we target 1.11 for Windows 7 compatibility.
    return RtlWriteRegistryValue(
        RTL_REGISTRY_DEVICEMAP,
        SERIAL_DEVICE_MAP,
        PdoName,
        REG_SZ,
        ComName,
        (ULONG)((wcslen(ComName) + 1) * sizeof(WCHAR)));
}

NTSTATUS
DeviceDeleteSerialCommMap(
    _In_  WDFDEVICE Device,
    _In_  PWSTR     PdoName
)
{
    UNREFERENCED_PARAMETER(Device);

    return RtlDeleteRegistryValue(
        RTL_REGISTRY_DEVICEMAP,
        SERIAL_DEVICE_MAP,
        PdoName);
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
