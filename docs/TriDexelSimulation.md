# Tri-dexel simulation mode

## Context

Nothing is built yet. This plan was saved for later, when Frank asks for it.


The simulation now keeps the stock as voxels: each cell is cut when its centre falls inside the tool. Slopes, round walls and tilted 3+2 cuts come out stair-stepped at the cell size.

A tri-dexel model instead keeps three grids of rays, along X, Y and Z, at one spacing. Each ray holds the exact intervals where it's inside the stock. Cuts are exact along every ray, so the surface built from the rays' ends is smooth and accurate at the same memory.

Frank asked for it as an option. His choices:
- **The option:** a Stock model choice in the Simulation dialog (Voxels / Tri-dexel). Changing it restarts at the same move, as the Resolution slider does.
- **Drawing:** a smooth surface through the dexels' exact ends.
- **What must work in tri-dexel mode:** Compare (Not enough / Too much removed), Save STL, and the collision checks (rapid into stock, shank, neck, holder).
- **What stays on voxels:** rest machining's remaining stock.
- **Resolution slider:** sets the dexel spacing, with its own stops; Auto divide is greyed.

## Approach

### Core: `core\TriDexel.h/.cpp` (new, in `core\CamCore.vcxproj`)

**`TriDexelStock`: the three grids**
- One spacing `s` over the stock box. The rays lie on the lines of one cubic lattice with nodes at the cell centres, like `VoxelStock::Centre`:
  - Z rays at (x_i, y_j).
  - X rays at (y_j, z_k).
  - Y rays at (x_i, z_k).
- Every lattice edge then lies along exactly one ray. Its crossing point is exact, which is what the mesh uses.
- **Storage:**
  - Each ray is `{ float enter, exit; int32 more; }`, measured from the box's lower corner along the ray. Most rays hold one interval. `more` indexes an overflow pool, for a ray cut into several pieces.
  - That's 12 bytes a ray, about 150 MB at the largest stop.
- **Members:**
  - `Reset(box, spacing)`, `FillAll()`.
  - `FillMesh(triangles)`: each ray's crossings with the triangles, sorted and paired, as `VoxelStock::FillMesh` counts crossings up each column.
  - `Volume()`: from the Z rays, s² times the intervals' lengths.
  - Dirty chunks of 32 × 32 × 32 lattice cells, with `MarkDirtyBox` and `ChunkCount` as in `VoxelStock`.
  - `ToVoxels(VoxelStock&)`: fills a voxel grid at spacing `s` from the Z rays' intervals, as whole spans of bits. It's for Compare.

**`DexelResolutionStops(box, maxRays = 4e6)`**
- Up to seven spacings, fine to coarse, following `ResolutionStops`' pattern (`core\Simulation.cpp:102`).
- The finest is where the largest grid has 4 million rays.
- The middle one is the default.

**Cutting: `CutDexels(stock, profile, from, to, axis)` and `CountDexels(..., stopAtFirst)`**
- These mirror `CutMove` and `CountMove` (`core\Simulation.cpp:600-840`). They take the same short steps and use the same `inside(along, distance)` test from `ToolProfile::RadiusAt`. They only look at rays crossing the step's box, grown by the reach.
- **A ray parallel to the tool's axis** (Z rays with the tool along ±Z, the usual 3-axis case) gets its interval in closed form. It's the column cut: the distance to the step's segment, then `HeightForRadius`, up to the tool length above the higher tip.
- **Any other ray:**
  - Every tool profile is convex (flat, ball, corner radius, V, chamfer, taper, drill), so its sweep along a straight step is convex. A ray meets it in at most one interval.
  - Golden section on how far the ray's points are outside the sweep finds a point inside.
  - Bisection then settles each end to 1e-4 mm.
- Each ray subtracts the interval from its list.
- Rays are cut on all cores (`std::execution::par` over the rays of each grid), because they're independent.
- Returns the length removed (for the rapid warning, any > 0) and marks the chunks touched.

**Mesh: `MeshDexelChunk` and `MeshDexelChunks`, as surface nets**
- A lattice node is inside where its Z ray covers it.
- In each lattice cell with mixed corners, one vertex goes at the average of the exact crossings on its 12 edges. Each crossing comes from the ray along that edge. If the ray disagrees with the corners, the edge's middle is used instead.
- One quad is made for each lattice edge with a crossing, joining its four cells' vertices. The quad faces from inside to outside.
- A cell's vertex depends only on its own edges, so chunks mesh on their own and still meet watertight. That matters for STL.
- Normals come from the quads, averaged per vertex.
- The result is the existing `StockMesh`, so `JoinMeshes`, `StockMeshToStl` and `DrawMesh` take it unchanged.
- Sharp corners are rounded within one spacing. That's the usual cost of surface nets.

### `core\Simulation`

- **`SimResolution`** gains `model` (`StockModel::Voxels`, the default, or `TriDexel`).
  - Rest machining's `ComputeRemainingStock` doesn't set it, so it stays on voxels (Frank's choice).
- **`Simulation`** keeps a `TriDexelStock m_dexels` and `m_startDexels` beside the voxels, plus `Model()`.
  - `Start` fills whichever stock the resolution asks for.
  - `Rewind` and `SeekTo` copy the start stock back.
  - `CutPiece` (`core\Simulation.cpp:1486`) calls `CutDexels`/`CountDexels` in tri-dexel mode for the cut, the rapid check, and the neck, shank and holder checks.
- **New `Simulation` members that pick the model**, so the add-on doesn't branch:
  - `ChunkCount()`, `DirtyChunks()`, `MeshChunks(chunks, meshes)`, `MeshWhole()`.
  - `CompareTo(part)`.
  - `StockVolume()`.

### Compare in tri-dexel mode

- The cut stock is turned into voxels at the dexel spacing (`ToVoxels`). The part is filled by `FillMesh` into `ResetLike` of that, as now.
- Then the existing `CompareStock` runs, keeping its one-cell tolerance, its volumes and its amber and red meshes.
- The differences stay blocky at the dexel spacing. Only the stock is smooth.

### Add-on

- **`addon\SimulationDialog`:**
  - A Stock model combo, `IDC_SIM_MODEL` (the next free 13xxx ID in `addon\resource.h`), with Voxels and Tri-dexel. It goes above Auto divide and Resolution, and the controls below move down.
  - `g_stockModel` remembers the choice as the other dialog settings do.
  - `ChosenResolution` returns the model. Tri-dexel greys Auto divide.
  - Changing the model resets the slider's range and stops, then calls `RestartSimulation`.
- **`addon\CamTabCommand.cpp`:**
  - `SimulationStops()` returns the dexel stops in tri-dexel mode.
  - `Simulate`, `RestartSimulation`, `UpdateSimulationScene` (lines ~1712-1940) and Save STL use the new `Simulation` members in place of `Stock()`.
  - `m_simulationPart` is built from the tri-dexel spacing for Compare.
  - The `[simulate]` log line gives the model, the spacing, the ray counts and the memory.
- **`addon\CAMForAlibre.rc`:** the combo in `IDD_SIMULATION`.

### Docs

- **Help:** the Simulation section of `help\CAMForAlibre.html` describes the Stock model field, and Resolution in each mode.
- **Spec:** `docs\SPEC.md` and the Claude doc.
- **`CLAUDE.md`:** a Current state line for this work.

## Tests: `tests\TriDexelTests.cpp`

- **Volumes:** a filled box's volume is exact, and a `FillMesh` sphere's is within 0.5%.
- **3-axis cuts:** a flat slot and a ball groove match the analytic volume within 0.1%. They're also checked against the voxel `CutMove` at a fine cell.
- **A tilted tool (3+2) slot:** the X, Y and Z rays agree at the lattice nodes.
- **Mesh:**
  - Watertight: every edge is shared by two triangles, both whole and joined from chunks.
  - Its enclosed volume is within 1% of `Volume()`.
- **Dirty chunks:** only the chunks near a cut are marked.
- **The simulation:** Rewind and SeekTo give the same volume, and a rapid into stock and the holder warnings fire as with voxels.
- **Timing:** a skipped test `tri-dexel timing*` runs the same facing case as `simulation timing*`, to compare it with voxels' 1.5 s.

## Verification

1. **Build:** Debug and Release, then run `CamCoreTests.exe`, which must pass.
2. **Timing:** run `-tc="tri-dexel timing*" --no-skip` and `-tc="simulation timing*" --no-skip`, and report both.
3. **Install** (Release), with Alibre closed.
4. **Frank's tests in Alibre:**
   1. Switch Stock model to Tri-dexel and back.
   2. Check that the slider's stops change and Auto divide greys.
   3. Simulate a Parallel Finishing job: the surface should look smooth, not stepped.
   4. Simulate a tilted 3+2 operation.
   5. Check Compare (Not enough and Too much removed, with volumes) in tri-dexel mode.
   6. Save STL and open it.
   7. Check that a rapid through stock and a holder hit still stop the simulation.
   8. Check that Generate with rest machining is unchanged.
   9. Check that the help's Simulation section has changed.
