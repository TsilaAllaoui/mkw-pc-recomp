@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul 2>&1
title Mario Kart World Recomp - first-time setup and build

rem First-time Windows bootstrap for an already extracted Mario Kart World 1.8.0 dump.
rem No winget, system Python, Vulkan SDK, or pre-existing developer shell is required.
rem Game files are read in place and are never copied, packed, modified, or deleted.

cd /d "%~dp0" || goto :bad_repo
set "MKW_REPO=%CD%"
set "MKW_BOOTSTRAP=%MKW_REPO%\toolchain\bootstrap"
set "MKW_MSYS=C:\msys64"
set "MKW_DOTNET=%MKW_REPO%\toolchain\dotnet"
set "MKW_UV=%MKW_REPO%\toolchain\uv\uv.exe"
set "MKW_PYTHON=%MKW_REPO%\.venv\Scripts\python.exe"
set "DOTNET_ROOT=%MKW_DOTNET%"
set "DOTNET_CLI_TELEMETRY_OPTOUT=1"
set "DOTNET_NOLOGO=1"
set "DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1"
set "NUGET_PACKAGES=%MKW_REPO%\toolchain\nuget-packages"
set "UV_CACHE_DIR=%MKW_REPO%\toolchain\uv-cache"
set "UV_PYTHON_INSTALL_DIR=%MKW_REPO%\toolchain\uv-python"
set "UV_NO_MODIFY_PATH=1"

echo.
echo ================================================================
echo   Mario Kart World Recomp - first-time setup and build
echo ================================================================
echo.
echo This downloads about 5-10 GB of build tools and needs roughly
echo 20 GB free on the repository drive. The clean build can take a while.
echo Two Windows administrator prompts may appear for MSYS2 and Microsoft
echo C++ Build Tools. No game data is uploaded or placed in this repository.
echo.

if /I not "%PROCESSOR_ARCHITECTURE%"=="AMD64" if /I not "%PROCESSOR_ARCHITEW6432%"=="AMD64" (
  echo [ERROR] This build requires 64-bit Windows on an x86-64 CPU.
  goto :fail
)

if not exist "%MKW_REPO%\tools\mkw_cli.py" (
  echo [ERROR] Run this file from the root of the mkw-recomp source tree.
  goto :fail
)

if "%MKW_REPO:~0,2%"=="\\" (
  echo [ERROR] A network/UNC repository path is not supported.
  echo Move the source tree to a local drive, for example C:\mkw-recomp.
  goto :fail
)

if not "%~1"=="" (
  set "MKW_GAME=%~1"
) else if not defined MKW_GAME (
  echo Dragging the dump folder onto this BAT also supplies this path.
  set /p "MKW_GAME=Full path to the folder containing exefs and romfs: "
)

if not defined MKW_GAME (
  echo [ERROR] No game dump folder was supplied.
  goto :fail
)
if not exist "%MKW_GAME%\" (
  echo [ERROR] The game dump folder does not exist:
  echo   "%MKW_GAME%"
  goto :fail
)

echo.
echo This target requires your own decrypted cumulative update 1.8.0 dump.
echo The folder may directly contain exefs and romfs, or contain
echo updates\917504\exefs and updates\917504\romfs.
choice /C YN /N /M "Is this your Mario Kart World 1.8.0 dump? [Y/N] "
if errorlevel 2 (
  echo [STOPPED] Supply the correct 1.8.0 dump and run this file again.
  goto :cancelled
)

if not defined MKW_JOBS set "MKW_JOBS=4"
echo(%MKW_JOBS%| %SystemRoot%\System32\findstr.exe /R "^[1-9][0-9]*$" >nul
if errorlevel 1 (
  echo [ERROR] MKW_JOBS must be a positive whole number. Four is a safe default.
  goto :fail
)

where powershell.exe >nul 2>&1
if errorlevel 1 (
  echo [ERROR] Windows PowerShell is missing. It is included with Windows 10 and 11.
  goto :fail
)

set "MKW_SPACE_PATH=%MKW_REPO%"
set "MKW_SPACE_GB=20"
call :check_space
if errorlevel 1 goto :fail
set "MKW_SPACE_PATH=C:\"
set "MKW_SPACE_GB=15"
call :check_space
if errorlevel 1 goto :fail
call :check_ram
if errorlevel 1 goto :fail

if not exist "%MKW_BOOTSTRAP%" mkdir "%MKW_BOOTSTRAP%"
if errorlevel 1 (
  echo [ERROR] Cannot create "%MKW_BOOTSTRAP%".
  goto :fail
)

call :install_msys2
if errorlevel 1 goto :fail

call :install_build_tools
if errorlevel 1 goto :fail

call :install_dotnet
if errorlevel 1 goto :fail

call :install_uv_and_python
if errorlevel 1 goto :fail

"%MKW_PYTHON%" -c "import ctypes; raise SystemExit(0 if ctypes.windll.kernel32.IsProcessorFeaturePresent(40) else 1)"
if errorlevel 1 (
  echo [ERROR] This CPU or virtual machine does not expose the required AVX2 instructions.
  goto :fail
)

set "PATH=%MKW_MSYS%\ucrt64\bin;%MKW_MSYS%\usr\bin;%MKW_DOTNET%;%PATH%"

echo.
echo [INPUT 1/1] Checking the dump before generating anything...
if not exist "%MKW_REPO%\build\mkw-local" mkdir "%MKW_REPO%\build\mkw-local"
"%MKW_PYTHON%" tools\mkw_probe.py probe --dump "%MKW_GAME%" --report build\mkw-local\probe.json
if errorlevel 1 (
  echo [ERROR] The folder is not a supported extracted Mario Kart World dump.
  echo Check that exefs contains main, subsdk0, sdk, and main.npdm.
  goto :fail
)

echo.
echo [BUILD 1/5] Selecting the existing loose RomFS without repacking it...
"%MKW_PYTHON%" tools\mkw_select_romfs.py --repo . --packed "%MKW_REPO%\build\mkw-local\not-used\romfs.bin" --dump "%MKW_GAME%" --auto
if errorlevel 1 goto :fail

echo.
echo [BUILD 2/5] Locking the supplied 1.8.0 ExeFS hashes locally...
"%MKW_PYTHON%" tools\mkw_cli.py prepare --repo . --dump "%MKW_GAME%" --i-confirm-update-180
if errorlevel 1 goto :fail

echo.
echo [BUILD 3/5] Building the shader translator...
if not exist "build\xlate\totk_xlate.exe" (
  "%MKW_PYTHON%" tools\mkw_cli.py xlate --repo .
  if errorlevel 1 goto :fail
) else if not exist "build\xlate\totk_xlate.id" (
  "%MKW_PYTHON%" tools\mkw_cli.py xlate --repo .
  if errorlevel 1 goto :fail
) else (
  echo Reusing build\xlate\totk_xlate.exe
)

echo.
echo [BUILD 4/5] Lifting the ARM64 game modules to generated C...
if not exist "generated\mkw180\layout.json" (
  "%MKW_PYTHON%" tools\mkw_cli.py generate --repo . --dump "%MKW_GAME%" --jobs %MKW_JOBS%
  if errorlevel 1 goto :fail
) else (
  echo Reusing generated\mkw180. Delete that ignored folder to force regeneration.
)

echo.
echo [BUILD 5/5] Compiling and linking the native Windows executable...
"%MKW_PYTHON%" tools\mkw_cli.py build --repo . --dump "%MKW_GAME%" --jobs %MKW_JOBS%
if errorlevel 1 goto :fail

if not exist "build\mkw180\dkb.exe" (
  echo [ERROR] The build command returned without producing build\mkw180\dkb.exe.
  goto :fail
)

echo.
echo [VERIFY] Running the executable's game-data-free self-test...
"build\mkw180\dkb.exe" --selftest
if errorlevel 1 (
  echo [ERROR] dkb.exe was built, but its self-test failed.
  goto :fail
)

echo.
echo ================================================================
echo   BUILD COMPLETE
echo ================================================================
echo.
echo Double-click this file to play:
echo   "%MKW_REPO%\build\mkw180\dkb.exe"
echo.
echo The executable remembers this dump location:
echo   "%MKW_GAME%"
echo Keep the dump in that location. No RomFS image was created or copied.
echo If you move the dump, rerun this BAT with the new folder.
if not exist "%SystemRoot%\System32\vulkan-1.dll" (
  echo.
  echo [WARNING] The Vulkan runtime was not found in Windows System32.
  echo Install the newest driver from your AMD, Intel, or NVIDIA GPU vendor before playing.
)
if defined MKW_REBOOT_RECOMMENDED (
  echo.
  echo Windows reported that a reboot is recommended after Build Tools setup.
  echo Reboot before playing if dkb.exe does not start normally.
)
echo.
pause
exit /b 0

:install_msys2
echo.
echo [TOOLS 1/4] MSYS2 UCRT64 Clang, Ninja, and native libraries...
set "MKW_DOWNLOAD_URL=https://repo.msys2.org/distrib/x86_64/msys2-base-x86_64-latest.sfx.exe"
set "MKW_DOWNLOAD_OUT=%MKW_BOOTSTRAP%\msys2-base-x86_64-latest.sfx.exe"
set "MKW_SIGNED_FILE=%MKW_BOOTSTRAP%\msys2-base-x86_64-latest.sfx.exe"
if not exist "%MKW_MSYS%\usr\bin\bash.exe" (
  call :download
  if errorlevel 1 exit /b 1
  call :verify_signature
  if errorlevel 1 exit /b 1
  echo Installing MSYS2 to C:\msys64. Approve the Windows administrator prompt.
  powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try{$p=Start-Process -FilePath $env:MKW_DOWNLOAD_OUT -ArgumentList '-y','-oC:\' -Verb RunAs -Wait -PassThru; exit $p.ExitCode}catch{Write-Error $_; exit 1}"
  if errorlevel 1 (
    echo [ERROR] MSYS2 extraction failed or the administrator prompt was cancelled.
    exit /b 1
  )
)
if not exist "%MKW_MSYS%\usr\bin\bash.exe" (
  echo [ERROR] MSYS2 did not create %MKW_MSYS%\usr\bin\bash.exe.
  exit /b 1
)

echo Updating the MSYS2 base system. The two passes are intentional...
"%MKW_MSYS%\usr\bin\bash.exe" -lc "pacman -Syuu --noconfirm"
if errorlevel 1 (
  echo [ERROR] The first MSYS2 update pass failed. Close every MSYS2 window and rerun this BAT.
  exit /b 1
)
"%MKW_MSYS%\usr\bin\bash.exe" -lc "pacman -Syuu --noconfirm"
if errorlevel 1 (
  echo [ERROR] The second MSYS2 update pass failed. Rerun this BAT; completed packages are reused.
  exit /b 1
)

set "MKW_MSYS_PACKAGES=mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-opus mingw-w64-ucrt-x86_64-libvpx mingw-w64-ucrt-x86_64-zstd mingw-w64-ucrt-x86_64-vulkan-devel mingw-w64-ucrt-x86_64-vulkan-memory-allocator mingw-w64-ucrt-x86_64-vulkan-validation-layers mingw-w64-ucrt-x86_64-shaderc mingw-w64-ucrt-x86_64-spirv-tools base-devel git unzip"
"%MKW_MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm %MKW_MSYS_PACKAGES%"
if errorlevel 1 (
  echo [ERROR] An MSYS2 package failed to install. Check the network and rerun this BAT.
  exit /b 1
)
if not exist "%MKW_MSYS%\ucrt64\bin\clang.exe" (
  echo [ERROR] MSYS2 finished without UCRT64 clang.exe.
  exit /b 1
)
if not exist "%MKW_MSYS%\ucrt64\bin\ninja.exe" (
  echo [ERROR] MSYS2 finished without UCRT64 ninja.exe.
  exit /b 1
)
exit /b 0

:install_build_tools
echo.
echo [TOOLS 2/4] Microsoft C++ Build Tools and Windows SDK for .NET NativeAOT...
set "MKW_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
call :find_vc_tools
if not errorlevel 1 exit /b 0

set "MKW_DOWNLOAD_URL=https://aka.ms/vs/17/release/vs_BuildTools.exe"
set "MKW_DOWNLOAD_OUT=%MKW_BOOTSTRAP%\vs_BuildTools.exe"
call :download
if errorlevel 1 exit /b 1
set "MKW_SIGNED_FILE=%MKW_DOWNLOAD_OUT%"
call :verify_signature
if errorlevel 1 exit /b 1
echo Installing the C++ workload. Approve the Windows administrator prompt.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try{$a='--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'; $p=Start-Process -FilePath $env:MKW_DOWNLOAD_OUT -ArgumentList $a -Verb RunAs -Wait -PassThru; if($p.ExitCode -eq 3010){exit 194}; exit $p.ExitCode}catch{Write-Error $_; exit 1}"
set "MKW_VS_EXIT=%ERRORLEVEL%"
if "%MKW_VS_EXIT%"=="194" set "MKW_REBOOT_RECOMMENDED=1"
if not "%MKW_VS_EXIT%"=="0" if not "%MKW_VS_EXIT%"=="194" (
  echo [ERROR] Microsoft Build Tools setup failed with exit code %MKW_VS_EXIT%.
  echo Rerun this BAT after resolving any error shown by the installer.
  exit /b 1
)
call :find_vc_tools
if errorlevel 1 (
  echo [ERROR] The Microsoft x64 C++ tools were not found after installation.
  if defined MKW_REBOOT_RECOMMENDED echo Reboot Windows, then run this BAT again.
  exit /b 1
)
exit /b 0

:find_vc_tools
if not exist "%MKW_VSWHERE%" exit /b 1
"%MKW_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>nul | %SystemRoot%\System32\findstr.exe /R "." >nul
if errorlevel 1 exit /b 1
if not exist "%ProgramFiles(x86)%\Windows Kits\10\Lib\*\um\x64\kernel32.lib" exit /b 1
exit /b 0

:install_dotnet
echo.
echo [TOOLS 3/4] Repository-local .NET 10 SDK...
if exist "%MKW_DOTNET%\dotnet.exe" (
  "%MKW_DOTNET%\dotnet.exe" --list-sdks 2>nul | %SystemRoot%\System32\findstr.exe /B "10." >nul
  if not errorlevel 1 exit /b 0
)
set "MKW_DOWNLOAD_URL=https://dot.net/v1/dotnet-install.ps1"
set "MKW_DOWNLOAD_OUT=%MKW_BOOTSTRAP%\dotnet-install.ps1"
call :download
if errorlevel 1 exit /b 1
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%MKW_DOWNLOAD_OUT%" -Channel 10.0 -Quality GA -InstallDir "%MKW_DOTNET%" -NoPath
if errorlevel 1 (
  echo [ERROR] The .NET 10 SDK download or installation failed.
  exit /b 1
)
if not exist "%MKW_DOTNET%\dotnet.exe" (
  echo [ERROR] .NET setup did not create "%MKW_DOTNET%\dotnet.exe".
  exit /b 1
)
"%MKW_DOTNET%\dotnet.exe" --list-sdks | %SystemRoot%\System32\findstr.exe /B "10." >nul
if errorlevel 1 (
  echo [ERROR] The local dotnet installation has no .NET 10 SDK.
  exit /b 1
)
exit /b 0

:install_uv_and_python
echo.
echo [TOOLS 4/4] Repository-local Python 3.12 and virtual environment...
set "MKW_DOWNLOAD_URL=https://astral.sh/uv/install.ps1"
set "MKW_DOWNLOAD_OUT=%MKW_BOOTSTRAP%\uv-install.ps1"
set "UV_INSTALL_DIR=%MKW_REPO%\toolchain\uv"
if not exist "%MKW_UV%" (
  call :download
  if errorlevel 1 exit /b 1
  powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%MKW_DOWNLOAD_OUT%"
  if errorlevel 1 (
    echo [ERROR] uv installation failed.
    exit /b 1
  )
)
if not exist "%MKW_UV%" (
  echo [ERROR] uv setup did not create "%MKW_UV%".
  exit /b 1
)

"%MKW_UV%" python install 3.12
if errorlevel 1 (
  echo [ERROR] uv could not download Python 3.12.
  exit /b 1
)
if exist "%MKW_PYTHON%" (
  "%MKW_PYTHON%" -c "import sys; raise SystemExit(0 if sys.version_info[:2] == (3, 12) else 1)"
  if errorlevel 1 (
    echo [ERROR] Existing .venv is not Python 3.12.
    echo Rename .venv so it is preserved, then rerun this BAT.
    exit /b 1
  )
) else (
  "%MKW_UV%" venv --python 3.12 "%MKW_REPO%\.venv"
  if errorlevel 1 (
    echo [ERROR] Could not create the Python virtual environment.
    exit /b 1
  )
)
"%MKW_UV%" pip install --python "%MKW_PYTHON%" -r "%MKW_REPO%\tools\requirements.txt"
if errorlevel 1 (
  echo [ERROR] A pinned Python dependency failed to install.
  exit /b 1
)
exit /b 0

:download
echo Downloading %MKW_DOWNLOAD_URL%
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12; for($i=1;$i -le 3;$i++){try{Invoke-WebRequest -UseBasicParsing -Uri $env:MKW_DOWNLOAD_URL -OutFile $env:MKW_DOWNLOAD_OUT; exit 0}catch{if($i -eq 3){Write-Error $_; exit 1}; Start-Sleep -Seconds 2}}"
if errorlevel 1 (
  echo [ERROR] Download failed after three attempts:
  echo   %MKW_DOWNLOAD_URL%
  exit /b 1
)
exit /b 0

:verify_signature
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$s=Get-AuthenticodeSignature -LiteralPath $env:MKW_SIGNED_FILE; if($s.Status -ne 'Valid'){Write-Error ('Invalid or missing Authenticode signature: '+$s.Status); exit 1}; Write-Host ('Verified publisher: '+$s.SignerCertificate.Subject)"
if errorlevel 1 (
  echo [ERROR] Refusing to run an installer whose Windows signature is not valid:
  echo   %MKW_SIGNED_FILE%
  exit /b 1
)
exit /b 0

:check_space
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$root=[IO.Path]::GetPathRoot($env:MKW_SPACE_PATH); $d=New-Object IO.DriveInfo($root); $free=[math]::Floor($d.AvailableFreeSpace/1GB); Write-Host ('Free space on '+$root+': '+$free+' GB'); if($free -lt [int]$env:MKW_SPACE_GB){Write-Error ('At least '+$env:MKW_SPACE_GB+' GB free is required.'); exit 1}"
exit /b %ERRORLEVEL%

:check_ram
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "$b=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory; if(-not $b){Write-Warning 'Could not determine installed memory'; exit 0}; $gb=[math]::Round($b/1GB,1); Write-Host ('Installed memory: '+$gb+' GB'); if($b -lt 15GB){Write-Error 'At least 16 GB RAM plus a Windows-managed page file is required to run this recompilation.'; exit 1}"
exit /b %ERRORLEVEL%

:bad_repo
echo [ERROR] The source tree must be on a local drive, not a UNC network path.
goto :fail

:cancelled
echo.
pause
exit /b 2

:fail
set "MKW_FAILURE=%ERRORLEVEL%"
if "%MKW_FAILURE%"=="0" set "MKW_FAILURE=1"
echo.
echo ================================================================
echo   SETUP OR BUILD STOPPED
echo ================================================================
echo Fix the error above and run this BAT again. Downloads, installed tools,
echo generated C, and compiled objects are reused; it will not start over.
echo.
pause
exit /b %MKW_FAILURE%
