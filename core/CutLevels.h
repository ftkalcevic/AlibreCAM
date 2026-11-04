// CutLevels.h : how deep an operation cuts, and in what steps. No Alibre or MFC code belongs here.
//
// Shared by the operations that cut in levels, such as Facing, but not Drill.
// Depths are in millimetres along the operation's origin Z.

#pragma once

#include <string>
#include <vector>

namespace camcore
{
    // Where the cut geometry sits, which the depth is measured from.
    enum class CutGeometryLocation
    {
        AtTop,
        AtBottom,
        PickTop         // a Z the user gives, or picks in the view
    };

    enum class CutLevelOrder
    {
        DepthFirst,     // each area down to depth before the next
        LevelFirst      // every area at one level before the next level
    };

    // The Cut Levels tab.
    struct CutLevelSettings
    {
        CutGeometryLocation location = CutGeometryLocation::AtTop;
        double topZ = 0.0;              // Pick Top only

        // Rough and finish add up to the total.
        double totalDepth = 1.0;
        double roughDepth = 0.8;
        double finishDepth = 0.2;

        // The most each pass takes, at most its depth.
        double roughStep = 0.4;
        double finishStep = 0.2;

        bool clearIslandTops = false;
        bool useModelForDepth = false;

        CutLevelOrder order = CutLevelOrder::DepthFirst;
    };

    // Rough and finish closer than this to the total count as adding up to it.
    constexpr double kCutDepthTolerance = 0.0005;

    // Returns a sentence for each problem. Empty means the settings are valid.
    // A total of 0 is allowed, and means one pass at the top of the cut.
    std::vector<std::string> ValidateCutLevels(const CutLevelSettings& settings);

    // One level an operation cuts at, along its origin Z.
    struct CutLevel
    {
        double z = 0.0;
        bool finish = false;    // a finish level, which will have its own feeds and speeds
    };

    // Where the cut starts, given the cut geometry's Z. At Top starts at the
    // geometry, At Bottom the total depth above it, and Pick Top at its Z.
    double CutTopZ(const CutLevelSettings& settings, double geometryZ);

    // The levels to cut at, highest first: the rough depth in equal steps no
    // more than the rough depth per cut, then the finish depth the same way.
    // A total of 0 gives one finish level at the top.
    std::vector<CutLevel> CutLevelHeights(const CutLevelSettings& settings, double geometryZ);
}
