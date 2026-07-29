// RPC 数据类型示例 - 服务端业务函数
#include <stdio.h>
#include <stdlib.h>
#include "data.h"

// 两个整数相加
long AddNumbers(long a, long b)
{
    printf("  AddNumbers(%ld, %ld)\n", a, b);
    return a + b;
}

// 交换两个整数（[in,out] 参数示例）
void SwapLongs(long* a, long* b)
{
    long tmp = *a;
    *a = *b;
    *b = tmp;
    printf("  SwapLongs -> a=%ld, b=%ld\n", *a, *b);
}

// 求数组元素之和（conformant array 示例）
long SumArray(long count, long* numbers)
{
    long sum = 0;
    printf("  SumArray(count=%ld) [", count);
    for (long i = 0; i < count; i++) {
        sum += numbers[i];
        if (i > 0) printf(", ");
        printf("%ld", numbers[i]);
    }
    printf("] = %ld\n", sum);
    return sum;
}

// 计算点集的包围盒（结构体嵌套 + 数组示例）
Rect GetBoundingBox(long count, Point* points)
{
    Rect r;
    r.topLeft.x = points[0].x;
    r.topLeft.y = points[0].y;
    r.bottomRight.x = points[0].x;
    r.bottomRight.y = points[0].y;
    for (long i = 1; i < count; i++) {
        if (points[i].x < r.topLeft.x) r.topLeft.x = points[i].x;
        if (points[i].y < r.topLeft.y) r.topLeft.y = points[i].y;
        if (points[i].x > r.bottomRight.x) r.bottomRight.x = points[i].x;
        if (points[i].y > r.bottomRight.y) r.bottomRight.y = points[i].y;
    }
    printf("  GetBoundingBox -> 左上(%ld,%ld) 右下(%ld,%ld)\n",
        r.topLeft.x, r.topLeft.y, r.bottomRight.x, r.bottomRight.y);
    return r;
}

// 获取指定范围内的所有素数（服务端分配 out 数组示例）
void GetPrimeNumbers(long max, long* count, long** primes)
{
    if (max < 2) { *count = 0; *primes = NULL; return; }

    // 埃拉托斯特尼筛法
    char* sieve = (char*)malloc(max + 1);
    for (long i = 0; i <= max; i++) sieve[i] = 1;
    sieve[0] = sieve[1] = 0;
    for (long i = 2; i * i <= max; i++) {
        if (sieve[i]) {
            for (long j = i * i; j <= max; j += i)
                sieve[j] = 0;
        }
    }

    // 统计素数个数
    long n = 0;
    for (long i = 2; i <= max; i++) if (sieve[i]) n++;

    // 注意：out 参数的内存必须用 midl_user_allocate 分配
    // RPC 运行时会在客户端调用 midl_user_free 释放
    long* result = (long*)midl_user_allocate(n * sizeof(long));
    long idx = 0;
    for (long i = 2; i <= max; i++)
        if (sieve[i]) result[idx++] = i;

    free(sieve);
    *count = n;
    *primes = result;
    printf("  GetPrimeNumbers(max=%ld) -> 共 %ld 个素数\n", max, n);
}
