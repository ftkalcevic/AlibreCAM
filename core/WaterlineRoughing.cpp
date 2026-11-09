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
            problems.push_back("The linear extension or overtravel must be 0 or more.");
        if (!(settings.slopeFrom >= 0.0 && settings.slopeTo <= 90.0 && settings.slopeFrom < settings.slopeTo))
            problems.push_back("The slope's From must be less than its To, both 0 to 90 degrees.");
        if (settings.restMachining && (!(settings.restMinimum >= 0.0) || !std::isfinite(settings.restMinimum)))
            problems.push_back("The minimum material must be 0 or more.");
        if (!(settings.approachDistance >= 0.0) || !std::isfinite(settings.approachDistance))
            problems.push_back("The approach distance must be 0 or more.");
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
        case WaterlineStepdownMode::Scallop:
            if (!positive(settings.stepdownScallop))
                problems.push_back("The stepdown's scallop height must be more than 0.");
            break;
        }
        if ((settings.useTop && !std::isfinite(settings.top)) || (settings.useBottom && !std::isfinite(settings.bottom)))
            problems.push_back("The top and bottom must be numbers.");
        if (settings.useTop && settings.useBottom && !(settings.top > settings.bottom))
            problems.push_back("The top must be above the bottom.");
        if (settings.adaptive)
        {
            for (const std::string& problem : ValidateRampEntry(settings.rampEntry))
                problems.push_back(problem);
        }
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
                             double bottom, double ballRadius)
    {
        switch (settings.stepdownMode)
        {
        case WaterlineStepdownMode::Scallop:
        {
            // Two levels dz apart leave a ridge h high on a steep wall when dz = 2 x sqrt(h(2r - h)).
            if (!(ballRadius > 0.0))
                return 0.0;
            const double h = (std::min)(settings.stepdownScallop, ballRadius);
            return 2.0 * std::sqrt(h * (2.0 * ballRadius - h));
        }
        case WaterlineStepdownMode::Percent:
            return toolDiameter * settings.stepdownPercent / 100.0;
        case WaterlineStepdownMode::Levels:
            return (top - bottom) / (std::max)(1, settings.levels);
        default:
            return settings.stepDown;
        }
    }

    WaterlineRoughingSettings WaterlineFinishingDefaults()
    {
        WaterlineRoughingSettings settings;
        settings.stockToLeave = 0.0;
        settings.stepoverPercent = 10.0;
        settings.stepoverDistance = 0.5;
        settings.stepDown = 0.5;
        return settings;
    }

    WaterlineRoughingSettings ParallelFinishingDefaults()
    {
        WaterlineRoughingSettings settings;
        settings.stockToLeave = 0.0;
        settings.direction = CutDirection::Mixed;
        settings.stepoverPercent = 10.0;
        settings.stepoverDistance = 0.5;
        settings.stepdownMode = WaterlineStepdownMode::Levels;
        settings.levels = 1;
        settings.stepDown = 0.5;
        settings.clearFlats = false;
        return settings;
    }
}
