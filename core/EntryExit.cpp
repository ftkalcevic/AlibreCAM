// EntryExit.cpp : how an operation's tool gets into and out of each cut.

#include "EntryExit.h"

#include <cmath>

namespace camcore
{
    namespace
    {
        void CheckLead(const LeadMotion& lead, const std::string& name, std::vector<std::string>& problems)
        {
            if (lead.shape == LeadShape::Linear)
            {
                if (!(lead.length > 0.0))
                    problems.push_back("The " + name + " length must be more than 0.");
                if (!(lead.angle >= -180.0) || !(lead.angle <= 180.0))
                    problems.push_back("The " + name + " angle must be from -180 to 180 degrees.");
            }
            else if (lead.radius == 0.0 || !std::isfinite(lead.radius))
            {
                problems.push_back("The " + name + " radius can't be 0.");
            }
        }
    }

    std::vector<std::string> ValidateEntryExit(const EntryExitSettings& settings)
    {
        std::vector<std::string> problems;

        if (settings.entry == EntryMode::Entry2D)
        {
            CheckLead(settings.engage, "engage", problems);
        }
        else if (settings.entry == EntryMode::AlongPath3D || settings.entry == EntryMode::Helix)
        {
            if (!(settings.alongPathAngle > 0.0) || !(settings.alongPathAngle < 90.0))
                problems.push_back("The along path angle must be more than 0 and less than 90 degrees.");
            if (!(settings.alongPathHeight > 0.0))
                problems.push_back("The along path height must be more than 0.");
            if (settings.entry == EntryMode::Helix && !(settings.helixPercent > 0.0 && settings.helixPercent <= 200.0))
                problems.push_back("The helix diameter must be more than 0 and at most 200% of the tool's diameter.");
        }

        if (settings.exit == ExitMode::Exit2D)
            CheckLead(settings.retract, "retract", problems);

        return problems;
    }
}
