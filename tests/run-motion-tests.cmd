@echo off
rem 使用 Windows 本机 C 编译器测试纯状态机，不使用 Xtensa 编译器运行主机测试。
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build-host-tests mkdir build-host-tests
cl /nologo /utf-8 /W4 /WX /std:c11 /I main\include /Fo:build-host-tests\ /Fe:build-host-tests\motion_test_host.exe main\drivers\motion.c tests\motion_test.c
if errorlevel 1 exit /b 1
build-host-tests\motion_test_host.exe
