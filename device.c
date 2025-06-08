#include "internal.h" 


NTSTATUS
DeviceCreate(
    _In_  WDFDRIVER         Driver,          
    _In_  PWDFDEVICE_INIT   DeviceInit,      
    _Out_ PDEVICE_CONTEXT* DeviceContext  
)
{
    NTSTATUS                status;         
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    WDFDEVICE               device;          
    PDEVICE_CONTEXT         deviceContext;   
    UNREFERENCED_PARAMETER(Driver);       

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
        &deviceAttributes,
        DEVICE_CONTEXT);

    deviceAttributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    deviceAttributes.EvtCleanupCallback = vSeriousEvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit,
        &deviceAttributes,
        &device);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfDeviceCreate failed 0x%x", status);
        return status;
    }

    deviceContext = GetDeviceContext(device);
    deviceContext->Device = device;

    *DeviceContext = deviceContext;

    return status;
}


NTSTATUS
DeviceConfigure(
    _In_  PDEVICE_CONTEXT   DeviceContext
)
{
    NTSTATUS                status;
    WDFDEVICE               device = DeviceContext->Device;
    WDFKEY                  key;
    LPGUID                  guid;
    //errno_t                 errorNo;

    DECLARE_CONST_UNICODE_STRING(portName, REG_VALUENAME_PORTNAME);
    DECLARE_UNICODE_STRING_SIZE(comPort, 10);

    guid = (LPGUID)&GUID_DEVINTERFACE_COMPORT;

    status = WdfDeviceCreateDeviceInterface(
        device,
        guid,
        NULL);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: Cannot create device interface");
        goto Exit;
    }

    status = WdfDeviceOpenRegistryKey(
        device,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_QUERY_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: Failed to retrieve device hardware key root");
        goto Exit;
    }

    status = WdfRegistryQueryUnicodeString(
        key,
        &portName,
        NULL,
        &comPort);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: Failed to read PortName");
        goto Exit;
    }

    status = DevicePlugIn(DeviceContext, &comPort);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    DeviceContext->Active = TRUE;

    status = DeviceGetPdoName(DeviceContext);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = DeviceWriteLegacyHardwareKey(
        DeviceContext->PdoName,
        comPort.Buffer,
        DeviceContext->Device);
    if (NT_SUCCESS(status)) {
        DeviceContext->CreatedLegacyHardwareKey = TRUE;
    }

    status = QueueCreate(DeviceContext);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

Exit:
    return status;
}


NTSTATUS
DeviceGetPdoName(
    _In_  PDEVICE_CONTEXT   DeviceContext
)
{
    NTSTATUS                status;
    WDFDEVICE               device = DeviceContext->Device;
    WDF_OBJECT_ATTRIBUTES   attributes;
    WDFMEMORY               memory;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device; // Tie lifetime of allocated memory to device

    status = WdfDeviceAllocAndQueryProperty(
        device,
        DevicePropertyPhysicalDeviceObjectName, // Property to query
        NonPagedPoolNx,
        &attributes,
        &memory);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: Failed to query PDO name");
        goto Exit;
    }

    DeviceContext->PdoName = (PWCHAR)WdfMemoryGetBuffer(memory, NULL);

    Trace(TRACE_LEVEL_INFO,
        "PDO Name is %ws", DeviceContext->PdoName);

Exit:
    return status;
}


// Write the legacy hardware key (serial port mapping) to the registry under DeviceMap\SERIALCOMM
NTSTATUS
DeviceWriteLegacyHardwareKey(
    _In_  PWSTR             PdoName,  // Physical Device Object name string
    _In_  PWSTR             ComPort,  // COM port string (e.g., "COM1")
    _In_  WDFDEVICE         Device
)
{
    WDFKEY                  key = NULL;   // Registry key handle
    NTSTATUS                status;
    UNICODE_STRING          pdoString = { 0 };
    UNICODE_STRING          comPort = { 0 };

    DECLARE_CONST_UNICODE_STRING(deviceSubkey, SERIAL_DEVICE_MAP);

    RtlInitUnicodeString(&pdoString, PdoName);
    RtlInitUnicodeString(&comPort, ComPort);

    status = WdfDeviceOpenDevicemapKey(Device,
        &deviceSubkey,
        KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &key);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: Failed to open DEVICEMAP\\SERIALCOMM key 0x%x", status);
        goto exit;
    }

    status = WdfRegistryAssignUnicodeString(key,
        &pdoString,
        &comPort);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: Failed to write to DEVICEMAP\\SERIALCOMM key 0x%x", status);
        goto exit;
    }

exit:
    if (key != NULL) {
        WdfRegistryClose(key);
        key = NULL;
    }

    return status;
}


VOID
vSeriousEvtDeviceCleanup(
    _In_  WDFOBJECT         Object
)
{
    WDFDEVICE               device = (WDFDEVICE)Object;
    PDEVICE_CONTEXT         deviceContext = GetDeviceContext(device);
    NTSTATUS                status;
    WDFKEY                  key = NULL;
    UNICODE_STRING          pdoString = { 0 };

    DECLARE_CONST_UNICODE_STRING(deviceSubkey, SERIAL_DEVICE_MAP);

    // Only remove key if it was created during device lifetime
    if (deviceContext->CreatedLegacyHardwareKey == TRUE) {

        RtlInitUnicodeString(&pdoString, deviceContext->PdoName);

        status = WdfDeviceOpenDevicemapKey(device,
            &deviceSubkey,
            KEY_SET_VALUE,
            WDF_NO_OBJECT_ATTRIBUTES,
            &key);

        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR,
                "Error: Failed to open DEVICEMAP\\SERIALCOMM key 0x%x", status);
            goto exit;
        }

        status = WdfRegistryRemoveValue(key,
            &pdoString);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR,
                "Error: Failed to delete %S key, 0x%x", pdoString.Buffer, status);
            goto exit;
        }

        status = WdfRegistryRemoveKey(key);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR,
                "Error: Failed to delete %S, 0x%x", SERIAL_DEVICE_MAP, status);
            goto exit;
        }
    }

exit:
    if (key != NULL) {
        WdfRegistryClose(key);
        key = NULL;
    }

    return;
}

NTSTATUS
DevicePlugIn(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ PCUNICODE_STRING ComPortName  // e.g. "COM1"
)
{
    NTSTATUS status = STATUS_SUCCESS;
    errno_t errorNo;

    UNICODE_STRING symbolicLinkName;
    symbolicLinkName.Buffer = DeviceContext->SymbolicLinkBuffer;
    symbolicLinkName.MaximumLength = sizeof(DeviceContext->SymbolicLinkBuffer);

    // Compose symbolic link name: "\\DosDevices\\COMx"
    // Calculate length safely (number of WCHARs, excluding NULL)
    symbolicLinkName.Length = (USHORT)(
        (wcslen(SYMBOLIC_LINK_NAME_PREFIX) + wcslen(ComPortName->Buffer)) * sizeof(WCHAR)
        );

    if (symbolicLinkName.Length >= symbolicLinkName.MaximumLength) {
        Trace(TRACE_LEVEL_ERROR, "Symbolic link buffer too small");
        return STATUS_BUFFER_OVERFLOW;
    }

    errorNo = wcscpy_s(symbolicLinkName.Buffer,
        SYMBOLIC_LINK_NAME_LENGTH,
        SYMBOLIC_LINK_NAME_PREFIX);
    if (errorNo != 0) {
        Trace(TRACE_LEVEL_ERROR, "wcscpy_s failed with %d", errorNo);
        return STATUS_INVALID_PARAMETER;
    }

    errorNo = wcscat_s(symbolicLinkName.Buffer,
        SYMBOLIC_LINK_NAME_LENGTH,
        ComPortName->Buffer);
    if (errorNo != 0) {
        Trace(TRACE_LEVEL_ERROR, "wcscat_s failed with %d", errorNo);
        return STATUS_INVALID_PARAMETER;
    }

    DeviceContext->SymbolicLinkName = symbolicLinkName;

    status = WdfDeviceCreateSymbolicLink(DeviceContext->Device, &DeviceContext->SymbolicLinkName);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "WdfDeviceCreateSymbolicLink failed 0x%x", status);
    }

    return status;
}

NTSTATUS
DeviceUnplug(
    _In_ PDEVICE_CONTEXT DeviceContext
)
{
    NTSTATUS status = STATUS_SUCCESS;
    if (DeviceContext->SymbolicLinkName.Buffer != NULL &&
        DeviceContext->SymbolicLinkName.Length > 0)
    {
        status = IoDeleteSymbolicLink(&DeviceContext->SymbolicLinkName);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR, "IoDeleteSYmbolicLink failed 0x%x", status);
            return status;
        }

        DeviceContext->SymbolicLinkName.Length = 0;
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