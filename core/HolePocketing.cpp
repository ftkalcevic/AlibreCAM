// HolePocketing.cpp : the Hole Pocketing operation's own settings.

#include "HolePocketing.h"

#include <cmath>

namespace camcore
{
    std::vector<std::string> ValidateHolePocketing(const HolePocketingSettings& settings)
    {
        std::vector<std::string> problems;

        if (settings.location == CutGeometryLocation::PickTop && !std::isfinite(settings.topZ))
            problems.push_back("The top Z must be a number.");
        if (!(settings.depth > 0.0))
            problems.push_back("The hole depth must be more than 0.");
        if (!settings.useArcDiameter && !(settings.diameter > 0.0))
            problems.push_back("The hole diameter must be more than 0.");
        if (settings.direction == CutDirection::Mixed)
            problems.push_back("Hole pocketing cuts Climb or Conventional.");

        const ToolRelativeSize& step = settings.stepover;
        if (step.usePercent ? !(step.percent > 0.0 && step.percent <= 100.0) : !(step.distance > 0.0))
        {
            problems.push_back(step.usePercent ? "The stepover must be more than 0% and at most 100% of the tool."
                                               : "The stepover must be more than 0.");
        }

        switch (settings.stepDown)
        {
        case HoleStepDown::Percent:
            if (!(settings.stepDownPercent > 0.0))
                problems.push_back("The step down must be more than 0% of the tool.");
            break;
        case HoleStepDown::Distance:
            if (!(settings.stepDownDistance > 0.0))
                problems.push_back("The step down must be more than 0.");
            break;
        case HoleStepDown::Levels:
            if (settings.stepDownLevels < 1)
                problems.push_back("The number of levels must be at least 1.");
            break;
        }

        if (!(settings.helixDiameter > 0.0))
            problems.push_back("The helix diameter must be more than 0.");
        if (settings.pitch == HelixPitch::Angle && !(settings.pitchAngle > 0.0 && settings.pitchAngle < 90.0))
            problems.push_back("The helix angle must be more than 0 and less than 90 degrees.");
        if (settings.pitch == HelixPitch::Height && !(settings.pitchHeight > 0.0))
            problems.push_back("The helix height must be more than 0.");
        if (!(settings.approachDistance >= 0.0))
            problems.push_back("The approach distance can't be negative.");

        return problems;
    }
}
