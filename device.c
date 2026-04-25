#include "internal.h"


VOID DevicePlugInWorkItemCallback(_In_ WDFWORKITEM WorkItem)
{
    PCONTROLLER_CONTEXT controllerContext = NULL;
    PDEVICE_CONTEXT deviceContext = NULL;
    WDFDEVICE newDevice = NULL;
    NTSTATUS status;

    // Get the controller context (assuming work item's parent is Controller device)
    controllerContext = WdfObjectGetTypedContext(WdfWorkItemGetParentObject(WorkItem), CONTROLLER_CONTEXT);

    if (!controllerContext) {
        Trace(TRACE_LEVEL_ERROR, "ControllerContext NULL in workitem");
        return;
    }

    WdfWaitLockAcquire(controllerContext->StateLock, NULL);

    if (controllerContext->COMDevice != NULL) {
        Trace(TRACE_LEVEL_ERROR, "Device already active, ignoring plugin request");
        WdfWaitLockRelease(controllerContext->StateLock);
        return;
    }

    status = DevicePlugIn(controllerContext, &deviceContext);
    if (NT_SUCCESS(status)) {
        controllerContext->COMDevice = deviceContext;
        controllerContext->Active = TRUE;
        newDevice = deviceContext->Device;
    }
    else {
        Trace(TRACE_LEVEL_ERROR, "DevicePlugIn failed 0x%x", status);
    }

    WdfWaitLockRelease(controllerContext->StateLock);

    if (newDevice != NULL) {
        status = WdfFdoAddStaticChild(controllerContext->Controller, newDevice);

        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR, "WdfFdoAddStaticChild failed 0x%x", status);

            WdfObjectDelete(newDevice);

            WdfWaitLockAcquire(controllerContext->StateLock, NULL);
            controllerContext->COMDevice = NULL;
            controllerContext->Active = FALSE;
            WdfWaitLockRelease(controllerContext->StateLock);
        }
    }
}

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

    if (ControllerContext == NULL || ControllerContext->Controller == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    // Check IRQL - must be PASSIVE_LEVEL
    KIRQL irql = KeGetCurrentIrql();
    Trace(TRACE_LEVEL_ERROR, "DevicePlugIn: Current IRQL = %d", irql);
    if (irql != PASSIVE_LEVEL) {
        Trace(TRACE_LEVEL_ERROR, "DevicePlugIn: IRQL must be PASSIVE_LEVEL");
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (ControllerContext->SymbolicLinkName.Buffer == NULL ||
        ControllerContext->SymbolicLinkName.Length == 0 ||
        ControllerContext->SymbolicLinkName.MaximumLength == 0 ||
        ControllerContext->SymbolicLinkName.Length > SYMBOLIC_LINK_NAME_LENGTH - sizeof(WCHAR))
    {
        Trace(TRACE_LEVEL_ERROR, "Invalid SymbolicLinkName");
        return STATUS_INVALID_PARAMETER;
    }

    // Verify SymbolicLinkName is null-terminated inside buffer
    {
        size_t wcharCount = ControllerContext->SymbolicLinkName.MaximumLength / sizeof(WCHAR);
        BOOLEAN nullTerminated = FALSE;
        for (size_t i = 0; i < wcharCount; ++i) {
            if (ControllerContext->SymbolicLinkName.Buffer[i] == L'\0') {
                nullTerminated = TRUE;
                break;
            }
        }
        if (!nullTerminated) {
            Trace(TRACE_LEVEL_ERROR, "DevicePlugIn: SymbolicLinkName is not null-terminated");
            return STATUS_INVALID_PARAMETER;
        }
    }

    // Ensure null-termination (if space permits)
    if ((&ControllerContext->SymbolicLinkName)->Length < (&ControllerContext->SymbolicLinkName)->MaximumLength) {
        (&ControllerContext->SymbolicLinkName)->Buffer[(&ControllerContext->SymbolicLinkName)->Length / sizeof(WCHAR)] = L'\0';
    }

    Trace(TRACE_LEVEL_ERROR, "DevicePlugIn called");
    Trace(TRACE_LEVEL_ERROR, "SymbolicLinkName.Length = %hu", ControllerContext->SymbolicLinkName.Length);
    Trace(TRACE_LEVEL_ERROR, "SymbolicLinkName.MaximumLength = %hu", ControllerContext->SymbolicLinkName.MaximumLength);
    Trace(TRACE_LEVEL_ERROR, "SymbolicLinkName.Buffer = %p", ControllerContext->SymbolicLinkName.Buffer);

    if (ControllerContext->SymbolicLinkName.Buffer) {
        // Log a printable (safe-length) version of the symbolic link name
        WCHAR safeName[SYMBOLIC_LINK_NAME_LENGTH / sizeof(WCHAR) + 1] = { 0 };
        size_t copyLen = min(SYMBOLIC_LINK_NAME_LENGTH / sizeof(WCHAR), ControllerContext->SymbolicLinkName.Length / sizeof(WCHAR));
        RtlCopyMemory(safeName, ControllerContext->SymbolicLinkName.Buffer, copyLen * sizeof(WCHAR));
        safeName[copyLen] = L'\0';
        Trace(TRACE_LEVEL_ERROR, "SymbolicLinkName (as string): %ws", safeName);
    }

    Trace(TRACE_LEVEL_ERROR, "DevicePlugIn called");

    deviceInit = WdfPdoInitAllocate(ControllerContext->Controller);
    if (!deviceInit) {
        Trace(TRACE_LEVEL_ERROR, "WdfPdoInitAllocate failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Trace(TRACE_LEVEL_ERROR, "WdfPdoInitAllocate succeeded");

    // Set empty PnP/Power callbacks structure (no callbacks needed for now)
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpCallbacks);

    // Set Hardware ID
    UNICODE_STRING hardwareId;
    RtlInitUnicodeString(&hardwareId, L"ACPI\\PNP0501");
    status = WdfPdoInitAssignDeviceID(deviceInit, &hardwareId);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        Trace(TRACE_LEVEL_ERROR, "AssignDeviceID failed 0x%x", status);
        return status;
    }
    status = WdfPdoInitAddHardwareID(deviceInit, &hardwareId);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        Trace(TRACE_LEVEL_ERROR, "AddHardwareID failed 0x%x", status);
        return status;
    }

    UNICODE_STRING compatId;
    RtlInitUnicodeString(&compatId, L"PORTS\\COMPORT");
    status = WdfPdoInitAddCompatibleID(deviceInit, &compatId);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        Trace(TRACE_LEVEL_ERROR, "AddCompatibleID failed 0x%x", status);
        return status;
    }

    status = WdfPdoInitAssignInstanceID(deviceInit, &ControllerContext->SymbolicLinkName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        Trace(TRACE_LEVEL_ERROR, "AssignInstanceID failed 0x%x", status);
        return status;
    }
    
    DECLARE_CONST_UNICODE_STRING(deviceDesc, L"vSerious Virtual COM Port");
    WdfPdoInitAddDeviceText(deviceInit, &deviceDesc, NULL, 0x409);
    WdfPdoInitSetDefaultLocale(deviceInit, 0x409);
    //WdfDeviceInitSetDeviceType(deviceInit, FILE_DEVICE_SERIAL_PORT);
    //WdfDeviceInitSetExclusive(deviceInit, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    
    status = WdfDeviceCreate(&deviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "WdfDeviceCreate failed 0x%x", status);
        return status;
    }
    Trace(TRACE_LEVEL_ERROR, "WdfDeviceCreate succeeded");
    
    deviceContext = GetDeviceContext(device);
    deviceContext->Device = device;
    
    // Build full symbolic link name safely
    DECLARE_UNICODE_STRING_SIZE(fullSymLink, SYMBOLIC_LINK_NAME_LENGTH + SYMBOLIC_LINK_NAME_PREFIX_LEN);
    RtlInitEmptyUnicodeString(&fullSymLink, fullSymLink.Buffer, fullSymLink.MaximumLength);
    status = RtlAppendUnicodeToString(&fullSymLink, SYMBOLIC_LINK_NAME_PREFIX);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(device);
        return status;
    }
    status = RtlAppendUnicodeStringToString(&fullSymLink, &ControllerContext->SymbolicLinkName);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(device);
        return status;
    }
    Trace(TRACE_LEVEL_ERROR, "Full symbolic link: %wZ", &fullSymLink);
    /*
    status = WdfDeviceCreateSymbolicLink(device, &fullSymLink);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "CreateSymbolicLink failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }
    */
    status = WdfDeviceCreateDeviceInterface(device, (LPGUID)&GUID_DEVINTERFACE_COMPORT, NULL);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "CreateDeviceInterface failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }
    Trace(TRACE_LEVEL_ERROR, "Device interface created successfully");
    /*
    status = DeviceGetPdoName(deviceContext);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "DeviceGetPdoName failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }
    */
    WDFKEY          key;
    WDF_OBJECT_ATTRIBUTES keyAttributes;

    // 1) Initialize attributes so the key is parented to the device:
    WDF_OBJECT_ATTRIBUTES_INIT(&keyAttributes);
    keyAttributes.ParentObject = device;

    status = WdfDeviceOpenRegistryKey(device,
        PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE,
        &keyAttributes,
        &key);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "WdfDeviceOpenRegistryKey failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }

    DECLARE_CONST_UNICODE_STRING(portNameLabel, REG_VALUENAME_PORTNAME);
    status = WdfRegistryAssignUnicodeString(key, &portNameLabel, &ControllerContext->SymbolicLinkName);
    WdfRegistryClose(key);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "WdfRegistryAssignUnicodeString failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }

    Trace(TRACE_LEVEL_ERROR, "Registry PortName assigned: %wZ", &ControllerContext->SymbolicLinkName);

    status = DeviceWriteLegacyHardwareKey(fullSymLink.Buffer, device);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "DeviceWriteLegacyHardwareKey failed 0x%x", status);
        deviceContext->CreatedLegacyHardwareKey = FALSE;
    }
    else {
        deviceContext->CreatedLegacyHardwareKey = TRUE;
    }

    status = QueueCreateDevice(deviceContext);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "QueueCreateDevice failed 0x%x", status);
        WdfObjectDelete(device);
        return status;
    }
    
    *OutDeviceContext = deviceContext;
    Trace(TRACE_LEVEL_ERROR, "DevicePlugIn completed successfully");
    return STATUS_SUCCESS;
}



NTSTATUS
DeviceUnplug(
    _In_ PCONTROLLER_CONTEXT ControllerContext
)
{
    NTSTATUS status = STATUS_SUCCESS;

    WdfWaitLockAcquire(ControllerContext->StateLock, NULL);

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
    if (deviceContext != NULL && deviceContext->Device != NULL) {
        WdfObjectDelete(deviceContext->Device);
    }

    WdfWaitLockRelease(ControllerContext->StateLock);

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
