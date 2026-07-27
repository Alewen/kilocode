#pragma once

#include <fltKernel.h>
#include <ntifs.h>
#include "Pidmap.h"
#include "PidPortMap.h"

/* =========================
   Sandbox Domain Management — types, globals, API
   ========================= */

#define KG_POOL_TAG                 'GKiK'
#define KG_MAX_DOMAIN               1024
#define KG_MAX_RULES_PER_DOMAIN     256

////////////////////////////////////////////////////////////////////////////////
// 沙箱域 ID 类型（ULONG，全局唯一，递增分配）
////////////////////////////////////////////////////////////////////////////////
typedef ULONG KG_SANDBOX_ID;

////////////////////////////////////////////////////////////////////////////////
// 沙箱级别类型（UCHAR，定义进程在沙箱内的访问权限等级）
////////////////////////////////////////////////////////////////////////////////
typedef UCHAR KG_SANDBOX_LEVEL;

typedef struct _KG_PATH_RULE {
    LIST_ENTRY Link;           // 链表节点，挂载到域规则链表 RuleList
    UNICODE_STRING PathPrefix; // 路径前缀（如 \Device\...\AAA\wtest）
    KG_SANDBOX_LEVEL Level;    // 该路径前缀对应的沙箱级别（0/1/2）
} KG_PATH_RULE;

#define KG_EVENT_RING_SIZE      256
#define KG_EVENT_BATCH_SIZE     32
#define KG_TRACKER_SIZE         256

////////////////////////////////////////////////////////////////////////////////
// 进程启动失败事件
// 记录沙箱内进程因缺少依赖 DLL 而启动失败的信息
// Pid        - 失败进程的 PID
// ImageName  - 失败进程的映像文件完整路径
// DllPath    - 缺失导致失败的 DLL 名称
// Timestamp  - 进程创建时间
////////////////////////////////////////////////////////////////////////////////
typedef struct _KG_FAILURE_EVENT {
    ULONG Pid;
    WCHAR ImageName[256];
    WCHAR DllPath[256];
    LARGE_INTEGER Timestamp;
} KG_FAILURE_EVENT;

typedef struct _KG_EVENT_RING {
    KG_FAILURE_EVENT Events[KG_EVENT_RING_SIZE]; // 环形缓冲区，16 个事件槽
    LONG WriteIndex;   // 写入位置（生产者，由 KgPushEvent 维护）
    LONG ReadIndex;    // 读取位置（消费者，由 IOCTL 读取取出时更新）
    KSPIN_LOCK Lock;   // 保护写入/读取索引的自旋锁
} KG_EVENT_RING;

////////////////////////////////////////////////////////////////////////////////
// Deny 事件环形缓冲区（per-domain，与 DLL 事件独立）
// 记录文件访问被拒绝时的发起进程 PID、进程名、目标文件路径
////////////////////////////////////////////////////////////////////////////////

#define KG_DENY_RING_SIZE               256
#define KG_DENY_EVENT_BATCH_SIZE        32

typedef struct _KG_DENY_EVENT {
    ULONG Pid;
    WCHAR ProcessName[32];
    WCHAR FilePath[260];
    LARGE_INTEGER Timestamp;
    ULONG Flags;    // 0=file deny, 1=net deny
} KG_DENY_EVENT;

typedef struct _KG_DENY_EVENT_RING {
    KG_DENY_EVENT Events[KG_DENY_RING_SIZE];
    LONG WriteIndex;
    LONG ReadIndex;
    KSPIN_LOCK Lock;
} KG_DENY_EVENT_RING;

typedef struct _KG_DLL_ENTRY {
    LIST_ENTRY Link;  // 链表节点，挂载到追踪器的 DllList
    WCHAR Name[64];   // DLL 文件名（不含路径，如 "helper.dll"）
} KG_DLL_ENTRY;

typedef struct _KG_PENDING_TRACKER {
    HANDLE Pid;                    // 追踪的进程 PID
    ULONG SlotIndex;               // 进程所属沙箱域的槽位索引
    LONG ImageCount;               // 进程中已加载的映像（DLL）数量
    LARGE_INTEGER CreationTime;    // 进程创建时间（用于事件排序）
    BOOLEAN Used;                  // 条目是否已分配（= TRUE 表示正在使用）
    WCHAR ImageName[256];          // 进程映像完整路径（NT 路径）
    LIST_ENTRY DllList;            // 已加载 DLL 链表头（KgRecordDllLoad 填充）
    LONG DllCount;                 // DllList 中的条目数
} KG_PENDING_TRACKER;

typedef struct _KG_POLICY_DOMAIN {
    KG_SANDBOX_ID Id;                       // 沙箱域唯一 ID（递增分配）
    BOOLEAN Active;                         // 域是否已激活（TRUE=启用）
    KSPIN_LOCK Lock;                        // 保护规则链表的自旋锁
    LIST_ENTRY RuleList;                    // 路径规则链表头（KG_PATH_RULE 节点）
    ULONG RuleCount;                        // 当前规则数量（上限 KG_MAX_RULES_PER_DOMAIN=256）
    volatile LONG RuleEpoch;                // 规则版本号，每次增删规则时递增
    KG_EVENT_RING EventRing;                // 该域内进程启动失败事件环形缓冲区
    KG_DENY_EVENT_RING DenyRing;            // 该域内文件访问拒绝事件环形缓冲区
    WCHAR NetExeList[KG_MAX_NET_EXE][260];  // 网络例外进程 NT 路径列表
    ULONG NetExeCount;                      // 网络例外列表中的条目数
    BOOLEAN NetBlockEnabled;                // 是否启用网络拦截
    BOOLEAN IsEnableDenyEvent;              // 是否向缓冲写入 DENY 事件信息
} KG_POLICY_DOMAIN;

typedef struct _KG_POLICY_STATE {
    BOOLEAN FileAccessEnabled;              // 文件访问控制开关（默认打开）
    BOOLEAN HandleProtectionEnabled;        // 句柄权限保护开关
    BOOLEAN ProcessInheritanceEnabled;      // 进程继承开关（子进程自动入沙箱）
} KG_POLICY_STATE;

////////////////////////////////////////////////////////////////////////////////
// 全局唯一的一个结构体
////////////////////////////////////////////////////////////////////////////////
typedef struct _KG_SYSTEM_STATE {
    EX_RUNDOWN_REF RundownRef;               // 全局状态生命周期保护（RCU 风格）
    KG_POLICY_DOMAIN Domain[KG_MAX_DOMAIN];  // 沙箱域数组（1024 个域，含 Domain[0] 全局域）
    KG_PID_MAP* PidMap;                      // PID→域槽位哈希映射表（全局唯一）
    KG_POLICY_STATE Policy;                  // 策略开关集合
    volatile LONG DroppedRuleCount;          // 因域不存在被丢弃的规则计数
} KG_SYSTEM_STATE;

extern KG_SYSTEM_STATE* gState;
extern KSPIN_LOCK gStateLock;
extern KG_SANDBOX_ID gNextSid;

extern KG_PENDING_TRACKER gTracker[KG_TRACKER_SIZE];
extern KSPIN_LOCK gTrackerLock;

////////////////////////////////////////////////////////////////////////////////
// 功能：分配并初始化系统状态结构体，包括 PID 映射表、Domain[0] 默认域、策略设置
// 参数：无
// 返回值：新分配的系统状态指针；失败返回 NULL
////////////////////////////////////////////////////////////////////////////////
KG_SYSTEM_STATE* KgAllocState(VOID);

////////////////////////////////////////////////////////////////////////////////
// 功能：释放系统状态结构体及其所有子结构的内存
//       包括 PID 映射表、所有沙箱域的路径规则等
// 参数：
//   state - 要释放的系统状态指针；允许 NULL
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgFreeState(KG_SYSTEM_STATE* state);

////////////////////////////////////////////////////////////////////////////////
// 功能：获取系统状态的 Rundown 保护引用，确保在访问期间状态不会被释放
//       用于多线程安全读取全局状态
// 参数：无
// 返回值：系统状态指针；若驱动正在卸载返回 NULL
////////////////////////////////////////////////////////////////////////////////
KG_SYSTEM_STATE* KgAcquireState(VOID);

////////////////////////////////////////////////////////////////////////////////
// 功能：释放通过 KgAcquireState 获取的 Rundown 保护引用
//       与 KgAcquireState 成对使用
// 参数：
//   state - 系统状态指针
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgReleaseState(KG_SYSTEM_STATE* state);

////////////////////////////////////////////////////////////////////////////////
// 功能：在系统状态中查找指定 ID 的沙箱域槽位
//       先通过哈希快速定位，若失败则线性扫描
// 参数：
//   state - 系统状态指针
//   id    - 要查找的沙箱域 ID
// 返回值：域槽位索引（0~1023）；未找到返回 (ULONG)-1
////////////////////////////////////////////////////////////////////////////////
ULONG KgFindDomainSlot(KG_SYSTEM_STATE* state, KG_SANDBOX_ID id);

////////////////////////////////////////////////////////////////////////////////
// 功能：向指定沙箱域添加一条路径规则
//       规则包含路径前缀和对应的沙箱级别
// 参数：
//   domain     - 目标沙箱域指针
//   pathPrefix - 路径前缀（UNICODE_STRING）
//   level      - 沙箱级别（访问权限等级）
// 返回值：STATUS_SUCCESS 成功；STATUS_TOO_MANY_COMMANDS 规则数已达上限；
//         STATUS_INSUFFICIENT_RESOURCES 内存不足
////////////////////////////////////////////////////////////////////////////////
NTSTATUS KgAddPathRule(KG_POLICY_DOMAIN* domain, PUNICODE_STRING pathPrefix, KG_SANDBOX_LEVEL level);

////////////////////////////////////////////////////////////////////////////////
// 功能：查询域对文件路径的权限等级
//       遍历规则链表做最长前缀匹配，返回匹配规则的 Level
//       规则在域创建时一次性设置，只读访问无需加锁
// 参数：
//   domain - 目标沙箱域指针
//   path   - 待匹配的文件完整路径
// 返回值：匹配规则的 Level（0/1/2）；无匹配返回 0
////////////////////////////////////////////////////////////////////////////////
KG_SANDBOX_LEVEL KgFindPathRule(KG_POLICY_DOMAIN* domain, PUNICODE_STRING path);

////////////////////////////////////////////////////////////////////////////////
// 功能：释放沙箱域中所有路径规则的内存并重置规则计数
// 参数：
//   domain - 目标沙箱域指针
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgFreePathRules(KG_POLICY_DOMAIN* domain);

////////////////////////////////////////////////////////////////////////////////
// 功能：初始化进程追踪器数组 gTracker，清零所有槽位并初始化自旋锁
//       在驱动入口阶段一次性调用
// 参数：无
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgInitTracker(VOID);

////////////////////////////////////////////////////////////////////////////////
// 功能：根据 PID 查找已分配的进程追踪器
// 参数：
//   pid - 目标进程 PID
// 返回值：找到的追踪器指针；未找到返回 NULL
////////////////////////////////////////////////////////////////////////////////
KG_PENDING_TRACKER* KgFindTracker(HANDLE pid);

////////////////////////////////////////////////////////////////////////////////
// 功能：为指定进程分配一个新的追踪器，记录映像加载和 DLL 依赖信息
//       在进程创建或 ATTACH_PROCESS 时调用
// 参数：
//   pid       - 目标进程 PID
//   slotIndex - 沙箱域槽位索引
//   imageName - 进程映像名称（可选，允许 NULL）
// 返回值：新分配的追踪器指针；失败返回 NULL（追踪器已满）
////////////////////////////////////////////////////////////////////////////////
KG_PENDING_TRACKER* KgAllocTracker(HANDLE pid, ULONG slotIndex, PUNICODE_STRING imageName);

////////////////////////////////////////////////////////////////////////////////
// 功能：释放指定进程的追踪器及其关联的 DLL 列表内存
//       在进程退出或不再需要追踪时调用
// 参数：
//   pid - 目标进程 PID
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgFreeTracker(HANDLE pid);

////////////////////////////////////////////////////////////////////////////////
// 功能：将进程启动失败事件推入沙箱域的事件环形缓冲区
//       供用户态 bwrap.exe 通过 IOCTL_KG_QUERY_EVENTS 轮询读取
// 参数：
//   domain    - 目标沙箱域指针
//   tracker   - 进程追踪器，包含 PID 和映像名称
//   missingDll - 缺失导致失败的 DLL 名称（可选，允许 NULL）
// 返回值：无
////////////////////////////////////////////////////////////////////////////////
VOID KgPushEvent(KG_POLICY_DOMAIN* domain, KG_PENDING_TRACKER* tracker, PCUNICODE_STRING missingDll);

BOOLEAN KgIsAncestorOfBindRoot(KG_POLICY_DOMAIN* domain, PUNICODE_STRING path);

BOOLEAN KgIsTraversalAllowed(KG_SYSTEM_STATE* state, ULONG slotIndex, PUNICODE_STRING path);

////////////////////////////////////////////////////////////////////////////////
// 策略规则条目结构
// Sid   - 规则所属的沙箱域 ID
// Level - 沙箱级别（访问权限等级）
// Path  - 规则匹配的路径前缀（宽字符，最长 1024 字符）
////////////////////////////////////////////////////////////////////////////////
typedef struct _KG_POLICY_RULE_ENTRY {
    KG_SANDBOX_ID Sid;
    KG_SANDBOX_LEVEL Level;
    WCHAR Path[1024];
} KG_POLICY_RULE_ENTRY;

////////////////////////////////////////////////////////////////////////////////
// 批量策略输入结构（IOCTL_KG_SET_POLICY_BATCH）
// RuleCount - 规则条目数量
// Rules     - 规则数组（柔性数组，实际大小由 RuleCount 决定）
////////////////////////////////////////////////////////////////////////////////
typedef struct _KG_POLICY_BATCH_INPUT {
    ULONG RuleCount;
    KG_POLICY_RULE_ENTRY Rules[1];
} KG_POLICY_BATCH_INPUT;

typedef struct _KG_CONTROL_SANDBOX {
    BOOLEAN Destroy;      // TRUE=销毁沙箱域，FALSE=创建沙箱域
    KG_SANDBOX_ID Sid;    // 创建时返回新域 ID，销毁时指定要销毁的域 ID
} KG_CONTROL_SANDBOX;

typedef struct _KG_ATTACH_PROCESS_INPUT {
    HANDLE Pid;           // 要绑定/解绑的进程 PID
    KG_SANDBOX_ID Sid;    // 绑定到的沙箱域 ID
    BOOLEAN Detach;       // FALSE=绑定，TRUE=解绑
} KG_ATTACH_PROCESS_INPUT;

typedef struct _KG_STATS {
    LONG DroppedRuleCount;  // 因域不存在被丢弃的规则计数（调试用）
} KG_STATS;

typedef struct _KG_QUERY_EVENTS_INPUT {
    KG_SANDBOX_ID Sid;  // 要查询事件的沙箱域 ID
} KG_QUERY_EVENTS_INPUT;

////////////////////////////////////////////////////////////////////////////////
// 查询沙箱域事件输出结构
// EventCount - 返回的事件数量
// Events     - 失败事件环形缓冲区内容快照
////////////////////////////////////////////////////////////////////////////////
typedef struct _KG_QUERY_EVENTS_OUTPUT {
    ULONG EventCount;
    KG_FAILURE_EVENT Events[KG_EVENT_BATCH_SIZE];
} KG_QUERY_EVENTS_OUTPUT;

typedef struct _KG_QUERY_DENY_EVENTS_OUTPUT {
    ULONG EventCount;
    KG_DENY_EVENT Events[KG_DENY_EVENT_BATCH_SIZE];
} KG_QUERY_DENY_EVENTS_OUTPUT;

VOID KgPushDenyEvent(KG_POLICY_DOMAIN* domain, HANDLE pid, PUNICODE_STRING filePath);
VOID KgPushNetDenyEvent(ULONG slotIndex, HANDLE pid, PCUNICODE_STRING infoStr);

#define IOCTL_KG_SET_NET_EXE_LIST \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_WRITE_DATA)

typedef struct _KG_NET_EXE_LIST_INPUT {
    KG_SANDBOX_ID Sid;
    ULONG Count;
    WCHAR Paths[KG_MAX_NET_EXE][260];
} KG_NET_EXE_LIST_INPUT;

#define IOCTL_KG_SET_DENY_LOG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80A, METHOD_BUFFERED, FILE_WRITE_DATA)

typedef struct _KG_SET_DENY_LOG_INPUT {
    KG_SANDBOX_ID Sid;
    BOOLEAN IsEnableDeny;
} KG_SET_DENY_LOG_INPUT;

#define KG_PORT_MSG_PROCESS_CREATE     1
#define KG_PORT_MSG_PROCESS_EXIT       2
#define KG_PORT_MSG_READY_FOR_INJECT   3

typedef struct _KG_PORT_MESSAGE {
    ULONG MsgType;
    ULONG ParentPid;
    ULONG Pid;
    ULONG SID;
    WCHAR ImageName[260];
} KG_PORT_MESSAGE;
