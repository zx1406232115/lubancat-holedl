@echo off
setlocal

REM ============================================================
REM  deploy.bat - One-click deploy holedl to LubanCat board
REM
REM  Usage (double-click, or run in PowerShell from project root):
REM      scripts\deploy.bat
REM      scripts\deploy.bat 192.168.1.101     (specify board IP)
REM
REM  You will be asked for the board password 1-2 times: temppwd
REM  (Nothing shows while typing the password - that is normal)
REM ============================================================

set "BOARD=%~1"
if "%BOARD%"=="" set "BOARD=192.168.1.101"
set "BUSER=cat"
set "PROJ=%~dp0.."
pushd "%PROJ%"

echo.
echo ============================================================
echo   Deploy holedl to board
echo   Board : %BUSER%@%BOARD%
echo   Project: %CD%
echo ============================================================
echo.

where ssh >nul 2>&1
if errorlevel 1 (echo [ERROR] ssh not found & goto :end)
where scp >nul 2>&1
if errorlevel 1 (echo [ERROR] scp not found & goto :end)

echo [1/4] Pinging %BOARD% ...
ping -n 1 -w 2000 %BOARD% >nul 2>&1
if errorlevel 1 (
  echo   [ERROR] cannot ping %BOARD%
  echo   Check: board powered on, network cable plugged, correct IP
  goto :end
)
echo   OK - board is online

echo.
echo [2/4] Checking passwordless login ...
ssh -o BatchMode=yes -o ConnectTimeout=5 %BUSER%@%BOARD% "echo ok" >nul 2>&1
if errorlevel 1 (
  echo   Not configured. You will be asked for password: temppwd
  echo.
  echo   To enable passwordless login later, run this ONCE in PowerShell:
  echo     type $env:USERPROFILE\.ssh\id_ed25519.pub ^| ssh %BUSER%@%BOARD% "mkdir -p ~/.ssh ^&^& chmod 700 ~/.ssh ^&^& cat ^>^> ~/.ssh/authorized_keys ^&^& chmod 600 ~/.ssh/authorized_keys"
) else (
  echo   Passwordless login already works
)

echo.
echo [3/4] Copying project to ~/linux-homework ...
ssh %BUSER%@%BOARD% "rm -rf ~/linux-homework" 2>nul
scp -r -q "%CD%" %BUSER%@%BOARD%":~/"
if errorlevel 1 (
  echo   [ERROR] copy failed
  goto :end
)
echo   OK

echo.
echo [4/4] Building and self-testing ON THE BOARD ...
echo ============================================================
ssh -t %BUSER%@%BOARD% "cd ~/linux-homework && bash scripts/board-build.sh"
set "RC=%errorlevel%"
echo ============================================================

echo.
if "%RC%"=="0" (
  echo   DONE - build OK, all 27 tests passed
  echo.
  echo   For the interactive demo ^(press Ctrl-C to test resume^):
  echo     ssh %BUSER%@%BOARD% "cd ~/linux-homework && bash tests/demo.sh"
) else (
  echo   Some tests FAILED ^(exit=%RC%^) - please send the output above back
)

:end
echo.
pause
popd
endlocal
