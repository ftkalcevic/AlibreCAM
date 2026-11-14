# Lathe support: staged plan

## Context

Frank wants CAMForAlibre to drive a lathe as well as a mill. The machine gets a type (Mill or Lathe). A lathe job gets rod stock and a single Add submenu of turning operations. He asked for the plan to be saved, then built and tested one stage at a time. The operation list isn't final yet ("add as many as you can think of").

**Decided by Frank:**
- **The spindle axis comes from Set Origin.** Its Z is the spindle axis, picked from a cylindrical face, round edge or design axis as now. Its position is the work zero, and its X is the radial direction.
- **Lathe tool types:** turning insert, grooving/parting, threading, and boring bar.

Nothing is built yet. Stages are added and tested one at a time, when Frank picks the next.

---

## Approach

- **A turning toolpath is a 2D profile.** It's built in the Set Origin's XZ plane: Z along the spindle, X the radius, positive away from the axis.
  - Stored as ordinary `ToolpathMove`s in the origin's frame, with Y = 0.
  - Drawing, saving, arc fitting (ZX plane, `ArcPlane::ZX`) and `SimplifyStraightRuns` all work unchanged.
- **The part's turned outline** is a new core `TurnedProfile`: the largest radius of the parts at each Z.
  - It comes from the parts' facets, already read by `ReadAssemblyPartFacets`.
  - Each triangle is swept round the axis, giving the radius envelope over Z.
  - The profile is sampled finely, then simplified.
  - It's used for outside work. Inside work (bore) takes the smallest radius of the hole.
- **Selected features stay optional.** Generate uses the whole turned profile limited to a Z range (Start Z / End Z, with Locate picks as now). Picked faces, if given, limit the Z range.
- **Every lathe operation shares the same tabs:** Tool, Feeds & Speeds, Clearance (lathe form), and its own tab.

## Stage 1: Machine type and rod stock

- **Machine type** (`core\Machine`): `MachineType Mill|Lathe`, saved as `type`; a missing value reads as Mill.
  - The Machine tab gets a Type combo.
  - A lathe greys Axes/Rotary, travel Y, and the tool holder. Travel X becomes diameter capacity.
  - Machine format stays 3, as the field is optional.
- **Stock kinds** (`core\Stock`): `Rod` and `PartRod`.
  - **Rod Stock:** diameter and length, along the Set Origin's Z through its position. Front face offset from the work zero, as in Box Stock's corner.
  - **Part Rod Stock:** the cylinder round the chosen parts (all if none) about that axis. It has three offsets: radial, +Z end and −Z end, matching Part Box's ±.
  - **Import Stock:** as for a mill.
  - **Functions:** `RodStockCylinder`, `PartRodStockCylinder` (from the parts' points), `CylinderEdges` (two circles plus four lines) for drawing, and `CylinderMesh` for the simulation and the stock's top.
  - **Axis:** the Rod holds no axis of its own. It uses the first unsuppressed Set Origin (`WorkZeroIndex`). With none, the stock shows as unreadable and the tree warns.
- **Tree:** the Stock menu shows Rod Stock, Part Rod Stock and Import Stock on a lathe, and the box kinds on a mill.
  - A stock of the wrong kind for the machine gets a tree warning.
  - It's never deleted automatically.
- **Dialogs** (`addon\StockDialog`):
  - `CRodStockDlg` is modal, like `CBoxStockDlg`.
  - `CPartRodStockDlg` is modeless, reusing `CPartBoxStockDlg`'s part picking.
  - The tree text is `Stock - Rod Stock`.
- **Saving:** `setup.stock.kind` gains `rod`/`partRod`, with `rodDiameter`, `rodLength`, `rodFront` and the radial and end offsets. `.cam` format version 16.
- **Tests:** round trips, validation, the cylinders from the parts' points, and older files.

## Stage 2: The lathe Add menu, operations and tools

- **Folder menu on a lathe:** one **Add Turning** submenu replaces Add Holes / 2 1/2 Axis / 3 Axis. It lists these operations:
  - Facing
  - Turn Roughing
  - Turn Finishing
  - Grooving
  - Threading
  - Parting
  - Boring (inside rough and finish)
  - Drilling (on the axis: peck, through the tailstock or turret)
  - Turn Chamfer (edge breaks on the profile's corners)
  - Grooving Face (axial grooves)
  - Knurling (a dwell pass, very simple)
- **On a mill, the folder menu doesn't change.**
  - Mill operations in a lathe job (or the reverse) get a tree warning, "Not for this machine's type", and Generate skips them.
- **New `StepType`s:** `LatheFacing`, `TurnRoughing`, `TurnFinishing`, `Grooving`, `Threading`, `Parting`, `Boring`, `LatheDrilling`, `TurnChamfer`, `FaceGrooving` and `Knurling`.
  - Tree images go in every strip, and help gets a section for each operation.
- **Tool types** (`core\ToolLibrary`; library and machine files version 4):
  - **Turning insert:** insert shape (C 80°, D 55°, V 35°, T 60°, W 80°, S 90°, R round), nose radius, approach (lead) angle, hand (left, right or neutral), and cutting edge length.
  - **Grooving / parting:** blade width, corner radius, maximum depth, and hand.
  - **Threading:** thread angle (60, 55 or 29), tip radius or flat, and internal or external.
  - **Boring bar:** as a turning insert, plus minimum bore diameter and reach.
  - **The lathe tools have:**
    - Their own drawings in `DrawToolDiagram`, with letters on the labels as now.
    - A tool offset (turret) number in place of the mill's T number, posted as `T0101`.
  - **Which list each machine type shows:**
    - The Tool dialog lists only the types for the machine's type.
    - The operation's Tool tab offers only the tools that fit the operation. For example, a Grooving operation offers only grooving tools.
- **Settings per operation** (core structs plus a dialog tab each), building the dialog before the toolpath as Drill and Facing were:
  - **Facing:** Start and End X (diameter), Z to face to, stepdown, finish pass, and direction (inward or outward).
  - **Turn Roughing:** Start and End Z, stock to leave in X and Z, depth of cut (radius), retract, outside or inside, and a Z or X direction (longitudinal or facing roughing).
  - **Turn Finishing:** Start and End Z, passes, stock to leave, and an optional spring pass.
  - **Grooving:** position by a picked face or Z, width (several plunges if wider than the tool), depth, peck, and dwell.
  - **Threading:** Start and End Z, pitch, thread form (ISO metric, UN, BSW, ACME), major and minor diameter (from the form or typed), number of passes or first depth with degression, infeed angle, spring passes, and internal or external.
  - **Parting:** Z, peck, a reduced feed near the centre, and stop diameter.
  - **Boring:** as Turn Roughing and Finishing, inside.
  - **Drilling:** depth, peck, dwell, and retract (reusing `DrillSettings`).
- **Saving:** `.cam` format version 17.

## Stage 3: The turned profile and Facing / Turn Roughing / Turn Finishing toolpaths

- **`core\TurnedProfile`:** the radius envelope from the parts' triangles about the origin's Z, simplified to lines and arcs, with tests on a cylinder, a cone and a sphere.
- **`core\TurnToolpath`:**
  - **Facing:** passes across X at Z steps.
  - **Roughing:** horizontal passes at decreasing X, each clipped to the profile offset by the stock to leave and the tool's nose radius. The tool's back angle keeps it out of undercuts it can't reach, from the insert angle and approach.
  - **Finishing:** the profile offset by the nose radius (nose radius compensation done in the CAM), with lead in and lead out.
- **Drawing:** the profile in orange, and the toolpaths in the origin's XZ plane, so they show beside the part.

## Stage 4: Grooving, Parting, Drilling and Threading toolpaths

- **Grooving:** plunges stepped by the blade width less overlap, then a finish across the bottom.
- **Parting:** pecks down to the stop diameter.
- **Drilling:** along −Z on the axis, reusing `GenerateDrillToolpath` with X = 0.
- **Threading:** passes with the form's depths, saved as moves with a new `MoveKind::Thread` (pitch). Posts write it as a canned cycle or a synchronised move.

## Stage 5: Lathe post

- **`posts\LinuxCNC Lathe.post`:**
  - G18, G7 diameter mode, G21/G90, tool changes as `T0101 M6`.
  - G96/G97 constant surface speed with a maximum RPM, which needs the Feeds tab to offer surface speed on a lathe.
  - G2/G3 in the XZ plane, G33 or a G76 thread cycle, and G95 feed per revolution as an option.
- **`ScriptedPost`** gains: diameter mode (X doubled), `[thread]` and `[css]` sections, and the turret tool number.
- **Tests:** post output for each lathe operation.

## Stage 6: Lathe simulation

- **Model:** the stock as a radius per Z (a 1D height field about the axis). Each move removes material by the insert's shape swept in XZ.
- **Drawing:** revolved into a mesh.
- **Checks:** the Compare against the turned profile, and a rapid into stock warning.
- Much simpler and faster than the voxel simulation, which stays for mills.

## Open questions for later stages, not needed for stage 1

- **Live tooling or a C axis:** left out.
- **Where the tool sits:** a rear turret flips X. Should that be a machine setting, "Tool post: front/rear"?
- **Feed units:** per revolution or per minute by default.
- **The final operation list:** Frank will trim the 11 operations listed in Stage 2.

## Files (for the whole plan)

- **Core:** `core\Machine`, `core\Stock`, `core\ToolLibrary`, `core\CamJob`, and new `core\TurnedProfile`, `core\TurnToolpath`, `core\LatheSettings` and `core\LatheSimulation`.
- **Add-on:** `addon\MachineDialog`, `addon\StockDialog`, `addon\ToolDialogs`, `addon\ToolDrawing`, `addon\OperationDialog`, `addon\CamTabCommand`, `addon\CAMForAlibre.rc`, `addon\resource.h`, and the tree and toolbar strips.
- **Posts and documentation:** `posts\`, help, `docs\SPEC.md` and the Claude doc, `CLAUDE.md`, and `TODO.md`.

## Verification (each stage)

- **Build:** Debug and Release.
- **Tests:** run `CamCoreTests.exe`, with new tests per stage.
- **Install**, with Alibre closed first.
- **Frank in Alibre:** a numbered test list per stage. For stage 1:
  1. Switch the machine to Lathe and back.
  2. Add a Set Origin on a turned part's cylindrical face.
  3. Add a Rod Stock, then a Part Rod Stock: check how they're drawn and edited, and that they save and reload.
  4. Check the stock menus for each machine type, and the warnings.
