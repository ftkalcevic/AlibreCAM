// Boundary.h : turns picked outlines into one closed polygon. No Alibre or MFC code belongs here.
//
// Points are in an operation's origin frame, in millimetres. Only X and Y shape
// the polygon; every point is put at the given level.

#pragma once

#include <vector>

#include "Origin.h"

namespace camcore
{
    using Polyline = std::vector<Vec3>;

    // Points closer than this are the same point.
    constexpr double kBoundaryJoinTolerance = 0.001;

    // Joins pieces, such as a face's loop edges or loose edges, into one closed
    // polygon. It starts with the first piece, then repeatedly adds whichever
    // remaining piece has an end nearest to where the chain has got to, turned
    // round if that end is its last. Gaps are bridged with straight lines, and the
    // chain is closed back to its start. Repeated points are dropped, and the
    // result runs anticlockwise seen from +Z, without the first point repeated
    // at the end. Fewer than 3 distinct points gives an empty polygon.
    Polyline ClosePolygon(const std::vector<Polyline>& pieces, double level);

    // The polygon's area in X and Y: positive anticlockwise, negative clockwise.
    double SignedArea(const Polyline& polygon);
}
