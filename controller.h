#pragma once

#define CONTROLLER_SYMBOLIC_LINK   L"\\DosDevices\\vSerious"

#define IOCTL_VSERIOUS_SET_ACTIVE CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_VSERIOUS_GET_ACTIVE CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x801, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_VSERIOUS_SET_COM_NAME CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_VSERIOUS_GET_COM_NAME CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x803, METHOD_BUFFERED, FILE_READ_DATA)
// SDK-side data path. Cristina opens \\.\COMx and uses ReadFile/WriteFile;
// sCristina opens \\.\vSerious and uses these IOCTLs. Buffers are separate
// per-direction in the child PDO's QUEUE_CONTEXT, so writes never echo into
// their own reads and the two endpoints don't race.
#define IOCTL_VSERIOUS_READ  CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x804, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_VSERIOUS_WRITE CTL_CODE(FILE_DEVICE_SERIAL_PORT, 0x805, METHOD_BUFFERED, FILE_WRITE_DATA)

#define COM_NAME_MAX_CCH 16

// Forward-declare to avoid pulling queue.h here (queue.h pulls device.h
// which pulls this).
struct _QUEUE_CONTEXT;

typedef struct _CONTROLLER_CONTEXT
{
    WDFDEVICE       Controller;

    BOOLEAN         Active;

    UNICODE_STRING  SymbolicLinkName;

    WCHAR SymbolicLinkBuffer[64];

    WCHAR ComName[COM_NAME_MAX_CCH];

    // Set by QueueCreateDevice on the active child's queue context so
    // controller-side IOCTL_VSERIOUS_READ / IOCTL_VSERIOUS_WRITE can route to
    // the child PDO's per-direction ring buffers without iterating the list.
    struct _QUEUE_CONTEXT* ActiveChildQueue;

    // Manual queue for parked IOCTL_VSERIOUS_READ requests. Lives on the
    // controller so that parking is same-device (no cross-device forwarding,
    // which has bitten us with a SYSTEM_SERVICE_EXCEPTION). The child's
    // EvtIoWrite drains it inline (retrieve + complete) when new PC→HW bytes
    // arrive — no forwarding required.
    WDFQUEUE SdkReadQueue;

} CONTROLLER_CONTEXT, * PCONTROLLER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROLLER_CONTEXT, GetControllerContext);

typedef struct _VSERIOUS_PDO_IDENTIFICATION_DESCRIPTION
{
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header;
    WCHAR ComName[COM_NAME_MAX_CCH];

} VSERIOUS_PDO_IDENTIFICATION_DESCRIPTION,
* PVSERIOUS_PDO_IDENTIFICATION_DESCRIPTION;

NTSTATUS
ControllerCreate(
    _In_  WDFDRIVER         Driver,
    _In_  PWDFDEVICE_INIT   DeviceInit,
    _Out_ PCONTROLLER_CONTEXT*  ControllerContext
);
