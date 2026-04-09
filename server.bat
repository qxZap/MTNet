@echo off
echo MTNet 1.1
echo ====================================
echo.

REM Detect Python interpreter: prefer `py -3`, then `python`, then common install locations
set "PY_CMD="
py -3 --version >nul 2>&1
if %errorlevel% equ 0 (
    set "PY_CMD=py -3"
) else (
    python --version >nul 2>&1
    if %errorlevel% equ 0 (
        set "PY_CMD=python"
    ) else (
        set "PY_EXE="
        for /d %%D in ("%LOCALAPPDATA%\Programs\Python\Python*") do (
            if exist "%%D\python.exe" set "PY_EXE=%%D\python.exe"
        )
        if not defined PY_EXE (
            for /d %%D in ("C:\Program Files\Python*") do (
                if exist "%%D\python.exe" set "PY_EXE=%%D\python.exe"
            )
        )
        if defined PY_EXE set "PY_CMD=%PY_EXE%"
    )
)

if not defined PY_CMD (
    echo ERROR: Python is not installed or not in PATH.
    echo.
    echo Please install Python 3.8 or higher from: https://python.org
    echo Make sure to check "Add Python to PATH" during installation, or install the "py" launcher.
    echo.
    pause
    exit /b 1
)

echo Using Python: %PY_CMD%
set "ACTION=%~1"
set "REQS=fastapi>=0.100.0 uvicorn[standard]>=0.23.0 httpx>=0.24.0"

echo Verifying required Python imports...
%PY_CMD% -c "import fastapi, uvicorn, httpx" >nul 2>&1
if %errorlevel% neq 0 (
    if /i "%ACTION%"=="install" goto InstallDeps

    echo ERROR: Required Python packages are not installed.
    echo Run: %~nx0 install
    echo.
    echo Exit code 0 indicates a dependency install is required.
    exit /b 0
)

if /i "%ACTION%"=="install" (
    echo Dependencies already installed; skipping install.
) else (
    goto CheckPorts
)

:InstallDeps
echo Checking/Installing required packages...
%PY_CMD% -m pip --version >nul 2>&1
if %errorlevel% neq 0 (
    echo Pip not found for this Python. Attempting to ensure pip is available...
    %PY_CMD% -m ensurepip --upgrade >nul 2>&1
)
%PY_CMD% -m pip install --upgrade pip >nul 2>&1

echo Installing packages: %REQS%
%PY_CMD% -m pip install %REQS%
if %errorlevel% neq 0 (
    echo.
    echo ERROR: Failed to install required packages.
    echo Try running: %PY_CMD% -m pip install %REQS%
    echo.
    pause
    exit /b 1
)

echo Requirements installed successfully.
echo.

goto CheckPorts

:CheckPorts
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
