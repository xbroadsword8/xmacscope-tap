#!/bin/bash
# 從 Steinberg 官方下載 ASIO SDK 到 third_party/ASIOSDK（不進版本庫）。
# 這個子專案依 SDK 的 GPLv3 授權選項使用它；見 README.md。
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p third_party && cd third_party
[ -d ASIOSDK ] && { echo "已經有 third_party/ASIOSDK"; exit 0; }
curl -sSL --fail -A "Mozilla/5.0" -o asiosdk.zip "https://www.steinberg.net/asiosdk"
unzip -q asiosdk.zip && rm asiosdk.zip
head -3 ASIOSDK/changes.txt
