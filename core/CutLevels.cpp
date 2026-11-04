// CutLevels.cpp : how deep an operation cuts, and in what steps.

#include "CutLevels.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    std::vector<std::string> ValidateCutLevels(const CutLevelSettings& settings)
    {
        std::vector<std::string> problems;

        if (!(settings.totalDepth >= 0.0))
            problems.push_back("The total cut depth can't be negative.");
        if (!(settings.roughDepth >= 0.0) || !(settings.finishDepth >= 0.0))
            problems.push_back("The rough and finish depths can't be negative.");
        else if (std::fabs(settings.roughDepth + settings.finishDepth - settings.totalDepth) > kCutDepthTolerance)
            problems.push_back("The rough and finish depths must add up to the total cut depth.");

        // A step is only needed for a depth that's cut.
        if (settings.roughDepth > 0.0 &&
            (!(settings.roughStep > 0.0) || settings.roughStep > settings.roughDepth + kCutDepthTolerance))
            problems.push_back("The rough depth per cut must be more than 0, and no more than the rough depth.");
        if (settings.finishDepth > 0.0 &&
            (!(settings.finishStep > 0.0) || settings.finishStep > settings.finishDepth + kCutDepthTolerance))
            problems.push_back("The finish depth per cut must be more than 0, and no more than the finish depth.");

        return problems;
    }

    double CutTopZ(const CutLevelSettings& settings, double geometryZ)
    {
        switch (settings.location)
        {
        case CutGeometryLocation::AtBottom: return geometryZ + settings.totalDepth;
        case CutGeometryLocation::PickTop:  return settings.topZ;
        case CutGeometryLocation::AtTop:
        default:                            return geometryZ;
        }
    }

    namespace
    {
        // Steps down through depth from start, each the same and no more than step.
        void AddLevels(std::vector<CutLevel>& levels, double start, double depth, double step, bool finish)
        {
            if (!(depth > kCutDepthTolerance))
                return;
            const double most = (step > 0.0) ? (std::min)(step, depth) : depth;
            const int count = (std::max)(1, static_cast<int>(std::ceil(depth / most - 1e-9)));
            for (int i = 1; i <= count; ++i)
            {
                CutLevel level;
                level.z = start - depth * i / count;
                level.finish = finish;
                levels.push_back(level);
            }
        }
    }

    std::vector<CutLevel> CutLevelHeights(const CutLevelSettings& settings, double geometryZ)
    {
        const double top = CutTopZ(settings, geometryZ);
        std::vector<CutLevel> levels;
        AddLevels(levels, top, settings.roughDepth, settings.roughStep, false);
        AddLevels(levels, top - (std::max)(0.0, settings.roughDepth), settings.finishDepth, settings.finishStep, true);

        // Nothing to take off still means one pass at the top.
        if (levels.empty())
        {
            CutLevel level;
            level.z = top;
            level.finish = true;
            levels.push_back(level);
        }
        return levels;
    }
}
