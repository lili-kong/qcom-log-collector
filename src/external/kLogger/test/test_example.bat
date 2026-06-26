@echo off

REM Check if -b flag is provided
if "%1"=="-b" (
    echo Building example...
    g++ -std=c++17 -I ../include example.cpp -o example.exe -static
    if %ERRORLEVEL% NEQ 0 (
        echo Build failed!
        exit /b 1
    )
    echo Build successful!
    echo.
)

REM Check if example.exe exists
if not exist example.exe (
    echo Error: example.exe not found!
    echo Please build first using: test_example.bat -b
    exit /b 1
)

echo Running example...
example.exe
