@echo off
rem Abre o instalador: escolhe a ISO do jogo, gera o jogo a partir dela e cria o atalho.
rem Opens the setup: picks the game's ISO, builds the game from it and makes the shortcut.
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tools\setup\setup.ps1"
