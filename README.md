# AlibreCAM — Alibre Design add-on (C++/MFC)

A minimal integrated Alibre Design add-on. It registers with Alibre, gets its
own **AlibreCAM** tab on the ribbon, and that tab has an **About** button that
opens an About dialog.

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
installed, or it can't find yours, pin it by creating `AlibreCAM.user.props`
next to the project:

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

Start Alibre Design. A **AlibreCAM** ribbon tab appears, and its **About**
button opens the dialog. Alibre reads add-ons only at startup, so restart it
after every install or update.

To remove it: `powershell -ExecutionPolicy Bypass -File deploy\uninstall.ps1`.

## Debugging

Install the Debug build (`install.ps1 -Configuration Debug`), then press F5 in
Visual Studio. The project launches `Alibre Design.exe` and breakpoints in
`AddOnInterface.cpp` will hit.

## How it works

| File | Purpose |
|---|---|
| `AlibreCAM.cpp/.h` | MFC app object and the exported `AddOnLoad`, `AddOnInvoke`, `AddOnUnload` and `GetAddOnInterface` functions (listed in `AlibreCAM.def`) |
| `AddOnInterface.cpp/.h` | `IAlibreAddOn`: menu tree (root *AlibreCAM* → *About*), `UseDedicatedRibbonTab` = TRUE, `MenuIcon`, and `InvokeCommand` |
| `AboutDlg.cpp/.h`, `AlibreCAM.rc` | The About dialog |
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
