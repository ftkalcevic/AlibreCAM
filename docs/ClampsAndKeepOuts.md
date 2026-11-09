# Clamps and keep-outs: suggested design

## Context

Nothing stops a toolpath hitting a clamp, a vice or a screw. `TODO.md` lists "Clamps and vices as keep-out for toolpaths and the simulation". A crash here costs a cutter or a spindle, so it's the most valuable safety feature left.

Today, a clamp modelled in the assembly is treated as *part of the job*: `ReadAssemblyPartFacets` takes every visible part. So the 3D operations avoid it by accident, but the simulation's Not enough removed / Too much removed compares against it, and 2 1/2 axis operations ignore it.

## Suggestion in one paragraph

A new **Fixtures** node in Setup, under Stock. A fixture is either an **assembly part you pick** (a modelled clamp or vice) or a plain **keep-out box** you type in (for when you don't want to model it). Fixtures are never "the part". The simulation always checks against them; 3D operations avoid them; 2 1/2 axis operations warn at Generate rather than silently changing your cut.

## What each fixture is

- **Fixtures node:** Setup ▸ Fixtures, with a child per fixture. Right-click: Add Parts, Add Keep-Out Box, then Edit, Suppress, Delete on each.
- **Parts:** picked as Part Box Stock picks them (`ReadSelectedParts`), stored by occurrence key, so they follow the model if a clamp moves.
- **Box:** corner and size in assembly axes, as Box Stock (`CBoxStockDlg`, `BoxStockBox`).
- **Fixture Clearance:** one job-wide margin, default 2 mm, how far the tool must stay from any fixture.
- **Drawn** in the view in their own colour as edges, like stock (`MeshOutlineEdges`, `BoxEdges`), with a toolbar show/hide toggle.
- **Saved** under `setup.fixtures`, `.cam` format version 16, and carried in templates (boxes useful; picked parts need checking on another assembly).

## What they do

| Where | Behaviour |
|---|---|
| **The part** | Fixture parts are left out of `ReadAssemblyPartFacets`, so they stop counting in comparisons and flat-level detection. |
| **Clearance plane** | Automatic and Stock max Z rise to clear the highest fixture, so rapids at clearance can't hit one. An Absolute Z or Part max Z clearance below a fixture is a warning at Generate. |
| **Simulation** | A fixture voxel grid is filled with `FillMesh`. Tool, neck, shank and holder are checked with `CountMove` as they are against stock, including rapids. A hit stops with "The tool hits fixture X" and the buttons carry on. Fixtures are drawn shaded. |
| **3D operations** (Plunge Roughing, Waterline, Parallel) | Fixture triangles, grown by the clearance, are added to the obstacles the tool's tip can't go through (`PartHeightField` via `partTriangles`, without affecting flat levels or surface refinement). They then avoid them with no new toolpath code. |
| **2 1/2 axis operations** (Facing, Profiling, Pocketing, Engrave, Chamfer, Drill, Hole Pocketing) | After Generate, each toolpath is checked against the fixtures (a new core `CheckFixtures`). A collision is reported in the status and as a tree warning, naming the operation. The toolpath is left as generated. |

**Why warn for 2 1/2 axis, not trim:** trimming silently leaves stock uncut or changes the cut in ways you'd only find on the machine, and a warning tells you to move the clamp or shrink the area. Automatic trimming can come later as an option.

## Order of work

1. Fixtures node, dialogs, drawing, saving, and leaving them out of the part.
2. Simulation collisions and drawing.
3. Clearance plane, then 3D avoidance and the 2 1/2 axis check at Generate.

## Limits to expect

- Fixtures apply to every operation. Clamps that move between setups (a flip, say) would need a per-Set Origin list, which I'd leave until it's needed; Suppress covers it for now.
- A suppressed or hidden fixture part is not checked. Hidden parts stay out of the part list as they do now.
- The check is against the tool's real shape in the simulation, but the 2 1/2 axis check at Generate uses cylinders.

## Decisions for you

1. **Both kinds:** picked parts and typed boxes (suggested), or only one?
2. **2 1/2 axis:** warn only (suggested), or trim the toolpath round fixtures?
3. **Clearance:** one job-wide Fixture Clearance, 2 mm (suggested), or one per fixture?
4. **Scope:** all operations (suggested), or per Set Origin from the start?
5. **Templates:** carry fixtures (suggested), or leave them out?

## Files

`core\Stock` or a new `core\Fixtures`, `core\CamJob`, `core\Simulation`, `core\Clearance`, `addon\CamTabCommand`, `addon\StockDialog` (or a new `addon\FixtureDialog`), `addon\CamViewCommand`, `addon\SimulationDialog`, `addon\CAMForAlibre.rc`, `addon\resource.h`, the tree and toolbar strips, help, spec, `CLAUDE.md`, `TODO.md`, and new tests.

## Verification

- **Core tests:** saving round trip and older jobs; a tool and holder hitting a fixture in the simulation, and a rapid over one; automatic clearance above a tall fixture; Waterline Roughing keeping the clearance from a fixture; `CheckFixtures` flagging a profile passing through a box and passing one that clears it.
- **Build** Debug and Release, run `CamCoreTests.exe`, install.
- **Frank in Alibre:** model a clamp, add it as a fixture, and check that it's drawn, that the simulation stops on a crash, that a 3D operation goes round it, and that a Profiling through it warns.
