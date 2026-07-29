// RPC 基础示例 - 服务端业务函数实现
#include <stdio.h>
#include <stdlib.h>
#include "hello.h"

// 客户端调用的远程过程：打印收到的字符串
void HelloProc(const unsigned char* pszString)
{
    printf("服务端收到: %s\n", pszString);
}

// 关闭服务端的远程过程
void Shutdown(void)
{
    printf("服务端正在关闭...\n");
    RpcMgmtStopServerListening(NULL);
}
