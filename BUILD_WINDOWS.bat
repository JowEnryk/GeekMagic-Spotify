@echo off
setlocal
cd /d "%~dp0"

echo ==============================================
echo  GeekMagic Spotify v0.1.8 - BUILD
echo ==============================================
echo.

python --version >nul 2>&1
if errorlevel 1 (
  echo ERROR: Python no esta disponible en PATH.
  pause
  exit /b 1
)

echo [1/3] Instalando/actualizando PlatformIO...
python -m pip install --upgrade platformio
if errorlevel 1 goto :error

echo.
echo [2/3] Compilando firmware para SmallTV-Ultra...
python -m platformio run -e ultra
if errorlevel 1 goto :error

echo.
echo [3/3] Copiando BIN a dist...
if not exist dist mkdir dist
copy /Y ".pio\build\ultra\firmware.bin" "dist\GeekMagicSpotify-v0.1.8.bin" >nul
if errorlevel 1 goto :error

echo.
echo ==============================================
echo COMPILACION CORRECTA
echo Archivo:
echo   dist\GeekMagicSpotify-v0.1.8.bin
echo ==============================================
echo.
echo NO lo subas directamente al firmware stock Ultra.
echo La primera instalacion usa el loader indicado en README.txt.
pause
exit /b 0

:error
echo.
echo ERROR durante la compilacion. Copia el texto de esta ventana y pasamelo.
pause
exit /b 1
