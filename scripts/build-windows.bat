@echo off
setlocal
cd /d "%~dp0.."
if not exist build mkdir build

where cl.exe >nul 2>nul
if errorlevel 1 (
  echo Abra este arquivo no "Developer Command Prompt for Visual Studio".
  echo O SDK do Windows e o compilador MSVC sao necessarios.
  exit /b 1
)

cl.exe /nologo /std:c++20 /EHsc /O2 /W4 /DUNICODE /D_UNICODE ^
  client\ilrdos_client.cpp /Fe:build\ILrDOS-Terminal.exe ^
  /link Ws2_32.lib Bcrypt.lib
if errorlevel 1 exit /b 1
echo Criado: build\ILrDOS-Terminal.exe
