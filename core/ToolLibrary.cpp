// ToolLibrary.cpp : cutting tools and the library file that holds them.

#include "ToolLibrary.h"

#include <exception>
#include <set>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "FileIO.h"
#include "ToolJson.h"

namespace camcore
{
    namespace
    {
        namespace fs = std::filesystem;
        using nlohmann::json;

        // The add-on was once called AlibreCAM; the files keep this name so they still load.

        const char* const kFormatName = "AlibreCAM tool library";
        const double kMillimetresPerInch = 25.4;

        ToolLibraryLoadResult LoadFailure(LoadStatus status, const std::string& message)
        {
            ToolLibraryLoadResult result;
            result.status = status;
            result.message = message;
            return result;
        }

        std::string TypeText(ToolType type)
        {
            switch (type)
            {
            case ToolType::Drill:            return "drill";
            case ToolType::BallMill:         return "ballMill";
            case ToolType::CornerRadiusMill: return "cornerRadiusMill";
            case ToolType::VMill:            return "vMill";
            case ToolType::ChamferMill:      return "chamferMill";
            case ToolType::TaperMill:        return "taperMill";
            case ToolType::ThreadMill:       return "threadMill";
            default:                         return "flatEndMill";
            }
        }

        std::string UnitText(ToolUnit unit)
        {
            return (unit == ToolUnit::Inches) ? "in" : "mm";
        }

    }

    json FeedsToJson(const FeedSettings& feeds)
    {
        return
        {
            { "spindleRpm", feeds.spindleRpm },
            { "plunge", feeds.plunge },
            { "approach", feeds.approach },
            { "engage", feeds.engage },
            { "cut", feeds.cut },
            { "retract", feeds.retract },
            { "departure", feeds.departure },
            { "transferAtRapid", feeds.transferAtRapid },
            { "transfer", feeds.transfer }
        };
    }

    FeedSettings FeedsFromJson(const json& item)
    {
        FeedSettings feeds;
        feeds.spindleRpm = item.at("spindleRpm").get<double>();
        feeds.plunge = item.at("plunge").get<double>();
        feeds.approach = item.at("approach").get<double>();
        feeds.engage = item.at("engage").get<double>();
        feeds.cut = item.at("cut").get<double>();
        feeds.retract = item.at("retract").get<double>();
        feeds.departure = item.at("departure").get<double>();
        feeds.transferAtRapid = item.at("transferAtRapid").get<bool>();
        feeds.transfer = item.at("transfer").get<double>();
        return feeds;
    }

    json ToolToJson(const Tool& tool)
        {
            json item =
            {
                { "id", tool.id },
                { "number", tool.number },
                { "name", tool.name },
                { "type", TypeText(tool.type) },
                { "unit", UnitText(tool.unit) },
                { "diameter", tool.diameter },
                { "fluteLength", tool.fluteLength },
                { "overallLength", tool.overallLength }
            };

            // Only the fields the type has.
            const ToolFields fields = FieldsOf(tool.type);
            if (fields.fluteCount)
                item["fluteCount"] = tool.fluteCount;
            if (fields.pointAngle)
                item["pointAngle"] = tool.pointAngle;
            if (fields.cornerRadius)
                item["cornerRadius"] = tool.cornerRadius;
            if (fields.includedAngle)
                item["includedAngle"] = tool.includedAngle;
            if (fields.tipDiameter)
                item["tipDiameter"] = tool.tipDiameter;
            if (fields.taperAngle)
                item["taperAngle"] = tool.taperAngle;
            if (fields.pitch)
                item["pitch"] = tool.pitch;
            if (fields.threadAngle)
                item["threadAngle"] = tool.threadAngle;
            if (tool.reducedNeck)
            {
                item["reducedNeck"] = true;
                item["neckLength"] = tool.neckLength;
                item["shankDiameter"] = tool.shankDiameter;
            }

            json feeds = json::array();
            for (const ToolFeeds& row : tool.feeds)
            {
                json entry = FeedsToJson(row.feeds);
                entry["material"] = row.materialId;
                feeds.push_back(entry);
            }
            item["feeds"] = feeds;
            return item;
        }

    // Throws json::exception if a field is missing or the wrong type.
    Tool ToolFromJson(const json& item)
        {
            Tool tool;
            tool.id = item.at("id").get<std::string>();
            tool.number = item.at("number").get<int>();
            tool.name = item.at("name").get<std::string>();

            const std::string type = item.at("type").get<std::string>();
            bool known = false;
            for (ToolType candidate : ToolTypes())
            {
                if (type == TypeText(candidate))
                {
                    tool.type = candidate;
                    known = true;
                }
            }
            if (!known)
                throw std::runtime_error("unknown tool type '" + type + "'");

            const std::string unit = item.at("unit").get<std::string>();
            if (unit == "in")
                tool.unit = ToolUnit::Inches;
            else if (unit == "mm")
                tool.unit = ToolUnit::Millimetres;
            else
                throw std::runtime_error("unknown unit '" + unit + "'");

            tool.diameter = item.at("diameter").get<double>();
            tool.fluteLength = item.at("fluteLength").get<double>();
            tool.overallLength = item.at("overallLength").get<double>();

            const ToolFields fields = FieldsOf(tool.type);
            if (fields.fluteCount)
                tool.fluteCount = item.at("fluteCount").get<int>();
            if (fields.pointAngle)
                tool.pointAngle = item.at("pointAngle").get<double>();
            if (fields.cornerRadius)
                tool.cornerRadius = item.at("cornerRadius").get<double>();
            if (fields.includedAngle)
                tool.includedAngle = item.at("includedAngle").get<double>();
            if (fields.tipDiameter)
                tool.tipDiameter = item.at("tipDiameter").get<double>();
            if (fields.taperAngle)
                tool.taperAngle = item.at("taperAngle").get<double>();
            if (fields.pitch)
                tool.pitch = item.at("pitch").get<double>();
            if (fields.threadAngle)
                tool.threadAngle = item.at("threadAngle").get<double>();

            // Older tools have no reduced neck.
            const auto neck = item.find("reducedNeck");
            if (neck != item.end() && neck->get<bool>())
            {
                tool.reducedNeck = true;
                tool.neckLength = item.at("neckLength").get<double>();
                tool.shankDiameter = item.at("shankDiameter").get<double>();
            }

            // Version 1 libraries have no feeds.
            const auto feeds = item.find("feeds");
            if (feeds != item.end())
            {
                for (const json& entry : feeds->get_ref<const json::array_t&>())
                {
                    ToolFeeds row;
                    row.materialId = entry.at("material").get<std::string>();
                    row.feeds = FeedsFromJson(entry);
                    tool.feeds.push_back(row);
                }
            }
            return tool;
        }

    std::string ToolTypeTitle(ToolType type)
    {
        switch (type)
        {
        case ToolType::Drill:            return "Drill";
        case ToolType::BallMill:         return "Ball mill";
        case ToolType::CornerRadiusMill: return "Corner radius mill";
        case ToolType::VMill:            return "V-mill";
        case ToolType::ChamferMill:      return "Chamfer mill";
        case ToolType::TaperMill:        return "Taper mill";
        case ToolType::ThreadMill:       return "Thread mill";
        default:                         return "Flat end mill";
        }
    }

    const std::vector<ToolType>& ToolTypes()
    {
        static const std::vector<ToolType> types = {
            ToolType::FlatEndMill, ToolType::BallMill, ToolType::CornerRadiusMill, ToolType::VMill,
            ToolType::ChamferMill, ToolType::TaperMill, ToolType::ThreadMill, ToolType::Drill,
        };
        return types;
    }

    ToolFields FieldsOf(ToolType type)
    {
        ToolFields fields;
        fields.fluteCount = type != ToolType::Drill;
        fields.pointAngle = type == ToolType::Drill;
        fields.cornerRadius = type == ToolType::CornerRadiusMill;
        fields.includedAngle = type == ToolType::VMill || type == ToolType::ChamferMill;
        fields.tipDiameter = type == ToolType::VMill || type == ToolType::ChamferMill || type == ToolType::TaperMill;
        fields.taperAngle = type == ToolType::TaperMill;
        fields.pitch = type == ToolType::ThreadMill;
        fields.threadAngle = type == ToolType::ThreadMill;
        return fields;
    }

    std::string NewToolId()
    {
        return NewStableId();
    }

    std::vector<std::string> ValidateTool(const Tool& tool)
    {
        std::vector<std::string> problems;

        if (tool.id.empty())
            problems.push_back("The tool has no ID.");
        if (tool.number < 1)
            problems.push_back("The tool number must be 1 or more.");
        if (tool.name.empty())
            problems.push_back("The tool needs a name.");
        if (!(tool.diameter > 0.0))
            problems.push_back("The diameter must be more than 0.");
        if (!(tool.fluteLength > 0.0))
            problems.push_back("The flute length must be more than 0.");
        if (!(tool.overallLength >= tool.fluteLength))
            problems.push_back("The overall length can't be less than the flute length.");
        if (tool.reducedNeck)
        {
            if (!(tool.neckLength >= tool.fluteLength && tool.neckLength <= tool.overallLength))
                problems.push_back("The neck length must be from the flute length to the overall length.");
            if (!(tool.shankDiameter >= tool.diameter))
                problems.push_back("The shank diameter can't be less than the diameter.");
        }

        const ToolFields fields = FieldsOf(tool.type);
        if (fields.pointAngle && !(tool.pointAngle > 0.0 && tool.pointAngle <= 180.0))
            problems.push_back("The point angle must be more than 0 and no more than 180 degrees.");
        if (fields.fluteCount && tool.fluteCount < 1)
            problems.push_back("The flute count must be 1 or more.");
        if (fields.cornerRadius && !(tool.cornerRadius > 0.0 && tool.cornerRadius <= tool.diameter / 2.0))
            problems.push_back("The corner radius must be more than 0 and no more than half the diameter.");
        if (fields.includedAngle && !(tool.includedAngle > 0.0 && tool.includedAngle < 180.0))
            problems.push_back("The included angle must be more than 0 and less than 180 degrees.");
        if (fields.tipDiameter && !(tool.tipDiameter >= 0.0 && tool.tipDiameter < tool.diameter))
            problems.push_back("The tip diameter can't be negative, and must be less than the diameter.");
        if (fields.taperAngle && !(tool.taperAngle > 0.0 && tool.taperAngle < 90.0))
            problems.push_back("The taper angle must be more than 0 and less than 90 degrees.");
        if (fields.pitch && !(tool.pitch > 0.0))
            problems.push_back("The pitch must be more than 0.");
        if (fields.threadAngle && !(tool.threadAngle > 0.0 && tool.threadAngle < 180.0))
            problems.push_back("The thread angle must be more than 0 and less than 180 degrees.");

        std::set<std::string> materials;
        for (const ToolFeeds& row : tool.feeds)
        {
            if (row.materialId.empty())
                problems.push_back("A row of speeds and feeds has no material.");
            else if (!materials.insert(row.materialId).second)
                problems.push_back("A material has two rows of speeds and feeds.");
            for (const std::string& problem : ValidateFeeds(row.feeds))
                problems.push_back("Speeds and feeds: " + problem);
        }
        return problems;
    }

    std::vector<std::string> ValidateLibrary(const ToolLibrary& library)
    {
        std::vector<std::string> problems;
        std::set<int> numbers;
        std::set<std::string> ids;

        for (const Tool& tool : library.tools)
        {
            const std::string label = "Tool " + std::to_string(tool.number) + " (" + tool.name + "): ";
            for (const std::string& problem : ValidateTool(tool))
                problems.push_back(label + problem);

            if (!numbers.insert(tool.number).second)
                problems.push_back(label + "Another tool has the same number.");
            if (!ids.insert(tool.id).second)
                problems.push_back(label + "Another tool has the same ID.");
        }
        return problems;
    }

    Tool ToolInMillimetres(const Tool& tool)
    {
        Tool result = tool;
        if (tool.unit == ToolUnit::Millimetres)
            return result;

        result.unit = ToolUnit::Millimetres;
        result.diameter *= kMillimetresPerInch;
        result.fluteLength *= kMillimetresPerInch;
        result.overallLength *= kMillimetresPerInch;
        result.neckLength *= kMillimetresPerInch;
        result.shankDiameter *= kMillimetresPerInch;
        result.cornerRadius *= kMillimetresPerInch;
        result.tipDiameter *= kMillimetresPerInch;
        result.pitch *= kMillimetresPerInch;
        return result;
    }

    const Tool* FindToolById(const ToolLibrary& library, const std::string& id)
    {
        for (const Tool& tool : library.tools)
        {
            if (tool.id == id)
                return &tool;
        }
        return nullptr;
    }

    const ToolFeeds* FeedsFor(const Tool& tool, const std::string& materialId)
    {
        for (const ToolFeeds& row : tool.feeds)
        {
            if (row.materialId == materialId)
                return &row;
        }
        return nullptr;
    }

    void SetFeedsFor(Tool& tool, const std::string& materialId, const FeedSettings& feeds)
    {
        for (ToolFeeds& row : tool.feeds)
        {
            if (row.materialId == materialId)
            {
                row.feeds = feeds;
                return;
            }
        }
        ToolFeeds row;
        row.materialId = materialId;
        row.feeds = feeds;
        tool.feeds.push_back(row);
    }

    int NextFreeToolNumber(const ToolLibrary& library)
    {
        std::set<int> used;
        for (const Tool& tool : library.tools)
            used.insert(tool.number);

        int number = 1;
        while (used.count(number) != 0)
            ++number;
        return number;
    }

    ToolLibraryLoadResult LoadToolLibrary(const fs::path& path)
    {
        std::error_code error;
        if (!fs::exists(path, error))
            return LoadFailure(LoadStatus::NotFound, "File not found: " + path.u8string());

        std::string text;
        if (!ReadWholeFile(path, text))
            return LoadFailure(LoadStatus::Unreadable, "Could not read " + path.u8string());

        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded())
            return LoadFailure(LoadStatus::Unreadable, "Not valid JSON: " + path.u8string());

        if (!root.is_object())
            return LoadFailure(LoadStatus::NotACamFile, "Not a tool library: " + path.u8string());

        const auto format = root.find("format");
        if (format == root.end() || !format->is_string() || format->get<std::string>() != kFormatName)
            return LoadFailure(LoadStatus::NotACamFile, "Not a tool library: " + path.u8string());

        const auto version = root.find("formatVersion");
        if (version == root.end() || !version->is_number_integer() || version->get<int>() < 1)
            return LoadFailure(LoadStatus::NotACamFile, "The format version is missing or invalid: " + path.u8string());

        if (version->get<int>() > kToolLibraryFormatVersion)
        {
            return LoadFailure(LoadStatus::NewerVersion,
                               "This library uses format version " + std::to_string(version->get<int>()) +
                                   ", but this add-on supports up to version " +
                                   std::to_string(kToolLibraryFormatVersion) + ".");
        }

        ToolLibraryLoadResult result;
        try
        {
            for (const json& item : root.at("tools"))
                result.library.tools.push_back(ToolFromJson(item));
        }
        catch (const std::exception& problem)
        {
            return LoadFailure(LoadStatus::NotACamFile, "The library has a damaged tool: " + std::string(problem.what()));
        }

        const std::vector<std::string> problems = ValidateLibrary(result.library);
        if (!problems.empty())
        {
            ToolLibraryLoadResult invalid = LoadFailure(LoadStatus::NotACamFile, problems.front());
            return invalid;
        }

        result.status = LoadStatus::Ok;
        return result;
    }

    SaveResult SaveToolLibrary(const ToolLibrary& library, const fs::path& path)
    {
        SaveResult result;

        const std::vector<std::string> problems = ValidateLibrary(library);
        if (!problems.empty())
        {
            result.message = problems.front();
            return result;
        }

        std::string text;
        try
        {
            json tools = json::array();
            for (const Tool& tool : library.tools)
                tools.push_back(ToolToJson(tool));

            const json root =
            {
                { "format", kFormatName },
                { "formatVersion", kToolLibraryFormatVersion },
                { "tools", tools }
            };
            text = root.dump(2) + "\n";
        }
        catch (const std::exception& problem)
        {
            result.message = std::string("Could not prepare the file: ") + problem.what();
            return result;
        }

        result.ok = WriteFileAtomically(path, text, result.message);
        return result;
    }

    std::string StoredLibraryPath(const fs::path& camPath, const fs::path& libraryPath)
    {
        if (libraryPath.empty())
            return std::string();

        if (!camPath.empty() && camPath.parent_path() == libraryPath.parent_path())
            return libraryPath.filename().u8string();

        return libraryPath.u8string();
    }

    fs::path ResolveLibraryPath(const fs::path& camPath, const std::string& stored)
    {
        if (stored.empty())
            return fs::path();

        const fs::path path = fs::u8path(stored);
        if (path.has_parent_path() || camPath.empty())
            return path;

        return camPath.parent_path() / path;
    }
}
