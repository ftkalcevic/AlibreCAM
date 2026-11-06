# Discussion log

A running log of what Frank asked for, what was decided, and where it ended up. Newest at the bottom. Decisions are also recorded in the spec (`docs\SPEC.md`) and `CLAUDE.md`. This log keeps the conversation behind them.

Entries before 2026-11-06 are summarised from memory of the session and are less detailed.

## Earlier in the session (summary)

- **Facing toolpath**
  - Climb, Conventional and Mixed, with step and overtravel, Linear or Spiral, from the Cut Start corner. A 90° cut angle runs along X.
  - Climb was backwards, and was fixed: the material is kept on the cutter's right.
  - Frank kept the Cut Start behaviour for one-way passes.
  - Mixed steps over square to the passes, at the further-out end, cutting air rather than feeding back.
  - Cut levels were added.
  - Entry/exit: 2D linear and radial, and along a 3D path, applied at each level. Angles and radii are signed. Positive is away from the geometry, and the exit side was fixed.
- **Generate button** on every operation dialog. It applies the dialog and generates, and the dialog stays open.
- **Arcs:** kept in facing boundaries, radial leads and arc step-overs, and drawn in magenta.
- **Profiling:** operation, tab and toolpath, and spline edges.
  - Open profiles end square to the edge's end.
  - Lines are joined without gaps, starting at the earliest feature's loose end.
  - Narrow places are left uncut, and there's no alternating direction.
  - Tabs are the material's length, on the low levels, on all passes.
- **Suppress** on Set Origin, folder and operation. Suppressed items are grey, and so are their children.
- **Pocketing:** operation, tab and toolpath (Offset, Linear and Spiral; not Radial). Islands are ignored. The tool stays down when it's safe. Linear gets a final lap, and Offset alternates direction each loop. Several closed loops are separate pockets.
- **Several geometries per operation.** Edges are grouped where they touch. Heights come from Cut Levels only, because these are 2 1/2 axis operations.
- **Hole Pocketing:** operation, tab and toolpath (helix down, spiral out, clean-up lap, helix engage).
- **Highlighting:** an operation's geometry is shown in orange only while it's selected in the tree, or while its dialog is open.
- **Toolbar:** icon buttons for Draw over model and Show toolpaths, and a Settings gear that opens CAM View Size (Small, Medium or Large).
- **Posts:** a scripted plain-text post language, chosen over a scripting package. It ships with a LinuxCNC post. Posts are installed with the add-on, plus `%APPDATA%` overrides. A tool change writes just M6, and 3+2 is refused by a 3 axis post.
- **TODO.md** started with Frank's list.

## 2026-11-06

- **Help:**
  - A single HTML page, opened at the right section.
  - A "?" button on the CAM toolbar and at the bottom left of every dialog.
  - Fields with undecided behaviour say "To be completed."
  - **Fix:** Chrome's `--single-argument` took the quotes as part of the address, so the address is now passed unquoted.
- **Reusing one browser tab** for help was looked at. Frank decided to keep the browser as it is.
- **TODO priorities** are marked 🔴 high, 🟡 medium and ⚪ low, with a key at the bottom.
- **Stock**
  - The menu has Box Stock, Part Box Stock, Import Stock, Edit and Delete. Only one stock is allowed, and the node reads "Stock - kind".
  - Decided: assembly axes; Part Box picks parts (all by default); the kinds are greyed while a stock exists, with Edit added; the stock is drawn as a wireframe with a Show stock toggle.
- **Stock in clearance:** Stock max Z uses the stock, and Automatic is 5 mm above the higher of the parts and the stock.
- **Set Origin can pick design items:** a plane or axis for Z, a point for the position, and an axis for X. They're stored by name.
- **Machine, materials and tools**
  - Machines are files in `%APPDATA%\AlibreCAM\machines\`, and a new job takes the last machine used.
  - `tools.json` isn't loaded automatically. Tools come in and go out by Import and Export.
  - Materials are one shared list, and the job picks one.
  - Each tool has feeds per material. The Feeds & Speeds tab's **From Tool** button fills it, and only the button does.
  - A machine has 3 or 4 axes, with the rotary axis along -X, +X, -Y or +Y.
- **Machine travel** in X, Y and Z, and the spindle's maximum speed. These aren't used for checks yet.
- **Bulk edits**
  - Ctrl/Shift+click in the tree chooses several operations.
  - The dialog shows only the tabs they all share.
  - It works per field: values that differ show blank, and only changed fields are applied.
- **Tool types:** ball mill, corner radius mill, V-mill, chamfer mill, taper mill and thread mill. Each has its own fields: corner radius, included angle and tip diameter, taper angle per side, or pitch and thread angle.

## 2026-11-07

- **Work instructions:** the help page must be updated in the same change whenever the UI or behaviour changes. This is in `CLAUDE.md`.
- **Set Origin:** a new step no longer starts picking the plane on its own. It waits for the Pick button.
- **Degree sign bug:** "Turn X about Z" showed "Â°". Source files have no byte-order mark, so non-ASCII text in strings is now written as escapes. This is in `CLAUDE.md`.
- **Chamfer operation**
  - Its tab has the Profiling tab's fields plus Chamfer Width, Tooltip Clearance and Max Depth/Cut.
  - Defaults: 1 mm, 0.5 mm and 1 mm.
- **Chamfer toolpath**
  - It needs a V-mill or chamfer mill. There's no Cut Levels tab and no Tabs group, and Stepover makes the side passes.
  - The depth is width ÷ tan(half angle). The tool centre runs tip radius + clearance × tan(half angle) + stock to leave from the edge.
  - **Fix (Frank caught it):** the passes split only the chamfer's own depth, and the tip goes the clearance below each pass. A 1 mm chamfer at 0.5 per cut is 2 passes, not 3.
- **Engrave operation and toolpath**
  - Faces give every edge, holes included, and lines needn't close.
  - Edges are merged into polylines where they touch, which Frank asked for. Where three or more meet, the earliest in the list carries on.
  - The tool's centre follows the line, at each cut level.
  - Frank asked for clearer names than zig and zig zag. The traversal options are "Lift and return to the start" and "Stay down and run back". A closed loop that stays down carries on round.
- **Clear Toolpath** after Generate, on every menu that has Generate.
- **Tree images:** a gear for Setup, a folder for folders, a shape for each tool type, and a unique image for each operation. They're drawn at 16, 24 and 32 px.
- **Error marks in the tree:** a red "!" over the image when:
  - an operation has no toolpath;
  - a Set Origin is incomplete;
  - the machine, material or stock is missing or can't be read.

  Folders are never marked. Suppressed operations aren't marked either, which was my choice.
- **Simulation**
  - Decided: full 3D voxels; shown in Alibre's view; play, pause, speed, rewind, step by move and by operation, a scrub bar, and a warning on a rapid into stock; scope from the right-click menus; it needs a stock.
  - Each tool type cuts with its own profile, and tilted origins cut along their own axis.
  - Timed in Release: a 100 × 100 × 25 block at 0.2 mm cells is faced in 1.2 s, and meshes in 0.5 s.
  - Not yet tried in Alibre.
- **Discussion.md** started (this file), to be kept up to date.
- **Simulation status bug (Frank caught it):** the dialog showed "Facing 1 â€” move 94" because the em dash was typed into the source, not written as an escape. It's fixed, and so is the same slip in CLAUDE.md's example. A new test (`tests\SourceTests.cpp`) now fails if any string in `addon\` or `core\` has a non-ASCII character.
