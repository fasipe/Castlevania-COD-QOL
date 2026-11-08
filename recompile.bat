@echo off
rem Gera o codigo recompilado do jogo (recomp\gen e recomp\vu) a partir da ISO e compila o codport
rem com ele. Passo 1: o codport precisa existir (e estar atualizado: o gerador faz parte dele);
rem passo 2: a lista de funcoes (tools\recomp\functions.txt, ou do Ghidra se faltar); passo 3: os
rem microprogramas do VU1, lidos da ISO pelo indice tools\recomp\vu-blocks.idx, mais os de
rem recomp\vu-blocks.txt se existir (coletados jogando com CODPORT_VU_COLLECT=recomp\vu-blocks.txt).
setlocal
cd /d "%~dp0"
call "%~dp0build.bat" || exit /b 1
if not exist tools\recomp\functions.txt powershell -NoProfile -File tools\ghidra\export-functions.ps1 || exit /b 1
build\codport.exe --recompile tools\recomp\functions.txt recomp\gen %* || exit /b 1
set "VUBLOCKS=tools\recomp\vu-blocks.idx"
if exist recomp\vu-blocks.txt set "VUBLOCKS=%VUBLOCKS%;recomp\vu-blocks.txt"
build\codport.exe --vu-recompile "%VUBLOCKS%" recomp\vu || exit /b 1
call "%~dp0build.bat"
