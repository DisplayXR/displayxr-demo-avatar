@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: VS 2022 not found
    exit /b 1
)
set "PATH=%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe;%PATH%"
cd /d "%~dp0\.."

REM --- Vulkan SDK ----------------------------------------------------------------
REM Use the VULKAN_SDK env var the LunarG installer sets (any version), instead of
REM a hardcoded C:/VulkanSDK/<ver> path. find_package(Vulkan) picks it up via the
REM VULKAN_SDK environment, so no -DCMAKE_PREFIX_PATH is needed.
if "%VULKAN_SDK%"=="" (
    echo ERROR: VULKAN_SDK is not set. Install the Vulkan SDK from https://vulkan.lunarg.com
    echo        and open a fresh terminal so VULKAN_SDK is exported, then re-run.
    exit /b 1
)

REM --- OpenXR loader -------------------------------------------------------------
REM Auto-provision the prebuilt Khronos loader, pinned to the same spec revision as
REM the vendored openxr_includes/ headers (XR_CURRENT_API_VERSION = 1.1.51). Cached
REM under build\openxr_sdk so a fresh clone builds with no manually-staged SDK.
REM (Mirrors what .github/workflows/build-windows.yml does in CI.)
set "OPENXR_VER=1.1.51"
set "OPENXR_DIR=%CD%\build\openxr_sdk"
if not exist "%OPENXR_DIR%\x64\lib\openxr_loader.lib" (
    echo === Provisioning OpenXR loader %OPENXR_VER% ===
    if not exist build mkdir build
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ErrorActionPreference='Stop';" ^
      "$u='https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-%OPENXR_VER%/openxr_loader_windows-%OPENXR_VER%.zip';" ^
      "Invoke-WebRequest -Uri $u -OutFile 'build\openxr_loader.zip';" ^
      "Expand-Archive -Path 'build\openxr_loader.zip' -DestinationPath 'build\openxr_sdk' -Force;" ^
      "Remove-Item 'build\openxr_loader.zip' -Force" || exit /b 1
)

echo === Configuring ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOpenXR_ROOT="%CD:\=/%/build/openxr_sdk" || exit /b 1
echo === Building ===
cmake --build build || exit /b 1
echo === DONE ===
