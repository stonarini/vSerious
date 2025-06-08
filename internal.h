#pragma once

#include <initguid.h>
#include <ntddk.h>
#include <wdf.h>
#include <ntddser.h>

#define _NTDEF_

#include "serial.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD vSeriousEvtDeviceAdd;

#include "device.h"
#include "ringbuffer.h"
#include "queue.h"

#define Trace(level, _fmt_, ...)                    \
    DbgPrintEx(DPFLTR_DEFAULT_ID, level,            \
                _fmt_ "\n", __VA_ARGS__)

#define TRACE_LEVEL_ERROR   DPFLTR_ERROR_LEVEL
#define TRACE_LEVEL_INFO    DPFLTR_INFO_LEVEL
