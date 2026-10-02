// xMacScope Tap 的測試程式：扮演 DAW。
// Copyright (C) 2026 金皓哲 (xmac jin)
// SPDX-License-Identifier: GPL-3.0-only
//
// 用法：host.exe <驅動.dll> <CLSID> [--no-timeinfo] [--seconds N] [--cycles N] [--burn N] [--no-reader] [--reader-pause]
//   --no-reader     不扮演示波器：分線盒應該完全不抄
//   --reader-pause  扮演示波器，但中途停 3 秒再回來：分線盒應該停抄、再接著抄成新的一段
//   --seconds N  串流跑 N 秒（預設 1.5），每 10 秒印一次資源用量
//   --cycles N   生命週期測試：建立緩衝區 → 開始 → 停止 → 釋放，重複 N 次；每 50 次整個驅動卸載重載
//   --burn N     另外開 N 條全速空轉的執行緒，製造 CPU 負載
// 直接從 DLL 載入驅動（不經登錄檔）。同一組檢查對「假驅動」和「代理 + 假驅動」各跑一次，結果必須相同。
#include "../src/sdk.h"
#include "../src/tap_shared.h"
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

enum { kChannels = 2 };
static inline long out_pattern(long long pos, long ch) { return static_cast<long>(pos * 3 + ch * 1000003 + 7); }
static inline long in_pattern(long long pos, long ch) { return static_cast<long>(pos * 5 - ch * 999983 - 11); }

static IASIO* g_drv = nullptr;
static ASIOBufferInfo g_infos[kChannels * 2];
static long g_block = 256;
static bool g_use_time_info = true;
static volatile long long g_callbacks = 0, g_input_errors = 0, g_position_errors = 0, g_time_info_calls = 0, g_plain_calls = 0;
static long long g_expected_pos = 0, g_last_pos = -1;

static void process(long long pos, long index) {
    if (pos != g_expected_pos) g_position_errors++;
    for (int k = 0; k < kChannels * 2; k++) {
        long* buf = static_cast<long*>(g_infos[k].buffers[index]);
        long ch = g_infos[k].channelNum;
        if (g_infos[k].isInput) {
            for (long i = 0; i < g_block; i++)
                if (buf[i] != in_pattern(pos + i, ch)) g_input_errors++;
        } else {
            for (long i = 0; i < g_block; i++) buf[i] = out_pattern(pos + i, ch);
        }
    }
    g_last_pos = pos;
    g_expected_pos = pos + g_block;
    g_callbacks++;
}
static void bufferSwitch(long index, ASIOBool) {
    g_plain_calls++;
    ASIOSamples p; ASIOTimeStamp ts;
    g_drv->getSamplePosition(&p, &ts);
    process((static_cast<long long>(p.hi) << 32) | p.lo, index);
}
static ASIOTime* bufferSwitchTimeInfo(ASIOTime* t, long index, ASIOBool) {
    g_time_info_calls++;
    if (t->timeInfo.sampleRate != 48000.0 || t->timeInfo.speed != 1.0) g_position_errors++;
    process((static_cast<long long>(t->timeInfo.samplePosition.hi) << 32) | t->timeInfo.samplePosition.lo, index);
    return nullptr;
}
static void sampleRateDidChange(ASIOSampleRate) {}
static long asioMessage(long selector, long value, void*, double*) {
    switch (selector) {
    case kAsioSelectorSupported: return value == kAsioSupportsTimeInfo || value == kAsioEngineVersion;
    case kAsioEngineVersion: return 2;
    case kAsioSupportsTimeInfo: return g_use_time_info ? 1 : 0;
    default: return 0;
    }
}
static ASIOCallbacks g_cb = {bufferSwitch, sampleRateDidChange, asioMessage, bufferSwitchTimeInfo};

static int g_fail = 0;
static bool g_quiet = false;
#define CHECK(cond, ...) do { bool ok_ = (cond); if (!ok_ || !g_quiet) { printf("%s  ", ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); printf("\n"); } if (!ok_) g_fail++; } while (0)

// ---- 資源用量 ----
struct Usage { double private_mb, working_mb; DWORD handles; double cpu_s; };
static Usage usage() {
    Usage u = {};
    PROCESS_MEMORY_COUNTERS_EX pmc = {}; pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        u.private_mb = pmc.PrivateUsage / 1048576.0; u.working_mb = pmc.WorkingSetSize / 1048576.0;
    }
    GetProcessHandleCount(GetCurrentProcess(), &u.handles);
    FILETIME c, e, k, us;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &us)) {
        ULARGE_INTEGER a, b; a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime; b.LowPart = us.dwLowDateTime; b.HighPart = us.dwHighDateTime;
        u.cpu_s = (a.QuadPart + b.QuadPart) / 1e7;
    }
    return u;
}
static void print_usage(const char* label, const Usage& u) {
    printf("      %-10s private %.2f MB, working set %.2f MB, handles %lu, cpu %.1f s\n", label, u.private_mb, u.working_mb, u.handles, u.cpu_s);
}

// ---- 扮演示波器：讀分線盒的共享記憶體並逐樣本驗證 ----
// 經過分線盒時才有這塊共享記憶體；直接接假驅動時沒有，相關檢查略過。
static XmTapShared* g_sh = nullptr;
static volatile bool g_reader_run = false, g_reader_beat = true;
static long long g_tap_frames = 0, g_tap_errors = 0, g_tap_overruns = 0, g_tap_segments = 0, g_tap_start_pos_errors = 0;

static void tap_drain(uint64_t& read, uint32_t& seg, bool& have_prev, uint32_t& prev0) {
    XmTapShared* sh = g_sh;
    const uint32_t* r0 = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(sh) + xm_tap_channel_offset(0));
    const uint32_t* r1 = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(sh) + xm_tap_channel_offset(1));
    auto consume = [&](uint64_t upto) {
        for (; read < upto; read++) {
            const uint32_t i = static_cast<uint32_t>(read & (kXmTapRingFrames - 1));
            const uint32_t a = r0[i], b = r1[i];
            // 樣式對取樣位置是線性的：同一個位置兩聲道差 1000003，相鄰位置差 3（都以 32 位元繞回計）
            if (b - a != 1000003u) g_tap_errors++;
            if (have_prev && a - prev0 != 3u) g_tap_errors++;
            prev0 = a;
            have_prev = true;
            g_tap_frames++;
        }
    };
    const uint32_t s = sh->segment;
    if (s != seg) {                       // 新的一段：先把上一段還沒讀的讀完，再從新的一段的起點讀
        const uint64_t start = sh->segment_start_frames;
        if (start > read && start - read < kXmTapRingFrames - 8192) consume(start);
        seg = s;
        read = start;
        have_prev = false;
        g_tap_segments++;
        const long long sp = sh->segment_start_pos;
        if (sp >= 0 && sh->write_frames > read) {
            if (r0[read & (kXmTapRingFrames - 1)] != static_cast<uint32_t>(out_pattern(sp, 0))) g_tap_start_pos_errors++;
        }
    }
    const uint64_t w = sh->write_frames;
    if (w - read > kXmTapRingFrames - 8192) {   // 讀太慢，被寫入者追上
        g_tap_overruns++;
        read = w;
        have_prev = false;
        return;
    }
    consume(w);
}
static DWORD WINAPI reader_main(void*) {
    uint64_t read = 0; uint32_t seg = g_sh->segment; bool have_prev = false; uint32_t prev0 = 0;
    read = g_sh->write_frames;
    while (g_reader_run) {
        if (g_reader_beat) {
            g_sh->reader_tick = GetTickCount64();
            tap_drain(read, seg, have_prev, prev0);
        }
        Sleep(5);
    }
    tap_drain(read, seg, have_prev, prev0);   // 停止後把剩下的讀完
    return 0;
}

static volatile bool g_burn = false;
static DWORD WINAPI burner(void*) { volatile unsigned long long x = 1; while (g_burn) x = x * 6364136223846793005ULL + 1442695040888963407ULL; return 0; }

// ---- 載入／釋放驅動 ----
static HMODULE g_module = nullptr;
static CLSID g_clsid;
static bool open_driver(const char* dll) {
    if (!g_module) g_module = LoadLibraryA(dll);
    if (!g_module) { printf("FAIL  LoadLibrary %s (error %lu)\n", dll, GetLastError()); return false; }
    typedef HRESULT(__stdcall * GetClassObject)(REFCLSID, REFIID, void**);
    auto get = reinterpret_cast<GetClassObject>(reinterpret_cast<void*>(GetProcAddress(g_module, "DllGetClassObject")));
    IClassFactory* factory = nullptr;
    if (!get || FAILED(get(g_clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory)))) { printf("FAIL  DllGetClassObject\n"); return false; }
    void* p = nullptr;
    HRESULT hr = factory->CreateInstance(nullptr, g_clsid, &p);
    if (FAILED(hr)) { printf("FAIL  CreateInstance 0x%08lx\n", static_cast<unsigned long>(hr)); return false; }
    g_drv = static_cast<IASIO*>(p);
    char err[256] = "";
    ASIOBool ok = g_drv->init(nullptr);
    if (!ok) { g_drv->getErrorMessage(err); printf("FAIL  init: %s\n", err); g_fail++; return false; }
    long mn, mx, gran;
    g_drv->getBufferSize(&mn, &mx, &g_block, &gran);
    g_expected_pos = 0; g_last_pos = -1;
    return true;
}
static bool make_buffers() {
    for (int c = 0; c < kChannels; c++) {
        g_infos[c] = {ASIOFalse, c, {nullptr, nullptr}};
        g_infos[kChannels + c] = {ASIOTrue, c, {nullptr, nullptr}};
    }
    return g_drv->createBuffers(g_infos, kChannels * 2, g_block, &g_cb) == ASE_OK;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: host <driver.dll> <clsid> [--no-timeinfo] [--seconds N] [--cycles N] [--burn N]\n"); return 2; }
    double seconds = 1.5; long cycles = 0, burn = 0; bool no_reader = false, reader_pause = false;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--no-timeinfo")) g_use_time_info = false;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = atol(argv[++i]);
        else if (!strcmp(argv[i], "--burn") && i + 1 < argc) burn = atol(argv[++i]);
        else if (!strcmp(argv[i], "--no-reader")) no_reader = true;
        else if (!strcmp(argv[i], "--reader-pause")) reader_pause = true;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitialize(nullptr);
    wchar_t w[64];
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, w, 64);
    if (FAILED(CLSIDFromString(w, &g_clsid))) { printf("bad clsid\n"); return 2; }
    if (!open_driver(argv[1])) return 1;

    char name[64] = "";
    g_drv->getDriverName(name);
    printf("      driver name: %s, version %ld, block %ld\n", name, g_drv->getDriverVersion(), g_block);
    CHECK(g_drv->getDriverVersion() == 4242, "driver version forwarded (4242)");
    long in = 0, out = 0, li = 0, lo = 0;
    CHECK(g_drv->getChannels(&in, &out) == ASE_OK && in == 2 && out == 2, "channels %ld in / %ld out", in, out);
    CHECK(g_drv->getLatencies(&li, &lo) == ASE_OK && li == 311 && lo == 523, "latencies %ld / %ld (expect 311 / 523)", li, lo);
    ASIOSampleRate rate = 0;
    CHECK(g_drv->getSampleRate(&rate) == ASE_OK && rate == 48000.0, "sample rate %.0f", rate);
    CHECK(g_drv->canSampleRate(44100.0) == ASE_NoClock, "unsupported rate is refused with the driver's own error code");
    ASIOClockSource clock = {}; long nclock = 1;
    CHECK(g_drv->getClockSources(&clock, &nclock) == ASE_OK && !strcmp(clock.name, "DummyClock"), "clock source '%s'", clock.name);
    ASIOChannelInfo ci = {}; ci.channel = 1; ci.isInput = ASIOFalse;
    CHECK(g_drv->getChannelInfo(&ci) == ASE_OK && ci.type == ASIOSTInt32LSB && !strcmp(ci.name, "Dummy Out 2"), "channel info '%s'", ci.name);
    CHECK(g_drv->controlPanel() == ASE_NotPresent, "controlPanel result forwarded");

    HANDLE burners[64] = {};
    if (burn > 64) burn = 64;
    if (burn > 0) { g_burn = true; for (long i = 0; i < burn; i++) burners[i] = CreateThread(nullptr, 0, burner, nullptr, 0, nullptr); printf("      CPU load: %ld spinning threads\n", burn); }

    if (cycles > 0) {
        // ---- 生命週期測試：反覆建立與釋放，看有沒有漏記憶體或漏控制代碼 ----
        g_quiet = true;
        Usage base = {};
        long long total_cb = 0;
        for (long n = 1; n <= cycles; n++) {
            if (!make_buffers()) { CHECK(false, "createBuffers at cycle %ld", n); break; }
            if (g_drv->start() != ASE_OK) { CHECK(false, "start at cycle %ld", n); break; }
            Sleep(8);
            g_drv->stop();
            XmTestStats st = {};
            g_drv->future(kXmTestGetStats, &st);
            if (st.pattern_errors != 0 || g_input_errors != 0 || g_position_errors != 0) { CHECK(false, "data error at cycle %ld", n); break; }
            g_drv->disposeBuffers();
            if (n % 50 == 0) {
                // 整個驅動卸載再重載（代理會跟著釋放並重新載入它後面的真驅動）
                total_cb += g_callbacks; g_callbacks = 0; g_time_info_calls = 0; g_plain_calls = 0;
                if (g_drv->Release() != 0) { CHECK(false, "driver object not released at cycle %ld", n); break; }
                g_drv = nullptr;
                // 連 DLL 本身也卸載再載入，和 DAW 切換音訊裝置時做的事一樣
                FreeLibrary(g_module);
                g_module = nullptr;
                if (!open_driver(argv[1])) break;
            }
            if (n == 100) { base = usage(); print_usage("cycle 100", base); }
            if (n % 1000 == 0) { char l[32]; snprintf(l, sizeof l, "cycle %ld", n); print_usage(l, usage()); }
        }
        g_quiet = false;
        Usage end = usage();
        print_usage("end", end);
        total_cb += g_callbacks;
        printf("      %ld cycles, %lld callbacks in total\n", cycles, total_cb);
        CHECK(g_fail == 0, "all %ld create/start/stop/dispose cycles without error", cycles);
        if (cycles > 100) {
            CHECK(end.private_mb - base.private_mb < 0.5, "no memory growth after cycle 100 (%.3f MB)", end.private_mb - base.private_mb);
            CHECK(end.handles <= base.handles + 2, "no handle growth after cycle 100 (%lu -> %lu)", base.handles, end.handles);
        }
        if (g_drv) CHECK(g_drv->Release() == 0, "driver object released");
    } else {
        // ---- 串流測試 ----
        CHECK(make_buffers(), "createBuffers");
        bool same = true;
        for (int c = 0; c < kChannels; c++)
            for (int h = 0; h < 2; h++) {
                XmTestBufferQuery q = {c, h, nullptr};
                if (g_drv->future(kXmTestGetBufferPtr, &q) != ASE_SUCCESS || q.ptr != g_infos[c].buffers[h]) same = false;
            }
        CHECK(same, "output buffers are the real driver's own (zero-copy) and unknown future() selectors are forwarded");
        // 共享記憶體是分線盒在 createBuffers 時建立的；直接接假驅動時不存在
        HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kXmTapMappingName);
        if (map) g_sh = static_cast<XmTapShared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        HANDLE reader = nullptr;
        if (g_sh) {
            CHECK(g_sh->magic == kXmTapMagic && g_sh->version == kXmTapVersion && (g_sh->generation & 1) == 0 && g_sh->sample_rate == 48000 &&
                      g_sh->block_frames == static_cast<uint32_t>(g_block) && g_sh->sample_type == ASIOSTInt32LSB && g_sh->bytes_per_sample == 4 &&
                      g_sh->writer_pid == GetCurrentProcessId() && !strcmp(g_sh->driver_name, "XmDummy") && !strcmp(g_sh->channel_name[1], "Dummy Out 2"),
                  "tap: shared memory header describes the stream (rate %u, block %u, type %u, driver '%s')", g_sh->sample_rate, g_sh->block_frames,
                  g_sh->sample_type, g_sh->driver_name);
            if (!no_reader) {
                g_sh->reader_tick = GetTickCount64();   // 開始之前就有心跳，分線盒從第一個可抄的週期開始抄
                g_reader_run = true;
                reader = CreateThread(nullptr, 0, reader_main, nullptr, 0, nullptr);
            } else {
                g_sh->reader_tick = 0;
            }
        }
        const uint64_t tap_w0 = g_sh ? g_sh->write_frames : 0;
        Usage start = usage();
        LARGE_INTEGER qf, q0, q1; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
        CHECK(g_drv->start() == ASE_OK, "start");
        if (reader_pause && reader) {
            Sleep(1500);
            g_reader_beat = false;          // 示波器「當掉」3 秒（超過分線盒的 2 秒逾時）
            Sleep(3000);
            g_sh->reader_tick = GetTickCount64();
            g_reader_beat = true;           // 回來了
            Sleep(1500);
        }
        Usage warm = {};
        double waited = 0;
        while (waited < seconds) {
            double step = seconds - waited < 10 ? seconds - waited : 10;
            Sleep(static_cast<DWORD>(step * 1000));
            waited += step;
            if (waited >= 10 && warm.private_mb == 0) warm = usage();
            if (seconds > 15) { char l[32]; snprintf(l, sizeof l, "%.0f s", waited); print_usage(l, usage()); printf("      callbacks so far %lld\n", g_callbacks); }
        }
        CHECK(g_drv->stop() == ASE_OK, "stop");
        QueryPerformanceCounter(&q1);
        double wall = static_cast<double>(q1.QuadPart - q0.QuadPart) / qf.QuadPart;
        long long at_stop = g_callbacks;
        Sleep(100);
        CHECK(g_callbacks == at_stop, "no callbacks after stop");
        Usage end = usage();

        XmTestStats st = {};
        CHECK(g_drv->future(kXmTestGetStats, &st) == ASE_SUCCESS, "stats query");
        printf("      host callbacks %lld (timeInfo %lld, plain %lld), driver callbacks %lld, %.0f callbacks/s\n", g_callbacks, g_time_info_calls, g_plain_calls, st.callbacks, g_callbacks / wall);
        printf("      callback time (driver side): mean %.0f ns, max %.1f us, over 50 us: %lld\n", st.callbacks ? static_cast<double>(st.cb_ns_sum) / st.callbacks : 0.0, st.cb_ns_max / 1000.0, st.cb_over_50us);
        printf("      process: cpu %.1f s over %.1f s wall (%.1f%% of one core)\n", end.cpu_s - start.cpu_s, wall, 100.0 * (end.cpu_s - start.cpu_s) / wall);
        print_usage("start", start); print_usage("end", end);
        CHECK(g_callbacks > 50 && g_callbacks == st.callbacks, "every driver callback reached the host exactly once");
        CHECK(g_use_time_info ? (g_time_info_calls == g_callbacks && g_plain_calls == 0) : (g_plain_calls == g_callbacks && g_time_info_calls == 0),
              "callback kind matches what the host asked for (%s)", g_use_time_info ? "bufferSwitchTimeInfo" : "bufferSwitch");
        CHECK(g_position_errors == 0 && g_last_pos == st.last_position, "sample positions exact and contiguous (last %lld)", g_last_pos);
        CHECK(g_input_errors == 0, "input data bit-exact (%lld wrong samples)", g_input_errors);
        CHECK(st.pattern_errors == 0, "output data bit-exact at the driver (%lld wrong samples)", st.pattern_errors);
        if (g_sh) {
            if (reader) { g_reader_run = false; WaitForSingleObject(reader, 3000); CloseHandle(reader); }
            const long long written = static_cast<long long>(g_sh->write_frames - tap_w0);
            const long long expect = (g_callbacks - 1) * g_block;   // 每個週期抄上一塊，開始後第一個週期不抄
            printf("      tap: %lld frames written, %lld read back, %lld segment(s), %lld overrun(s)\n", written, g_tap_frames, g_tap_segments, g_tap_overruns);
            if (no_reader) {
                CHECK(written == 0, "tap: nothing is copied while no reader is listening");
            } else if (reader_pause) {
                CHECK(written > 0 && written < expect && g_tap_segments == 2, "tap: copying stopped while the reader was away and resumed as a new segment");
                CHECK(g_tap_frames == written && g_tap_errors == 0 && g_tap_start_pos_errors == 0 && g_tap_overruns == 0,
                      "tap: every copied sample is bit-exact and contiguous within each segment (%lld wrong)", g_tap_errors);
            } else {
                CHECK(written == expect, "tap: one block copied per callback (%lld of %lld frames)", written, expect);
                CHECK(g_tap_frames == written && g_tap_overruns == 0 && g_tap_segments == 1, "tap: reader received every frame as one unbroken segment");
                CHECK(g_tap_errors == 0 && g_tap_start_pos_errors == 0, "tap: copied data bit-exact, contiguous, and stamped with the right sample position (%lld wrong)", g_tap_errors);
            }
        }
        if (warm.private_mb > 0) {
            CHECK(end.private_mb - warm.private_mb < 0.5, "no memory growth after the first 10 s (%.3f MB)", end.private_mb - warm.private_mb);
            CHECK(end.handles <= warm.handles + 2, "no handle growth (%lu -> %lu)", warm.handles, end.handles);
        }
        CHECK(g_drv->disposeBuffers() == ASE_OK, "disposeBuffers");
        CHECK(g_drv->Release() == 0, "driver object released");
    }
    if (burn > 0) { g_burn = false; WaitForMultipleObjects(static_cast<DWORD>(burn), burners, TRUE, 2000); }
    printf("%s (%d failed)\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS", g_fail);
    return g_fail ? 1 : 0;
}
