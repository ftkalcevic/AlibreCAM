# Pencil machining: saved idea (not planned in detail)

A new "Pencil Finishing" operation under Add 3 Axis: a small ball follows the inside creases where two surfaces meet.

1. **Height map:** `TipHeights` and `DropCutter`, with the tool's true shape.
2. **Find creases:** cells where the tip surface's slope changes abruptly across a valley (the contact jumps between faces), sharper than a Crease Angle.
3. **Lines:** thin to one cell wide, chain into paths, split at junctions, drop scraps.
4. **Exact:** re-drop each point with `DropCutter`, and `ExactHeights` for a ball; then simplify and fit arcs.
5. **Order:** nearest path next, staying down on short clear links, cutting uphill where possible.

**Settings:** the usual Features, Tool (ball or corner radius only), Feeds & Speeds, Clearance; Stock to Leave, Overtravel, Approach Distance, Slope limits; Crease Angle, Minimum Length; Passes Each Side and Stepover (offset passes cover the "Valley remachining" TODO item too).

**What defines "needs cleaning":** geometry only first (every crease sharp enough); then optionally a reference tool's diameter (cut only where that ball couldn't reach, with the band from the difference of the two tools' height maps). Not the simulated stock: finishing leftovers are often thinner than a cell.

**Hard part:** clean lines where creases fade out or meet at junctions; `CountLapGlitches` and the replay tests would help.
