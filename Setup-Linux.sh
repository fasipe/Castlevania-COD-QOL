#!/bin/sh
# Setup for Linux and the Steam Deck (see README): builds the game from your disc, as Setup.exe does on
# Windows. In a terminal (on the Deck, Konsole in desktop mode): sh Setup-Linux.sh
# Instalador para Linux e Steam Deck (veja o README): gera o jogo a partir do seu disco, como o Setup.exe
# no Windows. Num terminal (no Deck, o Konsole no modo desktop): sh Setup-Linux.sh
cd "$(dirname "$0")" || exit 1
python3 tools/setup/linux-setup.py "$@"
status=$?
printf '\nEnter...'
read -r _
exit $status
