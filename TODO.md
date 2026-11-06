# AlibreCAM to-do list

Work still to do, beyond the build order in `CLAUDE.md`. Decisions about any of
these belong in the spec (`docs\SPEC.md`) once they're made.

- Operations
  - Thread milling
  - 🔴 Chamfer
  - 🔴 Engrave
  - 🔴 V-carve
  - 4th axis rotate(?), or do we just set the origin
  - 3D
    - Horizontal roughing
    - Plunge roughing
    - Parallel finishing
    - Horizontal finishing
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
- 🔴 Implement stock properly
- 🔴 Implement machine settings
- Add simulate
- More stock alignment options
- 4th axis: rotate and machine, etc.
- 🔴 Help
- 🔴 Show some summary information under each op, eg tool
- 🔴 More details in tool
- 🔴 Add material page
- 🔴 Add default settings for tool + material.
    - 🔴 spindle speed will vary from machine to machine - do tools belong with the machine?
- 🔴 Allow selection of the default speeds/feeds from tool
- New dialogs should inherit speed/feeds for the last (?)  copy/paste(?)
- 🔴 Bulk edit - select multiple operations and edit common features - tool, speeds/feeds, etc

Priority: 🔴 high, 🟡 medium, ⚪ low. Items without a mark aren't prioritised yet.