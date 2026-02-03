@echo off
rem Memory Pool Build Script

rem 自动检测编译器路径
set CXX=
if not "%CXX%"=="" (
    rem 使用用户指定的CXX环境变量
) else if exist "%ProgramFiles%\mingw64\bin\g++.exe" (
    set CXX=%ProgramFiles%\mingw64\bin\g++.exe
) else if exist "C:\msys64\mingw64\bin\g++.exe" (
    set CXX=C:\msys64\mingw64\bin\g++.exe
) else if exist "D:\MSYS2\mingw64\bin\g++.exe" (
    set CXX=D:\MSYS2\mingw64\bin\g++.exe
) else (
    rem 尝试使用PATH环境变量中的g++
    for /f "delims=" %%i in ('where g++ 2^>nul') do (
        set CXX=%%i
        goto :found_compiler
    )
    echo [Error] No C++ compiler found!
    echo Please install MinGW-w64 or set CXX environment variable
    echo Examples:
    echo   set CXX=C:\path\to\g++.exe
    echo   set CXX=D:\MSYS2\mingw64\bin\g++.exe
    pause
    exit /b 1
)

:found_compiler
set SRCS=src\main.cpp
set TARGET=MemoryPool.exe
set FLAGS=-std=c++11 -Iinclude -O2 -Wall

echo ==========================================
echo Compiler: %CXX%
echo Source:   %SRCS%
echo Output:   %TARGET%
echo ==========================================

echo Building...
"%CXX%" %FLAGS% -o %TARGET% %SRCS%

if %errorlevel% neq 0 (
    echo [Error] Build failed!
    pause
    exit /b 1
) else (
    echo [Success] Build completed. Generated %TARGET%
    echo.
    echo Launching %TARGET%...
    %TARGET%
)
