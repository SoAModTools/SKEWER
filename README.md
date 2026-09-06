# SKEWER

Skies Keyed Encounters: Weighting, Editing, and Regions.

SKEWER is a self-contained Windows C++/Qt desktop application for inspecting and editing *Skies of Arcadia* random-encounter regions and encounter tables. Current support targets Dreamcast.

## Features

- Discover supported fields from an extracted game-data root.
- Visualize encounter collision and field-context geometry, with encounter regions colored by selector.
- Select triangles and edit their encounter selectors alongside all eight ECT tables, with undo and redo.
- Optionally display encounter formations and enemy names from ALX 5.0.0 CSV data.
- Preserve edits and viewer state in a portable workspace beside the application.
- Validate workspace patches and export only modified Dreamcast ECT and MLD files.

## Supported formats

| Format                                         | Purpose                                             | Support                                     |
| ---------------------------------------------- | --------------------------------------------------- | ------------------------------------------- |
| ECT                                            | Encounter tables                                    | Inspect, edit, and export                   |
| MLD                                            | Collision geometry and triangle encounter selectors | Inspect geometry; edit and export selectors |
| SCT                                            | Optional field-state presets for context geometry   | Read-only                                   |
| ALX 5.0.0 `enemy.csv` and `enemyencounter.csv` | Formation and enemy-name context                    | Optional, read-only                         |

Area 99, GameCube fields, and ALX editing and export are not currently supported.

## Building

### Requirements

- Visual Studio with the MSVC `v145` toolchain.
- Qt 6.10.3 `msvc2022_64`, registered with Qt/MSBuild as `6.10.3_msvc2022_64`.
- The SPICE Git submodule initialized.

Initialize the submodules:

```powershell
git submodule update --init --recursive
```

Build from an elevated PowerShell prompt:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
    SKEWER.sln `
    /p:Configuration=Debug `
    /p:Platform=x64 `
    /p:QtMsBuild="$env:LOCALAPPDATA\QtMsBuild" `
    /m:1 `
    /nr:false `
    /v:minimal
```

The application and its deployed Qt runtime are written to `bin/x64/Debug/` or `bin/x64/Release/`.

## Testing

After building the Debug configuration, run:

```powershell
.\bin\x64\Debug\SkewerTests.exe
```

## Usage

Launch `SkewerQt.exe`, choose **File > Open Game Data Root...**, and select an extracted game root or its `FIELD` directory (currently supports only a single FIELD directory). Select an enabled field to view its encounter regions, edit triangle selectors and encounter tables, and review validation diagnostics. Use **File > Export Workspace Patches...** to publish the modified Dreamcast files.

For optional formation details, choose **ALX > Select ALX Data Directory...** and select the ALX 5.0.0 directory containing both `enemy.csv` and `enemyencounter.csv`.

SKEWER stores its `workspace` beside the executable and does not fall back to AppData. The executable directory must be writable to persist edits and  viewer state between sessions.

## Acknowledgements

SKEWER uses [SPICE](SPICE/README.md) for Dreamcast field parsing, writing, and format support. SPICE and its third-party components remain subject to their respective licenses and notices.

SKEWER is an independent fan and research project and is not affiliated with or endorsed by the rights holders of *Skies of Arcadia*.
