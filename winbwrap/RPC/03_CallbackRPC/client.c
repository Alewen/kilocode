// RPC 上下文句柄示例 - 客户端
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "callback.h"

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
    unsigned char* pszStringBinding = NULL;
    unsigned char* pszProtocolSequence = (unsigned char*)"ncalrpc";
    unsigned char* pszEndpoint = (unsigned char*)"CallbackDemo";

    printf("RPC 上下文句柄客户端\n");
    printf("========================================\n");

    status = RpcStringBindingComposeA(
        NULL, pszProtocolSequence, NULL, pszEndpoint, NULL, &pszStringBinding);
    if (status) { printf("RpcStringBindingCompose 失败: 0x%x\n", status); return 1; }

    status = RpcBindingFromStringBindingA(pszStringBinding, &CallbackBinding);
    if (status) { printf("RpcBindingFromStringBinding 失败: 0x%x\n", status); return 1; }

    RpcTryExcept
    {
        CONV_HANDLE hConv = NULL;

        // 第 1 步：开始一个会话，服务端创建上下文句柄
        printf("1. 开始会话（5 条数据）\n");
        StartConversion(&hConv, 5);
        printf("   上下文句柄已创建: %p\n", hConv);

        // 第 2 步：逐条发送数据，服务端在上下文里累积状态
        const char* items[] = {
            "你好", "RPC", "上下文", "句柄", "世界"
        };
        for (int i = 0; i < 5; i++) {
            printf("2.%d 发送: %s\n", i + 1, items[i]);
            SendItem(&hConv, (const unsigned char*)items[i]);
        }

        // 第 3 步：结束会话，服务端释放上下文
        printf("3. 结束会话\n");
        EndConversion(&hConv);
        printf("   上下文句柄已变为: %p\n", hConv);
    }
    RpcExcept(1)
    {
        unsigned long code = RpcExceptionCode();
        printf("RPC 异常: 0x%lx\n", code);
    }
    RpcEndExcept

    RpcStringFreeA(&pszStringBinding);
    RpcBindingFree(&CallbackBinding);

    printf("\n结束。\n");
    return 0;
}
