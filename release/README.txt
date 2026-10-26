NFL Head Coach 09 for PC
========================

An unofficial PC version of NFL Head Coach 09 (Xbox 360), built on your own PC from your own copy of the game.
It is a fan project: not made, endorsed or supported by Electronic Arts, the NFL or the NFL Players Association.

You need your own copy of the game, and the setup program makes the PC version from it. The only game file that comes
with it is the game's Title Update #2, so both game versions (1.0 and 1.2) are installed.


WHAT YOU NEED
-------------

- NFL Head Coach 09 for Xbox 360, US version, as a copy of your own disc: the game's files (default.xex and the
  five qkl_*.ast files, about 5.4 GB). Other versions (other regions, the PS3 version) do not work.
- Nothing for version 1.2: its Title Update #2 is included. Version 1.2 fixes the CPU teams' contracts in careers
  and changes the gameplay; both versions are installed and you choose one in the launcher.
- A PC with:
    Windows 10 or 11, 64-bit
    a processor with AVX2: Intel Core 4th generation (2013) or newer, AMD Ryzen or newer
    a graphics card with Vulkan 1.3 and a current driver: NVIDIA GTX 900 series, AMD RX 400 series,
      Intel UHD 600 or newer
    about 1 GB of free disk space (6.5 GB if you install from a disc image: the game files are copied out of it)

Where the files come from: copy them from your own disc and console. With a modified Xbox 360, copy the game's
folder to a USB drive; with a PC disc drive that reads Xbox 360 discs, make an image of the disc. This project does
not provide the game or links to it.


INSTALLING
----------

1. Run HC09Setup.exe.
2. Under "Your game", press Folder... and choose the folder with default.xex and the five qkl_*.ast files.
   (Disc images (.iso) also work, but that is still in beta: if it fails, use the folder.)
3. Title update: leave it empty to use the included one. (You may choose your own copy instead:
   TU_12K2221_0000004000000.00000000000G1 or the default.xexp inside it.)
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
           anti-aliasing, frame rate (60 by default, up to 240), V-sync, fullscreen, and the graphics card when
           the PC has two. Alt+Enter or F11 switches fullscreen while playing.
Controls   Which controller to use, the button icons (Xbox or PlayStation), keyboard on/off, sound and volume, and a
           test panel that shows the controller's buttons and sticks live.
Saves      Shared saves for both versions or one set each, copying saves between versions, Open saves folder,
           and importing or exporting PlayStation 3 (RPCS3) saves.
Patches    Xenia game patches (see XENIA PATCHES below).

On battery, the launcher offers Laptop settings once (60 fps, V-sync, no anti-aliasing, the integrated graphics).


STEAM DECK (AND LINUX)
----------------------

The game runs through Proton, Steam's compatibility layer. Do the setup in Desktop Mode.

1. Copy your game folder (default.xex and the five qkl_*.ast files) to the Deck, for example to
   /home/deck/Games/HC09/game, and unzip this download there too.
2. In Steam: Games > Add a Non-Steam Game to My Library, Browse, show All files, pick HC09Setup.exe. In its
   Properties > Compatibility, force "Proton Experimental". Start it from Steam.
3. In setup: Your game = Z:\home\deck\Games\HC09\game, Install to = Z:\home\deck\Games\HC09\install. Press Install
   (about 5 minutes per game version on the Deck).
4. Add hc09-launcher.exe from the install folder to Steam the same way, with Proton Experimental forced. Play from
   this entry, also in Game Mode.
5. The Steam Deck fix: start the launcher from Steam. It shows a line like
       LD_PRELOAD="$LD_PRELOAD:/home/deck/Games/HC09/install/libhc09_userptr.so" %command%
   Press OK to copy it, close the launcher, and paste it into the launcher entry's Properties > General > Launch
   options. Without it the game shows a black screen with sound (the Deck's graphics driver refuses the way the game
   hands its memory to the GPU under Proton; the small libhc09_userptr.so beside the launcher lifts that). Keep the
   "$LD_PRELOAD: part: Steam's overlay uses the same setting.
6. Start the launcher again, say Yes to the Steam Deck settings (60 fps, fullscreen, no anti-aliasing), pick game
   version 1.2 and press Play. On the OLED model set the refresh rate to 60 Hz in the Deck's performance menu.

Updating from 1.1.1: run the new HC09Setup.exe from Steam and install into the same folder, then do step 5 once.

Black screen with sound          the launch option of step 5 is missing or mistyped
Steam / ... buttons do nothing   keep "$LD_PRELOAD: at the start of the launch option
Login loop after a big SteamOS   sudo mv /etc/sddm.conf.d/zz-holo-autologin.conf ~/
update                           sudo systemctl restart sddm


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

Your careers, rosters and profile are in your Documents folder: Documents\NFL Head Coach 09\Saves\1.0 and \1.2
(with "Both versions use the same saves" on, both versions use the 1.0 folder; when OneDrive backs up your
Documents, they are in OneDrive\Documents). Saves > Open saves folder opens it.
Each save is a folder of its own (for example ROSTER-... or CAREER-...). Copy the NFL Head Coach 09 folder to back
them up or to take them to another PC. Saves work in both versions. Saves the launcher replaces are kept in
Saves\Backups first.

Saves from the Xenia emulator have the same layout: with the game closed, copy the save folders from Xenia's
content\...\45410841\00000001 folder into the saves folder.

PlayStation 3 saves (from the RPCS3 emulator) can be brought over and back: Saves > Import PS3 saves... and
Export to PS3.... A save copied off a real PS3 is encrypted: decrypt it first with Apollo Save Tool
(github.com/bucanero/apollo-ps3; it runs on a PS3 with homebrew or inside RPCS3), then import the BLUS30128-...
folder holding the decrypted HED-DATA and USR-DATA files.


XENIA PATCHES
-------------

The launcher's Patches tab runs Xenia game patches (.patch.toml files, the format Xenia Canary uses):

1. Press Open patches folder, put the .patch.toml files in it, then press Reload.
2. Tick the patches you want. Each file counts as made for game version 1.2; if it was made for 1.0, select the file
   and set "This file is for" to 1.0.
3. Press Play. The first time, and whenever you change which patches are on, the launcher rebuilds that game version
   with the patches (a few minutes, like at install), then starts it. After that it starts at once.

While patches are on, only their game version can be played (the Game tab locks the version until you turn them
off). With them off, the normal game plays again at once.

The patches are applied before the game's code is compiled for the PC, not while it runs. Most patches work as they
do in Xenia, including ones that add new code. A patch that changes code where the PC version hooks in code of its
own may not; the Patches tab says when that is the case. If a build fails, the details are in build-work-patched in
the install folder.


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
whether to keep your saves (they stay in Documents\NFL Head Coach 09) and removes everything else, including the
shortcuts.


KNOWN ISSUES
------------

- The Colts' stadium screen (jumbotron) has a blank panel: the art is missing in the game's own files.
- Installing from a disc image (.iso) is in beta; installing from the game's folder is the tested way.
- Xbox LIVE features do not work.


LEGAL
-----

NFL Head Coach 09 is a trademark of Electronic Arts. NFL and the NFL shield design are registered trademarks of the
National Football League. Team names, logos and player likenesses belong to their owners. This project contains none
of them and none of the game's code or data, except the game's Title Update #2 (Electronic Arts' file, included
unchanged so that version 1.2 installs): the game is built on your PC from your own copy.

This program uses third-party software under their own licenses: see THIRD-PARTY-NOTICES.txt and the licenses folder.
