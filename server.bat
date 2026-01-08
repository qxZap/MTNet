@echo off
echo Starting dual intercepting proxy servers...
echo Host Server: http://localhost:81 -> https://mt.racers.co.kr/
echo Web Server: http://localhost:82 -> https://www.racers.co.kr
echo.

python server.py

pause
