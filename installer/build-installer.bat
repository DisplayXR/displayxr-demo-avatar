@echo off
setlocal
set "REPO=%~dp0.."
set "BIN_DIR=%REPO%\build\windows"
set "OUT_DIR=%~dp0"
if "%OUT_DIR:~-1%"=="\" set "OUT_DIR=%OUT_DIR:~0,-1%"
if "%VERSION%"=="" set "VERSION=1.4.2"
REM Derive MAJOR/MINOR/PATCH from VERSION (e.g. 1.4.2 -> 1 / 4 / 2) so callers
REM (CI on a v* tag) only need to set VERSION, not all four vars.
for /f "tokens=1,2,3 delims=." %%a in ("%VERSION%") do (
    set "VERSION_MAJOR=%%a"
    set "VERSION_MINOR=%%b"
    set "VERSION_PATCH=%%c"
)
if "%VERSION_MAJOR%"=="" set "VERSION_MAJOR=1"
if "%VERSION_MINOR%"=="" set "VERSION_MINOR=0"
if "%VERSION_PATCH%"=="" set "VERSION_PATCH=0"

if not exist "%BIN_DIR%\model_viewer_handle_vk_win.exe" (
    echo ERROR: demo binary not found at %BIN_DIR%\model_viewer_handle_vk_win.exe
    echo Run scripts\build-with-deps.bat first.
    exit /b 1
)

"C:\Program Files (x86)\NSIS\makensis.exe" /DVERSION=%VERSION% /DVERSION_MAJOR=%VERSION_MAJOR% /DVERSION_MINOR=%VERSION_MINOR% /DVERSION_PATCH=%VERSION_PATCH% "/DBIN_DIR=%BIN_DIR%" "/DSOURCE_DIR=%REPO%" "/DOUTPUT_DIR=%OUT_DIR%" "%~dp0DisplayXRModelViewerInstaller.nsi" || exit /b 1

echo === DONE ===
echo Installer: %OUT_DIR%DisplayXRModelViewerSetup-%VERSION%.exe
