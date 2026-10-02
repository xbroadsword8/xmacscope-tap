// xMacScope Tap — ASIO pass-through proxy
// Copyright (C) 2026 金皓哲 (xmac jin)
// SPDX-License-Identifier: GPL-3.0-only
//
// 引入 Steinberg ASIO SDK 的介面定義（依 SDK 的 GPLv3 授權選項使用）。
//
// SDK 的 asio.h 只在 MSVC 下才加 `#pragma pack(push,4)`。所有原廠驅動與 DAW 都是 MSVC 編的，
// 結構是 4 位元組對齊；用 mingw 編譯時要自己補上同樣的對齊，否則 ASIOTime 這類結構的大小會不同。
#pragma once
#include <windows.h>
#include <objbase.h>

#if !defined(_MSC_VER)
#pragma pack(push, 4)
#endif
#include "iasiodrv.h"
#if !defined(_MSC_VER)
#pragma pack(pop)
#endif

// 測試用的擴充指令（future() 的 selector）。代理不認得它們，只會原樣轉發；
// 測試程式藉此確認「不認得的指令也照轉」以及「DAW 拿到的就是真驅動的緩衝區」。
enum : long {
    kXmTestGetBufferPtr = 0x584D0001,   // opt = XmTestBufferQuery*
    kXmTestGetStats = 0x584D0002,       // opt = XmTestStats*
};
struct XmTestBufferQuery { long channel; long half; void* ptr; };
struct XmTestStats {
    long long callbacks;       // 假驅動發出的回呼次數
    long long pattern_errors;  // 回呼返回後，輸出緩衝區的內容和預期樣式不符的樣本數
    long long last_position;   // 最後一次回呼的取樣位置
    long long cb_ns_sum;       // 回呼耗時總和（奈秒）：從假驅動呼叫出去到返回
    long long cb_ns_max;       // 單次回呼最長耗時（奈秒）
    long long cb_over_50us;    // 耗時超過 50 微秒的回呼次數
};
