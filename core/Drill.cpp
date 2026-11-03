// Drill.cpp : the Drill operation's own settings.

#include "Drill.h"

namespace camcore
{
    std::vector<std::string> ValidateDrillSettings(const DrillSettings& settings)
    {
        std::vector<std::string> problems;

        if (!(settings.depth > 0.0))
            problems.push_back("The depth must be more than 0.");
        if (settings.dwell == DwellMode::Seconds && !(settings.dwellSeconds > 0.0))
            problems.push_back("The dwell time must be more than 0.");
        if (settings.dwell == DwellMode::Revolutions && !(settings.dwellRevolutions > 0.0))
            problems.push_back("The dwell revolutions must be more than 0.");
        if (!(settings.approachDistance >= 0.0))
            problems.push_back("The approach distance can't be negative.");
        if (settings.pecking && !(settings.peckDepth > 0.0))
            problems.push_back("The peck depth must be more than 0, or left blank.");

        return problems;
    }
}
