@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if not defined MKW_GAME set "MKW_GAME=H:\Switch2\Mario_Kart_World_Update_1.8.0"
if not exist "build\mkw180\dkb.exe" (
  echo [ERROR] No build\mkw180\dkb.exe. Run BUILD-MKW.bat first.
  exit /b 1
)

set "TOTK_ASTC=gpu"
set "TOTK_ASTC_BC=1"
set "TOTK_MEMORY=auto"
set "TOTK_LANG=en-US"
if not defined TOTK_TRACE set "TOTK_TRACE=perf"

.venv\Scripts\python.exe tools\mkw_cli.py run --repo . --dump "%MKW_GAME%" --mode game --timeout 0 --trace "%TOTK_TRACE%"
exit /b %ERRORLEVEL%
