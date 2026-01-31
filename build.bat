@echo off
rem Memory Pool Build Script
set CXX=D:\Dev-Cpp\MinGW64\bin\g++.exe
set SRCS=src\main.cpp
set TARGET=MemoryPool.exe
set FLAGS=-std=c++11 -Iinclude -O2 -Wall

echo ==========================================
echo Compiler: %CXX%
echo Source:   %SRCS%
echo Output:   %TARGET%
echo ==========================================

if not exist "%CXX%" (
    echo [Error] Compiler not found at: %CXX%
    pause
    exit /b 1
)

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
