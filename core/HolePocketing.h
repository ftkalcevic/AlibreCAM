// HolePocketing.h : the Hole Pocketing operation's own settings. No Alibre or MFC code belongs here.
//
// Hole pocketing cuts round holes with an end mill on a spiral. Distances are
// in millimetres and angles in degrees.

#pragma once

#include <string>
#include <vector>

#include "CutLevels.h"  // CutGeometryLocation
#include "Facing.h"     // CutDirection, ToolRelativeSize

namespace camcore
{
    // How far each level steps down.
    enum class HoleStepDown
    {
        Percent,        // of the tool's diameter
        Distance,
        Levels          // the depth in this many equal levels
    };

    // How steeply the helix that engages the cut descends.
    enum class HelixPitch
    {
        Angle,          // its slope
        Height          // its drop for each turn
    };

    // The Hole Pocketing tab. Every value is kept, so switching a choice back restores it.
    struct HolePocketingSettings
    {
        CutGeometryLocation location = CutGeometryLocation::AtTop;
        double topZ = 0.0;                              // Pick Top only

        double depth = 10.0;                            // down from the cut geometry
        bool useModelForDepth = false;
        double diameter = 10.0;
        bool useArcDiameter = true;                     // each picked circle's own diameter
        bool cleanUpEachLevel = false;

        CutDirection direction = CutDirection::Climb;   // Climb or Conventional
        ToolRelativeSize stepover{ true, 50.0, 2.0 };

        HoleStepDown stepDown = HoleStepDown::Percent;
        double stepDownPercent = 25.0;
        double stepDownDistance = 1.0;
        int stepDownLevels = 4;

        double helixDiameter = 3.0;
        HelixPitch pitch = HelixPitch::Angle;
        double pitchAngle = 3.0;
        double pitchHeight = 1.0;
        double approachDistance = 1.0;
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    // Only the values the chosen options use are checked.
    std::vector<std::string> ValidateHolePocketing(const HolePocketingSettings& settings);
}
