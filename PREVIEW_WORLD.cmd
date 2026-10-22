@echo off
cd /d "%~dp0"
if not exist "Build\BellwetherRuntime.exe" (
  echo Build the engine with Tools\Validate.ps1 first.
  pause
  exit /b 1
)
if not exist "Cache\Bellwether\world.json" (
  echo Export the original game with Tools\ExportGodot.gd first. See README.md.
  pause
  exit /b 1
)
"Build\BellwetherRuntime.exe" --world "Cache\Bellwether"
