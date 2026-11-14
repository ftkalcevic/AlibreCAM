# CAMForAlibre to-do list

Work still to do, beyond the build order in `CLAUDE.md`. Decisions about any of
these belong in the spec (`docs\SPEC.md`) once they're made.

- Operations
  - Thread milling
  - 🔴 V-carve
  - 4th axis rotate(?), or do we just set the origin
  - 3D
    - Project finishing
    - 3D offset pocketing and profiling
    - Pencil tracing (idea saved in `docs\PencilMachining.md`)
    - Valley remachining
    - Plateau machining
    - Parallel hill machining
    - Radial machining
    - Spiral machining
    - Curve machining
    - Between 2 curves
- More stock alignment options
- 4th axis: rotate and machine, etc.
- Lathe support: machine type, rod stock, turning operations, tools, post and simulation (staged plan saved in `docs\LatheSupport.md`)
- Tri-dexel simulation mode: smooth, exact stock as an option beside voxels (plan saved in `docs\TriDexelSimulation.md`)
- 🔴 Show some summary information under each op, eg tool
- New dialogs should inherit speed/feeds for the last (?)  copy/paste(?)
- Rest machining: built for Waterline Roughing and Pocketing. Still to do: Facing, and pencil passes for finishing
- More posts (GRBL, Mach3/4, UCCNC, Masso)
- Undo in the CAM tree
- Inch display units
- Clamps and vices as keep-out for toolpaths and the simulation (design saved in `docs\ClampsAndKeepOuts.md`, its decisions still open)
- Tool holder from a model: pick an Alibre part (or file) as the holder's shape, used in the simulation in place of the plain cylinder
- Pictures on the dialog tabs showing what each parameter means, where it helps
- Performance
  - Spline faces are now read as NURBS (`GetBSplineCurvesData`) and worked out
    by CamCore. A face whose data can't be read that way (several surfaces in
    one face, or no reading fitting its facets) still asks Alibre a point at a
    time; if that turns up, read those too.

Priority: 🔴 high, 🟡 medium, ⚪ low. Items without a mark aren't prioritised yet.