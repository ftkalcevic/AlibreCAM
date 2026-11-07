// WaterlineRoughing.h : the Waterline Roughing operation's own settings. No Alibre or MFC code belongs here.
//
// Distances are in millimetres and angles in degrees.

#pragma once

#include <string>
#include <vector>

#include "CutLevels.h"      // CutLevelOrder
#include "Facing.h"         // CutDirection
#include "Pocketing.h"      // PocketLinearStart

namespace camcore
{
    // How each level is cleared.
    enum class WaterlinePattern
    {
        PartOffset,     // loops round the parts, each a stepover further out
        StockOffset,    // loops in from the stock's (the area's) edge, each a stepover further in
        Linear          // straight rows along the angle of cuts
    };

    // Where the offset patterns start.
    enum class WaterlineStart
    {
        Inside,         // nearest the parts, working out
        Outside         // at the stock's edge, working in
    };

    enum class WaterlineStepoverMode
    {
        Percent,        // of the tool's diameter
        Distance,
        Scallop         // from the height of the ridge left between passes
    };

    enum class WaterlineStepdownMode
    {
        Percent,        // of the tool's diameter
        Distance,
        Levels          // the depth from top to bottom divided into this many
    };

    // The Waterline Roughing tab and its Cut Levels tab.
    struct WaterlineRoughingSettings
    {
        // The Waterline Roughing tab.
        double stockToLeave = 0.5;      // left covering the parts, on their tops and sides
        WaterlinePattern pattern = WaterlinePattern::StockOffset;
        CutDirection direction = CutDirection::Climb;
        WaterlineStart start = WaterlineStart::Outside;
        PocketLinearStart linearStart = PocketLinearStart::Bottom;
        double cutAngle = 0.0;          // Linear: 90 runs along the origin's X, 0 along its Y
        WaterlineStepoverMode stepoverMode = WaterlineStepoverMode::Percent;
        double stepoverPercent = 50.0;
        double stepoverDistance = 3.0;
        double scallop = 0.05;
        double extension = 0.0;         // how far past an open edge (no part beyond) the tool's centre goes

        // The Cut Levels tab.
        WaterlineStepdownMode stepdownMode = WaterlineStepdownMode::Distance;
        double stepdownPercent = 50.0;
        double stepDown = 2.0;          // the stepdown distance (its name before the Cut Levels tab)
        int levels = 5;
        CutLevelOrder order = CutLevelOrder::DepthFirst;
        bool useTop = false;            // force the top of the cut
        double top = 0.0;               // in the origin's frame
        bool useBottom = false;         // force the bottom of the cut
        double bottom = 0.0;
        bool clearFlats = true;         // a level at each flat face, as well as the stepdown's
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateWaterlineRoughing(const WaterlineRoughingSettings& settings);

    // The stepover in mm for this tool. Scallop works from the tool's corner
    // radius (a ball mill's radius); a flat end mill steps over 90% of its diameter.
    double WaterlineStepover(const WaterlineRoughingSettings& settings, double toolDiameter, double cornerRadius);

    // The deepest step between levels in mm, for a cut from top to bottom.
    double WaterlineStepdown(const WaterlineRoughingSettings& settings, double toolDiameter, double top,
                             double bottom);
}
