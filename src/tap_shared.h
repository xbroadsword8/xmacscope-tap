// xMacScope Tap — 分線盒與示波器之間的共享記憶體格式
// Copyright (C) 2026 金皓哲 (xmac jin)
// SPDX-License-Identifier: GPL-3.0-only
//
// 這個檔案定義的是兩個獨立程式之間的資料格式；示波器那邊（app/src/capture/tap_source.rs）有一份對應的定義，
// 改這裡要同步改那邊，並把 kXmTapVersion 加一。
//
// 一個具名的檔案對應（kXmTapMappingName）：256 位元組的表頭，後面接每個聲道一條環形緩衝區。
// 只有一個寫入者（分線盒，在 DAW 的即時執行緒裡）和一個讀取者（示波器）。沒有鎖：
//   - 寫入者先寫資料，再把 write_frames 往前推；讀取者只讀到 write_frames 為止。
//   - 格式欄位用 generation 當版本號：奇數表示正在改，讀取者要等它變回偶數；變了就代表格式換了。
//   - 讀取者每隔一陣子更新 reader_tick。它超過 2 秒沒更新，寫入者就完全不抄（示波器沒開時零負擔）。
#pragma once
#include <stdint.h>

#define kXmTapMappingName "Local\\xMacScopeTap.v1"
enum : uint32_t {
    kXmTapMagic = 0x50544D58,          // 'XMTP'
    kXmTapVersion = 1,
    kXmTapHeaderBytes = 256,
    kXmTapRingFrames = 262144,         // 2 的次方；192 kHz 下約 1.4 秒
    kXmTapChannels = 2,
    kXmTapMaxBytesPerSample = 8,
    kXmTapReaderTimeoutMs = 2000,
};

#pragma pack(push, 8)
struct XmTapShared {
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;
    uint32_t ring_frames;
    volatile uint32_t generation;      // 偶數 = 下面的格式欄位穩定
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t sample_type;              // ASIOSampleType
    uint32_t bytes_per_sample;
    uint32_t block_frames;
    uint32_t writer_pid;
    volatile uint32_t segment;         // 每開始新的一段連續資料就加一（開始播放、讀取者回來）
    volatile uint64_t write_frames;    // 累計寫入的 frame 數；環形位置 = write_frames % ring_frames
    volatile uint64_t segment_start_frames;   // 這一段的第一個 frame 在 write_frames 計數裡的位置
    volatile int64_t segment_start_pos;       // 這一段第一個 frame 的驅動取樣位置；-1 = 不知道
    volatile uint64_t writer_tick;     // GetTickCount64()，最後一次寫入
    volatile uint64_t reader_tick;     // GetTickCount64()，讀取者的心跳
    char driver_name[64];              // 後面真正的驅動自稱的名稱
    char channel_name[kXmTapChannels][32];
    uint8_t reserved[40];
};
#pragma pack(pop)

// 聲道 c 的環形緩衝區起點（相對於對應的開頭）。每個聲道固定保留 ring_frames * 8 位元組，樣本依 bytes_per_sample 緊密排列。
static inline uint64_t xm_tap_channel_offset(uint32_t c) {
    return kXmTapHeaderBytes + static_cast<uint64_t>(c) * kXmTapRingFrames * kXmTapMaxBytesPerSample;
}
static inline uint64_t xm_tap_mapping_bytes() { return xm_tap_channel_offset(kXmTapChannels); }

#ifdef __cplusplus
static_assert(sizeof(XmTapShared) == kXmTapHeaderBytes, "XmTapShared 必須是 256 位元組");
#endif
