// Feeds.h : an operation's spindle speed and feed rates. No Alibre or MFC code belongs here.
//
// Feed rates are in millimetres per minute. They vary with the material, so
// each operation has its own, as the spec says.

#pragma once

#include <string>
#include <vector>

namespace camcore
{
    // The Feeds & Speeds tab every operation has.
    struct FeedSettings
    {
        double spindleRpm = 18000.0;

        double plunge = 300.0;
        double approach = 500.0;
        double engage = 500.0;
        double cut = 1000.0;
        double retract = 1000.0;
        double departure = 1000.0;

        // Moves between cuts go at rapid unless a feed is set. The feed is kept
        // while rapid is chosen.
        bool transferAtRapid = true;
        double transfer = 3000.0;
    };

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateFeeds(const FeedSettings& settings);
}
