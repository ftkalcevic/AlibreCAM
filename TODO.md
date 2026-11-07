# AlibreCAM to-do list

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
- Performance
  - Parallel Finishing and Waterline Finishing on spline faces (Waterline
    Finishing took 6 minutes on `Rough 3`): read each face's NURBS with
    `IADFace::GetBSplineCurvesData` and find closest points in CamCore on all
    cores, as spheres, cylinders, cones and tori already are, instead of asking
    Alibre (`GetClosestPoint` takes about 0.8 ms a point on a spline: 46 s for
    `Rough 3`). Check it against the face's facets and fall back to Alibre if
    they disagree. (Or, more simply, refine every fourth point instead of every
    second: about twice as fast, a little less exact on tight curves.)

Priority: 🔴 high, 🟡 medium, ⚪ low. Items without a mark aren't prioritised yet.