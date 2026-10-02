xMacScope Tap（DAW 分線盒）
Copyright (C) 2026 金皓哲 (xmac jin)

這是什麼
  一個 ASIO 相容的代理驅動。在 DAW（Pro Tools、Cubase 等）的音訊裝置選單選「xMacScope Tap」，
  它會把聲音原封不動轉給錄音介面原廠的驅動，同時抄一份給 xMacScope 顯示。
  不改變聲音，不增加延遲。

  後面接哪一台錄音介面，在 xMacScope 的「設定 › DAW › DAW 分線盒…」選。

狀態
  測試中。已在 RME 的介面與 Pro Tools 上測過；其他介面與 DAW 還沒測。
  遇到問題時，在 DAW 把音訊裝置選回錄音介面原本的驅動就恢復原狀。

解除安裝
  和 xMacScope 一起解除安裝。先關掉 DAW；解除安裝後，DAW 的音訊裝置要選回錄音介面原本的驅動。

授權
  本元件是自由軟體，依 GNU General Public License 第 3 版（GPL-3.0-only）授權，
  條文見同一個資料夾的 COPYING.txt。本元件不附任何擔保。

  它和 xMacScope 主程式是兩個獨立的程式，只透過共享記憶體交換資料；
  主程式的授權見上一層資料夾的 LICENSE.txt。

  完整原始碼（含建置腳本，以及用到的 Steinberg ASIO SDK 介面定義檔）在同一個資料夾的
  xmacscope-tap-source.zip。原始碼也公開在 GitHub：
  https://github.com/xbroadsword8/xmacscope-tap

  本元件使用 Steinberg ASIO SDK（依其 GPLv3 授權選項）。
  ASIO is a trademark and software of Steinberg Media Technologies GmbH.
