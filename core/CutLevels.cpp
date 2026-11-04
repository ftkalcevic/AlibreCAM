// CutLevels.cpp : how deep an operation cuts, and in what steps.

#include "CutLevels.h"

#include <cmath>

namespace camcore
{
    std::vector<std::string> ValidateCutLevels(const CutLevelSettings& settings)
    {
        std::vector<std::string> problems;

        if (!(settings.totalDepth > 0.0))
            problems.push_back("The total cut depth must be more than 0.");
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
}
