# AlibreCAM

A CAM add-on for Alibre Design assemblies, written in C++/MFC for x64. It adds a CAM tab to the Design Explorer and generates indexed 3+2 toolpaths and G-code. The first machine is a LinuxCNC 3-axis router with an optional 4th axis.

`README.md` covers build requirements, install scripts, and what each add-on file does.

The specification is `docs\SPEC.md`, a copy of the Claude doc at https://claude.ai/artifact/JZEKmxczFdEn2eV32A5acB, which is the master. If you can't update the Claude doc, record spec changes in `docs\SPEC.md` and tell Frank the two now differ.

## Working with Frank

- Frank commits and pushes himself. Don't commit unless he asks.
- Frank tests in Alibre by hand. End a change with a short numbered list of what to test, and say plainly what hasn't been built or run.
- Before calling something in Alibre, check it against the 27.0.1 headers (see below). Check the log before asking Frank what happened.
- Ask before making a product decision that isn't recorded here or in the spec. Propose options when you do.
- When a decision changes, update the spec too.

## Locations

| What | Where |
|---|---|
| This project | `D:\Projects\alibre\AlibreCAM` |
| Proof-of-concept test bed | `D:\Projects\alibre\camtest` |
| Test assemblies | `D:\Projects\alibre\camtest\examples\` (`test1.AD_ASM`, `test2.AD_ASM`) |
| Alibre API docs and samples (read-only, never modify) | `C:\AlibreToolBox\MyAddon - Claude Reference\MyAddon - Claude Reference\Reference\` |
| Generated 27.0.1 headers | `obj\x64\<cfg>\AlibreCAM\AlibreX_64.tlh`, `.tli`, `AlibreAddOn_64.tlh` (also in `camtest\obj\x64\Debug\`) |
| Log | `%LOCALAPPDATA%\AlibreCAM\AlibreCAM.log` |
| Default tool library | `%APPDATA%\AlibreCAM\tools.json` |
| Installed add-on | `C:\ProgramData\Alibre AddOns\AlibreCAM\` |

Target Alibre 27.0.1.27039. The API docs describe v29, so a documented call may not exist. The generated headers are the authority.

## Building and testing

- Open or build `AlibreCAM.sln`, x64 only, with Visual Studio 2022 (v143) and MFC installed. Output goes to `bin\x64\<cfg>\` and intermediates to `obj\x64\<cfg>\<Project>\`.
- Run `bin\x64\<cfg>\CamCoreTests.exe` after any change to `core\`. It doesn't need Alibre.
- Install with `powershell -ExecutionPolicy Bypass -File deploy\install.ps1 [-Configuration Debug]`. Close Alibre first and restart it afterwards, because it reads add-ons only at startup. The script asks for administrator rights.
- Warnings C4278 (`SendMessage`) and C4192 (`IStream`) from the type library imports are expected.

## Architecture

- `addon\` builds `AlibreCAM.dll`. It's the only code that calls Alibre or MFC.
- `core\` builds the `CamCore` static library. It has no Alibre or MFC code, uses UTF-8 text, and works in millimetres. Put logic here wherever possible so it can be tested.
- `tests\` builds `CamCoreTests.exe` with doctest.
- `third_party\` holds pinned copies with their licences: nlohmann/json 3.12.0, doctest 2.5.3 (tests only). Add Clipper2 2.0.1 (BSL 1.0) when Contour and Pocket start.

## Conventions

- Source files use CRLF line endings, except in `third_party\`.
- New add-on files go in both `addon\AlibreCAM.vcxproj` and its `.filters` file. New core files go in `core\CamCore.vcxproj`.
- In `addon\resource.h`, resources and dialog controls use 13xxx IDs. Tab window controls use an enum from 14001 in `CamTabCommand.cpp`.
- Entry points called from Alibre start with `AFX_MANAGE_STATE(AfxGetStaticModuleState())`, as do functions that open dialogs.
- Don't name members after Win32 macros such as `LoadLibrary`.
- Files are written atomically with `WriteFileAtomically`. Never overwrite a file that is damaged or from a newer format version.

## Current state

- Steps 1 and 2 are committed: tab, tree, `.cam` load and save, unsaved warnings, and the tool library.
- Still to confirm from step 1: answering Yes when closing the assembly or exiting Alibre, the different-assembly prompt, and the command being greyed in the Part workspace.
- Step 3, `Set Origin`, is written, built, and installed, but **not yet tested in Alibre** and uncommitted. It covers `core\Origin`, the sequence in `core\CamJob` (format version 2), `addon\CamViewCommand`, `addon\GeometryReader`, `addon\OriginDialog`, and `addon\DisplaySettings.h`.
- Folders (spec "groups", `StepType::Group`) are also written and installed, but untested and uncommitted: Add Folder on Operations, plus Rename (edited in place in the tree) and Delete on a folder.
- Step 4 has started. A Drill operation (`StepType::Drill`, saved with only its ID and name) can be added from a folder's Add Holes ▸ Drill. Operation items can be edited, renamed and deleted. `addon\OperationDialog` is the modeless operation dialog. Its six tabs (Features, Tool, Feeds & Speeds, Clearance, Sorting, Drilling) are empty `COperationPage`s, to be built one at a time, each becoming its own page class. Built and installed, but untested. Tree items for steps carry a `StepRef` (step index, plus operation index inside a folder) packed into the item data.
- Unproven in Alibre: whether typing a folder name in the tree's in-place edit reaches the edit, or whether Alibre takes the keys as shortcuts. Also whether `DrawMesh` honours alpha (the plane is meant to be translucent), whether `IsSenseReversed` gives the outward face normal, and keyboard use in the modeless dialog. Alibre's message loop doesn't call `IsDialogMessage`, so Tab and Enter may not work there.

## Build order

1. Done: CAM tab, tree, and `.cam` file.
2. Done: tool library and selecting a tool in the tree.
3. `Set Origin` and its drawing in the view. **This is the current step.** Built and installed, awaiting Frank's test.
4. Drill: picking geometry and drawing the toolpath with the selected tool. **Started:** the menus and an empty tabbed dialog are built, and the tabs come next, one at a time.
5. Machine, post language, and stock, through to G-code that can be cut on the router.
6. Facing, Contour, and Pocket.
7. Groups, out-of-date marking, and repair.
8. Indexed 4th axis.

## Product decisions

- **Tree:** fixed Setup at the top (Machine, Tools, Stock), then an ordered Operations sequence with groups and `Set Origin` steps. Groups don't nest.
- **Axes:** 3+2 indexed only in version 1. The origin can change during the job.
- **Job file:** a separate JSON `.cam` file next to the assembly, with a format version and the assembly's GUID. Saving is manual, with a warning about unsaved changes when closing.
- **Tools:** stored only in a library file, which has a default and a per-job override. Flat end mills and drills to start with.
- **Cutting settings:** spindle speed, feeds, stepdown, stepover, and peck depth belong to each operation, because they vary with the material. Tools may later hold per-material defaults that fill in a new operation.
- **Selected tool:** clicking a tool in the tree selects it and stores it in the job by tool ID. A tool has a stable ID, so renumbering doesn't break references.
- **Stock:** manual box, bounding box around parts, or an imported Alibre part. The bounding box has six offsets (+/-X, +/-Y, +/-Z), each added outside the part, with -Z usually 0. An imported part stays linked and has an XYZ position and rotation.
- **Machine:** configurable. A simple template post language supports different machines. The first machine has a manual tool change with a touch plate, handled in the post.
- **4th axis:** defined in the machine configuration, with any orientation. Frank's test axis runs along Y. The operator positions and zeroes it, and sets the CAM origin to match.
- **Out-of-date toolpaths:** keep and draw stale paths, mark them out of date, and provide a refresh button.
- **Toolpath display:** paths are visible when the CAM tab has focus, with a foreground toggle.
- **Geometry picking:** faces and edges only.
- **Set Origin:** Z points out of the material from a flat face. A circular edge gives its axis and centre, and a cylindrical face gives its axis. Position comes from an edge: the nearer end of a straight edge, or a circle's centre. X comes from an optional straight edge, otherwise assembly X projected onto the plane (assembly Y if that's too close to the tool axis). There are Flip Z, X quarter turns, and offsets in the origin's own axes. The full rules are in the spec.
- **Origin display:** only the active origin is drawn, with its axes and a translucent shaded XY plane. That's the selected step if it's a Set Origin, otherwise the nearest Set Origin above it (`ActiveOriginIndex`), or the dialog's step while it's open. The plane covers the parts (and later the stock) plus 10%. The colour is `kOriginPlaneColour` in `DisplaySettings.h` until program settings exist.
- **Open:** arc fitting at Contour and Pocket (Clipper2 gives straight lines only). Where machine and post files live is proposed but unconfirmed: installed `machines\`, with `%APPDATA%\AlibreCAM\` overriding it.

## Proven Alibre API behaviour (27.0.1)

These came from testing in `camtest`. `POC_TEST.md` is there, and its `CamViewCommand` and `PocEvents.cpp` are the patterns to reuse for the view command and event sink.

**Tab and commands**
- A two-way toggle command using `AddTab` and `OnShowUI` hosts the tab. Polling `IsWindowVisible` every 250 ms detects tab focus.
- `InvokeNewCommand` starts a regular view command.
- Native commands (Measure, right-click edit, Ctrl shortcuts) end the view command, but the tab stays open.
- Closing an assembly destroys the tab window without calling `OnTerminate`, so handle `OnDestroy`.
- Alibre can't cancel a close from here, so close warnings can only offer Yes or No.

**Graphics**
- Rebuild the graphics in every `On3DRender` with `Begin3DDisplay(VARIANT_TRUE, ...)`. They disappear on pan, zoom, or hover otherwise.
- `ToggleForegroundRendering` switches between clipped by the model and always visible.
- There's no `ClearSegment` in 27.0.1.
- A 200,000-point polyline takes 47 ms, about 9 redraws a second.

**Picking**
- `SelectedObjectsEx` returns an `IADTargetProxy` and the clicked point in assembly coordinates.
- `WorldTransform` is row-major with translation in row 4, verified for translation and rotation.
- `IADDesignSelectionFilter::PutSolid` works for faces, edges, parts, and faces plus edges.
- Vertex selection is refused in assemblies (hr=0x8004032B). It's a documented Alibre limitation, so don't retry it.

**Persistent keys**
- An occurrence's `GetKey` binds in the assembly session with `BindKeyToItem(..., ADObjectType_AD_OCCURRENCE)`.
- Face and edge keys bind in the part session with `ADObjectType_AD_TOPOLOGY`, not the assembly session.
- Different parts can share face keys, so store the occurrence too.
- Keys survive restarts, added features, and dimension changes. Binding after the owner is deleted fails with hr=0x8000FFFF, which means the item needs repair.

**Events**
- Use `GetEventManager` with `IConnectionPointContainer`. The vtable `IADEventsCallback` interface works.
- A move arrives as change types 307 and 48, naming the occurrence.
- Part edits arrive on the assembly session, mostly as types 1, 2, and 3 with no object, so they say something changed but not what.
- A face or edge `TimeStamp` changes only for what was edited. Compare for inequality only, and don't persist it.

**Units**
- The API uses model units, which are cm for new files and may differ for imports. Read `ModelUnits`. A 10-unit square measured 100 mm.

**Imported stock**
- `OpenFileEx(path, VARIANT_FALSE)` opens a part in 187 ms with no GUI. `GetFacetData` gives VT_R8 triplets, and it closes cleanly.
- Events can arrive after the close, so the handler must ignore them.

**Paths and identity**
- `GetGuidAndConstituentInformation(path, &guid, &a, &b, &c)` returns the assembly GUID.
- `GetfilePath` returns forward slashes, and an empty string if the file was never saved.

**Other signature traps**
- `loops->GetItem` takes a `long`, not a `_variant_t`.
