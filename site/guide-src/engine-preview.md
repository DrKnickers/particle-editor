# Live Engine View

The editor's own viewport is a close match to the game, but it is not the game's renderer. The
**live engine view** shows your effect drawn by Petroglyph's engine itself, and updates a moment
after each edit. It runs your own copy of the *EAW Terrain Editor* (the official map editor) in
the background as a render host, with a small add-on called **Alamo Preview**.

Setting it up takes a few minutes and is done once. Nothing in your game install is changed.

## What You Need

- **Windows** and *Star Wars: Empire at War — Forces of Corruption*, with the editor already
  finding your game install (see [Setup](setup)).
- **The 64-bit EAW Terrain Editor.** Download `EAW_Terrain_Editor_b.zip` from Petroglyph's
  [mod tools page](https://www.petroglyphgames.com/eawmodtool/) and extract it to a folder of
  your choice, for example `C:\Games\EAW Terrain Editor\`.

  > The `EAW Terrain Editor.exe` that comes with the game in `GameData` is an older 32-bit build
  > and will not work. Use the one from the mod tools page.

- **The Alamo Preview package.** Download `alamo-preview-<version>.zip` and extract it. The
  extracted folder holds a file called `package.json`; setup reads the rest from beside it.
- **Disk space.** Setup makes a private copy of the game's `Data` folder. If the game is on the
  same drive as your Windows user folder, the copy shares the game's files and takes almost no
  space. On a different drive it is a full copy of about 1.5 GB.

## Set It Up

1. Open **Edit → Preferences…** and find the **Engine preview** section.
2. Click **Set up**.
3. Next to **Map editor**, click **Browse** and choose the `EAW Terrain Editor.exe` you extracted.
4. Next to **Host package**, click **Browse** and choose `package.json` in the extracted Alamo
   Preview folder.
5. Click **Set up**. A progress bar shows each stage. You can close the dialog; setup keeps
   going and the Preferences section shows *Setting up* until it is done.

When it finishes, the dialog shows *Setup complete* and the path of the new copy, and the **Map
editor executable** field is filled in for you. The copy lives in your user folder, under
`%LOCALAPPDATA%\AloParticleEditor\EngineHost`.

## Use It

With an effect open, choose **View → Live engine view**. The first start takes several seconds
while the map editor loads; a badge on the viewport shows *Starting engine view…*, then the frame
rate once frames arrive. From then on, each edit appears in the engine view shortly after you make
it, and the engine view follows the editor's camera and ground. Choose **View → Live engine view**
again to turn it off.

## Remove It

In **Edit → Preferences… → Engine preview**, click **Remove setup** (it appears once setup has
run), then **Confirm remove**. This deletes only the files setup created, and clears the map editor
path if it points to that copy. If the copy shares files with
your game install, removing it only removes the copy's names for them; your game files stay as
they are.

## If Something Goes Wrong

- **"Unsupported map editor. Supported editors: …"** — the chosen `EAW Terrain Editor.exe` is not
  the 64-bit build this version of Alamo Preview supports. Download it again from the mod tools
  page and choose that copy.
- **"The host package file failed SHA-256 verification"** or **"The host package is missing …"** —
  a file in the extracted package is damaged or missing. Extract the zip again into an empty
  folder and choose its `package.json`.
- **"Engine preview setup requires a Forces of Corruption root (corruption) in the game
  paths."** — the editor's game paths don't include your Forces of Corruption `corruption`
  folder, so it has not found your install. Fix that first; see
  [Setup](setup).
- **"The managed engine host folder is not empty. Remove its previous setup first."** — an
  earlier setup is still there. Click **Remove setup** and try again.
- **"Stop the live engine view first; a process is running from the managed folder."** — turn
  off **View → Live engine view** and wait a few seconds before removing or setting up again.
- **The engine view stops and the badge says it closed.** The editor falls back to its own view.
  Turn the live engine view on again; if it keeps closing, remove the setup and set it up again.
