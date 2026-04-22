@echo off
echo ========================================
echo   TD5 K-Line Test v4 - COM8
echo   Cable switch: K-LINE (LEFT)
echo   Ignition: ON
echo ========================================
echo.
echo Choose mode:
echo   1 = Live data (poll known PIDs)
echo   2 = Probe all PIDs (find what your ECU supports)
echo.
set /p MODE="Enter 1 or 2: "
echo.
if "%MODE%"=="2" (
    python td5_kline_test.py --port COM8 --raw --probe
) else (
    python td5_kline_test.py --port COM8 --raw
)
echo.
pause
