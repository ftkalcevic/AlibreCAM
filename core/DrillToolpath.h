// DrillToolpath.h : calculates a Drill operation's toolpath. No Alibre or MFC code belongs here.

#pragma once

#include <string>

#include "CamJob.h"
#include "Toolpath.h"
#include "ToolLibrary.h"

namespace camcore
{
    // Everything a drill toolpath is worked out from.
    struct DrillToolpathInput
    {
        const SequenceStep* operation = nullptr;
        Frame frame;                    // the origin the operation runs under
        const Tool* tool = nullptr;     // in millimetres, from ToolInMillimetres

        // The parts' highest point along the frame's Z, for the clearance plane.
        bool havePartMaxZ = false;
        double partMaxZ = 0.0;

        // The stock's highest point along the frame's Z, when there's a stock.
        bool haveStockMaxZ = false;
        double stockMaxZ = 0.0;
    };

    // After a peck the tool comes back down at rapid to this far above where it
    // stopped, then feeds again.
    constexpr double kPeckReentryGap = 0.5;

    // The length of a drill's point: how much further the tip goes than the full diameter.
    double DrillPointLength(const Tool& tool);

    // Each hole, in the operation's order: across at the clearance plane (rapid, or
    // the transfer feed), down at rapid to the approach distance above the hole's
    // top, drilling at the plunge feed (pecking if set, with a full retract to the
    // approach height between pecks), the dwell, back to the approach height at the
    // retract feed, then up to the clearance plane at rapid.
    ToolpathResult GenerateDrillToolpath(const DrillToolpathInput& input);
}
