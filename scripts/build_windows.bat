@echo off
setlocal
cd /d "%~dp0\.."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || goto :error
cmake --build build || goto :error
echo.
echo Run: build\windows\model_viewer_handle_vk_win.exe
exit /b 0
:error
exit /b 1
