#include "internal.h" 

NTSTATUS
ControllerCreate(
	_In_  WDFDRIVER         Driver,
	_In_  PWDFDEVICE_INIT   DeviceInit,
	_Out_ PCONTROLLER_CONTEXT* ControllerContext
)
{
	NTSTATUS                status;
	WDF_OBJECT_ATTRIBUTES   deviceAttributes;
	WDFDEVICE               device;
	PCONTROLLER_CONTEXT     controllerContext;
	WDF_FILEOBJECT_CONFIG   fileConfig;
	UNREFERENCED_PARAMETER(Driver);

	WdfFdoInitSetFilter(DeviceInit);
	WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
	WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

	WDF_FILEOBJECT_CONFIG_INIT(
		&fileConfig,
		vSeriousEvtDeviceFileCreate,
		WDF_NO_EVENT_CALLBACK,
		WDF_NO_EVENT_CALLBACK
	);

	WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, CONTROLLER_CONTEXT);

	deviceAttributes.SynchronizationScope = WdfSynchronizationScopeDevice;
	deviceAttributes.EvtCleanupCallback = vSeriousEvtDeviceCleanup;
	deviceAttributes.ExecutionLevel = WdfExecutionLevelPassive;

	status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR,
			"ERROR: WdfDeviceCreate failed 0x%x", status);
		return status;
	}

	DECLARE_CONST_UNICODE_STRING(controllerSymbolicLink, CONTROLLER_SYMBOLIC_LINK);
	status = WdfDeviceCreateSymbolicLink(device, &controllerSymbolicLink);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, "ERROR: WdfDeviceCreateSymbolicLink failed 0x%x", status);
		return status;
	}

	controllerContext = GetControllerContext(device);
	controllerContext->Controller = device;
	controllerContext->Active = FALSE;
	controllerContext->COMDevice = NULL;

	WDF_OBJECT_ATTRIBUTES lockAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
	lockAttributes.ParentObject = device;

	status = WdfWaitLockCreate(&lockAttributes, &controllerContext->StateLock);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, "WdfWaitLockCreate failed 0x%x", status);
		return status;
	}

	RtlInitEmptyUnicodeString(
		&controllerContext->SymbolicLinkName,
		controllerContext->SymbolicLinkBuffer,
		sizeof(controllerContext->SymbolicLinkBuffer)
	);

	// 6. Create queue
	status = QueueCreateController(controllerContext);
	if (!NT_SUCCESS(status)) {
		WdfObjectDelete(device);
		return status;
	}

	// 7. Create Work Item
	WDF_WORKITEM_CONFIG workitemConfig;
	WDF_OBJECT_ATTRIBUTES attributes;

	WDF_WORKITEM_CONFIG_INIT(&workitemConfig, DevicePlugInWorkItemCallback);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = controllerContext->Controller;

	status = WdfWorkItemCreate(&workitemConfig, &attributes, &controllerContext->PlugInWorkItem);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, "WdfWorkItemCreate failed 0x%x", status);
	}

	*ControllerContext = controllerContext;

	return status;
}
