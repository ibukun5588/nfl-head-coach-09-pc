NFL Head Coach 09 for PC
========================

An unofficial PC version of NFL Head Coach 09 (Xbox 360), built on your own PC from your own copy of the game.
It is a fan project: not made, endorsed or supported by Electronic Arts, the NFL or the NFL Players Association.

No game files come with it. You need your own copy of the game, and the setup program makes the PC version from it.


WHAT YOU NEED
-------------

- NFL Head Coach 09 for Xbox 360, US version, as a copy of your own disc: the game's files (default.xex and the
  five qkl_*.ast files, about 5.4 GB). Other versions (other regions, the PS3 version) do not work.
- Optional: the game's Title Update #2 (version 1.2) from your own Xbox 360. Version 1.2 changes the gameplay and
  is the better one for careers. Both versions are installed when you give it; you choose one in the launcher.
- A PC with:
    Windows 10 or 11, 64-bit
    a processor with AVX2: Intel Core 4th generation (2013) or newer, AMD Ryzen or newer
    a graphics card with Vulkan 1.3 and a current driver: NVIDIA GTX 900 series, AMD RX 400 series,
      Intel UHD 600 or newer
    about 1 GB of free disk space (6.5 GB if you install from a disc image: the game files are copied out of it)

Where the files come from: copy them from your own disc and console. With a modified Xbox 360, copy the game's
folder to a USB drive; with a PC disc drive that reads Xbox 360 discs, make an image of the disc. The title update
is on your console's storage once the console has downloaded it. This project does not provide game files or links
to them.


INSTALLING
----------

1. Run HC09Setup.exe.
2. Under "Your game", press Folder... and choose the folder with default.xex and the five qkl_*.ast files.
   (Disc images (.iso) also work, but that is still in beta: if it fails, use the folder.)
3. Optional: under "Title update", choose your title update file (TU_12K2221_0000004000000.00000000000G1 or the
   default.xexp inside it).
4. Choose where to install (the default needs no administrator rights), then press Install.

Setup checks your files against the US retail game, then builds the game for your PC. This takes a few minutes
(about 2-5 minutes per version on a recent PC; longer on older ones). When it is done, press Play, or use the
"NFL Head Coach 09 (PC)" shortcut on the desktop or in the Start menu.

If your game files differ from the retail ones (for example modified files), setup says which, and you may go on:
the game may then not work correctly.

Setup writes hc09-setup.log in the install folder: send it if you need help with the installation.


THE LAUNCHER
------------

hc09-launcher.exe starts the game and holds its settings (saved in hc09.ini in the install folder).

Profile    Named sets of display, sound and controller settings ("TV", "Laptop", ...). Type a name and press
           Save profile; pick one from the list to use it.
Game       Your game folder, the game version (1.0, or 1.2 with the title update), the title update file, and
           "Check for a new version when the launcher starts" (off unless you turn it on; it only asks GitHub
           which version is the newest).
Display    Renderer (Native is fast and the default; Emulated is accurate but slow), resolution up to 4K,
           anti-aliasing, frame rate (60 by default, up to 240), V-sync, fullscreen.
           Alt+Enter or F11 switches fullscreen while playing.
Controls   Which controller to use, the button icons (Xbox or PlayStation), keyboard on/off, sound and volume, and a
           test panel that shows the controller's buttons and sticks live.
Saves      Shared saves for both versions or one set each, copying saves between versions, and importing or
           exporting PlayStation 3 (RPCS3) saves.


CONTROLLERS
-----------

- Xbox controllers work as they are.
- PlayStation (DualShock 4, DualSense), Nintendo Switch Pro and other controllers work through SDL; the game can
  show PlayStation button icons (Controls > Button icons).
- A DualShock 3 needs the DsHidMini driver.
- Steam: if Steam Input is on for the game, the controller shows up as an Xbox controller. Turn Steam Input off for
  it to get PlayStation icons.
- Keyboard: arrows = D-pad, W A S D = left stick, numpad 8 4 2 6 = right stick, Space = A, Esc = B, J = X, I = Y,
  Enter = START, Backspace = BACK, Q / E = LB / RB, Z / C = LT / RT.


SAVES
-----

Your careers, rosters and profile are in the install folder: hc09-cache (version 1.0) and hc09-cache-v12
(version 1.2), or hc09-cache for both when they share saves. Copy those folders to back them up. Saves work in both
versions.

PlayStation 3 saves (from the RPCS3 emulator) can be brought over and back: Saves > Import PS3 saves... and
Export to PS3.... A save copied off a real PS3 is still encrypted and cannot be read; use RPCS3's saves.


IF THE GAME CRASHES
-------------------

The launcher shows what happened and saves a crash report (a folder and a .zip in crash-reports in the install
folder): the error, a small snapshot of the program's threads (no game files), the end of the log, your settings and
a description of your PC. Sending it is up to you: "Report on GitHub" opens a prefilled report to attach it to.
The game's log is hc09.log in the install folder (Open log in the launcher).


UPDATING AND UNINSTALLING
-------------------------

To update, run the new HC09Setup.exe and install into the same folder: your saves and settings stay.
To uninstall, use Windows' Settings > Apps (or Control Panel > Programs): "NFL Head Coach 09 for PC". It asks
whether to keep your saves (they stay in the install folder) and removes everything else, including the shortcuts.


KNOWN ISSUES
------------

- The Colts' stadium screen (jumbotron) has a blank panel: the art is missing in the game's own files.
- Installing from a disc image (.iso) is in beta; installing from the game's folder is the tested way.
- Xbox LIVE features do not work.


LEGAL
-----

NFL Head Coach 09 is a trademark of Electronic Arts. NFL and the NFL shield design are registered trademarks of the
National Football League. Team names, logos and player likenesses belong to their owners. This project contains none
of them and none of the game's code or data: the game is built on your PC from your own copy.

This program uses third-party software under their own licenses: see THIRD-PARTY-NOTICES.txt and the licenses folder.
