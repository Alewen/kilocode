// RPC 基础示例 - 服务端主程序（注册接口、启动监听）
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "hello.h"

static void set_console_utf8(void)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// MIDL 内存分配回调：服务端和客户端都必须提供
void __RPC_FAR* __RPC_USER midl_user_allocate(size_t len)
{
    return malloc(len);
}

// MIDL 内存释放回调
void __RPC_USER midl_user_free(void __RPC_FAR* ptr)
{
    free(ptr);
}

int main(int argc, char* argv[])
{
    set_console_utf8();
    RPC_STATUS status;
    // 使用 ncalrpc 协议（本地 RPC，底层走 ALPC）
    unsigned char* pszProtocolSequence = (unsigned char*)"ncalrpc";
    // 端点名称，客户端通过它来连接
    unsigned char* pszEndpoint = (unsigned char*)"HelloWorld";
    // 服务端接口句柄（由 MIDL 生成的存根定义）
    RPC_IF_HANDLE ifSpec = Hello_v1_0_s_ifspec;
    RPC_BINDING_VECTOR* pBindingVec = NULL;

    printf("RPC Hello 服务端启动中...\n");

    // 第 1 步：注册协议序列和端点
    status = RpcServerUseProtseqEp(
        pszProtocolSequence,
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        pszEndpoint,
        NULL
    );
    if (status) {
        printf("RpcServerUseProtseqEp 失败: 0x%x\n", status);
        return 1;
    }

    // 第 2 步：注册 RPC 接口
    status = RpcServerRegisterIf(ifSpec, NULL, NULL);
    if (status) {
        printf("RpcServerRegisterIf 失败: 0x%x\n", status);
        return 1;
    }

    // 第 3 步：查询绑定句柄向量
    status = RpcServerInqBindings(&pBindingVec);
    if (status) {
        printf("RpcServerInqBindings 失败: 0x%x\n", status);
        return 1;
    }

    // 第 4 步：将接口注册到端点映射器（Endpoint Mapper）
    status = RpcEpRegister(ifSpec, pBindingVec, NULL, (unsigned char*)"HelloServer");
    if (status) {
        printf("RpcEpRegister 失败: 0x%x\n", status);
        return 1;
    }

    printf("服务端正在监听 ncalrpc:[HelloWorld]\n");

    // 第 5 步：启动监听线程（RpcMgmtWaitServerListen 不会自动启动监听）
    status = RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, 0);
    if (status) {
        printf("RpcServerListen 失败: 0x%x\n", status);
        return 1;
    }

    // 第 6 步：等待服务停止（阻塞直到有人调用 RpcMgmtStopServerListening）
    status = RpcMgmtWaitServerListen();
    if (status) {
        printf("RpcMgmtWaitServerListen 失败: 0x%x\n", status);
        return 1;
    }

    RpcBindingVectorFree(&pBindingVec);
    printf("服务端已停止。\n");
    return 0;
}
