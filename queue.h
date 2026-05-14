#pragma once

#define DATA_BUFFER_SIZE 1024

#define COMMAND_MATCH_STATE_IDLE   0
#define COMMAND_MATCH_STATE_GOT_A  1
#define COMMAND_MATCH_STATE_GOT_T  2

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

#define MAXULONG    0xffffffff

typedef struct _QUEUE_CONTEXT
{
    UCHAR           CommandMatchState;

    BOOLEAN         ConnectCommand;

    BOOLEAN         IgnoreNextChar;

    BOOLEAN         ConnectionStateChanged;

    BOOLEAN         CurrentlyConnected;

    // Direction-separated buffers. Single-buffer loopback breaks two-app
    // scenarios (sCristina emulating hw while Cristina is the PC) — each
    // side's write was echoing into its own read, plus both ends raced for
    // the same data. PcToHw carries Cristina→sCristina; HwToPc carries
    // sCristina→Cristina. Only the COM port queue context actually uses
    // these; the controller queue context inherits the type for convenience
    // but routes through its child's queue context.
    RING_BUFFER     RingBufferPcToHw;
    BYTE            BufferPcToHw[DATA_BUFFER_SIZE];

    RING_BUFFER     RingBufferHwToPc;
    BYTE            BufferHwToPc[DATA_BUFFER_SIZE];

    WDFQUEUE        Queue;              // Default parallel queue

    WDFQUEUE        ComReadQueue;       // Pending Cristina reads (\\.\COMx)

    WDFQUEUE        WaitMaskQueue;      // Pending ioctl wait-on-mask

    PDEVICE_CONTEXT DeviceContext;

    PCONTROLLER_CONTEXT ControllerContext;

} QUEUE_CONTEXT, * PQUEUE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QUEUE_CONTEXT, GetQueueContext);

EVT_WDF_IO_QUEUE_IO_READ            vSeriousDeviceEvtIoRead;
EVT_WDF_IO_QUEUE_IO_WRITE           vSeriousDeviceEvtIoWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL  vSeriousDeviceEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL  vSeriousControllerEvtIoDeviceControl;

NTSTATUS
QueueCreateDevice(
    _In_  PDEVICE_CONTEXT   DeviceContext
);

NTSTATUS
QueueCreateController(
    _In_  PCONTROLLER_CONTEXT   ControllerContext
);

NTSTATUS
QueueProcessWriteBytes(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_reads_bytes_(Length)
    PUCHAR            Characters,
    _In_  size_t            Length
);

NTSTATUS
QueueProcessGetLineControl(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
);

NTSTATUS
QueueProcessSetLineControl(
    _In_  PQUEUE_CONTEXT    QueueContext,
    _In_  WDFREQUEST        Request
);

NTSTATUS
RequestCopyFromBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             SourceBuffer,
    _In_  size_t            NumBytesToCopyFrom
);

NTSTATUS
RequestCopyToBuffer(
    _In_  WDFREQUEST        Request,
    _In_  PVOID             DestinationBuffer,
    _In_  size_t            NumBytesToCopyTo
);
