// ToolJson.h : tools and feeds as JSON, shared by the tool library and machine files.
// No Alibre or MFC code belongs here. Only core's own files include this.

#pragma once

#include <nlohmann/json.hpp>

#include "Feeds.h"
#include "ToolLibrary.h"

namespace camcore
{
    nlohmann::json FeedsToJson(const FeedSettings& feeds);

    // Throws nlohmann::json::exception if a field is missing or the wrong type.
    FeedSettings FeedsFromJson(const nlohmann::json& item);

    // A tool, with its feeds for each material.
    nlohmann::json ToolToJson(const Tool& tool);

    // Throws if a field is missing or the wrong type. Feeds are optional.
    Tool ToolFromJson(const nlohmann::json& item);
}
