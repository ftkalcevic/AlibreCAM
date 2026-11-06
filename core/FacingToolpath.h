// FacingToolpath.h : calculates a Facing operation's toolpath. No Alibre or MFC code belongs here.
//
// Each of the operation's areas is cut at each of the Cut Levels tab's levels,
// from the boundaries' Z as the cut geometry, in turn: each to its full depth
// (Depth First) or each level in every area first (Level First), with the
// Entry/Exit tab's motions into and out of each cut.
// A cut is a level of Mixed or Spiral, or each one-way Linear pass. A positive
// lead angle or radius puts the lead away from the geometry: an entry from outside
// the first pass, an exit out past the last pass (outside the loop for a spiral).
// A negative one puts it on the other side. A linear lead's angle is from the
// cut's line (0 in line, 90 square to it), and a radial lead is a quarter circle
// arc tangent to the cut. Arcs in the boundary are followed exactly. With arc
// step-overs, Mixed steps between passes on a half circle bulging out past their ends.
// Along 3D Path ramps down in line with the cut from the path height above it.
// Leads and ramps run at the cut feed. Without Apply at Each Cut Level, only the
// first level's cuts have entries and only the last level's have exits.

#pragma once

#include "CamJob.h"
#include "Toolpath.h"
#include "ToolLibrary.h"

namespace camcore
{
    // Everything a facing toolpath is worked out from. The boundary is the
    // operation's own, from Generate, in its origin's frame.
    struct FacingToolpathInput
    {
        const SequenceStep* operation = nullptr;
        const Tool* tool = nullptr;     // in millimetres, from ToolInMillimetres

        // The parts' highest point along the frame's Z, for the clearance plane.
        bool havePartMaxZ = false;
        double partMaxZ = 0.0;

        // The stock's highest point along the frame's Z, when there's a stock.
        bool haveStockMaxZ = false;
        double stockMaxZ = 0.0;
    };

    // The direction passes run in the frame's X and Y. 90 degrees is along X,
    // and 0 along Y.
    Vec3 PassDirection(double cutAngleDegrees);

    // Linear: parallel passes along the cut angle, a step distance apart, over the
    // boundary plus the overtravel. Climb and Conventional cut every pass the same
    // way, lifting to the clearance plane between them. Mixed cuts forwards and
    // backwards, stepping over at the cut feed square to the passes, at the end of
    // whichever pass reaches further out. Climb keeps the uncut material on
    // the cutter's right, for a clockwise spindle.
    // Spiral: a rectangular spiral round the boundary's rectangle plus the
    // overtravel, from the outside in, each loop a step distance inside the last.
    // Climb or Mixed turns clockwise, and Conventional anticlockwise.
    // Spiral and Mixed start at the Cut Start corner, seen from the frame's +Z.
    // One-way Linear passes start on that corner's side, and the direction picks
    // which end. All arrive and leave at the clearance plane.
    ToolpathResult GenerateFacingToolpath(const FacingToolpathInput& input);
}
