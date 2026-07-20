
#include <ntddk.h>
#include <fwpmk.h>
#include "KiloGuard.h"
#include "PidPortMap.h"

#define FWPS_CLASSIFY_OUT_FLAG_ABSORB 0x00000001

ULONG KgIsPidInSandBox(HANDLE pid);
VOID KgPushNetDenyEvent(ULONG slotIndex, HANDLE pid, PCUNICODE_STRING infoStr);

static WCHAR* KgUlongToWchar(ULONG val, WCHAR* end)
{ WCHAR t[12];ULONG i=0;if(val==0){*end++=L'0';return end;}while(val>0){t[i++]=(WCHAR)(L'0'+(val%10));val/=10;}while(i>0)*end++=t[--i];return end;}
static void KgFormatIpV4(WCHAR* b,ULONG a)
{b=KgUlongToWchar((a>>24)&0xFF,b);*b++=L'.';b=KgUlongToWchar((a>>16)&0xFF,b);*b++=L'.';b=KgUlongToWchar((a>>8)&0xFF,b);*b++=L'.';b=KgUlongToWchar(a&0xFF,b);*b=0;}
static void KgFormatIpV6(WCHAR* b,const UINT16*a6)
{const WCHAR h[]=L"0123456789abcdef";for(int i=0;i<7;i++){*b++=h[(a6[i]>>12)&0xF];*b++=h[(a6[i]>>8)&0xF];*b++=h[(a6[i]>>4)&0xF];*b++=h[a6[i]&0xF];*b++=L':';}*b++=h[(a6[7]>>12)&0xF];*b++=h[(a6[7]>>8)&0xF];*b++=h[(a6[7]>>4)&0xF];*b=h[a6[7]&0xF];}

typedef struct _FWPS_INCOMING_VALUE0{FWP_VALUE0 value;}FWPS_INCOMING_VALUE0;
typedef struct _FWPS_INCOMING_VALUES0{UINT16 layerId;UINT32 valueCount;FWPS_INCOMING_VALUE0*incomingValue;}FWPS_INCOMING_VALUES0;
typedef struct _FWPS_INCOMING_METADATA_VALUES0{UINT32 currentMetadataValues;UINT32 flags;UINT64 reserved;UINT8 d[16];UINT64 flowHandle;UINT32 ipHeaderSize;UINT32 transportHeaderSize;void*processPath;UINT64 token;UINT64 processId;}FWPS_INCOMING_METADATA_VALUES0;
typedef struct _FWPS_ACTION0{UINT32 type;UINT32 calloutId;}FWPS_ACTION0;
typedef struct _FWPS_FILTER3{UINT64 filterId;UINT64 weight;UINT16 subLayerWeight;UINT16 flags;UINT32 numFilterConditions;void*filterCondition;FWPS_ACTION0 action;UINT64 context;GUID providerKey;GUID layerKey;GUID subLayerKey;}FWPS_FILTER3;
typedef struct _FWPS_CLASSIFY_OUT0{UINT32 actionType;UINT32 _a;UINT64 outContext;UINT64 filterId;UINT32 rights;UINT32 flags;UINT32 reserved;}FWPS_CLASSIFY_OUT0;
typedef struct _FWPS_CALLOUT3{GUID calloutKey;UINT32 flags;void*classifyFn;void*notifyFn;void*flowDeleteFn;}FWPS_CALLOUT3;

NTSTATUS FwpsCalloutRegister3(PDEVICE_OBJECT,const FWPS_CALLOUT3*,UINT32*);
NTSTATUS FwpsCalloutUnregisterById0(UINT32);

#define FWPS_LAYER_ALE_RESOURCE_ASSIGNMENT_V4       36 // 38
#define FWPS_LAYER_ALE_RESOURCE_ASSIGNMENT_V6       38 // 39
#define FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V4          44
#define FWPS_LAYER_ALE_AUTH_RECV_ACCEPT_V6          46
#define FWPS_LAYER_ALE_AUTH_CONNECT_V4              48
#define FWPS_LAYER_ALE_AUTH_CONNECT_V6              50
#define FWPS_LAYER_OUTBOUND_TRANSPORT_V4            16
#define FWPS_LAYER_OUTBOUND_TRANSPORT_V6            18 // 17

#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_ADDRESS    6
#define FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_PORT        4
#define FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_REMOTE_ADDRESS    6
#define FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_LOCAL_PORT        4
#define FWPS_FIELD_OUTBOUND_TRANSPORT_V4_IP_LOCAL_PORT      4
#define FWPS_FIELD_OUTBOUND_TRANSPORT_V4_IP_REMOTE_ADDRESS  3
#define FWPS_FIELD_OUTBOUND_TRANSPORT_V6_IP_REMOTE_ADDRESS  3

#define RPC_C_AUTHN_WINNT 10

#define INITGUID
#include <guiddef.h>

DEFINE_GUID(FWPM_LAYER_ALE_AUTH_CONNECT_V4,0xc38d57d1,0x05a7,0x4c33,0x90,0x4f,0x7f,0xbc,0xee,0xe6,0x0e,0x82);
DEFINE_GUID(FWPM_LAYER_ALE_AUTH_CONNECT_V6,0x4a72393b,0x319f,0x44bc,0x84,0xc3,0xba,0x54,0xdc,0xb3,0xb6,0xb4);
DEFINE_GUID(FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4,0x1247d66d,0x0b60,0x4a15,0x8d,0x44,0x71,0x55,0xd0,0xf5,0x3a,0x0c);
DEFINE_GUID(FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6,0x55a650e1,0x5f0a,0x4eca,0xa6,0x53,0x88,0xf5,0x3b,0x26,0xaa,0x8c);
DEFINE_GUID(FWPM_LAYER_OUTBOUND_TRANSPORT_V4,0x09e61aea,0xd214,0x46e2,0x9b,0x21,0xb2,0x6b,0x0b,0x2f,0x28,0xc8);
DEFINE_GUID(FWPM_LAYER_OUTBOUND_TRANSPORT_V6,0xe1735bde,0x013f,0x4655,0xb3,0x51,0xa4,0x9e,0x15,0x76,0x2d,0xf0);

DEFINE_GUID(KG_SUBLAYER,0x3a5e8f71,0x9b2c,0x4d6e,0x8f,0x1a,0x2b,0x3c,0x4d,0x5e,0x6f,0x7a);
DEFINE_GUID(KG_CALLOUT_V4_CONNECT,0x4b6f9a82,0x0c3d,0x5e7f,0x9a,0x2b,0x3c,0x4d,0x5e,0x6f,0x7a,0x8b);
DEFINE_GUID(KG_CALLOUT_V6_CONNECT,0x5c7a0b93,0x1d4e,0x6f8a,0x0b,0x3c,0x4d,0x5e,0x6f,0x7a,0x8b,0x9c);
DEFINE_GUID(KG_CALLOUT_V4_RES,0x8f0d3ec6,0x4a7b,0x5c8d,0x9e,0x0f,0x1a,0x2b,0x3c,0x4d,0x5e,0x6f);
DEFINE_GUID(KG_CALLOUT_V6_RES,0x9a1e4f07,0x5b8c,0x6d9e,0x0f,0x1a,0x2b,0x3c,0x4d,0x5e,0x6f,0x7a);
DEFINE_GUID(KG_CALLOUT_V4_TRANS,0xab2f5a18,0x6c9d,0x7e0f,0x1a,0x2b,0x3c,0x4d,0x5e,0x6f,0x7a,0x8b);
DEFINE_GUID(KG_CALLOUT_V6_TRANS,0xbc3a6b29,0x7d0e,0x8f1a,0x2b,0x3c,0x4d,0x5e,0x6f,0x7a,0x8b,0x9c);
#undef INITGUID

UCHAR gPidNetCache[65536];

static UINT32 gCalloutV4Connect=0;
static UINT32 gCalloutV6Connect=0;
static UINT32 gCalloutV4Res=0;
static UINT32 gCalloutV6Res=0;
static UINT32 gCalloutV4Trans=0;
static UINT32 gCalloutV6Trans=0;
static UINT64 gFilterV4Connect=0;
static UINT64 gFilterV6Connect=0;
static UINT64 gFilterV4Res=0;
static UINT64 gFilterV6Res=0;
static UINT64 gFilterV4Trans=0;
static UINT64 gFilterV6Trans=0;
static PDEVICE_OBJECT gNetDeviceObject=NULL;
static HANDLE gEngineHandle=NULL;

static USHORT gPortPidMap[65536];

static PCSTR KgLyrToStr(UINT16 lyr)
{
    switch (lyr)
    {
    case  0: return "INBOUND_IPPACKET_V4";              // 入站 IP 包（IPv4）
    case  1: return "INBOUND_IPPACKET_V4_DISCARD";      // 入站 IP 包丢弃（IPv4）
    case  2: return "INBOUND_IPPACKET_V6";              // 入站 IP 包（IPv6）
    case  3: return "INBOUND_IPPACKET_V6_DISCARD";      // 入站 IP 包丢弃（IPv6）
    case  4: return "OUTBOUND_IPPACKET_V4";             // 出站 IP 包（IPv4）
    case  5: return "OUTBOUND_IPPACKET_V4_DISCARD";     // 出站 IP 包丢弃（IPv4）
    case  6: return "OUTBOUND_IPPACKET_V6";             // 出站 IP 包（IPv6）
    case  7: return "OUTBOUND_IPPACKET_V6_DISCARD";     // 出站 IP 包丢弃（IPv6）
    case  8: return "IPFORWARD_V4";                     // IP 转发（IPv4）
    case  9: return "IPFORWARD_V4_DISCARD";             // IP 转发丢弃（IPv4）
    case 10: return "IPFORWARD_V6";                     // IP 转发（IPv6）
    case 11: return "IPFORWARD_V6_DISCARD";             // IP 转发丢弃（IPv6）
    case 12: return "INBOUND_TRANSPORT_V4";             // 入站传输层（TCP/UDP，IPv4）
    case 13: return "INBOUND_TRANSPORT_V4_DISCARD";     // 入站传输层丢弃（IPv4）
    case 14: return "INBOUND_TRANSPORT_V6";             // 入站传输层（TCP/UDP，IPv6）
    case 15: return "INBOUND_TRANSPORT_V6_DISCARD";     // 入站传输层丢弃（IPv6）
    case 16: return "OUTBOUND_TRANSPORT_V4";            // 出站传输层（TCP/UDP，IPv4）
    case 17: return "OUTBOUND_TRANSPORT_V4_DISCARD";    // 出站传输层丢弃（IPv4）
    case 18: return "OUTBOUND_TRANSPORT_V6";            // 出站传输层（TCP/UDP，IPv6）
    case 19: return "OUTBOUND_TRANSPORT_V6_DISCARD";    // 出站传输层丢弃（IPv6）
    case 20: return "STREAM_V4";                        // 流层（TCP 数据流，IPv4）
    case 21: return "STREAM_V4_DISCARD";                // 流层丢弃（IPv4）
    case 22: return "STREAM_V6";                        // 流层（TCP 数据流，IPv6）
    case 23: return "STREAM_V6_DISCARD";                // 流层丢弃（IPv6）
    case 24: return "DATAGRAM_DATA_V4";                 // 数据报（UDP，IPv4）
    case 25: return "DATAGRAM_DATA_V4_DISCARD";         // 数据报丢弃（IPv4）
    case 26: return "DATAGRAM_DATA_V6";                 // 数据报（UDP，IPv6）
    case 27: return "DATAGRAM_DATA_V6_DISCARD";         // 数据报丢弃（IPv6）
    case 28: return "INBOUND_ICMP_ERROR_V4";            // 入站 ICMP 错误（IPv4）
    case 29: return "INBOUND_ICMP_ERROR_V4_DISCARD";    // 入站 ICMP 错误丢弃（IPv4）
    case 30: return "INBOUND_ICMP_ERROR_V6";            // 入站 ICMP 错误（IPv6）
    case 31: return "INBOUND_ICMP_ERROR_V6_DISCARD";    // 入站 ICMP 错误丢弃（IPv6）
    case 32: return "OUTBOUND_ICMP_ERROR_V4";           // 出站 ICMP 错误（IPv4）
    case 33: return "OUTBOUND_ICMP_ERROR_V4_DISCARD";   // 出站 ICMP 错误丢弃（IPv4）
    case 34: return "OUTBOUND_ICMP_ERROR_V6";           // 出站 ICMP 错误（IPv6）
    case 35: return "OUTBOUND_ICMP_ERROR_V6_DISCARD";   // 出站 ICMP 错误丢弃（IPv6）
    case 36: return "RESOURCE_ASSIGNMENT_V4";           // 绑定本地端口/地址（bind，IPv4）
    case 37: return "RESOURCE_ASSIGNMENT_V4_DISCARD";   // 绑定端口丢弃（IPv4）
    case 38: return "RESOURCE_ASSIGNMENT_V6";           // 绑定本地端口/地址（bind，IPv6）
    case 39: return "RESOURCE_ASSIGNMENT_V6_DISCARD";   // 绑定端口丢弃（IPv6）
    case 40: return "AUTH_LISTEN_V4";                   // 监听（listen，IPv4）
    case 41: return "AUTH_LISTEN_V4_DISCARD";           // 监听丢弃（IPv4）
    case 42: return "AUTH_LISTEN_V6";                   // 监听（listen，IPv6）
    case 43: return "AUTH_LISTEN_V6_DISCARD";           // 监听丢弃（IPv6）
    case 44: return "AUTH_RECV_ACCEPT_V4";              // 接收/接受连接（accept，IPv4）
    case 45: return "AUTH_RECV_ACCEPT_V4_DISCARD";      // 接受连接丢弃（IPv4）
    case 46: return "AUTH_RECV_ACCEPT_V6";              // 接收/接受连接（accept，IPv6）
    case 47: return "AUTH_RECV_ACCEPT_V6_DISCARD";      // 接受连接丢弃（IPv6）
    case 48: return "AUTH_CONNECT_V4";                  // 发起出站连接（connect，IPv4）
    case 49: return "AUTH_CONNECT_V4_DISCARD";          // 出站连接丢弃（IPv4）
    case 50: return "AUTH_CONNECT_V6";                  // 发起出站连接（connect，IPv6）
    case 51: return "AUTH_CONNECT_V6_DISCARD";          // 出站连接丢弃（IPv6）
    case 52: return "FLOW_ESTABLISHED_V4";              // 连接已建立（IPv4）
    case 53: return "FLOW_ESTABLISHED_V4_DISCARD";      // 连接建立丢弃（IPv4）
    case 54: return "FLOW_ESTABLISHED_V6";              // 连接已建立（IPv6）
    case 55: return "FLOW_ESTABLISHED_V6_DISCARD";      // 连接建立丢弃（IPv6）
    default: return "?";
    }
}

static VOID KgNetClassify(const FWPS_INCOMING_VALUES0*in,const FWPS_INCOMING_METADATA_VALUES0*im,PVOID ld,const VOID*cc,const FWPS_FILTER3*f,UINT64 fc,FWPS_CLASSIFY_OUT0*co)
{
    (void)ld;
    (void)cc;
    (void)f;
    (void)fc;
    co->actionType=FWP_ACTION_PERMIT;
    UINT64 pid=im->processId;
    if(!pid) {
      return;
    }
    
    UINT16 lyr=in->layerId;
    UCHAR st=gPidNetCache[pid&0xFFFF];

    /* Trace: layer, PID from metadata, cache status */
    if(lyr==FWPS_LAYER_ALE_AUTH_CONNECT_V4)
    {
        UINT32 ra=in->incomingValue[FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_ADDRESS].value.uint32;
        UINT32 lp=in->incomingValue[FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_LOCAL_PORT].value.uint16;
        USHORT origPid=gPortPidMap[lp&0xFFFF];
        KG_LOG("KiloGuard: Net V4 Pid=%I64u localPort=%u remoteIP=%d.%d.%d.%d NetBlock=%u origPid=%u\n",
            pid,lp,(ra>>24)&0xFF,(ra>>16)&0xFF,(ra>>8)&0xFF,ra&0xFF,st,origPid);
    }
    else if(lyr==FWPS_LAYER_ALE_AUTH_CONNECT_V6)
    {
        UINT16 lp6=in->incomingValue[FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_LOCAL_PORT].value.uint16;
        KG_LOG("KiloGuard: Net V6 Pid=%I64u localPort=%u NetBlock=%u\n", pid, lp6, st);
    }

    if (st!=KG_NET_BLOCK) {
        return;
    }
    if(lyr==FWPS_LAYER_ALE_AUTH_CONNECT_V4) {
        UINT32 a=in->incomingValue[FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_ADDRESS].value.uint32;
        if((a&0xFF000000)==0x7F000000)
            return;
    }
    else if(lyr==FWPS_LAYER_ALE_AUTH_CONNECT_V6) {
        UINT16*a6=(UINT16*)in->incomingValue[FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_REMOTE_ADDRESS].value.byteArray16;
        if(a6&&a6[0]==0&&a6[1]==0&&a6[2]==0&&a6[3]==0&&a6[4]==0&&a6[5]==0&&a6[6]==0&&a6[7]==0x0100)
            return;
    }
    {
        ULONG s=KgIsPidInSandBox((HANDLE)(ULONG_PTR)pid);
        if(s!=-1) {
            WCHAR b[48];
            if(lyr==FWPS_LAYER_ALE_AUTH_CONNECT_V4) {
                ULONG a=in->incomingValue[FWPS_FIELD_ALE_AUTH_CONNECT_V4_IP_REMOTE_ADDRESS].value.uint32;
                KgFormatIpV4(b,a);
            } else if (lyr==FWPS_LAYER_ALE_AUTH_CONNECT_V6) {
                UINT16*a6=(UINT16*)in->incomingValue[FWPS_FIELD_ALE_AUTH_CONNECT_V6_IP_REMOTE_ADDRESS].value.byteArray16;
                if(a6) {
                    KgFormatIpV6(b,a6);
                } else
                    b[0]=0;
            }
            UNICODE_STRING u;
            RtlInitUnicodeString(&u,b);
            KgPushNetDenyEvent(s,(HANDLE)(ULONG_PTR)pid,&u);
        }
    }
    // ALE 层，告诉引擎丢弃此包
    co->rights=0;
    co->actionType=FWP_ACTION_BLOCK;
}

static NTSTATUS KgNetNotify(UINT32 nt,const GUID*fk,FWPS_FILTER3*f)
{
    (void)nt;
    (void)fk;
    (void)f;
    return STATUS_SUCCESS;
}

static VOID KgNetResClassify(const FWPS_INCOMING_VALUES0*in,const FWPS_INCOMING_METADATA_VALUES0*im,PVOID ld,const VOID*cc,const FWPS_FILTER3*f,UINT64 fc,FWPS_CLASSIFY_OUT0*co)
{
    (void)ld;
    (void)cc;
    (void)f;
    (void)fc;
    co->actionType=FWP_ACTION_PERMIT;
    UINT64 pid=im->processId;
    if(!pid)
        return;
    UINT16 lyr=in->layerId;
    UINT32 p=in->incomingValue[4].value.uint16;
    KG_LOG("KiloGuard: Net PID=%I64u val4_port=%u TRACE INFO: %s(%u)\n", pid, p, KgLyrToStr(lyr), lyr);
    if (p && p < 65536)
        gPortPidMap[p] = (USHORT)(pid & 0xFFFF);
}

static VOID KgNetTransClassify(const FWPS_INCOMING_VALUES0*in,const FWPS_INCOMING_METADATA_VALUES0*im,PVOID ld,const VOID*cc,const FWPS_FILTER3*f,UINT64 fc,FWPS_CLASSIFY_OUT0*co)
{
    (void)ld;
    (void)cc;
    (void)f;
    (void)fc;
    co->actionType=FWP_ACTION_PERMIT;
    UINT16 lyr=in->layerId;
    // 出站数据包的本地源端口号
    UINT32 p=in->incomingValue[FWPS_FIELD_OUTBOUND_TRANSPORT_V4_IP_LOCAL_PORT].value.uint16;
    if(!p || p >= 65536) // 端口无效，直接跳过
        return;
    USHORT pid=gPortPidMap[p];
    if(!pid) // 该端口无绑定进程，跳过
      return;
    UCHAR st=gPidNetCache[pid];
    if(st!=KG_NET_BLOCK) // 该进程未被标记拦截，跳过
      return;
    KG_LOG("KiloGuard: Net srcPort=%u Pid=%u NetBlock=%u\n",p,pid,st);
    if (lyr==FWPS_LAYER_OUTBOUND_TRANSPORT_V4) {
        UINT32 ra=in->incomingValue[FWPS_FIELD_OUTBOUND_TRANSPORT_V4_IP_REMOTE_ADDRESS].value.uint32;
        if((ra&0xFF000000)==0x7F000000)
            return;
        KG_LOG("KiloGuard: NetBLOCK_V4 port=%u pid=%u dst=%d.%d.%d.%d\n",p,pid,(ra>>24)&0xFF,(ra>>16)&0xFF,(ra>>8)&0xFF,ra&0xFF);
    } else {
        UINT16*a6=(UINT16*)in->incomingValue[FWPS_FIELD_OUTBOUND_TRANSPORT_V6_IP_REMOTE_ADDRESS].value.byteArray16;
        if(a6&&a6[0]==0&&a6[1]==0&&a6[2]==0&&a6[3]==0&&a6[4]==0&&a6[5]==0&&a6[6]==0&&a6[7]==0x0100)
            return;
        KG_LOG("KiloGuard: NetBLOCK_V6 port=%u pid=%u\n",p,pid);
    }
    // 传输层，告诉引擎丢弃此包
    co->rights=0;
    co->actionType=FWP_ACTION_BLOCK; // 真正执行
}

VOID KgSetPidNetCache(HANDLE pid,UCHAR s)
{
    gPidNetCache[(ULONG)(ULONG_PTR)pid & 0xFFFF] = s;
}

static NTSTATUS KgNetAddCalloutAndFilter(PDEVICE_OBJECT d,HANDLE e,const GUID*cg,const GUID*lg,void*fn,PCWSTR ds,UINT32*ci,UINT64*fi)
{
    FWPS_CALLOUT3 co;RtlZeroMemory(&co,sizeof(co));co.calloutKey=*cg;co.flags=0;co.classifyFn=fn;co.notifyFn=(void*)KgNetNotify;co.flowDeleteFn=NULL;
    NTSTATUS s=FwpsCalloutRegister3(d,&co,ci);if(!NT_SUCCESS(s))return s;
    FWPM_CALLOUT0 mc;RtlZeroMemory(&mc,sizeof(mc));mc.calloutKey=*cg;mc.displayData.name=(WCHAR*)L"KG net callout";mc.applicableLayer=*lg;mc.flags=0;
    s=FwpmCalloutAdd0(e,&mc,NULL,NULL);if(!NT_SUCCESS(s)){FwpsCalloutUnregisterById0(*ci);*ci=0;return s;}
    FWPM_FILTER0 ft;RtlZeroMemory(&ft,sizeof(ft));ft.layerKey=*lg;ft.subLayerKey=KG_SUBLAYER;ft.displayData.name=(WCHAR*)ds;ft.displayData.description=(WCHAR*)ds;
    ft.action.type=FWP_ACTION_CALLOUT_TERMINATING;ft.action.calloutKey=*cg;ft.numFilterConditions=0;ft.filterCondition=NULL;
    s=FwpmFilterAdd0(e,&ft,NULL,fi);if(!NT_SUCCESS(s)){FwpmCalloutDeleteByKey0(e,cg);FwpsCalloutUnregisterById0(*ci);*ci=0;*fi=0;}
    return s;
}

NTSTATUS KgRegisterNetCallbacks(PDEVICE_OBJECT dev)
{
    NTSTATUS s;gNetDeviceObject=dev;RtlZeroMemory(gPidNetCache,sizeof(gPidNetCache));RtlZeroMemory(gPortPidMap,sizeof(gPortPidMap));
    FWPM_SESSION0 sn;RtlZeroMemory(&sn,sizeof(sn));sn.flags=FWPM_SESSION_FLAG_DYNAMIC;
    HANDLE eh=NULL;s=FwpmEngineOpen0(NULL,RPC_C_AUTHN_WINNT,NULL,&sn,&eh);if(!NT_SUCCESS(s))return s;gEngineHandle=eh;
    FWPM_SUBLAYER0 sl;RtlZeroMemory(&sl,sizeof(sl));sl.subLayerKey=KG_SUBLAYER;sl.flags=0;sl.weight=0;sl.displayData.name=(WCHAR*)L"KiloGuard Network Sublayer";sl.displayData.description=(WCHAR*)L"KiloGuard sandbox net filter";
    s=FwpmSubLayerAdd0(eh,&sl,NULL);if(!NT_SUCCESS(s))goto ce;
    s=KgNetAddCalloutAndFilter(dev,eh,&KG_CALLOUT_V4_CONNECT,&FWPM_LAYER_ALE_AUTH_CONNECT_V4,(void*)KgNetClassify,L"v4c",&gCalloutV4Connect,&gFilterV4Connect);if(!NT_SUCCESS(s))goto dl;
    s=KgNetAddCalloutAndFilter(dev,eh,&KG_CALLOUT_V6_CONNECT,&FWPM_LAYER_ALE_AUTH_CONNECT_V6,(void*)KgNetClassify,L"v6c",&gCalloutV6Connect,&gFilterV6Connect);if(!NT_SUCCESS(s))goto u4c;
    s=KgNetAddCalloutAndFilter(dev,eh,&KG_CALLOUT_V4_RES,&FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4,(void*)KgNetResClassify,L"v4res",&gCalloutV4Res,&gFilterV4Res);if(!NT_SUCCESS(s))goto u6c;
    s=KgNetAddCalloutAndFilter(dev,eh,&KG_CALLOUT_V6_RES,&FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V6,(void*)KgNetResClassify,L"v6res",&gCalloutV6Res,&gFilterV6Res);if(!NT_SUCCESS(s))goto u4res;
    s=KgNetAddCalloutAndFilter(dev,eh,&KG_CALLOUT_V4_TRANS,&FWPM_LAYER_OUTBOUND_TRANSPORT_V4,(void*)KgNetTransClassify,L"v4t",&gCalloutV4Trans,&gFilterV4Trans);if(!NT_SUCCESS(s))goto u6res;
    s=KgNetAddCalloutAndFilter(dev,eh,&KG_CALLOUT_V6_TRANS,&FWPM_LAYER_OUTBOUND_TRANSPORT_V6,(void*)KgNetTransClassify,L"v6t",&gCalloutV6Trans,&gFilterV6Trans);if(!NT_SUCCESS(s))goto u4t;
    return STATUS_SUCCESS;
#define KG_CL(cid,fid,ck) do{if(eh&&fid)FwpmFilterDeleteById0(eh,fid);if(eh&&cid&&(ck))FwpmCalloutDeleteByKey0(eh,(ck));if(cid){FwpsCalloutUnregisterById0(cid);cid=0;fid=0;}}while(0)
u4t:KG_CL(gCalloutV4Trans,gFilterV4Trans,&KG_CALLOUT_V4_TRANS);
u6res:KG_CL(gCalloutV6Res,gFilterV6Res,&KG_CALLOUT_V6_RES);
u4res:KG_CL(gCalloutV4Res,gFilterV4Res,&KG_CALLOUT_V4_RES);
u6c:KG_CL(gCalloutV6Connect,gFilterV6Connect,&KG_CALLOUT_V6_CONNECT);
u4c:KG_CL(gCalloutV4Connect,gFilterV4Connect,&KG_CALLOUT_V4_CONNECT);
dl:if(eh)FwpmSubLayerDeleteByKey0(eh,&KG_SUBLAYER);
ce:if(eh){FwpmEngineClose0(eh);gEngineHandle=NULL;}return s;
}

VOID KgUnregisterNetCallbacks(VOID)
{
#define KG_UC(cid,fid,ck) do{if(cid){if(gEngineHandle&&fid)FwpmFilterDeleteById0(gEngineHandle,fid);if(gEngineHandle)FwpmCalloutDeleteByKey0(gEngineHandle,(ck));FwpsCalloutUnregisterById0(cid);cid=0;fid=0;}}while(0)
    KG_UC(gCalloutV6Trans,gFilterV6Trans,&KG_CALLOUT_V6_TRANS);
    KG_UC(gCalloutV4Trans,gFilterV4Trans,&KG_CALLOUT_V4_TRANS);
    KG_UC(gCalloutV6Res,gFilterV6Res,&KG_CALLOUT_V6_RES);
    KG_UC(gCalloutV4Res,gFilterV4Res,&KG_CALLOUT_V4_RES);
    KG_UC(gCalloutV6Connect,gFilterV6Connect,&KG_CALLOUT_V6_CONNECT);
    KG_UC(gCalloutV4Connect,gFilterV4Connect,&KG_CALLOUT_V4_CONNECT);
    if(gEngineHandle){FwpmSubLayerDeleteByKey0(gEngineHandle,&KG_SUBLAYER);FwpmEngineClose0(gEngineHandle);gEngineHandle=NULL;}
    gNetDeviceObject=NULL;
}
