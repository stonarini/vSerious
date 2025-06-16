#include "internal.h"

NTSTATUS
DevicePlugIn(
    _In_ PCONTROLLER_CONTEXT ControllerContext,
    _Out_ PDEVICE_CONTEXT* OutDeviceContext
)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    PWDFDEVICE_INIT deviceInit = NULL;
    PDEVICE_CONTEXT deviceContext;
    UNICODE_STRING comName;
    WCHAR symbolicLinkBuffer[SYMBOLIC_LINK_NAME_LENGTH];
    DECLARE_UNICODE_STRING_SIZE(symbolicLink, SYMBOLIC_LINK_NAME_LENGTH);
    WDFKEY key;
    LPGUID guid = (LPGUID)&GUID_DEVINTERFACE_COMPORT;
    DECLARE_CONST_UNICODE_STRING(local_SDDL_DEVOBJ_SYS_ALL_ADM_ALL, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

    deviceInit = WdfControlDeviceInitAllocate(WdfGetDriver(), &local_SDDL_DEVOBJ_SYS_ALL_ADM_ALL);
    if (!deviceInit) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfControlDeviceInitAllocate failed 0x%x", STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WdfDeviceInitSetDeviceType(deviceInit, FILE_DEVICE_SERIAL_PORT);
    WdfDeviceInitSetExclusive(deviceInit, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.SynchronizationScope = WdfSynchronizationScopeDevice;

    status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceCreate failed 0x%x", status);
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    deviceContext = GetDeviceContext(device);
    deviceContext->Device = device;

    comName = ControllerContext->SymbolicLinkName;
    RtlInitEmptyUnicodeString(&symbolicLink, symbolicLinkBuffer, sizeof(symbolicLinkBuffer));

    status = RtlAppendUnicodeToString(&symbolicLink, SYMBOLIC_LINK_NAME_PREFIX);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: RtlAppendUnicodeToString (prefix) failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }

    status = RtlAppendUnicodeStringToString(&symbolicLink, &ControllerContext->SymbolicLinkName);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: RtlAppendUnicodeStringToString failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }

    // 1. Create symbolic link
    status = WdfDeviceCreateSymbolicLink(device, &symbolicLink);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceCreateSymbolicLink failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }

    // 2. Create device interface
    status = WdfDeviceCreateDeviceInterface(device, guid, NULL);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceCreateDeviceInterface failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }

    // 3. Get PDO name
    status = DeviceGetPdoName(deviceContext);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(device);
        return status;
    }

    // 4. Write PortName to hardware key
    status = WdfDeviceOpenRegistryKey(device,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceOpenRegistryKey failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }

    DECLARE_CONST_UNICODE_STRING(portNameLabel, REG_VALUENAME_PORTNAME);
    status = WdfRegistryAssignUnicodeString(key, &portNameLabel, &comName);
    WdfRegistryClose(key);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfRegistryAssignUnicodeString failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }

    // 5. Legacy registry mapping
    status = DeviceWriteLegacyHardwareKey(deviceContext->PdoName, symbolicLinkBuffer, device);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: DeviceWriteLegacyHardwareKey failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }
    deviceContext->CreatedLegacyHardwareKey = TRUE;

    // 6. Create queue
    status = QueueCreateDevice(deviceContext);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(device);
        return status;
    }

    ControllerContext->Active = TRUE;
    *OutDeviceContext = deviceContext;

    return STATUS_SUCCESS;
}


NTSTATUS
DeviceUnplug(
    _In_ PCONTROLLER_CONTEXT ControllerContext
)
{
    NTSTATUS status = STATUS_SUCCESS;
    WDFKEY key = NULL;
    UNICODE_STRING pdoString;
    PDEVICE_CONTEXT deviceContext = ControllerContext->COMDevice;
    DECLARE_CONST_UNICODE_STRING(deviceSubkey, SERIAL_DEVICE_MAP);

    if (ControllerContext->SymbolicLinkName.Buffer != NULL &&
        ControllerContext->SymbolicLinkName.Length > 0)
    {
        status = IoDeleteSymbolicLink(&ControllerContext->SymbolicLinkName);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR, "ERROR: IoDeleteSymbolicLink failed 0x%x", status);
        }

        if (deviceContext->CreatedLegacyHardwareKey) {
            RtlInitUnicodeString(&pdoString, deviceContext->PdoName);

            status = WdfDeviceOpenDevicemapKey(deviceContext->Device,
                &deviceSubkey,
                KEY_SET_VALUE,
                WDF_NO_OBJECT_ATTRIBUTES,
                &key);
            if (NT_SUCCESS(status)) {
                WdfRegistryRemoveValue(key, &pdoString);
                WdfRegistryClose(key);
            }
            else {
                Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceOpenDevicemapKey failed 0x%x", status);
            }
            deviceContext->CreatedLegacyHardwareKey = FALSE;
        }
    }

    ControllerContext->Active = FALSE;

    WdfObjectDelete(deviceContext->Device);

    return status;
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
DeviceWriteLegacyHardwareKey(
    _In_  PWSTR PdoName,
    _In_  PWSTR ComPort,
    _In_  WDFDEVICE Device
)
{
    WDFKEY key = NULL;
    NTSTATUS status;
    UNICODE_STRING pdoString, comPortString;
    DECLARE_CONST_UNICODE_STRING(deviceSubkey, SERIAL_DEVICE_MAP);

    RtlInitUnicodeString(&pdoString, PdoName);
    RtlInitUnicodeString(&comPortString, ComPort);

    status = WdfDeviceOpenDevicemapKey(Device,
        &deviceSubkey,
        KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "Failed to open DEVICEMAP\\SERIALCOMM key 0x%x", status);
        return status;
    }

    status = WdfRegistryAssignUnicodeString(key, &pdoString, &comPortString);
    WdfRegistryClose(key);

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
