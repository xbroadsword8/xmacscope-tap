// xMacScope Tap 的測試用假驅動：扮演「真正的硬體 ASIO 驅動」。
// Copyright (C) 2026 金皓哲 (xmac jin)
// SPDX-License-Identifier: GPL-3.0-only
//
// 2 進 2 出、Int32、48 kHz、每塊 256 個樣本，用一條執行緒定時回呼。
// 它會檢查每次回呼返回後輸出緩衝區的內容是不是測試程式該寫的樣式，
// 藉此確認資料經過代理後一個位元都沒變。回報的延遲等數值故意用不常見的數字，
// 測試程式才分得出「代理有照轉」和「代理自己編了一個合理的值」。
#include "../src/sdk.h"
#include "../src/nolibstdcxx.h"
#include <cstring>

// {5D0B2B7E-3C1A-4E0C-9A51-7E2D6C8F4B10}
static const CLSID CLSID_Dummy = {0x5d0b2b7e, 0x3c1a, 0x4e0c, {0x9a, 0x51, 0x7e, 0x2d, 0x6c, 0x8f, 0x4b, 0x10}};

enum { kChannels = 2, kInLatency = 311, kOutLatency = 523 };

// 測試程式與假驅動共用的樣式（由取樣位置與聲道決定）
static inline long out_pattern(long long pos, long ch) { return static_cast<long>(pos * 3 + ch * 1000003 + 7); }
static inline long in_pattern(long long pos, long ch) { return static_cast<long>(pos * 5 - ch * 999983 - 11); }

class Dummy final : public IASIO {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_Dummy)) {
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
        if (n == 0) {
            stop();
            disposeBuffers();
            xm_destroy(this);
        }
        return n;
    }

    // 壓力測試用：XM_DUMMY_BLOCK = 每塊樣本數（預設 256），
    // XM_DUMMY_PERIOD_US = 兩次回呼之間休息多久（預設 5000；0 = 完全不休息，全速連發）
    // XM_DUMMY_REPEAT = N：每 N 次回呼就連續三次給同一半緩衝區（預設 0 = 兩半永遠輪流）。
    //   模仿 RME 的驅動在小緩衝區下趕不上時的行為；重複給的那一半，驅動不會先動它的內容。
    ASIOBool init(void*) override {
        char v[32];
        if (GetEnvironmentVariableA("XM_DUMMY_BLOCK", v, sizeof v)) block_ = atol(v);
        if (block_ < 16 || block_ > 8192) block_ = 256;
        if (GetEnvironmentVariableA("XM_DUMMY_PERIOD_US", v, sizeof v)) period_us_ = atol(v);
        if (GetEnvironmentVariableA("XM_DUMMY_REPEAT", v, sizeof v)) repeat_every_ = atol(v);
        QueryPerformanceFrequency(&qpf_);
        return ASIOTrue;
    }
    void getDriverName(char* name) override { strcpy(name, "XmDummy"); }
    long getDriverVersion() override { return 4242; }
    void getErrorMessage(char* s) override { strcpy(s, "dummy: no error"); }
    ASIOError start() override {
        if (running_ || !callbacks_.bufferSwitch) return ASE_NotPresent;
        running_ = true;
        thread_ = CreateThread(nullptr, 0, thread_main, this, 0, nullptr);
        return thread_ ? ASE_OK : ASE_HWMalfunction;
    }
    ASIOError stop() override {
        if (!running_) return ASE_OK;
        running_ = false;
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
        return ASE_OK;
    }
    ASIOError getChannels(long* in, long* out) override { *in = kChannels; *out = kChannels; return ASE_OK; }
    ASIOError getLatencies(long* in, long* out) override { *in = kInLatency; *out = kOutLatency; return ASE_OK; }
    ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
        *mn = *mx = *pref = block_; *gran = 0; return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate r) override { return r == 48000.0 ? ASE_OK : ASE_NoClock; }
    ASIOError getSampleRate(ASIOSampleRate* r) override { *r = 48000.0; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate r) override { return r == 48000.0 ? ASE_OK : ASE_NoClock; }
    ASIOError getClockSources(ASIOClockSource* c, long* n) override {
        c->index = 0; c->associatedChannel = -1; c->associatedGroup = -1; c->isCurrentSource = ASIOTrue;
        strcpy(c->name, "DummyClock"); *n = 1; return ASE_OK;
    }
    ASIOError setClockSource(long ref) override { return ref == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* pos, ASIOTimeStamp* ts) override {
        pos->hi = static_cast<unsigned long>(static_cast<unsigned long long>(position_) >> 32);
        pos->lo = static_cast<unsigned long>(position_);
        ts->hi = 0; ts->lo = static_cast<unsigned long>(position_ * 20833);
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (info->channel < 0 || info->channel >= kChannels) return ASE_InvalidParameter;
        info->isActive = ASIOTrue; info->channelGroup = 0; info->type = ASIOSTInt32LSB;
        wsprintfA(info->name, "Dummy %s %ld", info->isInput ? "In" : "Out", info->channel + 1);
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long channels, long size, ASIOCallbacks* callbacks) override {
        if (size != block_) return ASE_InvalidMode;
        for (long i = 0; i < channels; i++) {
            long ch = infos[i].channelNum;
            if (ch < 0 || ch >= kChannels) return ASE_InvalidParameter;
            long** slot = infos[i].isInput ? in_[ch] : out_[ch];
            for (int h = 0; h < 2; h++) {
                if (!slot[h]) slot[h] = static_cast<long*>(calloc(block_, sizeof(long)));
                infos[i].buffers[h] = slot[h];
            }
        }
        callbacks_ = *callbacks;
        time_info_ = callbacks_.asioMessage && callbacks_.asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        stop();
        for (int c = 0; c < kChannels; c++)
            for (int h = 0; h < 2; h++) {
                free(in_[c][h]); in_[c][h] = nullptr;
                free(out_[c][h]); out_[c][h] = nullptr;
            }
        callbacks_ = {};
        return ASE_OK;
    }
    ASIOError controlPanel() override { return ASE_NotPresent; }
    ASIOError future(long selector, void* opt) override {
        if (selector == kXmTestGetBufferPtr) {
            auto* q = static_cast<XmTestBufferQuery*>(opt);
            q->ptr = out_[q->channel][q->half];
            return ASE_SUCCESS;
        }
        if (selector == kXmTestGetStats) {
            auto* s = static_cast<XmTestStats*>(opt);
            s->callbacks = callback_count_; s->pattern_errors = pattern_errors_; s->last_position = last_position_;
            s->cb_ns_sum = cb_ns_sum_; s->cb_ns_max = cb_ns_max_; s->cb_over_50us = cb_over_50us_;
            return ASE_SUCCESS;
        }
        return ASE_InvalidParameter;
    }
    ASIOError outputReady() override { return ASE_OK; }

private:
    static DWORD WINAPI thread_main(void* self) { static_cast<Dummy*>(self)->run(); return 0; }
    void run() {
        long index = 0, prev_index = -1;
        while (running_) {
            for (int c = 0; c < kChannels; c++) {
                if (in_[c][index])
                    for (int i = 0; i < block_; i++) in_[c][index][i] = in_pattern(position_ + i, c);
                if (out_[c][index] && index != prev_index)
                    for (int i = 0; i < block_; i++) out_[c][index][i] = 0x7fffffff;   // 哨兵：DAW 沒寫就看得出來
            }
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            if (time_info_ && callbacks_.bufferSwitchTimeInfo) {
                ASIOTime t = {};
                t.timeInfo.speed = 1.0;
                t.timeInfo.samplePosition.hi = static_cast<unsigned long>(static_cast<unsigned long long>(position_) >> 32);
                t.timeInfo.samplePosition.lo = static_cast<unsigned long>(position_);
                t.timeInfo.systemTime.lo = static_cast<unsigned long>(position_ * 20833);
                t.timeInfo.sampleRate = 48000.0;
                t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
                callbacks_.bufferSwitchTimeInfo(&t, index, ASIOTrue);
            } else {
                callbacks_.bufferSwitch(index, ASIOTrue);
            }
            QueryPerformanceCounter(&t1);
            long long ns = (t1.QuadPart - t0.QuadPart) * 1000000000LL / qpf_.QuadPart;
            cb_ns_sum_ += ns;
            if (ns > cb_ns_max_) cb_ns_max_ = ns;
            if (ns > 50000) cb_over_50us_++;
            for (int c = 0; c < kChannels; c++)
                if (out_[c][index])
                    for (int i = 0; i < block_; i++)
                        if (out_[c][index][i] != out_pattern(position_ + i, c)) pattern_errors_++;
            last_position_ = position_;
            callback_count_++;
            position_ += block_;
            prev_index = index;
            if (!(repeat_every_ > 1 && callback_count_ % repeat_every_ <= 1)) index = 1 - index;
            if (period_us_ > 0) Sleep(static_cast<DWORD>((period_us_ + 999) / 1000));
        }
    }

    LONG refs_ = 0;
    long* in_[kChannels][2] = {};
    long* out_[kChannels][2] = {};
    ASIOCallbacks callbacks_ = {};
    bool time_info_ = false;
    volatile bool running_ = false;
    HANDLE thread_ = nullptr;
    long long position_ = 0;
    long long last_position_ = -1;
    long long callback_count_ = 0;
    long long pattern_errors_ = 0;
    long block_ = 256;
    long period_us_ = 5000;
    long repeat_every_ = 0;
    LARGE_INTEGER qpf_ = {};
    long long cb_ns_sum_ = 0, cb_ns_max_ = 0, cb_over_50us_ = 0;
};

class DummyFactory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        Dummy* d = xm_create<Dummy>();
        if (!d) return E_OUTOFMEMORY;
        d->AddRef();
        HRESULT hr = d->QueryInterface(riid, out);
        d->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
static DummyFactory g_factory;

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
    if (!IsEqualCLSID(clsid, CLSID_Dummy)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, out);
}
extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }
