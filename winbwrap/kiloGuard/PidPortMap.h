#pragma once

#include <ntddk.h>

#define KG_NET_ALLOW    0
#define KG_NET_BLOCK    1
#define KG_MAX_NET_EXE  64



typedef struct _KG_POLICY_DOMAIN KG_POLICY_DOMAIN;

extern UCHAR gPidNetCache[65536];

VOID KgSetPidNetCache(HANDLE pid, UCHAR status);
BOOLEAN KgIsNetExeInList(KG_POLICY_DOMAIN* domain, PUNICODE_STRING imagePath);
NTSTATUS KgRegisterNetCallbacks(PDEVICE_OBJECT deviceObject);
VOID KgUnregisterNetCallbacks(VOID);
