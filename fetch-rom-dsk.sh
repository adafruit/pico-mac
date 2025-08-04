#!/bin/sh
set -xe

if ! [ -f rom.bin ]; then
    curl -L 'https://archive.org/download/mac_rom_archive_-_as_of_8-19-2011/mac_rom_archive_-_as_of_8-19-2011.zip/4D1F8172%20-%20MacPlus%20v3.ROM' > rom.bin
fi

if ! [ -f umac0ro.img ]; then
    curl -L 'https://archive.org/download/mac_MacOS_6.0.8/MacOS_6.0.8_System_Startup.img' > 'umac0ro.img'
fi

sha256sum -c roms.sha256sum
