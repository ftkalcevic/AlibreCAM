# AlibreCAM — Alibre Design add-on (C++/MFC)

CAM for Alibre Design assemblies. The add-on gets its own **AlibreCAM** tab on the
ribbon. Its **CAM Tab** button opens a CAM tab in the Design Explorer, which holds
the job tree and saves the job to a `.cam` file next to the assembly.

The project has these parts:

| Folder | Purpose |
|---|---|
| `addon\` | `AlibreCAM.dll`, the only code that calls Alibre |
| `core\` | `CamCore` library: job model, tool library and file storage, with no Alibre or MFC code |
| `tests\` | `CamCoreTests.exe`, which runs without Alibre |
| `third_party\` | Copies of external libraries, with their licences |
| `deploy\` | Install and uninstall scripts |

The log is `%LOCALAPPDATA%\AlibreCAM\AlibreCAM.log`.

## Requirements

- Visual Studio 2022 with:
  - the **Desktop development with C++** workload
  - the **C++ MFC for latest v143 build tools (x86 & x64)** component (MFC is
    not part of the workload by default; without it you get `afxwin.h` not found)
  - a Windows 10/11 SDK
- Alibre Design (64-bit) installed under `C:\Program Files\Alibre Design <version>\`

## Build

1. Open `AlibreCAM.sln`.
2. Choose **Release | x64** (it's the only platform) and build.

`Alibre.props` finds the Alibre `Program` folder that holds
`AlibreX_64.tlb` and `AlibreAddOn_64.tlb`. The build log shows which one it
used ("Alibre type libraries: ..."). If you have several Alibre versions
installed, or it can't find yours, pin it by creating `addon\AlibreCAM.user.props`:

```xml
<Project>
  <PropertyGroup>
    <AlibreProgramDir>C:\Program Files\Alibre Design 2x.x.x.xxxxx\Program\</AlibreProgramDir>
  </PropertyGroup>
</Project>
```

The output goes to `bin\x64\Release\`: `AlibreCAM.dll`, `AlibreCAM.adc` and
`AlibreCAM.ico`.

The two compiler warnings about the type library imports (C4278 `SendMessage`
and C4192 `IStream`) are expected and harmless.

To run the tests, run `bin\x64\Debug\CamCoreTests.exe` from a command prompt.

## Install

Close Alibre Design, then from the project folder:

```
powershell -ExecutionPolicy Bypass -File deploy\install.ps1
```

(Add `-Configuration Debug` to install the Debug build.) The script asks for
administrator rights and then:

- copies the three files to `C:\ProgramData\Alibre AddOns\AlibreCAM\`
- adds the string value `{D3A31918-AFE0-45F2-A080-B531E6894179}` =
  that folder under `HKLM\SOFTWARE\Alibre Design Add-Ons`

Start Alibre Design. An **AlibreCAM** ribbon tab appears. Alibre reads add-ons only
at startup, so restart it after every install or update.

To remove it: `powershell -ExecutionPolicy Bypass -File deploy\uninstall.ps1`.

## Debugging

Install the Debug build (`install.ps1 -Configuration Debug`), then press F5 in
Visual Studio. The project launches `Alibre Design.exe` and breakpoints in
`AddOnInterface.cpp` will hit.

## How it works

| File | Purpose |
|---|---|
| `AlibreCAM.cpp/.h` | MFC app object and the exported `AddOnLoad`, `AddOnInvoke`, `AddOnUnload` and `GetAddOnInterface` functions (listed in `AlibreCAM.def`) |
| `AddOnInterface.cpp/.h` | `IAlibreAddOn`: menu tree (root *AlibreCAM* → *CAM Tab*, *About*), `UseDedicatedRibbonTab` = TRUE, `MenuIcon`, and `InvokeCommand` |
| `AboutDlg.cpp/.h`, `AlibreCAM.rc` | The About dialog |
| `CamTabCommand.cpp/.h` | The CAM tab: its window, job tree, and loading and saving the `.cam` file |
| `CamViewCommand.cpp/.h` | The view command that runs while the tab has focus, draws origins, and receives picks |
| `CommandBase.cpp/.h` | Default `IAlibreAddOnCommand` behaviour |
| `DisplaySettings.h` | Colours and sizes for CAM graphics, until program settings exist |
| `GeometryReader.cpp/.h` | Reads picked faces and edges, and the assembly's extents, in assembly millimetres |
| `ModelReader.cpp/.h` | Reads Alibre data into plain data for `CamCore` |
| `OperationDialog.cpp/.h` | The modeless operation dialog and its tabs |
| `OriginDialog.cpp/.h` | The modeless Set Origin dialog |
| `PostDialog.cpp/.h` | Finds the post scripts and chooses the post and output file before posting |
| `Help.cpp/.h` | Opens the help page in the default browser at a section |
| `StockDialog.cpp/.h` | The Box Stock, Part Box Stock and Imported Stock dialogs |
| `SettingsDialog.cpp/.h` | The Settings dialog (CAM view size), saved in `%APPDATA%\AlibreCAM\settings.json` |
| `Log.cpp/.h` | The log file |
| `ToolDialogs.cpp/.h` | The tool library and tool editing dialogs |
| `AlibreCAM.adc` | Add-on manifest. `specificationVersion="2"` is required for the ribbon tab and icons |
| `Alibre.props` | Finds the Alibre type libraries |
| `deploy\*.ps1` | Install and uninstall |

The About command is a stateless ("fire-and-forget") command:
`InvokeCommand` shows the dialog and returns a NULL `IAlibreAddOnCommand`. To
add more ribbon buttons, add a menu id in `AddOnInterface.h`, add it to the
`children` array in `SubMenuItems`, and handle it in `MenuItemText`,
`MenuItemState`, `MenuItemToolTip` and `InvokeCommand`.

The add-on GUID `{D3A31918-AFE0-45F2-A080-B531E6894179}` appears in
`AlibreCAM.adc` and both deploy scripts. If you change it, change all three.

## Posts

Post scripts are plain text `.post` files in `posts\`. `deploy\install.ps1` copies them into the add-on's `posts\` folder. To change one for a machine, copy it to `%APPDATA%\AlibreCAM\posts\` and edit the copy; a post there replaces one of the same file name. The template format is described at the top of `core\ScriptedPost.h`, and `posts\LinuxCNC.post` is the first.

## Help

The help is a single page, `help\AlibreCAM.html`, with a section for the CAM tab and for each dialog and operation tab. `deploy\install.ps1` copies it into the add-on's `help\` folder, and the ? buttons open it at their section.
