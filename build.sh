#!/bin/bash
# 在 Linux 上用 mingw-w64 交叉編譯：代理驅動、測試用假驅動、測試程式。
#   bash build.sh
set -euo pipefail
cd "$(dirname "$0")"
[ -d third_party/ASIOSDK ] || bash fetch-sdk.sh
mkdir -p build
CXX=x86_64-w64-mingw32-g++
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -fno-exceptions -fno-rtti -I third_party/ASIOSDK/common"
LINK="-static -static-libgcc -lole32 -luuid -ladvapi32 -lpsapi"
# 兩個 DLL 不連結 libstdc++（原因見 src/nolibstdcxx.h）；-nostdlib++ 讓任何漏網的引用變成連結錯誤
DLL_LINK="-nostdlib++ $LINK"
$CXX $FLAGS -shared -o build/xmacscope_tap.dll src/proxy.cpp src/exports.def $DLL_LINK
$CXX $FLAGS -shared -o build/xm_dummy.dll test/dummy_driver.cpp test/dummy.def $DLL_LINK
$CXX $FLAGS -o build/host.exe test/host.cpp -static-libstdc++ $LINK
$CXX $FLAGS -o build/realtest.exe test/realtest.cpp -static-libstdc++ $LINK -luser32
# 故意做壞的代理（會吞掉回呼）：測試程式對它必須報 FAIL，否則代表測試沒在測東西
$CXX $FLAGS -DXM_TEST_FAULT -shared -o build/xmacscope_tap_fault.dll src/proxy.cpp src/exports.def $DLL_LINK
ls -la build/ | awk 'NR>1 {print $5, $NF}'
