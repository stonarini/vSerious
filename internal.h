#pragma once

#include <initguid.h>
#include <ntddk.h>
#include <wdf.h>
#include <ntddser.h>
#include <devguid.h>
#include "serial.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD vSeriousEvtDeviceAdd;

// Set at DriverEntry from HKLM\SYSTEM\CCS\Services\vSerious\Parameters\CompatMode.
// When TRUE, child PDOs advertise as FTDI FT231X (VID_0403+PID_6015+CRxxxxxx) so
// third-party apps that key off Win32_PnPEntity.DeviceID (Bosch Cristina, etc.)
// can discover the virtual port without binary-patching.
extern BOOLEAN g_CompatMode;

typedef struct _CONTROLLER_CONTEXT CONTROLLER_CONTEXT, * PCONTROLLER_CONTEXT;

#include "device.h"
#include "controller.h"
#include "ringbuffer.h"
#include "queue.h"

#define Trace(level, _fmt_, ...)                    \
    DbgPrintEx(DPFLTR_DEFAULT_ID, level,            \
                _fmt_ "\n", __VA_ARGS__)

#define TRACE_LEVEL_ERROR   DPFLTR_ERROR_LEVEL
#define TRACE_LEVEL_INFO    DPFLTR_INFO_LEVEL
