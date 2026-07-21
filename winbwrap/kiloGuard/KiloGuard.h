#pragma once

#include <ntdef.h>

/* =========================
   Debug logging — variadic, DEBUG only, captured by DebugView
   ========================= */

#ifdef DEBUG
#define KG_LOG(fmt, ...) DbgPrint(fmt, __VA_ARGS__)
#else
#define KG_LOG(fmt, ...) ((VOID)0)
#endif

#define KG_NET_ALLOW    0
#define KG_NET_BLOCK    1

extern UCHAR gPidNetCache[65536];
extern USHORT gPidSlotFast[65536];
extern UCHAR gBwrapFast[65536];
