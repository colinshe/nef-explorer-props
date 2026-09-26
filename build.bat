@echo off
setlocal
cd /d "%~dp0"
where g++ >nul 2>&1
if errorlevel 1 (
  echo g++ was not found. Install a 64-bit MinGW-w64 toolchain and put it on PATH.
  exit /b 1
)
g++ -print-file-name=libwinpthread.a > "%TEMP%\nef-pthread-path.txt"
set /p PTHREAD=<"%TEMP%\nef-pthread-path.txt"
del "%TEMP%\nef-pthread-path.txt" >nul 2>&1
if "%PTHREAD%"=="libwinpthread.a" (
  echo libwinpthread.a was not found for this g++. The handler must not depend on libwinpthread-1.dll.
  exit /b 1
)
g++ -shared -O2 -std=c++17 -DUNICODE -D_UNICODE -static-libgcc -static-libstdc++ -o NefPropHandler.dll NefPropHandler.cpp NefPropHandler.def -lole32 -loleaut32 -luuid -lpropsys -lshlwapi -Wl,--whole-archive "%PTHREAD%" -Wl,--no-whole-archive
if errorlevel 1 exit /b 1
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE -static-libgcc -static-libstdc++ -o test_handler.exe test_handler.cpp -lole32 -loleaut32 -luuid -lpropsys -lshlwapi -lshell32 -Wl,--whole-archive "%PTHREAD%" -Wl,--no-whole-archive
if errorlevel 1 exit /b 1
echo built
exit /b 0
