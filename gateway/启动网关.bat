@echo off
chcp 65001 >nul
cd /d %~dp0
title openvela 智能厨房网关（自动重启）

echo ============================================================
echo   openvela 智能厨房网关
echo   目录: %~dp0
echo   日志会显示在本窗口；若异常退出会自动重启
echo ============================================================
echo.

:loop
python gateway.py
echo.
echo [%date% %time%] 网关退出（可能异常），5 秒后自动重启...
timeout /t 5 >nul
goto loop
