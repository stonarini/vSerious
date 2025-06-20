#pragma once

#define SYMBOLIC_LINK_NAME_LENGTH   32
#define SYMBOLIC_LINK_NAME_PREFIX   L"\\DosDevices\\"
#define REG_PATH_DEVICEMAP          L"HARDWARE\\DEVICEMAP"
#define SERIAL_DEVICE_MAP           L"SERIALCOMM"
#define REG_VALUENAME_PORTNAME      L"PortName"
#define REG_PATH_SERIALCOMM         REG_PATH_DEVICEMAP L"\\" SERIAL_DEVICE_MAP

typedef struct _DEVICE_CONTEXT
{
    WDFDEVICE       Device;

    ULONG           BaudRate;

    ULONG           ModemControlRegister;

    ULONG           FifoControlRegister;

    ULONG           LineControlRegister;

    UCHAR           ValidDataMask;

    SERIAL_TIMEOUTS Timeouts;

    BOOLEAN         CreatedLegacyHardwareKey;

    WDFMEMORY       PdoNameMemory;

    PWSTR           PdoName;

} DEVICE_CONTEXT, * PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);

NTSTATUS
DeviceGetPdoName(
    _In_  PDEVICE_CONTEXT   DeviceContext
);

NTSTATUS
DeviceWriteLegacyHardwareKey(
    _In_  PWSTR             ComPort,
    _In_  WDFDEVICE         Device
);

NTSTATUS
DevicePlugIn(
    _In_ PCONTROLLER_CONTEXT ControllerContext,
    _Out_ PDEVICE_CONTEXT* OutDeviceContext
);

NTSTATUS
DeviceUnplug(
    _In_ PCONTROLLER_CONTEXT ControllerContext
);

ULONG
GetBaudRate(
    _In_  PDEVICE_CONTEXT   DeviceContext
);

VOID
SetBaudRate(
    _In_  PDEVICE_CONTEXT   DeviceContext,
    _In_  ULONG             BaudRate
);

ULONG*
GetModemControlRegisterPtr(
    _In_  PDEVICE_CONTEXT   DeviceContext
);

ULONG*
GetFifoControlRegisterPtr(
    _In_  PDEVICE_CONTEXT   DeviceContext
);

ULONG*
GetLineControlRegisterPtr(
    _In_  PDEVICE_CONTEXT   DeviceContext
);

VOID
SetValidDataMask(
    _In_  PDEVICE_CONTEXT   DeviceContext,
    _In_  UCHAR             Mask
);

VOID
SetTimeouts(
    _In_  PDEVICE_CONTEXT   DeviceContext,
    _In_  SERIAL_TIMEOUTS   Timeouts
);

VOID
GetTimeouts(
    _In_  PDEVICE_CONTEXT   DeviceContext,
    _Out_ SERIAL_TIMEOUTS* Timeouts
);