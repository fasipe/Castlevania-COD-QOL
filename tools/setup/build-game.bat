@echo off
rem Builds the game's code from the disc image: "first" translates it and builds codport_game.dll
rem (what the game runs from its start, enough to play); "rest" builds codport_game_vu.dll (the VU
rem programs), codport_game_hot.dll (the rest of what the game runs) and codport_game_cold.dll
rem (everything else), each followed by a .ready file that tells a running game to load it.
rem With a release's codport.exe and codport.lib in build\ (and no build of the port there), only
rem the game's code is built, in build-game\; otherwise the port is built first.
setlocal
cd /d "%~dp0..\.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio com C++ nao encontrado / Visual Studio with C++ not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

set "OUT=build"
set "ONLY="
if not exist build\build.ninja if exist build\codport.exe if exist build\codport.lib (
  set "OUT=build-game"
  set "ONLY=-DCODPORT_GAME_ONLY=ON"
)

if /i "%~1"=="rest" goto rest

if not defined ONLY (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
  cmake --build build --target codport || exit /b 1
)
rem The parts built before are of another build of the code: not for a game started now.
for %%f in (codport_game_vu codport_game_hot codport_game_cold) do (
  if exist build\%%f.ready del build\%%f.ready
  if exist build\%%f.dll del build\%%f.dll
)
echo %time% translating the game / traduzindo o jogo
build\codport.exe --recompile tools\recomp\functions.txt recomp\gen || exit /b 1
set "VUBLOCKS=tools\recomp\vu-blocks.idx"
if exist recomp\vu-blocks.txt set "VUBLOCKS=%VUBLOCKS%;recomp\vu-blocks.txt"
build\codport.exe --vu-recompile "%VUBLOCKS%" recomp\vu || exit /b 1
rem The shortcut's icon: the game's memory card icon, drawn from the disc (no harm if it fails).
build\codport.exe --make-icon build\castlevania.ico
cmake -S . -B %OUT% -G Ninja -DCMAKE_BUILD_TYPE=Release %ONLY% || exit /b 1
echo %time% compiling / compilando
cmake --build %OUT% --target codport_game || exit /b 1
echo %time% done / pronto
exit /b 0

:rest
echo %time% compiling / compilando
cmake --build %OUT% --target codport_game_vu || exit /b 1
echo.> build\codport_game_vu.ready
cmake --build %OUT% --target codport_game_hot || exit /b 1
echo.> build\codport_game_hot.ready
echo %time% half / metade
cmake --build %OUT% --target codport_game_cold || exit /b 1
echo.> build\codport_game_cold.ready
echo %time% done / pronto
exit /b 0
