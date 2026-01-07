@echo off
echo Installing Python requirements for the intercepting proxy...
echo.

pip install -r requirements.txt

if %errorlevel% neq 0 (
    echo.
    echo ERROR: Failed to install requirements.
    echo Make sure Python and pip are installed and in your PATH.
    pause
    exit /b 1
)

echo.
echo Installation completed successfully!
echo You can now run server.bat to start the proxy server.
pause
