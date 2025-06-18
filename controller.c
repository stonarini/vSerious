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
	UNREFERENCED_PARAMETER(Driver);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, CONTROLLER_CONTEXT);

	deviceAttributes.SynchronizationScope = WdfSynchronizationScopeDevice;
	deviceAttributes.EvtCleanupCallback = vSeriousEvtDeviceCleanup;

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

	// 6. Create queue
	status = QueueCreateController(controllerContext);
	if (!NT_SUCCESS(status)) {
		WdfObjectDelete(device);
		return status;
	}

	*ControllerContext = controllerContext;

	return status;
}