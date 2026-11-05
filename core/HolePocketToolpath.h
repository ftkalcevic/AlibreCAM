// HolePocketToolpath.h : calculates a Hole Pocketing operation's toolpath. No Alibre or MFC code belongs here.
//
// Points are in the operation's origin frame, in millimetres.

#pragma once

#include "CamJob.h"
#include "ToolLibrary.h"
#include "Toolpath.h"

namespace camcore
{
    // Everything a hole pocketing toolpath is worked out from.
    struct HolePocketToolpathInput
    {
        const SequenceStep* operation = nullptr;
        Frame frame;                    // the origin the operation runs under
        const Tool* tool = nullptr;     // in millimetres, from ToolInMillimetres

        // The parts' highest point along the frame's Z, for the clearance plane.
        bool havePartMaxZ = false;
        double partMaxZ = 0.0;
    };

    // Each hole, in the Features list's order, is centred on its circle, hole
    // face or point, and is that circle's or face's diameter with Use Arc Dia as
    // Hole Dia, or the Hole Diameter otherwise. Its top is the feature's height
    // (At Top), the depth above it (At Bottom) or the Pick Top Z, and it's cut
    // the depth down in equal step-down levels, none deeper than the step down.
    // The tool comes across at the clearance plane to above the helix, down at
    // rapid to the approach distance above the top, then helixes down at the
    // engage feed at the Helix Dia (no more than the hole allows) and pitch to the
    // first level. At each level it laps the helix circle flat, spirals out to the
    // wall a stepover a turn, laps the wall, and laps it again with Clean up Pass,
    // all at the cut feed, before helixing down to the next level. Climb runs
    // anticlockwise (clockwise spindle) and Conventional clockwise. At the bottom
    // it moves in to the centre and up to the clearance plane. Every move round
    // is an arc; helixes descend as they go round.
    ToolpathResult GenerateHolePocketToolpath(const HolePocketToolpathInput& input);
}
