// RPC 数据类型示例 - 服务端主程序
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "data.h"

static void set_console_utf8(void)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// MIDL 内存管理回调
void __RPC_FAR* __RPC_USER midl_user_allocate(size_t len)
{
    return malloc(len);
}

void __RPC_USER midl_user_free(void __RPC_FAR* ptr)
{
    free(ptr);
}

int main(int argc, char* argv[])
{
    set_console_utf8();
    RPC_STATUS status;
    unsigned char* pszProtocolSequence = (unsigned char*)"ncalrpc";
    unsigned char* pszEndpoint = (unsigned char*)"DataDemo";
    RPC_IF_HANDLE ifSpec = DataDemo_v1_0_s_ifspec;
    RPC_BINDING_VECTOR* pBindingVec = NULL;

    printf("RPC 数据类型服务端启动中...\n");

    status = RpcServerUseProtseqEp(
        pszProtocolSequence,
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        pszEndpoint,
        NULL
    );
    if (status) { printf("RpcServerUseProtseqEp 失败: 0x%x\n", status); return 1; }

    status = RpcServerRegisterIf(ifSpec, NULL, NULL);
    if (status) { printf("RpcServerRegisterIf 失败: 0x%x\n", status); return 1; }

    status = RpcServerInqBindings(&pBindingVec);
    if (status) { printf("RpcServerInqBindings 失败: 0x%x\n", status); return 1; }

    status = RpcEpRegister(ifSpec, pBindingVec, NULL, (unsigned char*)"DataServer");
    if (status) { printf("RpcEpRegister 失败: 0x%x\n", status); return 1; }

    printf("服务端正在监听 ncalrpc:[DataDemo]\n");

    // 启动监听线程
    status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, 0);
    if (status) { printf("RpcServerListen 失败: 0x%x\n", status); return 1; }

    // 等待服务停止
    status = RpcMgmtWaitServerListen();
    if (status) { printf("RpcMgmtWaitServerListen 失败: 0x%x\n", status); return 1; }

    RpcBindingVectorFree(&pBindingVec);
    printf("服务端已停止。\n");
    return 0;
}
