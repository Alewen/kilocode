// RPC 上下文句柄示例 - 服务端业务函数
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "callback.h"

// 上下文句柄指向的真实状态结构体（仅服务端可见，客户端看到的是不透明指针）
typedef struct {
    long total;      // 预期接收的总条目数
    long received;   // 已接收条目数
    char buffer[256]; // 拼接缓冲区
} ConvState;

// 创建会话上下文
void StartConversion(CONV_HANDLE* hConv, long count)
{
    ConvState* state = (ConvState*)midl_user_allocate(sizeof(ConvState));
    state->total = count;
    state->received = 0;
    state->buffer[0] = '\0';
    *hConv = (CONV_HANDLE)state;
    printf("  StartConversion: 预计接收 %ld 条数据\n", count);
}

// 逐条发送数据，状态保存在上下文句柄中
void SendItem(CONV_HANDLE* hConv, const unsigned char* text)
{
    ConvState* state = (ConvState*)*hConv;
    state->received++;
    strncat_s(state->buffer, _countof(state->buffer), (const char*)text, _TRUNCATE);
    strncat_s(state->buffer, _countof(state->buffer), " ", _TRUNCATE);
    printf("  SendItem [%ld/%ld]: %s\n", state->received, state->total, text);
}

// 结束会话，释放上下文
void EndConversion(CONV_HANDLE* hConv)
{
    ConvState* state = (ConvState*)*hConv;
    printf("  EndConversion: 共接收 %ld/%ld 条数据\n", state->received, state->total);
    printf("  最终缓冲区: %s\n", state->buffer);
    midl_user_free(state);
    *hConv = NULL;
}

// 上下文句柄的 rundown 回调：客户端异常断开时，RPC 运行时自动调用此函数清理
void __RPC_USER CONV_HANDLE_rundown(CONV_HANDLE hConv)
{
    printf("  [rundown] 客户端异常断开，正在清理上下文句柄\n");
    if (hConv) midl_user_free(hConv);
}
