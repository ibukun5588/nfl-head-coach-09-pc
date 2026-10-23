# NFL Head Coach 09 for PC

An unofficial PC version of **NFL Head Coach 09** (Xbox 360), built on your own PC from your own copy of the game:
up to 4K and 240 fps, Xbox and PlayStation controllers, both game versions (1.0 and the 1.2 title update), and your
PlayStation 3 careers brought over.

It is a fan project, not made, endorsed or supported by Electronic Arts, the NFL or the NFL Players Association.
**No game files come with it**: you need your own copy of the Xbox 360 game, and the setup program builds the PC
version from it.

---

## What you need

| | |
|---|---|
| **The game** | NFL Head Coach 09 for **Xbox 360, US version**, copied from your own disc: `default.xex` and the five `qkl_*.ast` files (about 5.4 GB) in one folder |
| **Title update** (optional) | Title Update #2 (game version **1.2**) from your own Xbox 360: the `TU_12K2221_0000004000000.00000000000G1` file, or the `default.xexp` inside it |
| **Windows** | Windows 10 or 11, 64-bit |
| **Processor** | with AVX2: Intel Core 4th generation (2013) or newer, AMD Ryzen or newer |
| **Graphics** | Vulkan 1.3 with a current driver: NVIDIA GTX 900 series, AMD RX 400 series, Intel UHD 600 or newer |
| **Disk** | about 1 GB free (about 6.5 GB when installing from a disc image) |

The PS3 version of the game does **not** work as the source: the PC version is built from the Xbox 360 game. PS3
players can still bring their careers over (see [PlayStation 3 players](#playstation-3-players)).

This project does not provide game files or links to them. Copy them from your own disc and console: with a modified
Xbox 360, copy the game's folder to a USB drive; with a PC disc drive that reads Xbox 360 discs, make an image of the
disc. The title update is on your console's storage once the console has downloaded it.

## Installing

1. Download `HC09-PC-Setup-1.0.0.zip` from the [Releases](../../releases) page and unzip it anywhere.
2. Run **HC09Setup.exe**. Windows may say it protected your PC (the program is not signed): press
   *More info* > *Run anyway*.
3. **Your game**: press **Folder...** and choose the folder with `default.xex` and the five `qkl_*.ast` files.
   (A disc image `.iso` also works, but that is in beta: if it fails, use the folder.)
4. **Title update** (optional): choose your title update file to get game version 1.2 as well.
5. **Install to**: the default needs no administrator rights. Press **Install**.

Setup checks your files against the US retail game and builds the game for your PC: a few minutes per game version
(about 2 on a recent PC, longer on older ones). Then press **Play**, or use the *NFL Head Coach 09 (PC)* shortcut on
the desktop or in the Start menu.

If some of your game files differ from the retail ones (modified files), setup names them and asks whether to go on.

## Playing

**hc09-launcher.exe** starts the game. Its tabs:

- **Game**: your game folder, the **game version** (1.0 the original, or 1.2 with the title update; 1.2 plays
  differently and is the better one for careers), the title update, and an optional check for new versions.
- **Display**: renderer (*Native* is fast and the default; *Emulated* is accurate but slow), resolution up to 4K,
  anti-aliasing, frame rate (**60 by default**, up to 240), V-sync, fullscreen. *Alt+Enter* or *F11* switches
  fullscreen while playing.
- **Controls**: which controller to use, Xbox or PlayStation button icons, keyboard on/off, sound, and a test panel
  that shows your controller's buttons and sticks live.
- **Saves**: shared saves for both game versions, copying saves between them, and PS3 (RPCS3) save import/export.
- **Profile** (top): save named sets of display, sound and controller settings ("TV", "Laptop") and switch between them.

### Controllers

- Xbox controllers work as they are.
- DualShock 4, DualSense, Switch Pro and other controllers work through SDL; choose PlayStation button icons under
  *Controls* (or leave *Automatic*).
- A **DualShock 3** needs the [DsHidMini](https://github.com/nefarius/DsHidMini) driver.
- **Steam**: with Steam Input on for the game, the controller shows up as an Xbox controller. Turn Steam Input off
  for it to get PlayStation icons.
- Keyboard: arrows = D-pad, W A S D = left stick, numpad 8 4 2 6 = right stick, Space = A, Esc = B, J = X, I = Y,
  Enter = START, Backspace = BACK, Q / E = LB / RB, Z / C = LT / RT.

## Where your saves are

Everything is in the install folder (by default `%LOCALAPPDATA%\Programs\NFL Head Coach 09`):

| Game version | Saves folder |
|---|---|
| 1.0 | `hc09-cache\content\E000C0AC4E000009\00000001\` |
| 1.2 | `hc09-cache-v12\content\E000C0AC4E000009\00000001\` |
| both, with *Both versions use the same saves* on | `hc09-cache\content\E000C0AC4E000009\00000001\` |

Each save is a folder of its own name (`CAREER-<name>`, `ROSTER-<name>`, `PROFILE-HeadCoach`) holding one file of the
same name, plus a `<name>.name` file beside it. To back up your saves, copy the `hc09-cache` folders. To move them to
another PC, copy them into the install folder there. Saves work in both game versions (*Saves* > *Copy saves*). When
the launcher replaces a save it puts the old one in `content-backup` first.

## PlayStation 3 players

The PC version is built from the **Xbox 360** game, so you need the Xbox 360 copy to install it; your PS3 careers,
rosters and profile can then come along:

1. Your PS3 saves must be **decrypted**: saves made by the **RPCS3** emulator are; saves copied off a real PS3 are
   encrypted. Load a real PS3's saves in RPCS3, or decrypt them with a PS3 save tool, first. A decrypted save is a
   `BLUS30128-...` folder with `HED-DATA` and `USR-DATA` files in it.
2. In the launcher: **Saves** > **Import PS3 saves...** and choose RPCS3's savedata folder
   (`<RPCS3 folder>\dev_hdd0\home\00000001\savedata`) to import every NFL Head Coach 09 save in it, or one
   `BLUS30128-...` folder. Saves already in the PC version are only replaced if you say so (and backed up first).
3. To take a save back to RPCS3: **Saves**, pick the save, **Export to PS3...** and choose RPCS3's savedata folder.
   That folder needs one NFL Head Coach 09 save made by RPCS3 (its settings file and icon are reused). RPCS3's own
   saves are never replaced.

PlayStation controllers: see [Controllers](#controllers). The game can show PlayStation button icons.

## Warnings and limitations

- **US Xbox 360 version only.** Other regions and the PS3 game cannot be used to build it.
- **No game files are included**, and none are provided here or linked to.
- Installing from a **disc image (.iso) is in beta**; installing from the game's folder is the tested way.
- **Xbox LIVE** features do not work.
- The Colts' stadium screen (jumbotron) shows a blank panel: the art is missing in the game's own files.
- The *Emulated* renderer is for checking accuracy and is much slower than *Native*.
- Each game version takes a few minutes to build when installing, and needs the processor and graphics card listed
  above; setup and the game say plainly what is missing.
- Antivirus programs and Windows SmartScreen may warn about unsigned programs; the build step also runs a compiler
  (clang) on your PC, which is expected.

## If something goes wrong

- **Setup** writes `hc09-setup.log` in the install folder (or in `%TEMP%` if it stopped early).
- **The game** writes `hc09.log` in the install folder (*Open log* in the launcher).
- **After a crash** the launcher saves a report in `crash-reports` (the error, a small snapshot of the program's
  threads, the end of the log, your settings and a description of your PC; no game files) and offers
  *Report on GitHub*, which opens a prefilled issue to attach the report's `.zip` to. Sending it is up to you.

## Updating and uninstalling

- **Update**: run the new `HC09Setup.exe` and install into the same folder; saves and settings stay.
- **Uninstall**: Windows *Settings* > *Apps* > *NFL Head Coach 09 for PC*. It asks whether to keep your saves.

## Legal

NFL Head Coach 09 is a trademark of Electronic Arts. NFL and the NFL shield design are registered trademarks of the
National Football League. Team names, logos and player likenesses belong to their owners. This project contains none
of them and none of the game's code or data: the game is built on your PC from your own copy. Third-party software:
see [release/THIRD-PARTY-NOTICES.txt](release/THIRD-PARTY-NOTICES.txt).

Developers: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
