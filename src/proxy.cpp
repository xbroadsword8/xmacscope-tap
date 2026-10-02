// xMacScope Tap — ASIO pass-through proxy
// Copyright (C) 2026 金皓哲 (xmac jin)
// SPDX-License-Identifier: GPL-3.0-only
//
// 第一階段：純轉發，不旁聽。
//
// DAW 載入這個 DLL，以為它是一張音效卡；它把每一個呼叫原封不動轉給真正的硬體驅動。
// 設計上要守住的三件事：
//   1. 緩衝區是真驅動配置的，指標原樣交給 DAW —— 零拷貝，不增加延遲，資料逐位元相同。
//   2. 回呼由真驅動的即時執行緒發起，這裡只轉呼叫 —— 時脈與時間戳完全是真驅動的。
//   3. 即時路徑（下面四個 cb_ 函式與 tap_copy）不配置記憶體、不上鎖、不做會等待的系統呼叫。
//
// 第二階段：旁聽。每個週期一開始，把「上一個週期交給 DAW 填的那一半」的前兩個輸出聲道複製到共享記憶體，
// 示波器從那裡讀（格式見 tap_shared.h）。示波器沒在讀的時候完全不複製。
#include "sdk.h"
#include "nolibstdcxx.h"
#include "tap_shared.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>

// {A010EB30-1F0E-4ECE-A2C9-058C3FB84A1F}
static const CLSID CLSID_XmTap = {0xa010eb30, 0x1f0e, 0x4ece, {0xa2, 0xc9, 0x05, 0x8c, 0x3f, 0xb8, 0x4a, 0x1f}};
static const char kDriverName[] = "xMacScope Tap";
static const char kClsidString[] = "{A010EB30-1F0E-4ECE-A2C9-058C3FB84A1F}";

// 還沒接上真驅動時的回傳值（ASIOError 是 long，ASE_NotPresent 是列舉；先轉好免得每個三元運算式都混型別）
static const ASIOError kNotPresent = ASE_NotPresent;

static HMODULE g_module = nullptr;
static LONG g_objects = 0;

// ---- 診斷記錄 ---------------------------------------------------------------
// 預設關閉。HKCU\Software\xMacScope\Tap 的 Log 設成 "1" 才會寫 %TEMP%\xmacscope_tap.log，
// 用來看 DAW 實際上怎麼使用驅動。只在非即時的呼叫（init、createBuffers、start…）裡寫檔；
// 回呼裡只累加計數器，等 stop / disposeBuffers 時才一次寫出。
static HANDLE g_log = INVALID_HANDLE_VALUE;
static volatile LONG g_cb_switch = 0, g_cb_time_info = 0, g_cb_rate = 0, g_cb_message = 0, g_output_ready = 0;
static volatile LONG g_msg_seen[16] = {};

static void log_open();
static void logf(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char line[512];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [pid %lu tid %lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                     GetCurrentProcessId(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    va_end(ap);
    if (n > static_cast<int>(sizeof line) - 3) n = sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    DWORD written;
    WriteFile(g_log, line, n, &written, nullptr);
}

// ---- 旁聽 -------------------------------------------------------------------
static HANDLE g_tap_map = nullptr;
static XmTapShared* g_tap = nullptr;      // 共享記憶體；建不起來就是 nullptr，分線盒照常純轉發
static const void* g_tap_src[kXmTapChannels][2] = {};   // 要旁聽的輸出聲道，各兩個半緩衝區（真驅動的緩衝區）
static long g_tap_block = 0;              // 0 = 目前沒有緩衝區，不旁聽
static long g_tap_bytes = 0;              // 每個樣本幾個位元組
static bool g_tap_armed = false;          // 正在連續寫入一段資料
static long g_tap_prev = -1;              // 上一個週期交給 DAW 的是哪一半；-1 = 剛開始，還沒有 DAW 填過的資料
static volatile LONG g_tap_blocks = 0;    // 抄了幾塊（診斷用）

// 在回呼裡、轉給 DAW 之前呼叫。index 是這個週期要交給 DAW 的那一半；我們抄的是「上一個週期交給 DAW 的那一半」：
// DAW 已經填完它（不管是當場填還是稍後才填），內容是完整的。
//
// 不能假設兩半一定輪流（抄「另一半」）：RME 的驅動在小緩衝區下趕不上時，會連續兩三次給同一半
//（2026-10-02 實測，96 kHz、64 樣本，每分鐘數次；不經過分線盒也一樣）。那時「另一半」裡是更舊的資料。
// 記住上一次給的是哪一半，就不依賴驅動的順序；同一半連續給的時候，這裡是在 DAW 覆寫它之前抄走的。
// pos = 這個週期的取樣位置，不知道就給 -1。
static inline void tap_copy(long index, long long pos) {
    XmTapShared* sh = g_tap;
    const long block = g_tap_block;
    if (!sh || block <= 0 || (index != 0 && index != 1)) return;
    const long prev = g_tap_prev;
    g_tap_prev = index;
    const ULONGLONG now = GetTickCount64();   // 讀共享的系統時間，不進核心
    if (now - sh->reader_tick > kXmTapReaderTimeoutMs) {   // 示波器沒在讀：不抄
        g_tap_armed = false;
        return;
    }
    if (prev < 0) {                     // 開始後的第一個週期：還沒有 DAW 填過的資料
        g_tap_armed = false;
        return;
    }
    const uint64_t w = sh->write_frames;
    if (!g_tap_armed) {
        sh->segment_start_frames = w;
        sh->segment_start_pos = pos >= 0 ? pos - block : -1;
        sh->segment = sh->segment + 1;
        g_tap_armed = true;
    }
    const uint32_t at = static_cast<uint32_t>(w & (kXmTapRingFrames - 1));
    const uint32_t first = kXmTapRingFrames - at < static_cast<uint32_t>(block) ? kXmTapRingFrames - at : static_cast<uint32_t>(block);
    uint8_t* base = reinterpret_cast<uint8_t*>(sh);
    for (uint32_t c = 0; c < kXmTapChannels; c++) {
        const uint8_t* src = static_cast<const uint8_t*>(g_tap_src[c][prev]);
        if (!src) continue;
        uint8_t* ring = base + xm_tap_channel_offset(c);
        memcpy(ring + static_cast<size_t>(at) * g_tap_bytes, src, static_cast<size_t>(first) * g_tap_bytes);
        if (first < static_cast<uint32_t>(block))   // 繞回環形緩衝區的開頭
            memcpy(ring, src + static_cast<size_t>(first) * g_tap_bytes, static_cast<size_t>(block - first) * g_tap_bytes);
    }
    MemoryBarrier();                    // 資料先落地，再讓讀取者看到新的 write_frames
    sh->write_frames = w + block;
    sh->writer_tick = now;
    g_tap_blocks++;
}

// ---- 即時路徑 -------------------------------------------------------------
// ASIO 的回呼沒有 user pointer，只能用全域狀態；同一個行程同時只會有一個作用中的 ASIO 驅動。
static ASIOCallbacks g_host = {};   // DAW 給的回呼

static void cb_bufferSwitch(long index, ASIOBool direct) {
    g_cb_switch++;
    tap_copy(index, -1);
    if (g_host.bufferSwitch) g_host.bufferSwitch(index, direct);
}
static void cb_sampleRateDidChange(ASIOSampleRate rate) {
    g_cb_rate++;
    if (g_host.sampleRateDidChange) g_host.sampleRateDidChange(rate);
}
static long cb_asioMessage(long selector, long value, void* message, double* opt) {
    g_cb_message++;
    if (selector >= 0 && selector < 16) g_msg_seen[selector]++;
    return g_host.asioMessage ? g_host.asioMessage(selector, value, message, opt) : 0;
}
static ASIOTime* cb_bufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool direct) {
#ifdef XM_TEST_FAULT
    // 只給測試用的故障版：每 10 次吞掉一次回呼，用來確認測試程式真的抓得到代理出錯。
    static long n = 0;
    if (++n % 10 == 0) return nullptr;
#endif
    g_cb_time_info++;
    tap_copy(index, params && (params->timeInfo.flags & kSamplePositionValid)
                        ? (static_cast<long long>(params->timeInfo.samplePosition.hi) << 32) | params->timeInfo.samplePosition.lo
                        : -1);
    return g_host.bufferSwitchTimeInfo ? g_host.bufferSwitchTimeInfo(params, index, direct) : nullptr;
}
static ASIOCallbacks g_ours = {cb_bufferSwitch, cb_sampleRateDidChange, cb_asioMessage, cb_bufferSwitchTimeInfo};

// ---- 旁聽的設定（非即時）-----------------------------------------------------
static long tap_bytes_per_sample(long type) {
    switch (type) {
    case ASIOSTInt16LSB: case ASIOSTInt16MSB: return 2;
    case ASIOSTInt24LSB: case ASIOSTInt24MSB: return 3;
    case ASIOSTFloat64LSB: case ASIOSTFloat64MSB: return 8;
    case ASIOSTInt32LSB: case ASIOSTInt32MSB: case ASIOSTFloat32LSB: case ASIOSTFloat32MSB:
    case ASIOSTInt32LSB16: case ASIOSTInt32LSB18: case ASIOSTInt32LSB20: case ASIOSTInt32LSB24:
    case ASIOSTInt32MSB16: case ASIOSTInt32MSB18: case ASIOSTInt32MSB20: case ASIOSTInt32MSB24: return 4;
    default: return 0;   // DSD 等不旁聽
    }
}

// 建立（或打開已存在的）共享記憶體。失敗就維持 nullptr，分線盒照常純轉發。
static void tap_open() {
    if (g_tap) return;
    const DWORD bytes = static_cast<DWORD>(xm_tap_mapping_bytes());
    g_tap_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, kXmTapMappingName);
    if (!g_tap_map) {
        logf("tap: CreateFileMapping failed (%lu)", GetLastError());
        return;
    }
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    void* view = MapViewOfFile(g_tap_map, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (!view) {
        logf("tap: MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(g_tap_map);
        g_tap_map = nullptr;
        return;
    }
    XmTapShared* sh = static_cast<XmTapShared*>(view);
    if (!existed || sh->magic != kXmTapMagic || sh->version != kXmTapVersion) {
        memset(view, 0, bytes);          // 順便把每一頁都碰過，之後即時路徑不會遇到第一次存取的分頁錯誤
        sh->magic = kXmTapMagic;
        sh->version = kXmTapVersion;
        sh->header_bytes = kXmTapHeaderBytes;
        sh->ring_frames = kXmTapRingFrames;
        sh->channels = kXmTapChannels;
    } else {
        volatile uint8_t sink = 0;       // 已存在：一樣把每一頁碰過
        for (DWORD i = 0; i < bytes; i += 4096) sink = sink + static_cast<const uint8_t*>(view)[i];
    }
    VirtualLock(view, bytes);            // 盡量把這塊鎖在實體記憶體；沒有權限時會失敗，沒關係
    g_tap = sh;
    logf("tap: shared memory %s (%lu bytes)", existed ? "opened" : "created", bytes);
}

static void tap_close() {
    g_tap_block = 0;
    if (g_tap) {
        UnmapViewOfFile(g_tap);
        g_tap = nullptr;
    }
    if (g_tap_map) {
        CloseHandle(g_tap_map);
        g_tap_map = nullptr;
    }
}

// createBuffers 成功後呼叫：決定要旁聽哪兩個輸出聲道，並把格式寫進表頭。
static void tap_configure(IASIO* real, const ASIOBufferInfo* infos, long channels, long block) {
    g_tap_block = 0;                     // 回呼這時還沒開始
    g_tap_armed = false;
    g_tap_prev = -1;
    for (auto& ch : g_tap_src) ch[0] = ch[1] = nullptr;
    tap_open();
    if (!g_tap) return;
    // 優先取輸出聲道 0、1（通常是主輸出）；DAW 沒有為它們建緩衝區時，退而取它建的前兩個輸出
    long picked[kXmTapChannels] = {-1, -1};
    for (long i = 0; i < channels; i++)
        if (!infos[i].isInput && infos[i].channelNum >= 0 && infos[i].channelNum < static_cast<long>(kXmTapChannels)) picked[infos[i].channelNum] = i;
    if (picked[0] < 0 || picked[1] < 0) {
        long n = 0;
        picked[0] = picked[1] = -1;
        for (long i = 0; i < channels && n < static_cast<long>(kXmTapChannels); i++)
            if (!infos[i].isInput) picked[n++] = i;
    }
    if (picked[0] < 0) {
        logf("tap: host created no output buffers, nothing to tap");
        return;
    }
    if (picked[1] < 0) picked[1] = picked[0];   // 只有一個輸出：兩個聲道都用它
    ASIOChannelInfo info[kXmTapChannels] = {};
    for (uint32_t c = 0; c < kXmTapChannels; c++) {
        info[c].channel = infos[picked[c]].channelNum;
        info[c].isInput = ASIOFalse;
        real->getChannelInfo(&info[c]);
    }
    const long bytes = tap_bytes_per_sample(info[0].type);
    ASIOSampleRate rate = 0;
    real->getSampleRate(&rate);
    if (bytes == 0 || info[1].type != info[0].type || block <= 0 || block > static_cast<long>(kXmTapRingFrames / 4) || rate <= 0) {
        logf("tap: unsupported format (type %ld, block %ld, rate %.0f), not tapping", info[0].type, block, rate);
        return;
    }
    XmTapShared* sh = g_tap;
    sh->generation = sh->generation | 1;   // 奇數：格式正在改
    MemoryBarrier();
    sh->sample_rate = static_cast<uint32_t>(rate + 0.5);
    sh->sample_type = static_cast<uint32_t>(info[0].type);
    sh->bytes_per_sample = static_cast<uint32_t>(bytes);
    sh->block_frames = static_cast<uint32_t>(block);
    sh->writer_pid = GetCurrentProcessId();
    real->getDriverName(sh->driver_name);
    sh->driver_name[sizeof sh->driver_name - 1] = 0;
    for (uint32_t c = 0; c < kXmTapChannels; c++) lstrcpynA(sh->channel_name[c], info[c].name, sizeof sh->channel_name[c]);
    MemoryBarrier();
    sh->generation = sh->generation + 1;   // 回到偶數，而且和之前不同
    for (uint32_t c = 0; c < kXmTapChannels; c++) {
        g_tap_src[c][0] = infos[picked[c]].buffers[0];
        g_tap_src[c][1] = infos[picked[c]].buffers[1];
    }
    g_tap_bytes = bytes;
    g_tap_blocks = 0;
    MemoryBarrier();
    g_tap_block = block;                 // 最後才打開，回呼看到非零時其他欄位都已就緒
    logf("tap: output channels %ld ('%s') and %ld ('%s'), type %ld (%ld bytes), block %ld, rate %.0f", info[0].channel, info[0].name,
         info[1].channel, info[1].name, info[0].type, bytes, block, rate);
}

// ---- 找到並載入真正的驅動 ---------------------------------------------------
static bool parse_clsid(const char* s, CLSID* out) {
    wchar_t w[64];
    if (!MultiByteToWideChar(CP_ACP, 0, s, -1, w, 64)) return false;
    return SUCCEEDED(CLSIDFromString(w, out));
}

static bool read_reg_string(HKEY root, const char* path, const char* name, char* out, DWORD size) {
    HKEY key;
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    LONG r = RegQueryValueExA(key, name, nullptr, &type, reinterpret_cast<BYTE*>(out), &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_SZ;
}

static void log_open() {
    if (g_log != INVALID_HANDLE_VALUE) return;
    char v[8];
    if (!read_reg_string(HKEY_CURRENT_USER, "Software\\xMacScope\\Tap", "Log", v, sizeof v) || v[0] != '1') return;
    char path[MAX_PATH];
    DWORD n = GetTempPathA(sizeof path, path);
    if (!n || n > sizeof path - 32) return;
    strcat(path, "xmacscope_tap.log");
    g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    logf("---- loaded by %s", exe);
}

static void log_counters(const char* when) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    logf("%s: callbacks bufferSwitch=%ld bufferSwitchTimeInfo=%ld sampleRateDidChange=%ld asioMessage=%ld outputReady=%ld", when,
         g_cb_switch, g_cb_time_info, g_cb_rate, g_cb_message, g_output_ready);
    char buf[200] = "";
    int n = 0;
    for (int i = 0; i < 16; i++)
        if (g_msg_seen[i]) n += snprintf(buf + n, sizeof buf - n, " sel%d=%ld", i, g_msg_seen[i]);
    if (n) logf("%s: asioMessage selectors from driver:%s", when, buf);
}

// 目標驅動的 CLSID：使用者指定的（HKCU\Software\xMacScope\Tap\Target），
// 沒指定就用 HKLM\SOFTWARE\ASIO 底下第一個不是自己的驅動。
static bool find_target(CLSID* out, char* name, DWORD name_size) {
    char buf[64];
    if (read_reg_string(HKEY_CURRENT_USER, "Software\\xMacScope\\Tap", "Target", buf, sizeof buf) && parse_clsid(buf, out)) {
        if (!IsEqualCLSID(*out, CLSID_XmTap)) {
            lstrcpynA(name, buf, name_size);
            return true;
        }
    }
    HKEY asio;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &asio) != ERROR_SUCCESS) return false;
    bool found = false;
    for (DWORD i = 0; !found; i++) {
        char sub[256];
        DWORD len = sizeof sub;
        if (RegEnumKeyExA(asio, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        char path[320];
        snprintf(path, sizeof path, "SOFTWARE\\ASIO\\%s", sub);
        if (read_reg_string(HKEY_LOCAL_MACHINE, path, "CLSID", buf, sizeof buf) && parse_clsid(buf, out) &&
            !IsEqualCLSID(*out, CLSID_XmTap)) {
            lstrcpynA(name, sub, name_size);
            found = true;
        }
    }
    RegCloseKey(asio);
    return found;
}

// ASIO 的慣例：驅動物件的介面 ID 就是它自己的 CLSID。
static IASIO* create_real(const CLSID& clsid, HMODULE* test_module) {
    void* p = nullptr;
    // 測試用：直接從指定的 DLL 載入，不經登錄檔（不用在系統上註冊任何東西就能測）
    char dll[MAX_PATH];
    if (GetEnvironmentVariableA("XMACSCOPE_TAP_TARGET_DLL", dll, sizeof dll)) {
        HMODULE m = LoadLibraryA(dll);
        if (!m) return nullptr;
        typedef HRESULT(__stdcall * GetClassObject)(REFCLSID, REFIID, void**);
        auto get = reinterpret_cast<GetClassObject>(reinterpret_cast<void*>(GetProcAddress(m, "DllGetClassObject")));
        IClassFactory* factory = nullptr;
        if (!get || FAILED(get(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory)))) {
            FreeLibrary(m);
            return nullptr;
        }
        HRESULT hr = factory->CreateInstance(nullptr, clsid, &p);
        factory->Release();
        if (FAILED(hr)) {
            FreeLibrary(m);
            return nullptr;
        }
        *test_module = m;
        return static_cast<IASIO*>(p);
    }
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, &p))) return nullptr;
    return static_cast<IASIO*>(p);
}

// ---- 代理物件 ---------------------------------------------------------------
class Proxy final : public IASIO {
public:
    Proxy() { InterlockedIncrement(&g_objects); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_XmTap)) {
            *out = static_cast<IASIO*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs_);
        if (n == 0) xm_destroy(this);
        return n;
    }

    // IASIO
    ASIOBool init(void* sys_handle) override {
        log_open();
        if (real_) return ASIOTrue;
        CLSID target;
        char name[256] = "";
        char env[64];
        // 測試用：XMACSCOPE_TAP_TARGET_CLSID 直接指定目標
        if (GetEnvironmentVariableA("XMACSCOPE_TAP_TARGET_CLSID", env, sizeof env) && parse_clsid(env, &target)) {
            lstrcpynA(name, env, sizeof name);
        } else if (!find_target(&target, name, sizeof name)) {
            snprintf(error_, sizeof error_, "xMacScope Tap: no other ASIO driver found to forward to");
            logf("init: %s", error_);
            return ASIOFalse;
        }
        real_ = create_real(target, &test_module_);
        if (!real_) {
            snprintf(error_, sizeof error_, "xMacScope Tap: cannot load target driver %s", name);
            logf("init: %s", error_);
            return ASIOFalse;
        }
        if (!real_->init(sys_handle)) {
            real_->getErrorMessage(error_);
            logf("init: target %s refused: %s", name, error_);
            drop_real();
            return ASIOFalse;
        }
        char real_name[128] = "";
        real_->getDriverName(real_name);
        logf("init ok: target %s (%s), version %ld, sysHandle %p", name, real_name, real_->getDriverVersion(), sys_handle);
        return ASIOTrue;
    }
    void getDriverName(char* name) override { strcpy(name, kDriverName); }
    long getDriverVersion() override { return real_ ? real_->getDriverVersion() : 1; }
    void getErrorMessage(char* s) override {
        if (real_) real_->getErrorMessage(s);
        else strcpy(s, error_);
    }
    ASIOError start() override {
        g_tap_prev = -1;         // 每次開始後的第一個週期不抄
        g_tap_armed = false;
        ASIOError e = real_ ? real_->start() : kNotPresent;
        logf("start -> %ld", e);
        return e;
    }
    ASIOError stop() override {
        ASIOError e = real_ ? real_->stop() : kNotPresent;
        g_tap_armed = false;     // 停止後真驅動不再回呼
        logf("stop -> %ld (tapped %ld blocks)", e, g_tap_blocks);
        log_counters("stop");
        return e;
    }
    ASIOError getChannels(long* in, long* out) override { return real_ ? real_->getChannels(in, out) : kNotPresent; }
    ASIOError getLatencies(long* in, long* out) override { return real_ ? real_->getLatencies(in, out) : kNotPresent; }
    ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
        return real_ ? real_->getBufferSize(mn, mx, pref, gran) : kNotPresent;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override { return real_ ? real_->canSampleRate(r) : kNotPresent; }
    ASIOError getSampleRate(ASIOSampleRate* r) override { return real_ ? real_->getSampleRate(r) : kNotPresent; }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        ASIOError e = real_ ? real_->setSampleRate(r) : kNotPresent;
        logf("setSampleRate(%.0f) -> %ld", r, e);
        return e;
    }
    ASIOError getClockSources(ASIOClockSource* c, long* n) override { return real_ ? real_->getClockSources(c, n) : kNotPresent; }
    ASIOError setClockSource(long ref) override {
        ASIOError e = real_ ? real_->setClockSource(ref) : kNotPresent;
        logf("setClockSource(%ld) -> %ld", ref, e);
        return e;
    }
    ASIOError getSamplePosition(ASIOSamples* pos, ASIOTimeStamp* ts) override {
        return real_ ? real_->getSamplePosition(pos, ts) : kNotPresent;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override { return real_ ? real_->getChannelInfo(info) : kNotPresent; }
    ASIOError createBuffers(ASIOBufferInfo* infos, long channels, long size, ASIOCallbacks* callbacks) override {
        if (!real_) return ASE_NotPresent;
        if (!callbacks) return ASE_InvalidParameter;
        // 先放好 DAW 的回呼再交給真驅動：真驅動可能在 createBuffers 裡就回呼（例如問 kAsioSupportsTimeInfo）。
        g_host = *callbacks;
        // infos 由真驅動填入它自己配置的緩衝區指標，原樣回給 DAW（零拷貝）。
        ASIOError e = real_->createBuffers(infos, channels, size, &g_ours);
        if (e != ASE_OK) g_host = {};
        else tap_configure(real_, infos, channels, size);
        long ins = 0, outs = 0;
        for (long i = 0; i < channels; i++) (infos[i].isInput ? ins : outs)++;
        logf("createBuffers(%ld channels: %ld in, %ld out; size %ld; host callbacks: switch=%d rate=%d message=%d timeInfo=%d) -> %ld", channels, ins,
             outs, size, callbacks->bufferSwitch != nullptr, callbacks->sampleRateDidChange != nullptr, callbacks->asioMessage != nullptr,
             callbacks->bufferSwitchTimeInfo != nullptr, e);
        return e;
    }
    ASIOError disposeBuffers() override {
        if (!real_) return ASE_NotPresent;
        ASIOError e = real_->disposeBuffers();   // 返回之後真驅動不會再回呼
        g_tap_block = 0;         // 緩衝區已經不存在，不能再抄
        for (auto& ch : g_tap_src) ch[0] = ch[1] = nullptr;
        g_host = {};
        logf("disposeBuffers -> %ld", e);
        log_counters("disposeBuffers");
        return e;
    }
    ASIOError controlPanel() override {
        ASIOError e = real_ ? real_->controlPanel() : kNotPresent;
        logf("controlPanel -> %ld", e);
        return e;
    }
    // 不認得的擴充指令也原樣轉發。
    ASIOError future(long selector, void* opt) override {
        ASIOError e = real_ ? real_->future(selector, opt) : kNotPresent;
        // 每種指令只記第一次（有些宿主會在即時執行緒裡呼叫，不能每次都寫檔）
        for (int i = 0; i < 16; i++) {
            if (future_seen_[i] == selector) break;
            if (future_seen_[i] == 0) {
                future_seen_[i] = selector;
                logf("future(selector %ld / 0x%lx) -> %ld (first call)", selector, static_cast<unsigned long>(selector), e);
                break;
            }
        }
        return e;
    }
    ASIOError outputReady() override {
        g_output_ready++;
        return real_ ? real_->outputReady() : kNotPresent;
    }

    ~Proxy() {
        logf("released");
        drop_real();
        InterlockedDecrement(&g_objects);
    }

private:
    void drop_real() {
        if (real_) {
            real_->Release();
            real_ = nullptr;
        }
        g_host = {};
        tap_close();
        if (test_module_) {
            FreeLibrary(test_module_);
            test_module_ = nullptr;
        }
    }

    LONG refs_ = 0;
    IASIO* real_ = nullptr;
    HMODULE test_module_ = nullptr;
    long future_seen_[16] = {};
    char error_[256] = "";
};

// ---- COM 類別工廠與 DLL 進入點 ----------------------------------------------
class Factory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }    // 靜態物件
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        Proxy* p = xm_create<Proxy>();
        if (!p) return E_OUTOFMEMORY;
        p->AddRef();
        HRESULT hr = p->QueryInterface(riid, out);
        p->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_objects);
        else InterlockedDecrement(&g_objects);
        return S_OK;
    }
};
static Factory g_factory;

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
    if (!IsEqualCLSID(clsid, CLSID_XmTap)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, out);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }

static LONG set_reg_string(HKEY root, const char* path, const char* name, const char* value) {
    HKEY key;
    LONG r = RegCreateKeyExA(root, path, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExA(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), static_cast<DWORD>(strlen(value) + 1));
    RegCloseKey(key);
    return r;
}

// 註冊：COM 類別（HKCR\CLSID）加上 ASIO 驅動清單（HKLM\SOFTWARE\ASIO）。需要系統管理員權限。
extern "C" HRESULT __stdcall DllRegisterServer() {
    char path[MAX_PATH];
    if (!GetModuleFileNameA(g_module, path, sizeof path)) return E_FAIL;
    char key[160];
    snprintf(key, sizeof key, "CLSID\\%s", kClsidString);
    if (set_reg_string(HKEY_CLASSES_ROOT, key, nullptr, kDriverName) != ERROR_SUCCESS) return E_ACCESSDENIED;
    snprintf(key, sizeof key, "CLSID\\%s\\InprocServer32", kClsidString);
    if (set_reg_string(HKEY_CLASSES_ROOT, key, nullptr, path) != ERROR_SUCCESS) return E_ACCESSDENIED;
    if (set_reg_string(HKEY_CLASSES_ROOT, key, "ThreadingModel", "Apartment") != ERROR_SUCCESS) return E_ACCESSDENIED;
    snprintf(key, sizeof key, "SOFTWARE\\ASIO\\%s", kDriverName);
    if (set_reg_string(HKEY_LOCAL_MACHINE, key, "CLSID", kClsidString) != ERROR_SUCCESS) return E_ACCESSDENIED;
    if (set_reg_string(HKEY_LOCAL_MACHINE, key, "Description", kDriverName) != ERROR_SUCCESS) return E_ACCESSDENIED;
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    char key[160];
    snprintf(key, sizeof key, "SOFTWARE\\ASIO\\%s", kDriverName);
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, key);
    snprintf(key, sizeof key, "CLSID\\%s\\InprocServer32", kClsidString);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, key);
    snprintf(key, sizeof key, "CLSID\\%s", kClsidString);
    RegDeleteKeyA(HKEY_CLASSES_ROOT, key);
    return S_OK;
}
