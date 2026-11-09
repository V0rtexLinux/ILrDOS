@echo off
setlocal
title ILrDOS - Kokoro TTS service

set "GLADOS_DIR=%~1"
if "%GLADOS_DIR%"=="" (
  set /p "GLADOS_DIR=Path to the extracted glados-feira project: "
)

if not exist "%GLADOS_DIR%\pyproject.toml" (
  echo [ERROR] Could not find pyproject.toml in "%GLADOS_DIR%".
  echo Extract glados-feira.zip and pass that folder as the first argument.
  pause
  exit /b 1
)

for %%F in (
  "models\TTS\kokoro-v1.0.fp16.onnx"
  "models\TTS\kokoro-voices-v1.0.bin"
  "models\TTS\phomenizer_en.onnx"
) do (
  if not exist "%GLADOS_DIR%\%%~F" (
    echo [ERROR] Missing TTS asset: %GLADOS_DIR%\%%~F
    echo Copy the uploaded Kokoro model, voice file, and English phonemizer into models\TTS.
    pause
    exit /b 1
  )
)

where uv >nul 2>&1
if errorlevel 1 (
  echo [ERROR] Install uv and Python 3.12+ before starting the TTS service.
  pause
  exit /b 1
)

set "PYTHONPATH=%GLADOS_DIR%\src;%~dp0..\voice;%PYTHONPATH%"
pushd "%GLADOS_DIR%"
echo Starting ILrDOS Kokoro TTS API on port 5050.
echo Restrict the Windows firewall rule to your private Wi-Fi network.
uv run --extra cpu --extra api litestar --app tts_api:app run --host 0.0.0.0 --port 5050
set "EXIT_CODE=%ERRORLEVEL%"
popd
exit /b %EXIT_CODE%
