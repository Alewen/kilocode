// RPC 数据类型示例 - 客户端
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
    unsigned char* pszStringBinding = NULL;
    unsigned char* pszProtocolSequence = (unsigned char*)"ncalrpc";
    unsigned char* pszEndpoint = (unsigned char*)"DataDemo";

    printf("RPC 数据类型客户端\n");
    printf("========================================\n");

    status = RpcStringBindingComposeA(
        NULL, pszProtocolSequence, NULL, pszEndpoint, NULL, &pszStringBinding);
    if (status) { printf("RpcStringBindingCompose 失败: 0x%x\n", status); return 1; }

    status = RpcBindingFromStringBindingA(pszStringBinding, &DataBinding);
    if (status) { printf("RpcBindingFromStringBinding 失败: 0x%x\n", status); return 1; }

    RpcTryExcept
    {
        // 1. 基本参数传递
        printf("1. AddNumbers(12, 34) = %ld\n", AddNumbers(12, 34));
        printf("\n");

        // 2. [in,out] 参数
        long a = 100, b = 200;
        printf("2. SwapLongs: 交换前 a=%ld b=%ld\n", a, b);
        SwapLongs(&a, &b);
        printf("   SwapLongs: 交换后 a=%ld b=%ld\n", a, b);
        printf("\n");

        // 3. 数组参数（size_is）
        long nums[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        printf("3. SumArray(10 个数字) = %ld\n", SumArray(10, nums));
        printf("\n");

        // 4. 结构体嵌套 + 结构体返回值
        Point pts[] = { {10, 20}, {50, 80}, {5, 5}, {100, 30} };
        Rect r = GetBoundingBox(4, pts);
        printf("4. 包围盒: 左上(%ld,%ld) 右下(%ld,%ld)\n",
            r.topLeft.x, r.topLeft.y, r.bottomRight.x, r.bottomRight.y);
        printf("\n");

        // 5. 服务端分配的 out 数组
        long primeCount = 0;
        long* primes = NULL;
        GetPrimeNumbers(50, &primeCount, &primes);
        printf("5. 50 以内的素数（共 %ld 个）: ", primeCount);
        for (long i = 0; i < primeCount; i++) {
            if (i > 0) printf(", ");
            printf("%ld", primes[i]);
        }
        printf("\n");
        // 服务端分配的内存必须用 midl_user_free 释放
        midl_user_free(primes);
    }
    RpcExcept(1)
    {
        unsigned long code = RpcExceptionCode();
        printf("RPC 异常: 0x%lx\n", code);
    }
    RpcEndExcept

    RpcStringFreeA(&pszStringBinding);
    RpcBindingFree(&DataBinding);

    printf("\n结束。\n");
    return 0;
}
