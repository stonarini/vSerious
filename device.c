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

    deviceInit = WdfPdoInitAllocate(ControllerContext->Controller);
    if (!deviceInit) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: WdfPdoInitAllocate failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpCallbacks);

    // Set IDs
    WCHAR hwIdBuffer[64];
    UNICODE_STRING hardwareId;

    RtlInitEmptyUnicodeString(&hardwareId, hwIdBuffer, sizeof(hwIdBuffer));
    RtlAppendUnicodeToString(&hardwareId, L"vSerious\\");
    RtlAppendUnicodeStringToString(&hardwareId, &ControllerContext->SymbolicLinkName);
    status = WdfPdoInitAssignDeviceID(deviceInit, &hardwareId);
    if (!NT_SUCCESS(status)) return status;

    status = WdfPdoInitAddHardwareID(deviceInit, &hardwareId);
    if (!NT_SUCCESS(status)) return status;

    status = WdfPdoInitAddCompatibleID(deviceInit, &hardwareId);
    if (!NT_SUCCESS(status)) return status;

    DECLARE_CONST_UNICODE_STRING(deviceDesc, L"vSerious Virtual COM Port");
    WdfPdoInitAddDeviceText(deviceInit, &deviceDesc, NULL, 0);
    WdfPdoInitSetDefaultLocale(deviceInit, 0x409); // en-US

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
    status = DeviceWriteLegacyHardwareKey(symbolicLinkBuffer, device);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "ERROR: DeviceWriteLegacyHardwareKey failed 0x%x", status);
        deviceContext->CreatedLegacyHardwareKey = FALSE;
    }
    else {
        deviceContext->CreatedLegacyHardwareKey = TRUE;
    }

    // 6. Create queue
    status = QueueCreateDevice(deviceContext);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(device);
        return status;
    }

    ControllerContext->Active = TRUE;

    WdfFdoAddStaticChild(ControllerContext->Controller, device);

    *OutDeviceContext = deviceContext;

    return STATUS_SUCCESS;
}


NTSTATUS
DeviceUnplug(
    _In_ PCONTROLLER_CONTEXT ControllerContext
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT deviceContext = ControllerContext->COMDevice;
    UNICODE_STRING deviceSubkey;
    RtlInitUnicodeString(&deviceSubkey, SERIAL_DEVICE_MAP);

    if (ControllerContext->SymbolicLinkName.Buffer != NULL &&
        ControllerContext->SymbolicLinkName.Length > 0)
    {
        status = IoDeleteSymbolicLink(&ControllerContext->SymbolicLinkName);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR, "ERROR: IoDeleteSymbolicLink failed 0x%x", status);
        }

        if (deviceContext->CreatedLegacyHardwareKey) {
            HANDLE hKey = NULL;
            PDEVICE_OBJECT pDeviceObject = WdfDeviceWdmGetDeviceObject(deviceContext->Device);

            status = IoOpenDeviceRegistryKey(
                pDeviceObject,
                PLUGPLAY_REGKEY_DEVICE,
                KEY_SET_VALUE,
                &hKey
            );

            if (NT_SUCCESS(status)) {
                status = ZwDeleteValueKey(hKey, &deviceSubkey);
                if (!NT_SUCCESS(status)) {
                    Trace(TRACE_LEVEL_ERROR, "ERROR: ZwDeleteValueKey failed 0x%x", status);
                }

                ZwClose(hKey);
            }
            else {
                Trace(TRACE_LEVEL_ERROR, "ERROR: IoOpenDeviceRegistryKey failed 0x%x", status);
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
    _In_  PWSTR ComPort,
    _In_  WDFDEVICE Device
)
{
    HANDLE hKey = NULL;
    NTSTATUS status;
    UNICODE_STRING comPortString;
    UNICODE_STRING deviceSubkey;
    RtlInitUnicodeString(&deviceSubkey, SERIAL_DEVICE_MAP);
    PDEVICE_OBJECT deviceObject = WdfDeviceWdmGetDeviceObject(Device);

    status = IoOpenDeviceRegistryKey(
        deviceObject,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE,
        &hKey
    );

    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "Failed to open device registry key 0x%x", status);
        return status;
    }

    RtlInitUnicodeString(&comPortString, ComPort);

    // Set the value "SERIALCOMM" = ComPort in the device key
    status = ZwSetValueKey(
        hKey,
        &deviceSubkey,
        0,
        REG_SZ,
        comPortString.Buffer,
        (comPortString.Length + sizeof(WCHAR))
    );

    ZwClose(hKey);

    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "Failed to set SERIALCOMM registry value 0x%x", status);
    }

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
