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
    - Pencil tracing
    - Valley remachining
    - Plateau machining
    - Parallel hill machining
    - Radial machining
    - Spiral machining
    - Curve machining
    - Between 2 curves
- More stock alignment options
- 4th axis: rotate and machine, etc.
- 🔴 Show some summary information under each op, eg tool
- 🔴 More details in tool
- New dialogs should inherit speed/feeds for the last (?)  copy/paste(?)
- Rest machining: clear only what a bigger tool left, from the simulated stock or a previous tool's diameter
- More posts (GRBL, Mach3/4, UCCNC, Masso)
- Undo in the CAM tree
- Inch display units
- Clamps and vices as keep-out for toolpaths and the simulation
- Tool holder from a model: pick an Alibre part (or file) as the holder's shape, used in the simulation in place of the plain cylinder
- Pictures on the dialog tabs showing what each parameter means, where it helps
- Performance
  - Spline faces are now read as NURBS (`GetBSplineCurvesData`) and worked out
    by CamCore. A face whose data can't be read that way (several surfaces in
    one face, or no reading fitting its facets) still asks Alibre a point at a
    time; if that turns up, read those too.

Priority: 🔴 high, 🟡 medium, ⚪ low. Items without a mark aren't prioritised yet.