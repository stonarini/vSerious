#include "internal.h"
#include <ntstrsafe.h>

NTSTATUS
QueueCreateDevice(
    _In_  PDEVICE_CONTEXT   DeviceContext
)
{
    NTSTATUS                status;
    WDFDEVICE               device = DeviceContext->Device;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    WDF_OBJECT_ATTRIBUTES   queueAttributes;
    WDFQUEUE                queue;
    PQUEUE_CONTEXT          queueContext;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig,
        WdfIoQueueDispatchParallel);

    queueConfig.EvtIoRead = vSeriousDeviceEvtIoRead;
    queueConfig.EvtIoWrite = vSeriousDeviceEvtIoWrite;
    queueConfig.EvtIoDeviceControl = vSeriousDeviceEvtIoDeviceControl;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
        &queueAttributes,
        QUEUE_CONTEXT);

    status = WdfIoQueueCreate(
        device,
        &queueConfig,
        &queueAttributes,
        &queue);

    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfIoQueueCreate failed 0x%x", status);
        return status;
    }

    queueContext = GetQueueContext(queue);
    queueContext->Queue = queue;
    queueContext->DeviceContext = DeviceContext;

    // Cristina's pending reads on \\.\COMx land here.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "Error: ComReadQueue create failed 0x%x", status);
        return status;
    }
    queueContext->ComReadQueue = queue;

    // Wait-on-mask pending queue (IOCTL_SERIAL_WAIT_ON_MASK).
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "Error: WaitMaskQueue create failed 0x%x", status);
        return status;
    }
    queueContext->WaitMaskQueue = queue;

    RingBufferInitialize(&queueContext->RingBufferPcToHw,
        queueContext->BufferPcToHw, sizeof(queueContext->BufferPcToHw));
    RingBufferInitialize(&queueContext->RingBufferHwToPc,
        queueContext->BufferHwToPc, sizeof(queueContext->BufferHwToPc));

    // Tell the controller which queue context to route SDK IOCTLs to. Stays
    // valid for the lifetime of the child PDO. EvtChildListCreateDevice has
    // already populated controllerContext->ActiveChildQueue == NULL.
    if (DeviceContext->Device) {
        WDFDEVICE parent = WdfPdoGetParent(DeviceContext->Device);
        if (parent) {
            PCONTROLLER_CONTEXT cc = GetControllerContext(parent);
            cc->ActiveChildQueue = queueContext;
        }
    }

    return status;
}

NTSTATUS
QueueCreateController(
    _In_  PCONTROLLER_CONTEXT   ControllerContext
)
{
    NTSTATUS                status;
    WDFDEVICE               device = ControllerContext->Controller;
    WDF_IO_QUEUE_CONFIG     queueConfig;
    WDF_OBJECT_ATTRIBUTES   queueAttributes;
    WDFQUEUE                queue;
    PQUEUE_CONTEXT          queueContext;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig,
        WdfIoQueueDispatchParallel);

    queueConfig.EvtIoDeviceControl = vSeriousControllerEvtIoDeviceControl;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
        &queueAttributes,
        QUEUE_CONTEXT);

    status = WdfIoQueueCreate(
        device,
        &queueConfig,
        &queueAttributes,
        &queue);

    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfIoQueueCreate failed 0x%x", status);
        return status;
    }

    queueContext = GetQueueContext(queue);
    queueContext->Queue = queue;
    queueContext->ControllerContext = ControllerContext;

    // Parking queue for sCristina's pending IOCTL_VSERIOUS_READ requests.
    // Same device as where those IOCTLs land (the controller), so forwarding
    // into it is intra-device. Child EvtIoWrite drains it via inline
    // retrieve + complete, no cross-device forwarding.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR, "Error: SdkReadQueue create failed 0x%x", status);
        return status;
    }
    ControllerContext->SdkReadQueue = queue;

    return status;
}


NTSTATUS
RequestCopyFromBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             SourceBuffer,
    _In_  size_t            NumBytesToCopyFrom
)
{
    NTSTATUS                status;
    WDFMEMORY               memory;

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfRequestRetrieveOutputMemory failed 0x%x", status);
        return status;
    }

    status = WdfMemoryCopyFromBuffer(memory, 0,
        SourceBuffer, NumBytesToCopyFrom);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfMemoryCopyFromBuffer failed 0x%x", status);
        return status;
    }

    WdfRequestSetInformation(Request, NumBytesToCopyFrom);
    return status;
}


/* Utility function to retrieve value from req into a buffer*/
NTSTATUS
RequestCopyToBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             DestinationBuffer,
    _In_  size_t            NumBytesToCopyTo
)
{
    NTSTATUS                status;
    WDFMEMORY               memory;

    status = WdfRequestRetrieveInputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfRequestRetrieveInputMemory failed 0x%x", status);
        return status;
    }

    status = WdfMemoryCopyToBuffer(memory, 0,
        DestinationBuffer, NumBytesToCopyTo);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfMemoryCopyToBuffer failed 0x%x", status);
        return status;
    }

    return status;
}


VOID
vSeriousControllerEvtIoDeviceControl(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            OutputBufferLength,
    _In_  size_t            InputBufferLength,
    _In_  ULONG             IoControlCode
)
{
    NTSTATUS                status;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    PCONTROLLER_CONTEXT     controllerContext = queueContext->ControllerContext;
    UNREFERENCED_PARAMETER(OutputBufferLength);

    Trace(TRACE_LEVEL_INFO,
        "EvtIoDeviceControl 0x%x", IoControlCode);

    switch (IoControlCode)
    {
    case IOCTL_SERIAL_WAIT_ON_MASK:
    {
        //
        // NOTE: A wait-on-mask request should not be completed until either:
        //  1) A wait event occurs; or
        //  2) A set-wait-mask request is received
        //
        // This is a driver for a virtual serial port. Since there is no
        // actual hardware, we complete the request with some failure code.
        //
        WDFREQUEST savedRequest;

        status = WdfIoQueueRetrieveNextRequest(
            queueContext->WaitMaskQueue,
            &savedRequest);

        if (NT_SUCCESS(status)) {
            WdfRequestComplete(savedRequest,
                STATUS_UNSUCCESSFUL);
        }

        //
        // Keep the request in a manual queue and the framework will take
        // care of cancelling them when the app exits
        //
        status = WdfRequestForwardToIoQueue(
            Request,
            queueContext->WaitMaskQueue);

        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR,
                "Error: WdfRequestForwardToIoQueue failed 0x%x", status);
            WdfRequestComplete(Request, status);
        }

        //
        // Instead of "break", use "return" to prevent the current request
        // from being completed.
        //
        return;
    }

    case IOCTL_SERIAL_SET_WAIT_MASK:
    {
        //
        // NOTE: If a wait-on-mask request is already pending when set-wait-mask
        // request is processed, the pending wait-on-event request is completed
        // with STATUS_SUCCESS and the output wait event mask is set to zero.
        //
        WDFREQUEST savedRequest;

        status = WdfIoQueueRetrieveNextRequest(
            queueContext->WaitMaskQueue,
            &savedRequest);

        if (NT_SUCCESS(status)) {

            ULONG eventMask = 0;
            status = RequestCopyFromBuffer(
                savedRequest,
                &eventMask,
                sizeof(eventMask));

            WdfRequestComplete(savedRequest, status);
        }

        //
        // NOTE: The application expects STATUS_SUCCESS for these IOCTLs.
        //
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_VSERIOUS_SET_ACTIVE:
    {
        BOOLEAN activeFlag = FALSE;
        WDFCHILDLIST list;
        VSERIOUS_PDO_IDENTIFICATION_DESCRIPTION desc;

        if (controllerContext->ComName[0] == L'\0') {
            status = STATUS_INVALID_DEVICE_STATE;
            break;
        }

        status = RequestCopyToBuffer(Request, &activeFlag, sizeof(activeFlag));
        if (!NT_SUCCESS(status)) {
            break;
        }

        if (controllerContext->Active == activeFlag) {
            status = STATUS_SUCCESS;
            break;
        }

        list = WdfFdoGetDefaultChildList(controllerContext->Controller);

        // Zero the WHOLE struct (incl. any padding) so KMDF's default
        // memcmp-based identity comparator sees a stable identity each time.
        RtlZeroMemory(&desc, sizeof(desc));
        WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&desc.Header,
            sizeof(VSERIOUS_PDO_IDENTIFICATION_DESCRIPTION));
        status = RtlStringCchCopyW(desc.ComName,
            ARRAYSIZE(desc.ComName),
            controllerContext->ComName);
        if (!NT_SUCCESS(status)) {
            break;
        }

        if (activeFlag) {
            // AddOrUpdateAsPresent is idempotent — if the description already
            // exists in the child list (from a prior Activate that we never
            // tore down) KMDF returns STATUS_OBJECT_NAME_EXISTS, which is
            // NT_SUCCESS; the existing PDO stays put. New activation requires
            // no PDO creation, eliminating the recreate-collides-with-ghost
            // failure mode.
            status = WdfChildListAddOrUpdateChildDescriptionAsPresent(
                list, &desc.Header, NULL);
        }
        else {
            // Deactivate is a flag flip only — we intentionally do NOT mark
            // the child missing. Tearing the PDO down and rebuilding it on
            // the next Activate races PnP cleanup and produces symlink /
            // SERIALCOMM / devnode collisions; keeping the PDO alive makes
            // re-Activate trivially correct. Real removal: devcon remove
            // Root\vSerious (or change the COM name, which triggers a
            // synchronous old-child cleanup in SET_COM_NAME).
            status = STATUS_SUCCESS;
        }

        if (NT_SUCCESS(status)) {
            controllerContext->Active = (activeFlag != FALSE);
        }
        break;
    }
    case IOCTL_VSERIOUS_GET_ACTIVE:
    {
        BOOLEAN active = controllerContext->Active;
        status = RequestCopyFromBuffer(Request, &active, sizeof(active));
        break;
    }

    case IOCTL_VSERIOUS_SET_COM_NAME:
    {
        WCHAR rawBuffer[COM_NAME_MAX_CCH] = { 0 };
        size_t copySize;
        UNICODE_STRING link;
        UNICODE_STRING prefix;
        UNICODE_STRING bare;

        if (controllerContext->Active) {
            status = STATUS_DEVICE_BUSY;
            break;
        }

        if (InputBufferLength < sizeof(WCHAR)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        // Leave room for the trailing null terminator regardless of input length.
        copySize = InputBufferLength < (sizeof(rawBuffer) - sizeof(WCHAR))
            ? InputBufferLength
            : (sizeof(rawBuffer) - sizeof(WCHAR));
        status = RequestCopyToBuffer(Request, rawBuffer, copySize);
        if (!NT_SUCCESS(status)) {
            break;
        }

        // Validate "COM<digit>..." shape (case-insensitive).
        if ((rawBuffer[0] != L'C' && rawBuffer[0] != L'c') ||
            (rawBuffer[1] != L'O' && rawBuffer[1] != L'o') ||
            (rawBuffer[2] != L'M' && rawBuffer[2] != L'm') ||
            rawBuffer[3] < L'0' || rawBuffer[3] > L'9') {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        RtlZeroMemory(controllerContext->SymbolicLinkBuffer,
            sizeof(controllerContext->SymbolicLinkBuffer));

        RtlInitEmptyUnicodeString(&link,
            controllerContext->SymbolicLinkBuffer,
            sizeof(controllerContext->SymbolicLinkBuffer));

        RtlInitUnicodeString(&prefix, SYMBOLIC_LINK_NAME_PREFIX);
        RtlInitUnicodeString(&bare, rawBuffer);

        status = RtlAppendUnicodeStringToString(&link, &prefix);
        if (!NT_SUCCESS(status)) {
            break;
        }
        status = RtlAppendUnicodeStringToString(&link, &bare);
        if (!NT_SUCCESS(status)) {
            break;
        }

        controllerContext->SymbolicLinkName = link;

        // If a PDO with a different ComName is still parked in the child list
        // from a prior session (we no longer tear PDOs down on Deactivate),
        // mark it missing now so PnP retires it before the new name's PDO is
        // added on the next Activate.
        if (controllerContext->ComName[0] != L'\0' &&
            wcscmp(controllerContext->ComName, rawBuffer) != 0)
        {
            WDFCHILDLIST list = WdfFdoGetDefaultChildList(controllerContext->Controller);
            VSERIOUS_PDO_IDENTIFICATION_DESCRIPTION oldDesc;
            RtlZeroMemory(&oldDesc, sizeof(oldDesc));
            WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&oldDesc.Header,
                sizeof(VSERIOUS_PDO_IDENTIFICATION_DESCRIPTION));
            (VOID)RtlStringCchCopyW(oldDesc.ComName,
                ARRAYSIZE(oldDesc.ComName),
                controllerContext->ComName);
            (VOID)WdfChildListUpdateChildDescriptionAsMissing(list, &oldDesc.Header);
        }

        status = RtlStringCchCopyW(controllerContext->ComName,
            ARRAYSIZE(controllerContext->ComName),
            rawBuffer);
        break;
    }
    case IOCTL_VSERIOUS_GET_COM_NAME:
    {
        UNICODE_STRING* link = &controllerContext->SymbolicLinkName;
        if (link->Buffer == NULL || link->Length == 0) {
            status = STATUS_INVALID_DEVICE_STATE;
            break;
        }
        status = RequestCopyFromBuffer(Request, link->Buffer, link->Length);
        break;
    }

    case IOCTL_VSERIOUS_WRITE:
    {
        // sCristina pushed bytes meant for Cristina. Drop them in HW→PC and
        // wake any Cristina ReadFile that's parked waiting for data.
        PQUEUE_CONTEXT childQc = controllerContext->ActiveChildQueue;
        WDFMEMORY memory;
        size_t inLen;
        PUCHAR inBuf;
        size_t availableData = 0;
        WDFREQUEST savedRead;

        if (childQc == NULL) {
            status = STATUS_INVALID_DEVICE_STATE;
            break;
        }
        status = WdfRequestRetrieveInputMemory(Request, &memory);
        if (!NT_SUCCESS(status)) break;
        inBuf = (PUCHAR)WdfMemoryGetBuffer(memory, &inLen);

        status = RingBufferWrite(&childQc->RingBufferHwToPc, inBuf, inLen);
        if (!NT_SUCCESS(status)) break;
        WdfRequestSetInformation(Request, inLen);

        // Drain Cristina's pending COM reads.
        RingBufferGetAvailableData(&childQc->RingBufferHwToPc, &availableData);
        if (availableData > 0) {
            for (; ; ) {
                NTSTATUS s = WdfIoQueueRetrieveNextRequest(childQc->ComReadQueue, &savedRead);
                if (!NT_SUCCESS(s)) break;
                s = WdfRequestForwardToIoQueue(savedRead, childQc->Queue);
                if (!NT_SUCCESS(s)) WdfRequestComplete(savedRead, s);
            }
        }
        break;
    }

    case IOCTL_VSERIOUS_READ:
    {
        // sCristina wants bytes Cristina wrote. Drain PC→HW; park if empty.
        PQUEUE_CONTEXT childQc = controllerContext->ActiveChildQueue;
        WDFMEMORY memory;
        size_t outLen;
        PUCHAR outBuf;
        size_t bytesCopied = 0;

        if (childQc == NULL) {
            status = STATUS_INVALID_DEVICE_STATE;
            break;
        }
        status = WdfRequestRetrieveOutputMemory(Request, &memory);
        if (!NT_SUCCESS(status)) break;
        outBuf = (PUCHAR)WdfMemoryGetBuffer(memory, &outLen);

        status = RingBufferRead(&childQc->RingBufferPcToHw, outBuf, outLen, &bytesCopied);
        if (!NT_SUCCESS(status)) break;

        if (bytesCopied > 0) {
            WdfRequestSetInformation(Request, bytesCopied);
            break;
        }

        // No data — park in the controller's own SdkReadQueue (same device,
        // intra-device forward). Child EvtIoWrite retrieves + completes
        // inline when Cristina writes.
        status = WdfRequestForwardToIoQueue(Request, controllerContext->SdkReadQueue);
        if (NT_SUCCESS(status)) {
            return;   // pending; do NOT complete here
        }
        break;
    }

    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    WdfRequestComplete(Request, status);
}


VOID
vSeriousDeviceEvtIoDeviceControl(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            OutputBufferLength,
    _In_  size_t            InputBufferLength,
    _In_  ULONG             IoControlCode
)
{
    NTSTATUS                status;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    PDEVICE_CONTEXT         deviceContext = queueContext->DeviceContext;
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    Trace(TRACE_LEVEL_INFO,
        "EvtIoDeviceControl 0x%x", IoControlCode);

    switch (IoControlCode)
    {

    case IOCTL_SERIAL_SET_BAUD_RATE:
    {
        //
        // This is a driver for a virtual serial port. Since there is no
        // actual hardware, we just store the baud rate and don't do
        // anything with it.
        //
        SERIAL_BAUD_RATE baudRateBuffer = { 0 };

        status = RequestCopyToBuffer(Request,
            &baudRateBuffer,
            sizeof(baudRateBuffer));

        if (NT_SUCCESS(status)) {
            SetBaudRate(deviceContext, baudRateBuffer.BaudRate);
        };
        break;
    }

    case IOCTL_SERIAL_GET_BAUD_RATE:
    {
        SERIAL_BAUD_RATE baudRateBuffer = { 0 };

        baudRateBuffer.BaudRate = GetBaudRate(deviceContext);

        status = RequestCopyFromBuffer(Request,
            &baudRateBuffer,
            sizeof(baudRateBuffer));
        break;
    }

    case IOCTL_SERIAL_SET_MODEM_CONTROL:
    {
        //
        // This is a driver for a virtual serial port. Since there is no
        // actual hardware, we just store the modem control register
        // configuration and don't do anything with it.
        //
        ULONG* modemControlRegister = GetModemControlRegisterPtr(deviceContext);

        ASSERT(modemControlRegister);

        status = RequestCopyToBuffer(Request,
            modemControlRegister,
            sizeof(ULONG));
        break;
    }

    case IOCTL_SERIAL_GET_MODEM_CONTROL:
    {
        ULONG* modemControlRegister = GetModemControlRegisterPtr(deviceContext);

        ASSERT(modemControlRegister);

        status = RequestCopyFromBuffer(Request,
            modemControlRegister,
            sizeof(ULONG));
        break;
    }

    case IOCTL_SERIAL_SET_FIFO_CONTROL:
    {
        //
        // This is a driver for a virtual serial port. Since there is no
        // actual hardware, we just store the FIFO control register
        // configuration and don't do anything with it.
        //
        ULONG* fifoControlRegister = GetFifoControlRegisterPtr(deviceContext);

        ASSERT(fifoControlRegister);

        status = RequestCopyToBuffer(Request,
            fifoControlRegister,
            sizeof(ULONG));
        break;
    }

    case IOCTL_SERIAL_GET_LINE_CONTROL:
    {
        status = QueueProcessGetLineControl(
            queueContext,
            Request);
        break;
    }

    case IOCTL_SERIAL_SET_LINE_CONTROL:
    {
        status = QueueProcessSetLineControl(
            queueContext,
            Request);
        break;
    }

    case IOCTL_SERIAL_GET_TIMEOUTS:
    {
        SERIAL_TIMEOUTS timeoutValues = { 0 };

        status = RequestCopyFromBuffer(Request,
            (void*)&timeoutValues,
            sizeof(timeoutValues));
        break;
    }

    case IOCTL_SERIAL_SET_TIMEOUTS:
    {
        SERIAL_TIMEOUTS timeoutValues = { 0 };

        status = RequestCopyToBuffer(Request,
            (void*)&timeoutValues,
            sizeof(timeoutValues));

        if (NT_SUCCESS(status))
        {
            if ((timeoutValues.ReadIntervalTimeout == MAXULONG) &&
                (timeoutValues.ReadTotalTimeoutMultiplier == MAXULONG) &&
                (timeoutValues.ReadTotalTimeoutConstant == MAXULONG))
            {
                status = STATUS_INVALID_PARAMETER;
            }
        }

        if (NT_SUCCESS(status)) {
            SetTimeouts(deviceContext, timeoutValues);
        }

        break;
    }

    case IOCTL_SERIAL_WAIT_ON_MASK:
    {
        //
        // NOTE: A wait-on-mask request should not be completed until either:
        //  1) A wait event occurs; or
        //  2) A set-wait-mask request is received
        //
        // This is a driver for a virtual serial port. Since there is no
        // actual hardware, we complete the request with some failure code.
        //
        WDFREQUEST savedRequest;

        status = WdfIoQueueRetrieveNextRequest(
            queueContext->WaitMaskQueue,
            &savedRequest);

        if (NT_SUCCESS(status)) {
            WdfRequestComplete(savedRequest,
                STATUS_UNSUCCESSFUL);
        }

        //
        // Keep the request in a manual queue and the framework will take
        // care of cancelling them when the app exits
        //
        status = WdfRequestForwardToIoQueue(
            Request,
            queueContext->WaitMaskQueue);

        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR,
                "Error: WdfRequestForwardToIoQueue failed 0x%x", status);
            WdfRequestComplete(Request, status);
        }

        //
        // Instead of "break", use "return" to prevent the current request
        // from being completed.
        //
        return;
    }

    case IOCTL_SERIAL_SET_WAIT_MASK:
    {
        //
        // NOTE: If a wait-on-mask request is already pending when set-wait-mask
        // request is processed, the pending wait-on-event request is completed
        // with STATUS_SUCCESS and the output wait event mask is set to zero.
        //
        WDFREQUEST savedRequest;

        status = WdfIoQueueRetrieveNextRequest(
            queueContext->WaitMaskQueue,
            &savedRequest);

        if (NT_SUCCESS(status)) {

            ULONG eventMask = 0;
            status = RequestCopyFromBuffer(
                savedRequest,
                &eventMask,
                sizeof(eventMask));

            WdfRequestComplete(savedRequest, status);
        }

        //
        // NOTE: The application expects STATUS_SUCCESS for these IOCTLs.
        //
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_SERIAL_SET_QUEUE_SIZE:
    case IOCTL_SERIAL_SET_DTR:
    case IOCTL_SERIAL_CLR_DTR:
    case IOCTL_SERIAL_SET_RTS:
    case IOCTL_SERIAL_CLR_RTS:
    case IOCTL_SERIAL_SET_XON:
    case IOCTL_SERIAL_SET_XOFF:
    case IOCTL_SERIAL_SET_CHARS:
    case IOCTL_SERIAL_SET_HANDFLOW:
    case IOCTL_SERIAL_RESET_DEVICE:
    case IOCTL_SERIAL_PURGE:
    case IOCTL_SERIAL_SET_BREAK_ON:
    case IOCTL_SERIAL_SET_BREAK_OFF:
    case IOCTL_SERIAL_CLEAR_STATS:
        //
        // .NET SerialPort.Open() fires several of these (CLR_DTR, PURGE,
        // HANDFLOW); returning STATUS_SUCCESS as a stub is enough for it to
        // proceed past the configure step.
        //
        status = STATUS_SUCCESS;
        break;

    case IOCTL_SERIAL_GET_CHARS:
    {
        // SerialPort.Open's GetCommState reads this — if the buffer isn't
        // actually written to .NET interprets the random stack bytes as the
        // current DCB and BuildDcb can throw. Zero == no special chars set.
        PVOID buf = NULL;
        size_t bufLen = 0;
        status = WdfRequestRetrieveOutputBuffer(Request,
            sizeof(SERIAL_CHARS), &buf, &bufLen);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(buf, sizeof(SERIAL_CHARS));
            WdfRequestSetInformation(Request, sizeof(SERIAL_CHARS));
        }
        break;
    }

    case IOCTL_SERIAL_GET_HANDFLOW:
    {
        // Same story — zero out so .NET reads back "no flow control", which
        // matches what set_Handshake(0) configured anyway.
        PVOID buf = NULL;
        size_t bufLen = 0;
        status = WdfRequestRetrieveOutputBuffer(Request,
            sizeof(SERIAL_HANDFLOW), &buf, &bufLen);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(buf, sizeof(SERIAL_HANDFLOW));
            WdfRequestSetInformation(Request, sizeof(SERIAL_HANDFLOW));
        }
        break;
    }

    case IOCTL_SERIAL_GET_COMMSTATUS:
    {
        // SerialPort.Read/BytesToRead consults this. Report no errors, no
        // hold reasons, no data pending in either queue. Caller's buffer is
        // sized to sizeof(SERIAL_STATUS).
        PVOID buf = NULL;
        size_t bufLen = 0;
        status = WdfRequestRetrieveOutputBuffer(Request,
            sizeof(SERIAL_STATUS), &buf, &bufLen);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(buf, sizeof(SERIAL_STATUS));
            WdfRequestSetInformation(Request, sizeof(SERIAL_STATUS));
        }
        break;
    }

    case IOCTL_SERIAL_GET_MODEMSTATUS:
    case IOCTL_SERIAL_GET_DTRRTS:
    case IOCTL_SERIAL_GET_COMMCONFIG:
    {
        // Each returns a ULONG; zero is a fine default ("no modem signals,
        // no DTR/RTS asserted").
        PVOID buf = NULL;
        size_t bufLen = 0;
        status = WdfRequestRetrieveOutputBuffer(Request,
            sizeof(ULONG), &buf, &bufLen);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(buf, sizeof(ULONG));
            WdfRequestSetInformation(Request, sizeof(ULONG));
        }
        break;
    }

    case IOCTL_SERIAL_GET_PROPERTIES:
    {
        // SerialPort.Open issues this to learn the port's capabilities. We
        // advertise generous buffers and "any" settings — the call won't
        // open without it on some Windows builds.
        PVOID buf = NULL;
        size_t bufLen = 0;
        status = WdfRequestRetrieveOutputBuffer(Request,
            sizeof(SERIAL_COMMPROP), &buf, &bufLen);
        if (NT_SUCCESS(status)) {
            PSERIAL_COMMPROP p = (PSERIAL_COMMPROP)buf;
            RtlZeroMemory(p, sizeof(SERIAL_COMMPROP));
            p->PacketLength       = sizeof(SERIAL_COMMPROP);
            p->PacketVersion      = 2;
            p->ServiceMask        = SERIAL_SP_SERIALCOMM;
            p->MaxTxQueue         = 4096;
            p->MaxRxQueue         = 4096;
            p->MaxBaud            = SERIAL_BAUD_USER;
            p->ProvSubType        = SERIAL_SP_RS232;
            p->ProvCapabilities   = SERIAL_PCF_DTRDSR | SERIAL_PCF_RTSCTS
                                  | SERIAL_PCF_CD     | SERIAL_PCF_PARITY_CHECK
                                  | SERIAL_PCF_XONXOFF| SERIAL_PCF_TOTALTIMEOUTS
                                  | SERIAL_PCF_INTTIMEOUTS;
            p->SettableParams     = SERIAL_SP_BAUD | SERIAL_SP_DATABITS
                                  | SERIAL_SP_PARITY | SERIAL_SP_STOPBITS
                                  | SERIAL_SP_HANDSHAKING;
            p->SettableBaud       = SERIAL_BAUD_USER;
            p->SettableData       = SERIAL_DATABITS_5 | SERIAL_DATABITS_6
                                  | SERIAL_DATABITS_7 | SERIAL_DATABITS_8;
            p->SettableStopParity = SERIAL_STOPBITS_10 | SERIAL_STOPBITS_15
                                  | SERIAL_STOPBITS_20
                                  | SERIAL_PARITY_NONE | SERIAL_PARITY_ODD
                                  | SERIAL_PARITY_EVEN | SERIAL_PARITY_MARK
                                  | SERIAL_PARITY_SPACE;
            p->CurrentTxQueue     = 0;
            p->CurrentRxQueue     = 0;
            WdfRequestSetInformation(Request, sizeof(SERIAL_COMMPROP));
        }
        break;
    }

    default:
        Trace(TRACE_LEVEL_ERROR,
            "vSerious: unhandled IOCTL 0x%x (returning INVALID_PARAMETER)",
            IoControlCode);
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    WdfRequestComplete(Request, status);
}


VOID
vSeriousDeviceEvtIoWrite(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            Length
)
{
    NTSTATUS                status;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    PDEVICE_CONTEXT         deviceContext;
    WDFMEMORY               memory;
    WDFREQUEST              savedRequest;
    size_t                  availableData = 0;

    Trace(TRACE_LEVEL_INFO, "EvtIoWrite 0x%p", Request);

    deviceContext = queueContext->DeviceContext;

    status = WdfRequestRetrieveInputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfRequestRetrieveInputMemory failed 0x%x", status);
        WdfRequestComplete(Request, status);
        return;
    }

    // Cristina's COM port writes land in the PC→HW buffer (drained by
    // sCristina via IOCTL_VSERIOUS_READ).
    status = QueueProcessWriteBytes(
        queueContext,
        (PUCHAR)WdfMemoryGetBuffer(memory, NULL),
        Length);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    WdfRequestCompleteWithInformation(Request, status, Length);

    // Wake any sCristina reads parked in the controller's SdkReadQueue.
    // Retrieve + complete inline so we never forward across devices.
    {
        WDFDEVICE parent = WdfPdoGetParent(queueContext->DeviceContext->Device);
        PCONTROLLER_CONTEXT cc = parent ? GetControllerContext(parent) : NULL;
        WDFQUEUE sdkReadQueue = cc ? cc->SdkReadQueue : NULL;
        if (sdkReadQueue == NULL) return;

        RingBufferGetAvailableData(&queueContext->RingBufferPcToHw, &availableData);
        while (availableData > 0) {
            WDFMEMORY readMem;
            size_t readLen = 0;
            PUCHAR readBuf;
            size_t bytesCopied = 0;

            status = WdfIoQueueRetrieveNextRequest(sdkReadQueue, &savedRequest);
            if (!NT_SUCCESS(status)) break;

            status = WdfRequestRetrieveOutputMemory(savedRequest, &readMem);
            if (!NT_SUCCESS(status)) {
                WdfRequestComplete(savedRequest, status);
                RingBufferGetAvailableData(&queueContext->RingBufferPcToHw, &availableData);
                continue;
            }
            readBuf = (PUCHAR)WdfMemoryGetBuffer(readMem, &readLen);

            status = RingBufferRead(&queueContext->RingBufferPcToHw,
                readBuf, readLen, &bytesCopied);
            if (!NT_SUCCESS(status)) {
                WdfRequestComplete(savedRequest, status);
                break;
            }
            if (bytesCopied > 0) {
                WdfRequestCompleteWithInformation(savedRequest, STATUS_SUCCESS, bytesCopied);
            }
            else {
                // Buffer drained between the get-available and the read.
                // Re-park on the controller's queue (same-device forward).
                (VOID)WdfRequestForwardToIoQueue(savedRequest, sdkReadQueue);
                break;
            }
            RingBufferGetAvailableData(&queueContext->RingBufferPcToHw, &availableData);
        }
    }
}


VOID
vSeriousDeviceEvtIoRead(
    _In_  WDFQUEUE          Queue,
    _In_  WDFREQUEST        Request,
    _In_  size_t            Length
)
{
    NTSTATUS                status;
    PQUEUE_CONTEXT          queueContext = GetQueueContext(Queue);
    PDEVICE_CONTEXT         deviceContext;
    WDFMEMORY               memory;
    size_t                  bytesCopied = 0;

    Trace(TRACE_LEVEL_INFO,
        "EvtIoRead 0x%p", Request);

    deviceContext = queueContext->DeviceContext;

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        Trace(TRACE_LEVEL_ERROR,
            "Error: WdfRequestRetrieveOutputMemory failed 0x%x", status);
        WdfRequestComplete(Request, status);
        return;
    }

    // Cristina's COM port reads drain the HW→PC buffer (filled by sCristina
    // via IOCTL_VSERIOUS_WRITE).
    status = RingBufferRead(&queueContext->RingBufferHwToPc,
        (BYTE*)WdfMemoryGetBuffer(memory, NULL),
        Length,
        &bytesCopied);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    if (bytesCopied > 0) {
        WdfRequestCompleteWithInformation(Request, status, bytesCopied);
        return;
    }
    else {
        status = WdfRequestForwardToIoQueue(Request, queueContext->ComReadQueue);
        if (!NT_SUCCESS(status)) {
            Trace(TRACE_LEVEL_ERROR, "Error: forward COM read failed 0x%x", status);
            WdfRequestComplete(Request, status);
        }
    }
}


NTSTATUS
QueueProcessWriteBytes(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_reads_bytes_(Length)
    PUCHAR            Characters,
    _In_  size_t            Length
)
{
    NTSTATUS                status = STATUS_SUCCESS;
    UCHAR                   currentCharacter;
    UCHAR                   connectString[] = "\r\nCONNECT\r\n";
    UCHAR                   connectStringCch = ARRAY_SIZE(connectString) - 1;
    UCHAR                   okString[] = "\r\nOK\r\n";
    UCHAR                   okStringCch = ARRAY_SIZE(okString) - 1;

    while (Length != 0) {

        currentCharacter = *(Characters++);
        Length--;

        if (currentCharacter == '\0') {
            continue;
        }

        status = RingBufferWrite(&QueueContext->RingBufferPcToHw,
            &currentCharacter,
            sizeof(currentCharacter));
        if (!NT_SUCCESS(status)) {
            return status;
        }

        switch (QueueContext->CommandMatchState) {

        case COMMAND_MATCH_STATE_IDLE:

            if ((currentCharacter == 'a') || (currentCharacter == 'A')) {
                //
                //  got an A
                //
                QueueContext->CommandMatchState = COMMAND_MATCH_STATE_GOT_A;
                QueueContext->ConnectCommand = FALSE;
                QueueContext->IgnoreNextChar = FALSE;
            }
            break;

        case COMMAND_MATCH_STATE_GOT_A:

            if ((currentCharacter == 't') || (currentCharacter == 'T')) {
                //
                //  got a T
                //
                QueueContext->CommandMatchState = COMMAND_MATCH_STATE_GOT_T;
            }
            else {
                QueueContext->CommandMatchState = COMMAND_MATCH_STATE_IDLE;
            }

            break;

        case COMMAND_MATCH_STATE_GOT_T:

            if (!QueueContext->IgnoreNextChar) {
                //
                //  the last char was not a special char
                //  check for CONNECT command
                //
                if ((currentCharacter == 'A') || (currentCharacter == 'a')) {
                    QueueContext->ConnectCommand = TRUE;
                }

                if ((currentCharacter == 'D') || (currentCharacter == 'd')) {
                    QueueContext->ConnectCommand = TRUE;
                }
            }

            QueueContext->IgnoreNextChar = TRUE;

            if (currentCharacter == '\r') {
                //
                //  got a CR, send a response to the command
                //
                QueueContext->CommandMatchState = COMMAND_MATCH_STATE_IDLE;

                if (QueueContext->ConnectCommand) {
                    //
                    //  place <cr><lf>CONNECT<cr><lf>  in the buffer
                    //
                    status = RingBufferWrite(&QueueContext->RingBufferPcToHw,
                        connectString,
                        connectStringCch);
                    if (!NT_SUCCESS(status)) {
                        return status;
                    }
                    //
                    //  connected now raise CD
                    //
                    QueueContext->CurrentlyConnected = TRUE;
                    QueueContext->ConnectionStateChanged = TRUE;
                }
                else {
                    //
                    //  place <cr><lf>OK<cr><lf>  in the buffer
                    //
                    status = RingBufferWrite(&QueueContext->RingBufferPcToHw,
                        okString,
                        okStringCch);
                    if (!NT_SUCCESS(status)) {
                        return status;
                    }
                }
            }
            break;

        default:
            break;
        }
    }
    return status;
}


NTSTATUS
QueueProcessGetLineControl(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
)
{
    NTSTATUS                status;
    PDEVICE_CONTEXT         deviceContext;
    SERIAL_LINE_CONTROL     lineControl = { 0 };
    ULONG                   lineControlSnapshot;
    ULONG* lineControlRegister;

    deviceContext = QueueContext->DeviceContext;
    lineControlRegister = GetLineControlRegisterPtr(deviceContext);

    ASSERT(lineControlRegister);

    //
    // Take a snapshot of the line control register variable
    //
    lineControlSnapshot = ReadNoFence((LONG*)lineControlRegister);

    //
    // Decode the word length
    //
    if ((lineControlSnapshot & SERIAL_DATA_MASK) == SERIAL_5_DATA)
    {
        lineControl.WordLength = 5;
    }
    else if ((lineControlSnapshot & SERIAL_DATA_MASK) == SERIAL_6_DATA)
    {
        lineControl.WordLength = 6;
    }
    else if ((lineControlSnapshot & SERIAL_DATA_MASK) == SERIAL_7_DATA)
    {
        lineControl.WordLength = 7;
    }
    else if ((lineControlSnapshot & SERIAL_DATA_MASK) == SERIAL_8_DATA)
    {
        lineControl.WordLength = 8;
    }

    //
    // Decode the parity
    //
    if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_NONE_PARITY)
    {
        lineControl.Parity = NO_PARITY;
    }
    else if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_ODD_PARITY)
    {
        lineControl.Parity = ODD_PARITY;
    }
    else if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_EVEN_PARITY)
    {
        lineControl.Parity = EVEN_PARITY;
    }
    else if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_MARK_PARITY)
    {
        lineControl.Parity = MARK_PARITY;
    }
    else if ((lineControlSnapshot & SERIAL_PARITY_MASK) == SERIAL_SPACE_PARITY)
    {
        lineControl.Parity = SPACE_PARITY;
    }

    //
    // Decode the length of the stop bit
    //
    if (lineControlSnapshot & SERIAL_2_STOP)
    {
        if (lineControl.WordLength == 5)
        {
            lineControl.StopBits = STOP_BITS_1_5;
        }
        else
        {
            lineControl.StopBits = STOP_BITS_2;
        }
    }
    else
    {
        lineControl.StopBits = STOP_BIT_1;
    }

    //
    // Copy the information that was decoded to the caller's buffer
    //
    status = RequestCopyFromBuffer(Request,
        (void*)&lineControl,
        sizeof(lineControl));
    return status;
}


NTSTATUS
QueueProcessSetLineControl(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
)
{
    NTSTATUS                status;
    PDEVICE_CONTEXT         deviceContext;
    SERIAL_LINE_CONTROL     lineControl = { 0 };
    ULONG* lineControlRegister;
    UCHAR                   lineControlData = 0;
    UCHAR                   lineControlStop = 0;
    UCHAR                   lineControlParity = 0;
    ULONG                   lineControlSnapshot;
    ULONG                   lineControlNew;
    ULONG                   lineControlPrevious;
    ULONG                   i;

    deviceContext = QueueContext->DeviceContext;
    lineControlRegister = GetLineControlRegisterPtr(deviceContext);

    ASSERT(lineControlRegister);

    status = RequestCopyToBuffer(Request,
        (void*)&lineControl,
        sizeof(lineControl));

    //
    // Bits 0 and 1 of the line control register
    //
    if (NT_SUCCESS(status))
    {
        switch (lineControl.WordLength)
        {
        case 5:
            lineControlData = SERIAL_5_DATA;
            SetValidDataMask(deviceContext, 0x1f);
            break;

        case 6:
            lineControlData = SERIAL_6_DATA;
            SetValidDataMask(deviceContext, 0x3f);
            break;

        case 7:
            lineControlData = SERIAL_7_DATA;
            SetValidDataMask(deviceContext, 0x7f);
            break;

        case 8:
            lineControlData = SERIAL_8_DATA;
            SetValidDataMask(deviceContext, 0xff);
            break;

        default:
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    //
    // Bit 2 of the line control register
    //
    if (NT_SUCCESS(status))
    {
        switch (lineControl.StopBits)
        {
        case STOP_BIT_1:
            lineControlStop = SERIAL_1_STOP;
            break;

        case STOP_BITS_1_5:
            if (lineControlData != SERIAL_5_DATA)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            lineControlStop = SERIAL_1_5_STOP;
            break;

        case STOP_BITS_2:
            if (lineControlData == SERIAL_5_DATA)
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            lineControlStop = SERIAL_2_STOP;
            break;

        default:
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    //
    // Bits 3, 4 and 5 of the line control register
    //
    if (NT_SUCCESS(status))
    {
        switch (lineControl.Parity)
        {
        case NO_PARITY:
            lineControlParity = SERIAL_NONE_PARITY;
            break;

        case EVEN_PARITY:
            lineControlParity = SERIAL_EVEN_PARITY;
            break;

        case ODD_PARITY:
            lineControlParity = SERIAL_ODD_PARITY;
            break;

        case SPACE_PARITY:
            lineControlParity = SERIAL_SPACE_PARITY;
            break;

        case MARK_PARITY:
            lineControlParity = SERIAL_MARK_PARITY;
            break;

        default:
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }

    //
    // Update our line control register variable atomically
    //
    i = 0;
    do {
        i++;
        if ((i & 0xf) == 0) {
            //
            // We've been spinning in a loop for a while trying to
            // update the line control register variable atomically.
            // Yield the CPU for other threads for a while.
            //
            LARGE_INTEGER   interval;
            interval.QuadPart = 0;
            KeDelayExecutionThread(UserMode, FALSE, &interval);
        }

        lineControlSnapshot = ReadNoFence((LONG*)lineControlRegister);

        lineControlNew = (lineControlSnapshot & SERIAL_LCR_BREAK) |
            (lineControlData | lineControlParity | lineControlStop);

        lineControlPrevious = InterlockedCompareExchange(
            (LONG*)lineControlRegister,
            lineControlNew,
            lineControlSnapshot);

    } while (lineControlPrevious != lineControlSnapshot);

    return status;
}