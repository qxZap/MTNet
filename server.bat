@echo off
echo MTNet 1.1
echo ====================================
echo.

REM Check if Python is installed
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Python is not installed or not in PATH.
    echo.
    echo Please install Python 3.8 or higher from: https://python.org
    echo Make sure to check "Add Python to PATH" during installation.
    echo.
    pause
    exit /b 1
)

echo Python found. Checking for required packages...
echo.

REM Try to install requirements, with fallback to python -m pip
pip install -r requirements.txt >nul 2>&1
if %errorlevel% neq 0 (
    echo Pip not found in PATH, trying python -m pip...
    python -m pip install -r requirements.txt
    if %errorlevel% neq 0 (
        echo.
        echo ERROR: Failed to install required packages.
        echo Make sure you have internet connection and Python is properly installed.
        echo.
        pause
        exit /b 1
    )
)

echo Requirements installed successfully.
echo.

REM Check if ports are available using netstat (this will also detect if our server is running)
netstat -an | find "LISTENING" | find ":81" >nul 2>&1
if %errorlevel% equ 0 (
    echo ERROR: Port 81 is already in use.
    echo This usually means MTNet server is already running.
    echo.
    echo If you want to restart the server:
    echo 1. Close any existing server.bat windows
    echo 2. Wait a few seconds
    echo 3. Run server.bat again
    echo.
    echo If another application is using port 81, you may need to close it first.
    echo.
    pause
    exit /b 1
)

netstat -an | find "LISTENING" | find ":82" >nul 2>&1
if %errorlevel% equ 0 (
    echo ERROR: Port 82 is already in use.
    echo This usually means MTNet server is already running.
    echo.
    echo If you want to restart the server:
    echo 1. Close any existing server.bat windows
    echo 2. Wait a few seconds
    echo 3. Run server.bat again
    echo.
    echo If another application is using port 82, you may need to close it first.
    echo.
    pause
    exit /b 1
)

REM Check if ports are available using netstat
netstat -an | find "LISTENING" | find ":81" >nul 2>&1
if %errorlevel% equ 0 (
    echo ERROR: Port 81 is already in use by another application.
    echo Please close the application using port 81 or choose a different port.
    echo.
    pause
    exit /b 1
)

netstat -an | find "LISTENING" | find ":82" >nul 2>&1
if %errorlevel% equ 0 (
    echo ERROR: Port 82 is already in use by another application.
    echo Please close the application using port 82 or choose a different port.
    echo.
    pause
    exit /b 1
)

echo Starting MTNet server...
echo Host Server: http://localhost:81 -> https://mt.racers.co.kr/
echo Web Server: http://localhost:82 -> https://www.racers.co.kr
echo.

REM Start the server
python server.py

REM If server crashes, show error
if %errorlevel% neq 0 (
    echo.
    echo ERROR: Server failed to start properly.
    echo Check the error messages above for details.
    echo.
)

pause
