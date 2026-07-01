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

if not exist "%BIN_DIR%\avatar_handle_vk_win.exe" (
    echo ERROR: demo binary not found at %BIN_DIR%\avatar_handle_vk_win.exe
    echo Run scripts\build-with-deps.bat first.
    exit /b 1
)

REM ── Code signing (gated on %SIGN_CMD%) ──────────────────────────────
REM Signing is OFF unless SIGN_CMD is set in the environment (a signing-
REM capable build/release machine points it at the configured signer).
REM This repo carries NO cert and NO secret. A clone without SIGN_CMD
REM builds unsigned, exactly as before.
REM
REM The shipped binaries (demo exe + bundled openxr_loader.dll) MUST be
REM signed BEFORE makensis packs them — Smart App Control checks the
REM binaries extracted at install/load time, so signing the finished
REM installer alone is not enough. The installer .exe and the uninstaller
REM are signed at makensis time by the .nsi (!finalize + the two-pass
REM uninstaller block), driven by the /DSIGN_CMD passed below.
REM SIGN_ARG carries its own quotes so the empty (unsigned) case expands to
REM nothing rather than an empty quoted "" arg that makensis would choke on.
set "SIGN_ARG="
if defined SIGN_CMD (
    echo === Signing demo binaries [SIGN_CMD set] ===
    powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO%\scripts\sign-release.ps1" -Path "%BIN_DIR%" -SignCmd "%SIGN_CMD%" || exit /b 1
    set SIGN_ARG="/DSIGN_CMD=%SIGN_CMD%"
) else (
    echo === Signing skipped [SIGN_CMD not set] - installer will be UNSIGNED ===
)

"C:\Program Files (x86)\NSIS\makensis.exe" /DVERSION=%VERSION% /DVERSION_MAJOR=%VERSION_MAJOR% /DVERSION_MINOR=%VERSION_MINOR% /DVERSION_PATCH=%VERSION_PATCH% "/DBIN_DIR=%BIN_DIR%" "/DSOURCE_DIR=%REPO%" "/DOUTPUT_DIR=%OUT_DIR%" %SIGN_ARG% "%~dp0DisplayXRAvatarInstaller.nsi" || exit /b 1

echo === DONE ===
echo Installer: %OUT_DIR%DisplayXRAvatarSetup-%VERSION%.exe
