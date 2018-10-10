
Debian
====================
This directory contains files used to package locktripd/locktrip-qt
for Debian-based Linux systems. If you compile locktripd/locktrip-qt yourself, there are some useful files here.

## locktrip: URI support ##


locktrip-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install qtum-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your qtum-qt binary to `/usr/bin`
and the `../../share/pixmaps/bitcoin128.png` to `/usr/share/pixmaps`

locktrip-qt.protocol (KDE)

