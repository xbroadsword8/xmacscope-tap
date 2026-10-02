// xMacScope Tap
// Copyright (C) 2026 金皓哲 (xmac jin)
// SPDX-License-Identifier: GPL-3.0-only
//
// 這個 DLL 不連結 libstdc++。
//
// libstdc++ 在每個模組載入時會配置一塊約 73 KB 的「緊急例外記憶池」，DLL 卸載時不釋放。
// 驅動是被 DAW 載入、卸載的；每載入卸載一次就漏 73 KB（生命週期測試量到每次重載成長約 90 KB）。
// 所以這裡不用 operator new / delete（改用 malloc 加 placement new），
// 並自己提供純虛擬函式被呼叫時的處理函式，連結時加 -nostdlib++，有任何東西偷偷引用 libstdc++ 就會連結失敗。
#pragma once
#include <windows.h>
#include <cstdlib>
#include <new>   // 只用 placement new（純標頭，不需要函式庫）

extern "C" void __cxa_pure_virtual() { TerminateProcess(GetCurrentProcess(), 3); }

template <class T> static T* xm_create() {
    void* mem = malloc(sizeof(T));
    return mem ? new (mem) T() : nullptr;
}
template <class T> static void xm_destroy(T* p) {
    p->~T();
    free(p);
}
