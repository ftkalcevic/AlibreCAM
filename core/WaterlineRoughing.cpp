// WaterlineRoughing.cpp : the Waterline Roughing operation's own settings.

#include "WaterlineRoughing.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    std::vector<std::string> ValidateWaterlineRoughing(const WaterlineRoughingSettings& settings)
    {
        std::vector<std::string> problems;
        const auto positive = [](double value) { return value > 0.0 && std::isfinite(value); };
        if (!(settings.stockToLeave >= 0.0) || !std::isfinite(settings.stockToLeave))
            problems.push_back("The stock to leave must be 0 or more.");
        if (!(settings.extension >= 0.0) || !std::isfinite(settings.extension))
            problems.push_back("The linear extension must be 0 or more.");
        if (settings.pattern == WaterlinePattern::Linear && !std::isfinite(settings.cutAngle))
            problems.push_back("The angle of cuts must be a number.");

        // Only the value in use is checked.
        switch (settings.stepoverMode)
        {
        case WaterlineStepoverMode::Percent:
            if (!positive(settings.stepoverPercent) || settings.stepoverPercent > 100.0)
                problems.push_back("The stepover must be more than 0% and at most 100% of the tool.");
            break;
        case WaterlineStepoverMode::Distance:
            if (!positive(settings.stepoverDistance))
                problems.push_back("The stepover must be more than 0.");
            break;
        case WaterlineStepoverMode::Scallop:
            if (!positive(settings.scallop))
                problems.push_back("The scallop height must be more than 0.");
            break;
        }
        switch (settings.stepdownMode)
        {
        case WaterlineStepdownMode::Percent:
            if (!positive(settings.stepdownPercent))
                problems.push_back("The stepdown must be more than 0% of the tool.");
            break;
        case WaterlineStepdownMode::Distance:
            if (!positive(settings.stepDown))
                problems.push_back("The stepdown must be more than 0.");
            break;
        case WaterlineStepdownMode::Levels:
            if (settings.levels < 1)
                problems.push_back("There must be at least 1 level.");
            break;
        }
        if ((settings.useTop && !std::isfinite(settings.top)) || (settings.useBottom && !std::isfinite(settings.bottom)))
            problems.push_back("The top and bottom must be numbers.");
        if (settings.useTop && settings.useBottom && !(settings.top > settings.bottom))
            problems.push_back("The top must be above the bottom.");
        return problems;
    }

    double WaterlineStepover(const WaterlineRoughingSettings& settings, double toolDiameter, double cornerRadius)
    {
        switch (settings.stepoverMode)
        {
        case WaterlineStepoverMode::Distance:
            return settings.stepoverDistance;
        case WaterlineStepoverMode::Scallop:
        {
            // The ridge between two passes of a round corner of radius r is h high
            // when they're 2 x sqrt(h(2r - h)) apart.
            const double r = (std::min)(cornerRadius, toolDiameter / 2.0);
            if (!(r > 0.0))
                return 0.9 * toolDiameter;
            const double h = (std::min)(settings.scallop, r);
            return (std::min)(0.9 * toolDiameter, 2.0 * std::sqrt(h * (2.0 * r - h)));
        }
        default:
            return toolDiameter * settings.stepoverPercent / 100.0;
        }
    }

    double WaterlineStepdown(const WaterlineRoughingSettings& settings, double toolDiameter, double top,
                             double bottom)
    {
        switch (settings.stepdownMode)
        {
        case WaterlineStepdownMode::Percent:
            return toolDiameter * settings.stepdownPercent / 100.0;
        case WaterlineStepdownMode::Levels:
            return (top - bottom) / (std::max)(1, settings.levels);
        default:
            return settings.stepDown;
        }
    }
}
