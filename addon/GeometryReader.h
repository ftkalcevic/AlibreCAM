// GeometryReader.h : reads picked faces and edges, and the assembly's size, into CamCore's terms.
//
// Alibre works in each file's model units, in that file's own coordinates.
// Everything returned here is in assembly coordinates and millimetres.

#pragma once

#include "Origin.h"

// What a pick may be.
enum class PickFilter
{
    FacesAndEdges,
    Edges
};

// Millimetres per model unit for a session, or 10 (centimetres) if it can't be read.
double MillimetresPerModelUnit(IADDesignSession* pDesign);

// Reads the first selected item. Returns false and sets problem, in words for the
// user, if nothing usable is selected.
bool ReadSelectedPick(IADSession* pAssembly, PickFilter filter, camcore::PickedGeometry& pick,
                      std::wstring& problem);

// The assembly's extents. Returns false if it has no parts or they can't be read.
bool ReadAssemblyBounds(IADSession* pAssembly, camcore::Vec3& lower, camcore::Vec3& upper);

// Sets Alibre's selection filter so only what the pick allows can be clicked.
// Returns the previous setting so it can be put back.
ADSelectionFilterOption SetSolidSelectionFilter(IADSession* pAssembly, ADSelectionFilterOption option);

void ClearSelection(IADSession* pAssembly);
