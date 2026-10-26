@echo off
rem Builds dist\version.dll (needs Visual Studio with C++ tools).
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul || exit /b 1
cd /d "%~dp0"
if not exist dist mkdir dist
if not exist build mkdir build
cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD src\ChunkLoader.cpp /Fobuild\ /Fe:dist\version.dll /link /NOLOGO user32.lib || exit /b 1
del /q dist\version.exp dist\version.lib 2>nul
echo OK: dist\version.dll
