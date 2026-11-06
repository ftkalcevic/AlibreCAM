// Profiling.h : the Profiling operation's own settings. No Alibre or MFC code belongs here.
//
// Distances are in millimetres.

#pragma once

#include <string>
#include <vector>

#include "Facing.h"     // CutDirection

namespace camcore
{
    // Which side of the profile the tool cuts on, along the profile's direction.
    enum class ProfileSide
    {
        Right,
        Left
    };

    // For closed profiles, which side is cut in place of Right or Left.
    enum class ClosedProfileSide
    {
        Outside,
        Inside
    };

    // How tabs are spread round a profile.
    enum class TabSpacing
    {
        Count,          // this many tabs
        Distance        // a tab every this far
    };

    // The Profiling tab.
    struct ProfilingSettings
    {
        double stockToLeave = 0.0;                  // left beside the profile. Negative cuts into it.
        CutDirection direction = CutDirection::Mixed;
        ProfileSide startSide = ProfileSide::Right;
        bool useClosedSide = false;                 // closed curves use Outside or Inside
        ClosedProfileSide closedSide = ClosedProfileSide::Outside;
        bool useModelForSide = false;               // determine the side using the 3D model

        // The whole width cut away from the profile, in steps of at most stepPerCut.
        // A width of 0 is one pass along the profile.
        double cutWidth = 0.0;
        double stepPerCut = 0.0;

        bool useTabs = false;
        double tabHeight = 1.0;
        double tabLength = 5.0;
        TabSpacing tabSpacing = TabSpacing::Count;
        int tabCount = 4;
        double tabDistance = 50.0;
    };

    // Step/Cut closer than this to the width counts as no more than it.
    constexpr double kStepWidthTolerance = 0.0005;

    // Returns a sentence for each problem. Empty means the settings are valid.
    // Only the values the chosen options use are checked.
    std::vector<std::string> ValidateProfiling(const ProfilingSettings& settings);

    // The Chamfer operation's Chamfering tab has the Profiling tab's settings,
    // plus these. Distances are in millimetres.
    struct ChamferSettings
    {
        double width = 1.0;             // the chamfer's width
        double tipClearance = 0.5;      // how far the tool's tip goes below the chamfer
        double maxDepthPerCut = 1.0;    // the most each pass cuts down
    };

    std::vector<std::string> ValidateChamfer(const ChamferSettings& settings);
}
