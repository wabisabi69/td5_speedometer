@echo off
title TD5 K-Line Live Data
echo ============================================================
echo   TD5 K-Line Speedometer Data Feed
echo   Cable switch must be on K-LINE (LEFT position)
echo   Ignition must be ON
echo ============================================================
echo.

:: Check for pyserial
python -c "import serial" 2>nul
if errorlevel 1 (
    echo Installing pyserial...
    pip install pyserial
    echo.
)

:: Run the test script with raw hex + parsed metrics, looping forever
:loop
python "%~dp0td5_kline_test.py" --raw %*
echo.
echo Connection lost. Retrying in 3 seconds...
echo Press Ctrl+C to quit.
timeout /t 3 /nobreak >nul
goto loop
