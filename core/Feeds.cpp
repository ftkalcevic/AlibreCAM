// Feeds.cpp : an operation's spindle speed and feed rates.

#include "Feeds.h"

namespace camcore
{
    std::vector<std::string> ValidateFeeds(const FeedSettings& settings)
    {
        std::vector<std::string> problems;
        const auto positive = [&](double value, const char* name)
        {
            if (!(value > 0.0))
                problems.push_back(std::string("The ") + name + " must be more than 0.");
        };

        positive(settings.spindleRpm, "spindle speed");
        positive(settings.plunge, "plunge feed");
        positive(settings.approach, "approach feed");
        positive(settings.engage, "engage feed");
        positive(settings.cut, "cut feed");
        positive(settings.retract, "retract feed");
        positive(settings.departure, "departure feed");
        if (!settings.transferAtRapid)
            positive(settings.transfer, "transfer feed");
        return problems;
    }
}
