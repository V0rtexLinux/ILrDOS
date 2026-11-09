@echo off
setlocal
title ILrDOS - GLaDOS TTS model service

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
  "models\TTS\glados.onnx"
  "models\TTS\glados.json"
  "models\TTS\phomenizer_en.onnx"
  "models\TTS\phoneme_to_id.pkl"
  "models\TTS\lang_phoneme_dict.pkl"
  "models\TTS\token_to_idx.pkl"
  "models\TTS\idx_to_token.pkl"
) do (
  if not exist "%GLADOS_DIR%\%%~F" (
    echo [ERROR] Missing TTS asset: %GLADOS_DIR%\%%~F
    echo Copy the uploaded ONNX files into models\TTS and restore the small support files from glados-feira.zip.
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

pushd "%GLADOS_DIR%"
echo Starting the GLaDOS ONNX TTS API on port 5050.
echo Restrict the Windows firewall rule to your private Wi-Fi network.
uv run --extra cpu --extra api litestar --app glados.api.app:app run --host 0.0.0.0 --port 5050
set "EXIT_CODE=%ERRORLEVEL%"
popd
exit /b %EXIT_CODE%
