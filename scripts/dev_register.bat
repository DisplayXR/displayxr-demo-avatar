@echo off
setlocal EnableDelayedExpansion
REM ============================================================================
REM dev_register.bat ? Register the just-built dev binary with any DisplayXR
REM workspace controller (e.g. the DisplayXR Shell).
REM
REM Drops a registered-mode app manifest + icons into:
REM     %LOCALAPPDATA%\DisplayXR\apps\
REM
REM Per `docs/specs/displayxr-app-manifest.md` ?5, %LOCALAPPDATA% wins
REM precedence over %ProgramData%, so this dev manifest overrides whatever
REM the production installer dropped (if any) ? no admin needed, no
REM uninstall of the production demo, and no edit of the system-wide
REM ProgramData manifest. Workspace controllers re-scan on activate, so
REM restart the shell (or toggle the workspace) and the new tile points at
REM the dev build.
REM
REM Usage:
REM     scripts\build_windows.bat
REM     scripts\dev_register.bat
REM
REM To remove the dev registration (so the shell falls back to the
REM ProgramData entry):
REM     scripts\dev_register.bat --unregister
REM ============================================================================

REM Locate this repo's build dir relative to the script location.
set "REPO_ROOT=%~dp0.."
for %%I in ("%REPO_ROOT%") do set "REPO_ROOT=%%~fI"
set "BUILD_DIR=%REPO_ROOT%\build\windows"
set "EXE_PATH=%BUILD_DIR%\model_viewer_handle_vk_win.exe"
set "ICON_PNG=%BUILD_DIR%\icon.png"
set "ICON_SBS=%BUILD_DIR%\icon_sbs.png"

set "TARGET_DIR=%LOCALAPPDATA%\DisplayXR\apps"
set "MANIFEST_PATH=%TARGET_DIR%\model_viewer-dev.displayxr.json"

if /i "%~1" == "--unregister" goto :unregister

REM --- Register ---------------------------------------------------------------

if not exist "%EXE_PATH%" (
    echo [dev_register] ERROR: build artifact not found at:
    echo     %EXE_PATH%
    echo Run scripts\build_windows.bat first.
    exit /b 1
)

if not exist "%TARGET_DIR%" mkdir "%TARGET_DIR%"

REM Copy icons next to the manifest (icon paths in the manifest are
REM resolved relative to the manifest file per spec ?2.3).
if exist "%ICON_PNG%" copy /y "%ICON_PNG%" "%TARGET_DIR%\icon.png" >nul
if exist "%ICON_SBS%" copy /y "%ICON_SBS%" "%TARGET_DIR%\icon_sbs.png" >nul

REM Forward slashes in exe_path (spec accepts either; fwd slashes parse
REM cleanly with strict JSON readers).
set "EXE_FWD=%EXE_PATH:\=/%"

(
    echo {
    echo   "schema_version": 1,
    echo   "name": "3D Model Viewer (dev)",
    echo   "type": "3d",
    echo   "category": "demo",
    echo   "display_mode": "auto",
    echo   "description": "DEV BUILD: Interactive PBR viewer for 3D models (.glb / .gltf). Drag-and-drop a model or press L to load.",
    echo   "icon": "icon.png",
    echo   "icon_3d": "icon_sbs.png",
    echo   "icon_3d_layout": "sbs-lr",
    echo   "exe_path": "%EXE_FWD%"
    echo }
) > "%MANIFEST_PATH%"

echo [dev_register] Registered DEV build for the DisplayXR shell.
echo     exe:      %EXE_PATH%
echo     manifest: %MANIFEST_PATH%
echo Restart the shell ^(or toggle the workspace^) to pick up the new tile.
exit /b 0

REM --- Unregister -------------------------------------------------------------

:unregister
if exist "%MANIFEST_PATH%" del /f /q "%MANIFEST_PATH%"
if exist "%TARGET_DIR%\icon.png" del /f /q "%TARGET_DIR%\icon.png"
if exist "%TARGET_DIR%\icon_sbs.png" del /f /q "%TARGET_DIR%\icon_sbs.png"
echo [dev_register] Removed dev manifest. Shell will fall back to
echo any %%ProgramData%% entry on next workspace activation.
exit /b 0
