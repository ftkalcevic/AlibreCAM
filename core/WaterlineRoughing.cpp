// WaterlineRoughing.cpp : the Waterline Roughing operation's own settings.

#include "WaterlineRoughing.h"

#include <cmath>

namespace camcore
{
    std::vector<std::string> ValidateWaterlineRoughing(const WaterlineRoughingSettings& settings)
    {
        std::vector<std::string> problems;
        if (!(settings.stockToLeave >= 0.0) || !std::isfinite(settings.stockToLeave))
            problems.push_back("The stock to leave must be 0 or more.");
        if (!(settings.stepDown > 0.0) || !std::isfinite(settings.stepDown))
            problems.push_back("The cut depth step must be more than 0.");
        return problems;
    }
}
