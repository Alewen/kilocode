// RPC 基础示例 - 客户端
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "hello.h"

static void set_console_utf8(void)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// MIDL 内存分配回调
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
    unsigned char* pszStringBinding = NULL;
    unsigned char* pszUuid = NULL;
    unsigned char* pszProtocolSequence = (unsigned char*)"ncalrpc";
    unsigned char* pszNetworkAddress = NULL;
    unsigned char* pszEndpoint = (unsigned char*)"HelloWorld";
    unsigned char* pszOptions = NULL;

    printf("RPC Hello 客户端启动中...\n");

    // 第 1 步：构造绑定字符串
    status = RpcStringBindingComposeA(
        pszUuid,
        pszProtocolSequence,
        pszNetworkAddress,
        pszEndpoint,
        pszOptions,
        &pszStringBinding
    );
    if (status) {
        printf("RpcStringBindingCompose 失败: 0x%x\n", status);
        return 1;
    }

    // 第 2 步：从绑定字符串创建绑定句柄
    status = RpcBindingFromStringBindingA(
        pszStringBinding,
        &HelloBinding
    );
    if (status) {
        printf("RpcBindingFromStringBinding 失败: 0x%x\n", status);
        return 1;
    }

    // 第 3 步：像调用本地函数一样调用远程过程
    RpcTryExcept
    {
        printf("调用 HelloProc(\"来自客户端的问候！\")\n");
        HelloProc((unsigned char*)"来自客户端的问候！");
        printf("HelloProc 返回成功。\n");

        printf("调用 Shutdown()...\n");
        Shutdown();
    }
    RpcExcept(1)
    {
        unsigned long code = RpcExceptionCode();
        printf("RPC 异常: 0x%lx\n", code);
    }
    RpcEndExcept

    // 第 4 步：释放资源
    status = RpcStringFreeA(&pszStringBinding);
    if (status) {
        printf("RpcStringFree 失败: 0x%x\n", status);
    }

    status = RpcBindingFree(&HelloBinding);
    if (status) {
        printf("RpcBindingFree 失败: 0x%x\n", status);
    }

    printf("客户端结束。\n");
    return 0;
}
