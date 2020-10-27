
Debian
====================
This directory contains files used to package hydrad/hydra-qt
for Debian-based Linux systems. If you compile hydrad/hydra-qt yourself, there are some useful files here.

## hydra: URI support ##


hydra-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install hydra-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your hydra-qt binary to `/usr/bin`
and the `../../share/pixmaps/bitcoin128.png` to `/usr/share/pixmaps`

hydra-qt.protocol (KDE)

