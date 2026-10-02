// xMacScope Tap：對真正的硬體 ASIO 驅動做「直接」與「經過代理」的對照。
// Copyright (C) 2026 金皓哲 (xmac jin)
// SPDX-License-Identifier: GPL-3.0-only
//
// 用法：
//   realtest.exe direct <驅動 CLSID> [--seconds N]
//   realtest.exe proxy <代理.dll> <代理 CLSID> [--seconds N]      （目標驅動由環境變數 XMACSCOPE_TAP_TARGET_CLSID 指定）
//   加 --sine <dBFS>：在那兩個輸出通道送 1 kHz 正弦波（只支援 32 位元整數格式），用來驗證旁聽到示波器的讀值
//   加 --verify：位元一致測試。在那兩個輸出通道寫一串聽不到的虛擬亂數（32 位元整數與浮點格式約 −138 dBFS，
//                24 位元硬體會整個截掉；16 位元格式只動最低一個位元），同時扮演示波器讀分線盒的共享記憶體，
//                逐位元組比對抄到的和寫出去的是不是同一串。只有經過分線盒（proxy）時才有東西可比。
//   加 --block min|max|<N>：指定緩衝區大小（驅動允許宿主選擇時才有用；預設用驅動建議的大小）
//   加 --burn <N>：另外開 N 條全速空轉的執行緒製造 CPU 負載
//
// 安全起見：只開前兩個輸入與「最後兩個」輸出通道，輸出寫靜音（或上面兩種測試訊號）；不改取樣率、不開控制面板。
// 印出兩類資訊：
//   ID.*      驅動回報的靜態資訊，直接與經過代理必須逐字相同
//   TIME.*    回呼的時序統計，兩者應該在同一個量級
#include "../src/sdk.h"
#include "../src/tap_shared.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

static IASIO* g_drv = nullptr;
static ASIOBufferInfo g_infos[4];
static long g_out_bytes = 4, g_block = 0;
static LARGE_INTEGER g_qpf;
static volatile long long g_callbacks = 0, g_pos_breaks = 0, g_msgs[16] = {};
static long long g_last_pos = -1, g_last_qpc = 0;
static double g_iv_sum = 0, g_iv_sq = 0, g_iv_min = 1e18, g_iv_max = 0, g_dur_max = 0, g_dur_sum = 0;
static long g_in_peak = 0;
static volatile long g_rate_changes = 0;
static double g_sine_amp = 0, g_sine_phase = 0, g_sine_step = 0;   // amp = 0 表示送靜音
static long g_out_type = 0;
static double g_expect_us = 0;
static long long g_late15 = 0, g_late20 = 0;   // 回呼間隔超過理論值 1.5 倍、2 倍的次數
static long g_last_index = -1;
static long long g_index_repeats = 0, g_index_odd = 0;   // 驅動連續兩次給同一半緩衝區、給了 0/1 以外的值
// 旁聽到的整塊不對時，看它實際上是什麼：上一塊、上兩塊、下一塊、全零，或其他
static long long g_bad_blocks = 0, g_bad_prev1 = 0, g_bad_prev2 = 0, g_bad_next1 = 0, g_bad_zero = 0, g_bad_other = 0;

// ---- 位元一致測試（--verify）----
// 樣式只由「宿主寫出的第幾個 frame」決定，所以讀取端不用知道任何時間資訊就能重建預期值。
static bool g_verify = false;
static long long g_host_frames = 0, g_first_pos = -1;
static inline uint32_t pattern(uint64_t n) {
    uint64_t z = n + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return static_cast<uint32_t>(z ^ (z >> 31));
}
// 把第 n 個 frame、聲道 c 的樣本依驅動的格式寫進 dst；回傳 false 表示這種格式不支援（改送靜音）
static inline bool encode(uint64_t n, int c, long type, uint8_t* dst) {
    const uint32_t k = (pattern(n) >> (8 * c)) & 0xFF;
    switch (type) {
    case ASIOSTInt32LSB: { int32_t v = static_cast<int32_t>(k); memcpy(dst, &v, 4); return true; }
    case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24: { int32_t v = static_cast<int32_t>(k & 1); memcpy(dst, &v, 4); return true; }
    case ASIOSTInt24LSB: dst[0] = static_cast<uint8_t>(k & 1); dst[1] = 0; dst[2] = 0; return true;
    case ASIOSTInt16LSB: { int16_t v = static_cast<int16_t>(k & 1); memcpy(dst, &v, 2); return true; }
    case ASIOSTFloat32LSB: { float v = static_cast<float>(k) / 2147483648.0f; memcpy(dst, &v, 4); return true; }
    case ASIOSTFloat64LSB: { double v = static_cast<double>(k) / 2147483648.0; memcpy(dst, &v, 8); return true; }
    default: return false;
    }
}

// 扮演示波器：讀分線盒的共享記憶體，和預期的樣式逐位元組比對
static XmTapShared* g_sh = nullptr;
static volatile bool g_reader_run = false;
static long long g_tap_frames = 0, g_tap_errors = 0, g_tap_overruns = 0, g_tap_segments = 0, g_tap_align_fail = 0, g_tap_first_n = -1, g_tap_start_pos = -2;
static void tap_drain(uint64_t& read, uint32_t& seg, bool& aligned, uint64_t& n) {
    XmTapShared* sh = g_sh;
    const uint32_t s = sh->segment;
    if (s != seg) { seg = s; read = sh->segment_start_frames; aligned = false; g_tap_segments++; if (g_tap_segments == 1) g_tap_start_pos = sh->segment_start_pos; }
    const uint64_t w = sh->write_frames;
    if (w - read > kXmTapRingFrames - 8192) { g_tap_overruns++; read = w; aligned = false; return; }
    const uint32_t bps = sh->bytes_per_sample;
    const uint8_t* ring[2] = {reinterpret_cast<const uint8_t*>(sh) + xm_tap_channel_offset(0), reinterpret_cast<const uint8_t*>(sh) + xm_tap_channel_offset(1)};
    auto same = [&](uint64_t at, uint64_t frame) {
        const uint32_t i = static_cast<uint32_t>(at & (kXmTapRingFrames - 1));
        uint8_t want[8];
        for (int c = 0; c < 2; c++) {
            encode(frame, c, g_out_type, want);
            if (memcmp(ring[c] + static_cast<size_t>(i) * bps, want, bps)) return false;
        }
        return true;
    };
    if (!aligned) {
        // 一段資料從宿主的某一塊開頭開始：找出是哪一塊（正常情況是第 0 塊）
        if (w - read < 64) return;
        const long long blocks = g_host_frames / g_block;
        long long found = -1;
        for (long long j = 0; j <= blocks && found < 0; j++) {
            bool ok = true;
            for (uint64_t t = 0; t < 64 && ok; t++) ok = same(read + t, static_cast<uint64_t>(j) * g_block + t);
            if (ok) found = j;
        }
        if (found < 0) { g_tap_align_fail++; read = w; return; }
        n = static_cast<uint64_t>(found) * g_block;
        aligned = true;
        if (g_tap_first_n < 0) g_tap_first_n = static_cast<long long>(n);
    }
    auto zero = [&](uint64_t at) {
        const uint32_t i = static_cast<uint32_t>(at & (kXmTapRingFrames - 1));
        static const uint8_t z[8] = {};
        return !memcmp(ring[0] + static_cast<size_t>(i) * bps, z, bps) && !memcmp(ring[1] + static_cast<size_t>(i) * bps, z, bps);
    };
    for (; read < w; read++, n++) {
        if (!same(read, n)) {
            g_tap_errors++;
            // 每個不對的塊只分類一次：在塊的開頭、而且整塊都已經寫進來的時候
            if (n % g_block == 0 && w - read >= static_cast<uint64_t>(g_block)) {
                g_bad_blocks++;
                bool p1 = n >= static_cast<uint64_t>(g_block), p2 = n >= 2 * static_cast<uint64_t>(g_block), n1 = true, z0 = true;
                for (long t = 0; t < g_block; t++) {
                    if (p1 && !same(read + t, n + t - g_block)) p1 = false;
                    if (p2 && !same(read + t, n + t - 2 * g_block)) p2 = false;
                    if (n1 && !same(read + t, n + t + g_block)) n1 = false;
                    if (z0 && !zero(read + t)) z0 = false;
                }
                if (p1) g_bad_prev1++; else if (p2) g_bad_prev2++; else if (n1) g_bad_next1++; else if (z0) g_bad_zero++; else g_bad_other++;
            }
        }
        g_tap_frames++;
    }
}
static DWORD WINAPI reader_main(void*) {
    uint64_t read = g_sh->write_frames, n = 0; uint32_t seg = g_sh->segment; bool aligned = false;
    while (g_reader_run) {
        g_sh->reader_tick = GetTickCount64();
        tap_drain(read, seg, aligned, n);
        Sleep(4);
    }
    tap_drain(read, seg, aligned, n);   // 停止後把剩下的讀完
    return 0;
}

static volatile bool g_burn = false;
static DWORD WINAPI burner(void*) { volatile unsigned long long x = 1; while (g_burn) x = x * 6364136223846793005ULL + 1442695040888963407ULL; return 0; }

static long bytes_per_sample(ASIOSampleType t) {
    switch (t) {
    case ASIOSTInt16LSB: case ASIOSTInt16MSB: return 2;
    case ASIOSTInt24LSB: case ASIOSTInt24MSB: return 3;
    case ASIOSTFloat64LSB: case ASIOSTFloat64MSB: return 8;
    default: return 4;
    }
}

static void on_block(long long pos, bool pos_valid, long index) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_last_qpc) {
        double us = (now.QuadPart - g_last_qpc) * 1e6 / g_qpf.QuadPart;
        g_iv_sum += us; g_iv_sq += us * us;
        if (us < g_iv_min) g_iv_min = us;
        if (us > g_iv_max) g_iv_max = us;
        if (g_expect_us > 0) { if (us > g_expect_us * 1.5) g_late15++; if (us > g_expect_us * 2.0) g_late20++; }
    }
    g_last_qpc = now.QuadPart;
    if (index != 0 && index != 1) g_index_odd++;
    else if (index == g_last_index) g_index_repeats++;
    g_last_index = index;
    if (pos_valid) {
        if (g_last_pos >= 0 && pos != g_last_pos + g_block) g_pos_breaks++;
        if (g_last_pos < 0) g_first_pos = pos;
        g_last_pos = pos;
    }
    // 輸出寫靜音、（--verify）聽不到的樣式，或（--sine）1 kHz 正弦波
    uint8_t probe[8];
    if (g_verify && encode(0, 0, g_out_type, probe)) {
        for (int c = 0; c < 2; c++) {
            uint8_t* d = static_cast<uint8_t*>(g_infos[2 + c].buffers[index]);
            if (!d) continue;
            for (long i = 0; i < g_block; i++) encode(static_cast<uint64_t>(g_host_frames + i), c, g_out_type, d + static_cast<size_t>(i) * g_out_bytes);
        }
        g_host_frames += g_block;
    } else if (g_sine_amp > 0 && g_out_type == ASIOSTInt32LSB) {
        long* a = static_cast<long*>(g_infos[2].buffers[index]);
        long* b = static_cast<long*>(g_infos[3].buffers[index]);
        for (long i = 0; i < g_block; i++) {
            long v = static_cast<long>(g_sine_amp * sin(g_sine_phase) * 2147483647.0);
            g_sine_phase += g_sine_step;
            if (g_sine_phase > 6.283185307179586) g_sine_phase -= 6.283185307179586;
            if (a) a[i] = v;
            if (b) b[i] = v;
        }
    } else {
        for (int k = 2; k < 4; k++)
            if (g_infos[k].buffers[index]) memset(g_infos[k].buffers[index], 0, static_cast<size_t>(g_block) * g_out_bytes);
    }
    // 輸入只看一下峰值，確認真的有資料在流（假設 32 位元整數；其他格式只當參考）
    if (g_infos[0].buffers[index]) {
        const long* in = static_cast<const long*>(g_infos[0].buffers[index]);
        for (long i = 0; i < g_block; i++) { long a = in[i] < 0 ? -in[i] : in[i]; if (a > g_in_peak) g_in_peak = a; }
    }
    LARGE_INTEGER end; QueryPerformanceCounter(&end);
    double dur = (end.QuadPart - now.QuadPart) * 1e6 / g_qpf.QuadPart;
    g_dur_sum += dur; if (dur > g_dur_max) g_dur_max = dur;
    g_callbacks++;
}
static void bufferSwitch(long index, ASIOBool) {
    ASIOSamples p; ASIOTimeStamp ts;
    bool ok = g_drv->getSamplePosition(&p, &ts) == ASE_OK;
    on_block((static_cast<long long>(p.hi) << 32) | p.lo, ok, index);
}
static ASIOTime* bufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) {
    bool valid = (t->timeInfo.flags & kSamplePositionValid) != 0;
    on_block((static_cast<long long>(t->timeInfo.samplePosition.hi) << 32) | t->timeInfo.samplePosition.lo, valid, index);
    return nullptr;
}
static void sampleRateDidChange(ASIOSampleRate) { g_rate_changes++; }
static long asioMessage(long selector, long value, void*, double*) {
    if (selector >= 0 && selector < 16) g_msgs[selector]++;
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioSupportsTimeInfo || value == kAsioEngineVersion || value == kAsioResetRequest ||
               value == kAsioResyncRequest || value == kAsioLatenciesChanged || value == kAsioBufferSizeChange;
    case kAsioEngineVersion: return 2;
    case kAsioSupportsTimeInfo: return 1;
    case kAsioResetRequest: case kAsioResyncRequest: case kAsioLatenciesChanged: return 1;   // 只記錄，不處理
    default: return 0;
    }
}
static ASIOCallbacks g_cb = {bufferSwitch, sampleRateDidChange, asioMessage, bufferSwitchTimeInfo};

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { printf("usage: realtest direct <clsid> | proxy <dll> <clsid>  [--seconds N]\n"); return 2; }
    double seconds = 20, sine_db = 1;
    const char* block_arg = nullptr; long burn = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[i + 1]);
        if (!strcmp(argv[i], "--sine") && i + 1 < argc) sine_db = atof(argv[i + 1]);
        if (!strcmp(argv[i], "--verify")) g_verify = true;
        if (!strcmp(argv[i], "--block") && i + 1 < argc) block_arg = argv[i + 1];
        if (!strcmp(argv[i], "--burn") && i + 1 < argc) burn = atol(argv[i + 1]);
    }
    if (burn > 64) burn = 64;
    QueryPerformanceFrequency(&g_qpf);
    CoInitialize(nullptr);
    bool via_proxy = !strcmp(argv[1], "proxy");
    const char* clsid_str = via_proxy ? argv[3] : argv[2];
    wchar_t w[64]; CLSID clsid;
    MultiByteToWideChar(CP_ACP, 0, clsid_str, -1, w, 64);
    if (FAILED(CLSIDFromString(w, &clsid))) { printf("bad clsid\n"); return 2; }
    void* p = nullptr;
    if (via_proxy) {
        HMODULE m = LoadLibraryA(argv[2]);
        if (!m) { printf("FAIL LoadLibrary (error %lu)\n", GetLastError()); return 1; }
        typedef HRESULT(__stdcall * GetClassObject)(REFCLSID, REFIID, void**);
        auto get = reinterpret_cast<GetClassObject>(reinterpret_cast<void*>(GetProcAddress(m, "DllGetClassObject")));
        IClassFactory* f = nullptr;
        if (!get || FAILED(get(clsid, IID_IClassFactory, reinterpret_cast<void**>(&f))) || FAILED(f->CreateInstance(nullptr, clsid, &p))) { printf("FAIL create proxy\n"); return 1; }
    } else {
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &p);
        if (FAILED(hr)) { printf("FAIL CoCreateInstance 0x%08lx\n", static_cast<unsigned long>(hr)); return 1; }
    }
    g_drv = static_cast<IASIO*>(p);
    HWND hwnd = CreateWindowExA(0, "STATIC", "xm-realtest", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), nullptr);

    char name[128] = "", err[256] = "";
    if (!g_drv->init(hwnd)) { g_drv->getErrorMessage(err); printf("FAIL init: %s\n", err); g_drv->Release(); return 1; }
    g_drv->getDriverName(name);
    printf("INFO driver name: %s\n", name);
    printf("ID.version=%ld\n", g_drv->getDriverVersion());
    long in = 0, out = 0;
    ASIOError r = g_drv->getChannels(&in, &out);
    printf("ID.getChannels=%ld in=%ld out=%ld\n", static_cast<long>(r), in, out);
    long mn = 0, mx = 0, pref = 0, gran = 0;
    r = g_drv->getBufferSize(&mn, &mx, &pref, &gran);
    printf("ID.getBufferSize=%ld min=%ld max=%ld preferred=%ld granularity=%ld\n", static_cast<long>(r), mn, mx, pref, gran);
    ASIOSampleRate rate = 0;
    r = g_drv->getSampleRate(&rate);
    printf("ID.getSampleRate=%ld rate=%.1f\n", static_cast<long>(r), rate);
    const double rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
    printf("ID.canSampleRate=");
    for (double r : rates) printf("%.0f:%ld ", r, static_cast<long>(g_drv->canSampleRate(r)));
    printf("\n");
    ASIOClockSource clocks[32] = {}; long nclocks = 32;
    ASIOError ce = g_drv->getClockSources(clocks, &nclocks);
    printf("ID.clockSources=%ld n=%ld", static_cast<long>(ce), nclocks);
    for (long i = 0; i < nclocks && i < 32; i++) printf(" [%ld:%s%s]", clocks[i].index, clocks[i].name, clocks[i].isCurrentSource ? "*" : "");
    printf("\n");
    // 所有通道的資訊做成一個雜湊，另外印出頭尾幾個名字
    unsigned long long h = 1469598103934665603ULL;
    auto mix = [&](const void* d, size_t n) { const unsigned char* b = static_cast<const unsigned char*>(d); for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; } };
    char first_in[64] = "", last_out[64] = ""; long out_type = -1, in_type = -1;
    for (int dir = 0; dir < 2; dir++) {
        long n = dir ? in : out;
        for (long c = 0; c < n; c++) {
            ASIOChannelInfo ci = {}; ci.channel = c; ci.isInput = dir ? ASIOTrue : ASIOFalse;
            ASIOError e = g_drv->getChannelInfo(&ci);
            mix(&e, sizeof e); mix(&ci.channelGroup, sizeof ci.channelGroup); mix(&ci.type, sizeof ci.type); mix(ci.name, strlen(ci.name));
            if (dir && c == 0) { strcpy(first_in, ci.name); in_type = ci.type; }
            if (!dir && c == n - 1) { strcpy(last_out, ci.name); out_type = ci.type; }
        }
    }
    printf("ID.channelInfoHash=%016llx firstIn='%s' type=%ld lastOut='%s' type=%ld\n", h, first_in, in_type, last_out, out_type);
    if (in < 2 || out < 2 || pref <= 0) { printf("FAIL not enough channels\n"); g_drv->Release(); return 1; }

    g_block = pref;
    if (block_arg) {
        long want = !strcmp(block_arg, "min") ? mn : !strcmp(block_arg, "max") ? mx : atol(block_arg);
        if (want < mn || want > mx) { printf("FAIL block %ld outside the driver's range %ld..%ld\n", want, mn, mx); g_drv->Release(); return 1; }
        g_block = want;
    }
    printf("INFO block size used: %ld frames (%.3f ms)\n", g_block, rate > 0 ? g_block * 1000.0 / rate : 0.0);
    g_expect_us = rate > 0 ? g_block * 1e6 / rate : 0;
    g_out_bytes = bytes_per_sample(out_type);
    g_out_type = out_type;
    if (sine_db <= 0 && rate > 0) {
        g_sine_amp = pow(10.0, sine_db / 20.0);
        g_sine_step = 6.283185307179586 * 1000.0 / rate;
        printf("INFO sending 1 kHz sine at %.1f dBFS to the last two outputs\n", sine_db);
    }
    g_infos[0] = {ASIOTrue, 0, {nullptr, nullptr}};
    g_infos[1] = {ASIOTrue, 1, {nullptr, nullptr}};
    g_infos[2] = {ASIOFalse, out - 2, {nullptr, nullptr}};
    g_infos[3] = {ASIOFalse, out - 1, {nullptr, nullptr}};
    ASIOError e = g_drv->createBuffers(g_infos, 4, g_block, &g_cb);
    printf("ID.createBuffers=%ld\n", static_cast<long>(e));
    if (e != ASE_OK) { g_drv->getErrorMessage(err); printf("FAIL createBuffers: %s\n", err); g_drv->Release(); return 1; }
    long li = 0, lo = 0;
    r = g_drv->getLatencies(&li, &lo);
    printf("ID.getLatencies=%ld in=%ld out=%ld\n", static_cast<long>(r), li, lo);
    printf("ID.outputReady=%ld\n", static_cast<long>(g_drv->outputReady()));

    // --verify：共享記憶體是分線盒在 createBuffers 時建立的；直接接驅動時不存在
    HANDLE reader = nullptr;
    uint64_t tap_w0 = 0;
    uint8_t probe[8];
    const bool can_pattern = encode(0, 0, g_out_type, probe);
    if (g_verify) {
        printf("INFO verify: %s (sample type %ld)\n", can_pattern ? "writing an inaudible pattern to the last two outputs" : "sample type not supported, sending silence", g_out_type);
        HANDLE map = via_proxy ? OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kXmTapMappingName) : nullptr;
        if (map) g_sh = static_cast<XmTapShared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (g_sh && can_pattern) {
            printf("TAP.header rate=%u block=%u type=%u bytes=%u driver='%s' ch='%s','%s'\n", g_sh->sample_rate, g_sh->block_frames, g_sh->sample_type,
                   g_sh->bytes_per_sample, g_sh->driver_name, g_sh->channel_name[0], g_sh->channel_name[1]);
            tap_w0 = g_sh->write_frames;
            g_sh->reader_tick = GetTickCount64();   // 開始之前就有心跳，分線盒從第一個可抄的週期開始抄
            g_reader_run = true;
            reader = CreateThread(nullptr, 0, reader_main, nullptr, 0, nullptr);
        } else if (via_proxy) {
            printf("TAP.unavailable (no shared memory or unsupported sample type)\n");
        }
    }
    HANDLE burners[64] = {};
    if (burn > 0) { g_burn = true; for (long i = 0; i < burn; i++) burners[i] = CreateThread(nullptr, 0, burner, nullptr, 0, nullptr); printf("INFO CPU load: %ld spinning threads\n", burn); }

    e = g_drv->start();
    printf("ID.start=%ld\n", static_cast<long>(e));
    if (e == ASE_OK) {
        // 驅動可能需要訊息迴圈（有些驅動用視窗訊息通知），所以邊等邊處理訊息
        DWORD until = GetTickCount() + static_cast<DWORD>(seconds * 1000);
        while (GetTickCount() < until) {
            MSG msg;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
            Sleep(20);
        }
        printf("ID.stop=%ld\n", static_cast<long>(g_drv->stop()));
    }
    if (burn > 0) { g_burn = false; WaitForMultipleObjects(static_cast<DWORD>(burn), burners, TRUE, 2000); }
    long long n = g_callbacks;
    int verify_fail = 0;
    if (reader) {
        Sleep(50);
        g_reader_run = false; WaitForSingleObject(reader, 5000); CloseHandle(reader);
        const long long written = static_cast<long long>(g_sh->write_frames - tap_w0);
        const long long expect = (n - 1) * g_block;   // 每個週期抄上一塊，開始後第一個週期不抄
        printf("TAP.frames written=%lld read=%lld expected=%lld segments=%lld overruns=%lld align_failures=%lld\n", written, g_tap_frames, expect, g_tap_segments, g_tap_overruns, g_tap_align_fail);
        printf("TAP.first_frame=%lld (expect 0) segment_start_pos=%lld first_callback_pos=%lld\n", g_tap_first_n, g_tap_start_pos, g_first_pos);
        printf("TAP.wrong_samples=%lld\n", g_tap_errors);
        printf("TAP.wrong_blocks=%lld holding: previous block=%lld, two blocks back=%lld, next block=%lld, zeros=%lld, other=%lld\n", g_bad_blocks, g_bad_prev1,
               g_bad_prev2, g_bad_next1, g_bad_zero, g_bad_other);
        verify_fail = !(written == expect && g_tap_frames == written && g_tap_segments == 1 && g_tap_overruns == 0 && g_tap_align_fail == 0 &&
                        g_tap_first_n == 0 && g_tap_errors == 0 && (g_first_pos < 0 || g_tap_start_pos == g_first_pos));
        printf("TAP.result=%s\n", verify_fail ? "FAIL" : "PASS (bit-exact, one unbroken segment, every block copied once)");
    }
    double expect_us = g_expect_us;
    double mean = n > 1 ? g_iv_sum / (n - 1) : 0;
    double sd = n > 2 ? sqrt(fabs(g_iv_sq / (n - 1) - mean * mean)) : 0;
    printf("TIME.callbacks=%lld over %.0f s (expected about %.0f)\n", n, seconds, expect_us > 0 ? seconds * 1e6 / expect_us : 0.0);
    printf("TIME.interval_us mean=%.2f expected=%.2f min=%.1f max=%.1f stddev=%.1f\n", mean, expect_us, g_iv_min, g_iv_max, sd);
    printf("TIME.positionBreaks=%lld late_over_1.5x=%lld late_over_2x=%lld\n", g_pos_breaks, g_late15, g_late20);
    printf("TIME.bufferIndex repeats=%lld (same half twice in a row) other_values=%lld\n", g_index_repeats, g_index_odd);
    printf("TIME.hostCallback_us mean=%.3f max=%.1f\n", n ? g_dur_sum / n : 0.0, g_dur_max);
    printf("INFO input peak (ch 1, raw int32) = %ld\n", g_in_peak);
    printf("INFO asioMessage counts:");
    for (int i = 0; i < 16; i++) if (g_msgs[i]) printf(" sel%d=%lld", i, g_msgs[i]);
    printf("  sampleRateDidChange=%ld\n", g_rate_changes);
    printf("ID.disposeBuffers=%ld\n", static_cast<long>(g_drv->disposeBuffers()));
    printf("ID.release=%lu\n", g_drv->Release());
    if (hwnd) DestroyWindow(hwnd);
    return verify_fail;
}
