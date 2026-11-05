// PocketToolpath.h : calculates a Pocketing operation's toolpath. No Alibre or MFC code belongs here.
//
// It shares the profile's offsetting and geometry, so it's written in
// ProfileToolpath.cpp. Points are in the operation's origin frame, in millimetres.

#pragma once

#include "ProfileToolpath.h"

namespace camcore
{
    // Everything a pocketing toolpath is worked out from.
    struct PocketToolpathInput
    {
        const SequenceStep* operation = nullptr;
        const Tool* tool = nullptr;     // in millimetres, from ToolInMillimetres
        ProfileChain outline;           // from ChainProfile, closed, in the operation's frame

        // The parts' highest point along the frame's Z, for the clearance plane.
        bool havePartMaxZ = false;
        double partMaxZ = 0.0;
    };

    // The pocket is the area inside the closed outline, and the tool centre
    // stays its radius plus the stock to leave inside the outline (the wall).
    // Islands aren't left yet.
    // Offset Cuts: loops following the wall, each a stepover further in until
    // nothing fits, from the middle out (Inside) or the wall in (Outside).
    // Linear Cuts: straight rows along the angle, a stepover apart, from the
    // frame's -Y side (Start at Bottom) or +Y side (Start at Top), then a lap
    // round the wall.
    // Spiral Cuts: a spiral blending each loop into the next, from the middle
    // out or the wall in, then a lap round the wall.
    // Climb runs anticlockwise round loops (clockwise spindle), Conventional
    // clockwise. Mixed alternates loops for Offset Cuts and zigzags Linear rows,
    // whose laps climb. One-way rows lift between them. Elsewhere the tool stays
    // down from one loop or row to the next when the move is short and stays
    // inside the pocket, and otherwise lifts and enters again. Every cut level
    // repeats the pattern, top down. Radial Cuts aren't built yet.
    ToolpathResult GeneratePocketToolpath(const PocketToolpathInput& input);
}
