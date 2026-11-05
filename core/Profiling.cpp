// Profiling.cpp : the Profiling operation's own settings.

#include "Profiling.h"

#include <cmath>

namespace camcore
{
    std::vector<std::string> ValidateProfiling(const ProfilingSettings& settings)
    {
        std::vector<std::string> problems;

        if (!std::isfinite(settings.stockToLeave))
            problems.push_back("The stock to leave must be a number.");

        if (!(settings.cutWidth >= 0.0))
            problems.push_back("The total cut width can't be negative.");
        else if (settings.cutWidth > 0.0 &&
                 (!(settings.stepPerCut > 0.0) || settings.stepPerCut > settings.cutWidth + kStepWidthTolerance))
            problems.push_back("The step/cut must be more than 0, and no more than the total cut width.");

        if (settings.useTabs)
        {
            if (!(settings.tabHeight > 0.0))
                problems.push_back("The tab height must be more than 0.");
            if (!(settings.tabLength > 0.0))
                problems.push_back("The tab length must be more than 0.");
            if (settings.tabSpacing == TabSpacing::Count && settings.tabCount < 1)
                problems.push_back("The number of tabs must be at least 1.");
            if (settings.tabSpacing == TabSpacing::Distance && !(settings.tabDistance > 0.0))
                problems.push_back("The distance between tabs must be more than 0.");
        }

        return problems;
    }
}
