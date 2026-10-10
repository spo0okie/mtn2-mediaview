@echo off
rem Builds mediaview.dll (x64, Release) with the VS 2022 Build Tools toolchain
rem into dist\mtn.mediaview. libmpv-2.dll has to be put there by hand.
setlocal
set VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
"%CMAKE%" -S . -B build-x64 -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release || exit /b 1
"%CMAKE%" --build build-x64 || exit /b 1
echo Done: dist\mtn.mediaview\mediaview.dll
