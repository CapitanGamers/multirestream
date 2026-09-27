@echo off
setlocal
title Firewall 7788 / 1935
net session >nul 2>&1
if errorlevel 1 (
  echo Right-click this file and choose Run as administrator
  pause
  exit /b 1
)
netsh advfirewall firewall delete rule name="MR Control 7788" >nul 2>&1
netsh advfirewall firewall delete rule name="MR RTMP 1935" >nul 2>&1
netsh advfirewall firewall add rule name="MR Control 7788" dir=in action=allow protocol=TCP localport=7788
netsh advfirewall firewall add rule name="MR RTMP 1935" dir=in action=allow protocol=TCP localport=1935
echo.
echo Windows firewall opened for TCP 7788 and 1935
echo Also open those ports on the datacenter panel if it has its own firewall
echo.
pause
