@echo off
cd /d "%~dp0"
set XMACSCOPE_TAP_TARGET_DLL=%~dp0xm_dummy.dll
set XMACSCOPE_TAP_TARGET_CLSID={5D0B2B7E-3C1A-4E0C-9A51-7E2D6C8F4B10}
set XM_DUMMY_BLOCK=256
set XM_DUMMY_PERIOD_US=5000
echo started %date% %time% > soak-realtime.log
host.exe xmacscope_tap.dll {A010EB30-1F0E-4ECE-A2C9-058C3FB84A1F} --seconds 1800 >> soak-realtime.log 2>&1
echo finished %date% %time% >> soak-realtime.log
set XM_DUMMY_BLOCK=32
set XM_DUMMY_PERIOD_US=0
echo started %date% %time% > soak-stress.log
host.exe xmacscope_tap.dll {A010EB30-1F0E-4ECE-A2C9-058C3FB84A1F} --seconds 300 --burn 8 >> soak-stress.log 2>&1
echo finished %date% %time% >> soak-stress.log
