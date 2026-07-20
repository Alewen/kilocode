#pragma once

#include <ntddk.h>

#define KG_MAX_NET_EXE  64

typedef struct _KG_POLICY_DOMAIN KG_POLICY_DOMAIN;

VOID KgSetPidNetCache(HANDLE pid, UCHAR status);
BOOLEAN KgIsNetExeInList(KG_POLICY_DOMAIN* domain, PUNICODE_STRING imagePath);
NTSTATUS KgRegisterNetCallbacks(PDEVICE_OBJECT deviceObject);
VOID KgUnregisterNetCallbacks(VOID);
