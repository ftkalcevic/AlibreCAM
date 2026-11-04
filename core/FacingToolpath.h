// FacingToolpath.h : calculates a Facing operation's toolpath. No Alibre or MFC code belongs here.
//
// One level for now, at the boundary's Z. Cut levels and entry and exit motions
// come later.

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
