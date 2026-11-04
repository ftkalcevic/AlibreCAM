// EntryExit.h : how an operation's tool gets into and out of each cut. No Alibre or MFC code belongs here.
//
// Shared by the operations that cut in levels, such as Facing, but not Drill.
// Lengths are in millimetres and angles in degrees.

#pragma once

#include <string>
#include <vector>

namespace camcore
{
    enum class EntryMode
    {
        None,
        Entry2D,        // a 2D engage motion at the cut's level
        AlongPath3D     // ramping down along the cut's own path
    };

    enum class ExitMode
    {
        None,
        Exit2D          // a 2D retract motion at the cut's level
    };

    enum class LeadShape
    {
        Linear,         // a straight line of a length, at an angle to the cut
        Radial          // an arc of a radius
    };

    // A 2D engage or retract motion. Every value is kept, so switching the shape
    // back restores the old ones.
    struct LeadMotion
    {
        LeadShape shape = LeadShape::Linear;
        double length = 5.0;
        double angle = 45.0;
        double radius = 5.0;
    };

    // The Entry/Exit tab.
    struct EntryExitSettings
    {
        EntryMode entry = EntryMode::None;
        LeadMotion engage;
        double alongPathAngle = 2.0;
        double alongPathHeight = 2.0;

        ExitMode exit = ExitMode::None;
        LeadMotion retract;

        bool eachCutLevel = false;      // at every cut level, not just the first and last
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    // Only the values the chosen modes use are checked.
    std::vector<std::string> ValidateEntryExit(const EntryExitSettings& settings);
}
