@echo off
rem Run a command inside the MSVC x64 developer environment (with VS-bundled CMake + Ninja).
set VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
%*
