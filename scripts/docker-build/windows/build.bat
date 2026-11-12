@echo off
setlocal
cd /d "%~dp0"
if not exist artifacts mkdir artifacts

set "COMPOSE=docker compose"
podman compose version >nul 2>&1 && set "COMPOSE=podman compose"
echo Using: %COMPOSE%

%COMPOSE% build || goto :fail
%COMPOSE% run --rm build-capi || goto :fail
echo.
echo Done: %~dp0artifacts\pigzppc.dll
exit /b 0
:fail
echo Build failed.
exit /b 1
