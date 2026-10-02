# xMacScope Tap（ASIO 代理驅動）

**English summary** — xMacScope Tap is a thin ASIO proxy driver for Windows. A DAW selects "xMacScope Tap" as its audio device;
the proxy forwards every call unchanged to the real hardware driver (RME, PreSonus, MOTU…) and copies the first two output
channels into shared memory so the xMacScope loudness/spectrum monitor can display what the DAW is playing. Buffers and
clocking stay the hardware driver's own: no added latency, no bit changed. Status: beta, tested with an RME UFX+ and Pro Tools.
Licensed under GPL-3.0-only. Built on Linux with mingw-w64 (`bash build.sh`).

Copyright (C) 2026 金皓哲 (xmac jin)

---

讓 xMacScope 看得到走 ASIO 的 DAW（Pro Tools、Cubase、Reaper…）送出的聲音。

DAW 在音訊裝置選單選「xMacScope Tap」，它把所有呼叫原封不動轉給真正的硬體驅動（RME、PreSonus、MOTU…），
並把前兩個輸出聲道複製一份到共享記憶體給 xMacScope 讀。緩衝區與時脈都是真驅動的，代理不增加延遲、不改任何一個位元。

這個 repo 只有分線盒。xMacScope 主程式是另一個獨立的程式（不在此公開），它從共享記憶體讀資料，格式見 `src/tap_shared.h`。

## 現況（2026-10-02）

轉發、旁聽、xMacScope 端的讀取都已完成；用假驅動、RME 的原廠驅動（UFX+）與 Pro Tools 測過。
隨 xMacScope 1.0.19 的安裝檔以「測試中」的選用元件發行。其他介面與 DAW 還沒測。

| 檔案 | 內容 |
|---|---|
| `src/proxy.cpp` | 代理驅動本體（轉發、旁聽、註冊） |
| `src/tap_shared.h` | 和 xMacScope 之間的共享記憶體格式 |
| `src/sdk.h` | 引入 ASIO SDK 的介面定義（含 mingw 需要補的結構對齊） |
| `src/nolibstdcxx.h` | 讓 DLL 不連結 libstdc++（原因見下） |
| `test/dummy_driver.cpp` | 測試用的假驅動，扮演「真正的硬體驅動」，會檢查收到的資料 |
| `test/host.cpp` | 測試程式，扮演 DAW 與示波器，接假驅動用 |
| `test/realtest.cpp` | 測試程式，接真正的硬體驅動用：直接與經過代理的對照、位元一致、負載 |

第一階段（純轉發）用假驅動測的結果：
- 三個元件都能在 Linux 上交叉編譯。
- 在 Windows 上跑過下面「不需要音訊硬體」的測試：直接接假驅動與經過代理，兩種回呼模式各一次，
  24 項檢查全部通過，結果相同（約 96 次回呼、取樣位置連續、進出資料 0 個樣本不同、緩衝區指標就是假驅動自己的）。
- 兩個對照組也符合預期：故意做壞的代理（每 10 次吞掉一次回呼，`xmacscope_tap_fault.dll`）被測試抓到 3 項 FAIL；
  目標驅動不存在時 `init` 失敗並回報原因。

資源用量與壓力測試（2026-10-02，Windows、i7-13700K；全部以假驅動進行，`host.exe` 的 `--seconds` / `--cycles` / `--burn`）：

| 項目 | 直接接假驅動 | 經過代理 |
|---|---|---|
| 行程私有記憶體 | 1.16 MB | 1.36 MB（+0.2 MB） |
| 控制代碼 | 108 | 114（+6） |
| 每次回呼耗時，實際速度（256 樣本） | 平均 667 ns | 平均 763 ns |
| 每次回呼耗時，全速連發（32 樣本、8 條空轉執行緒搶 CPU） | 平均 114 ns | 平均 118 ns |
| 全速連發 45 秒 | 2.16 億次回呼、0 錯誤 | 2.10 億次回呼、0 錯誤 |
| 反覆建立釋放 3000 次（每 50 次連 DLL 卸載重載） | 記憶體持平 | 記憶體持平（1.16 → 1.20 MB） |

實際速度下行程的 CPU 用量量不出來（20 秒內 0.0 秒）。回呼耗時的差距在量測雜訊的等級，全速時約 4 ns。

**抓到並修掉的一個問題**：生命週期測試一開始量到每次卸載重載成長約 90 KB。原因是 libstdc++ 在模組載入時配置的
緊急例外記憶池（約 73 KB）在 DLL 卸載時不釋放。現在兩個 DLL 都不連結 libstdc++（見 `src/nolibstdcxx.h`，
連結加 `-nostdlib++`），測試也改成重載時連驅動 DLL 本身一起卸載。

長時間測試：`soak.cmd`（經過代理，30 分鐘實際速度後接 5 分鐘全速加負載），結果寫在同資料夾的 `soak-*.log`。

## 接上 RME 的測試結果（2026-10-02，UFX+、96 kHz）

- 驅動回報的靜態資訊（通道、緩衝區、取樣率、時脈、延遲、全部通道的資訊）：經過代理與直接逐行相同。
- Pro Tools 透過代理實際播放約 40 分鐘、114,798 次回呼，沒有錯誤；取樣率切換、緩衝區重建、Pro Tools 自己的擴充指令都照轉。
- 旁聽位元一致（`realtest --verify`）：2048 樣本與最小的 64 樣本都是 0 個樣本不同。
- 回呼間隔：無負載時經過代理與直接分不出差別；有 CPU 負載時各次之間差異很大，只能說同一個量級。
- 小緩衝區下抓到並修掉一個旁聽的錯誤：RME 的驅動趕不上時會連續兩三次給同一半緩衝區，原本「抄另一半」的做法會抄到舊資料。
  現在改成記住上一個週期交給 DAW 的是哪一半。假驅動可用 `XM_DUMMY_REPEAT=N` 重現這種情況。

尚未做：用介面的數位回送驗證送往硬體那條路的位元一致；192 kHz；連跑數小時；RME 以外的介面；Pro Tools 以外的 DAW。

## 建置

```bash
bash build.sh      # 第一次會先從 Steinberg 官方下載 SDK 到 third_party/
```

產物在 `build/`：`xmacscope_tap.dll`、`xm_dummy.dll`、`host.exe`，只相依 Windows 系統 DLL。

## 不需要音訊硬體、也不用在系統註冊的測試

把 `build/` 裡三個檔放到 Windows 的同一個資料夾，各跑一次，兩次的檢查項目必須全部 PASS：

```bat
rem 基準：測試程式直接接假驅動
host.exe xm_dummy.dll {5D0B2B7E-3C1A-4E0C-9A51-7E2D6C8F4B10}

rem 經過代理：測試程式 → 代理 → 假驅動
set XMACSCOPE_TAP_TARGET_DLL=xm_dummy.dll
set XMACSCOPE_TAP_TARGET_CLSID={5D0B2B7E-3C1A-4E0C-9A51-7E2D6C8F4B10}
host.exe xmacscope_tap.dll {A010EB30-1F0E-4ECE-A2C9-058C3FB84A1F}
```

再各加 `--no-timeinfo` 跑一次（走另一種回呼）。檢查的內容：回報值照轉（延遲、取樣率、時脈來源、通道資訊、錯誤碼）、
DAW 拿到的緩衝區就是真驅動自己的（零拷貝）、不認得的擴充指令照轉、每個回呼剛好到達一次、取樣位置連續、
輸入與輸出資料逐位元相同、停止後不再回呼、物件正確釋放。

## 接上真正的介面（會改到系統登錄，需要系統管理員權限）

```bat
regsvr32 xmacscope_tap.dll        rem 需要系統管理員；會寫 HKCR\CLSID 與 HKLM\SOFTWARE\ASIO
regsvr32 /u xmacscope_tap.dll     rem 移除
```

目標驅動：`HKCU\Software\xMacScope\Tap` 的 `Target`（CLSID 字串）；沒設定就用系統上第一個不是自己的 ASIO 驅動。
xMacScope 的「設定 › DAW › DAW 分線盒…」視窗寫的就是這個值。

xMacScope 的安裝檔不呼叫 `regsvr32`，由安裝程式直接寫同樣的登錄值；兩邊的內容要一致。

不註冊也能對真正的驅動測試（測試程式直接從檔案載入代理，代理再去開已註冊的原廠驅動）：

```bat
set XMACSCOPE_TAP_TARGET_CLSID={原廠驅動的 CLSID，在 HKLM\SOFTWARE\ASIO 底下}
realtest.exe direct {原廠驅動的 CLSID} --seconds 30
realtest.exe proxy xmacscope_tap.dll {A010EB30-1F0E-4ECE-A2C9-058C3FB84A1F} --verify --seconds 30
```

`realtest` 只開前兩個輸入與最後兩個輸出，不改取樣率、不開控制面板。`--verify` 會在那兩個輸出寫一串聽不到的樣式
（32 位元格式約 −138 dBFS，24 位元硬體會整個截掉），同時扮演示波器，逐位元組比對旁聽到的和寫出去的是不是同一串。
`--burn N` 加 CPU 負載，`--block min` 在驅動允許宿主選緩衝區大小時用最小值。

## 授權

這個子專案引用 Steinberg ASIO SDK 的介面定義，依 SDK 的 **GPLv3** 授權選項使用，所以這個資料夾的程式碼是 GPL-3.0。
SDK 本身不放進版本庫（`fetch-sdk.sh` 從官方下載）。xMacScope 主程式是另一個獨立的程式，不在這個授權範圍內。
要閉源散布這個代理，得改簽 Steinberg 的專有授權。產品名稱不能含「ASIO」（Steinberg 的商標）。
ASIO is a trademark and software of Steinberg Media Technologies GmbH.
