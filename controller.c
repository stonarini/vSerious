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
	WDF_CHILD_LIST_CONFIG   childListConfig;
	UNREFERENCED_PARAMETER(Driver);

	// Child-list config MUST be installed on the PWDFDEVICE_INIT BEFORE
	// WdfDeviceCreate; setting it later silently does nothing and the
	// SET_ACTIVE IOCTL fails with STATUS_INVALID_DEVICE_STATE.
	WDF_CHILD_LIST_CONFIG_INIT(&childListConfig,
		sizeof(VSERIOUS_PDO_IDENTIFICATION_DESCRIPTION),
		vSeriousEvtChildListCreateDevice);

	WdfFdoInitSetDefaultChildListConfig(DeviceInit,
		&childListConfig,
		WDF_NO_OBJECT_ATTRIBUTES);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, CONTROLLER_CONTEXT);

	deviceAttributes.SynchronizationScope = WdfSynchronizationScopeDevice;

	status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR,
			"ERROR: WdfDeviceCreate failed 0x%x", status);
		// On WdfDeviceCreate failure, the framework does NOT free DeviceInit
		// (this is the OPPOSITE of EvtChildListCreateDevice's contract).
		WdfDeviceInitFree(DeviceInit);
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

	status = QueueCreateController(controllerContext);
	if (!NT_SUCCESS(status)) {
		WdfObjectDelete(device);
		return status;
	}

	*ControllerContext = controllerContext;

	return status;
}
