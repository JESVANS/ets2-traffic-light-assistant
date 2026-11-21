@echo off
setlocal
cd /d "%~dp0"

set "MINGW=C:\mingw64\bin"
if not exist "%MINGW%\g++.exe" (
    echo [ERROR] g++.exe not found at %MINGW%
    exit /b 1
)
set "PATH=%MINGW%;%PATH%"

if not exist "build" mkdir "build"
if not exist "dist" mkdir "dist"

set "CFLAGS=-O2 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -Ithird_party\minhook"
set "CXXFLAGS=-std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -DNOMINMAX -finput-charset=UTF-8 -fexec-charset=UTF-8 -Isrc -Ithird_party\minhook -Ithird_party\scs-sdk-plugin\scs_sdk\include"

echo [1/3] Compiling MinHook...
"%MINGW%\gcc.exe" %CFLAGS% -c third_party\minhook\buffer.c -o build\mh_buffer.o || exit /b 1
"%MINGW%\gcc.exe" %CFLAGS% -c third_party\minhook\hook.c -o build\mh_hook.o || exit /b 1
"%MINGW%\gcc.exe" %CFLAGS% -c third_party\minhook\trampoline.c -o build\mh_trampoline.o || exit /b 1
"%MINGW%\gcc.exe" %CFLAGS% -c third_party\minhook\hde\hde64.c -o build\mh_hde64.o || exit /b 1

echo [2/3] Compiling plugin sources...
"%MINGW%\g++.exe" %CXXFLAGS% -c src\config.cpp -o build\config.o || exit /b 1
"%MINGW%\g++.exe" %CXXFLAGS% -c src\shm_reader.cpp -o build\shm_reader.o || exit /b 1
"%MINGW%\g++.exe" %CXXFLAGS% -c src\logic.cpp -o build\logic.o || exit /b 1
"%MINGW%\g++.exe" %CXXFLAGS% -c src\hud_math.cpp -o build\hud_math.o || exit /b 1
"%MINGW%\g++.exe" %CXXFLAGS% -c src\hud_painter.cpp -o build\hud_painter.o || exit /b 1
"%MINGW%\g++.exe" %CXXFLAGS% -c src\d3d11_hook.cpp -o build\d3d11_hook.o || exit /b 1
"%MINGW%\g++.exe" %CXXFLAGS% -c src\plugin_main.cpp -o build\plugin_main.o || exit /b 1

echo [3/3] Linking dist\ets2_tl_hud.dll...
"%MINGW%\g++.exe" -shared -static -static-libgcc -static-libstdc++ ^
    build\mh_buffer.o build\mh_hook.o build\mh_trampoline.o build\mh_hde64.o ^
    build\config.o build\shm_reader.o build\logic.o build\hud_math.o ^
    build\hud_painter.o build\d3d11_hook.o build\plugin_main.o ^
    src\ets2_tl_hud.def ^
    -o dist\ets2_tl_hud.dll ^
    -ld3d11 -lgdiplus -luser32 -lkernel32 -lgdi32 -Wl,--gc-sections -s || exit /b 1

echo [OK] Built: %~dp0dist\ets2_tl_hud.dll
exit /b 0
