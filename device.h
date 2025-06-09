#pragma once

#define SYMBOLIC_LINK_NAME_LENGTH   32
#define SYMBOLIC_LINK_NAME_PREFIX   L"\\DosDevices\\Global\\"
#define REG_PATH_DEVICEMAP          L"HARDWARE\\DEVICEMAP"
#define SERIAL_DEVICE_MAP           L"SERIALCOMM"
#define REG_VALUENAME_PORTNAME      L"PortName"
#define REG_PATH_SERIALCOMM         REG_PATH_DEVICEMAP L"\\" SERIAL_DEVICE_MAP

#define IOCTL_VSERIOUS_SET_ACTIVE CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_VSERIOUS_GET_ACTIVE CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x801, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_VSERIOUS_SET_COM_NAME CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_VSERIOUS_GET_COM_NAME CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x803, METHOD_BUFFERED, FILE_READ_DATA)


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

    PWSTR           PdoName;

    BOOLEAN         Active;

    UNICODE_STRING  SymbolicLinkName;

    WCHAR SymbolicLinkBuffer[64];

} DEVICE_CONTEXT, * PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);


NTSTATUS
DeviceCreate(
    _In_  WDFDRIVER         Driver,
    _In_  PWDFDEVICE_INIT   DeviceInit,
    _Out_ PDEVICE_CONTEXT* DeviceContext
);

NTSTATUS
DeviceConfigure(
    _In_  PDEVICE_CONTEXT   DeviceContext
);

NTSTATUS
DeviceGetPdoName(
    _In_  PDEVICE_CONTEXT   DeviceContext
);

NTSTATUS
DeviceWriteLegacyHardwareKey(
    _In_  PWSTR             PdoName,
    _In_  PWSTR             ComPort,
    _In_  WDFDEVICE         Device
);

EVT_WDF_DEVICE_CONTEXT_CLEANUP  vSeriousEvtDeviceCleanup;

NTSTATUS
DevicePlugIn(
    _In_ PDEVICE_CONTEXT DeviceContext
);

NTSTATUS
DeviceUnplug(
    _In_ PDEVICE_CONTEXT DeviceContext
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