@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem MKW build: reuse existing image or read extracted files directly. NEVER pack.
if not defined MKW_GAME set "MKW_GAME=H:\Switch2\Mario_Kart_World_Update_1.8.0"
if not defined MKW_PACKED_ROMFS set "MKW_PACKED_ROMFS=G:\mkw-recomp\output\MarioKartWorld\romfs\romfs.bin"
if not defined MKW_JOBS set "MKW_JOBS=%NUMBER_OF_PROCESSORS%"
if not defined MKW_JOBS set "MKW_JOBS=4"

if not exist ".venv\Scripts\python.exe" py -3 -m venv .venv || exit /b 1
if not exist ".venv\.mkw-deps-ready" (
  .venv\Scripts\python.exe -m pip install -r tools\requirements.txt zstandard || exit /b 1
  type nul > ".venv\.mkw-deps-ready"
)

.venv\Scripts\python.exe tools\mkw_select_romfs.py --repo . --packed "%MKW_PACKED_ROMFS%" --dump "%MKW_GAME%" --auto || exit /b 1
.venv\Scripts\python.exe tools\mkw_cli.py prepare --repo . --dump "%MKW_GAME%" --i-confirm-update-180 || exit /b 1
if not exist "build\xlate\totk_xlate.exe" .venv\Scripts\python.exe tools\mkw_cli.py xlate --repo . || exit /b 1
if not exist "generated\mkw180\layout.json" .venv\Scripts\python.exe tools\mkw_cli.py generate --repo . --dump "%MKW_GAME%" --jobs %MKW_JOBS% || exit /b 1

.venv\Scripts\python.exe tools\mkw_cli.py build --repo . --dump "%MKW_GAME%" --jobs %MKW_JOBS% || exit /b 1

echo [DONE] Native MKW: build\mkw180\dkb.exe
echo [NO-REPACK] RomFS was not packed, copied or hashed.
exit /b 0
