#pragma once

#include <fltKernel.h>

NTSTATUS KgRegisterRegCallbacks(PDRIVER_OBJECT DriverObject);
VOID KgUnregisterRegCallbacks(VOID);
