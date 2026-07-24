#include "median_filter.h"
#include <algorithm>

#define WINDOW 5

// --- 通用中位数核心 ---
static float medianCore(float raw, float buf[], int &idx, int &count)
{
    buf[idx] = raw;
    idx = (idx + 1) % WINDOW;
    if (count < WINDOW)
        count++;

    float sorted[WINDOW];
    for (int i = 0; i < count; i++)
        sorted[i] = buf[i];

    std::sort(sorted, sorted + count);

    if (count % 2 == 1)
        return sorted[count / 2];
    else
        return (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0f;
}

// --- 温度 ---
float medianFilterTemp(float raw)
{
    static float buf[WINDOW];
    static int idx = 0, count = 0;
    return medianCore(raw, buf, idx, count);
}

// --- 湿度 ---
float medianFilterHumi(float raw)
{
    static float buf[WINDOW];
    static int idx = 0, count = 0;
    return medianCore(raw, buf, idx, count);
}