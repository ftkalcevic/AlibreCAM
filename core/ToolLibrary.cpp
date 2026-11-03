// ToolLibrary.cpp : cutting tools and the library file that holds them.

#include "ToolLibrary.h"

#include <cstdio>
#include <exception>
#include <random>
#include <set>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "FileIO.h"

namespace camcore
{
    namespace
    {
        namespace fs = std::filesystem;
        using nlohmann::json;

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
            return (type == ToolType::Drill) ? "drill" : "flatEndMill";
        }

        std::string UnitText(ToolUnit unit)
        {
            return (unit == ToolUnit::Inches) ? "in" : "mm";
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

            if (tool.type == ToolType::Drill)
                item["pointAngle"] = tool.pointAngle;
            else
                item["fluteCount"] = tool.fluteCount;

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
            if (type == "drill")
                tool.type = ToolType::Drill;
            else if (type == "flatEndMill")
                tool.type = ToolType::FlatEndMill;
            else
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

            if (tool.type == ToolType::Drill)
                tool.pointAngle = item.at("pointAngle").get<double>();
            else
                tool.fluteCount = item.at("fluteCount").get<int>();

            return tool;
        }
    }

    std::string NewToolId()
    {
        std::random_device device;
        std::uniform_int_distribution<unsigned> byte(0, 255);

        std::string id;
        char text[3];
        for (int i = 0; i < 16; ++i)
        {
            std::snprintf(text, sizeof(text), "%02x", byte(device));
            id += text;
        }
        return id;
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

        if (tool.type == ToolType::Drill)
        {
            if (!(tool.pointAngle > 0.0 && tool.pointAngle <= 180.0))
                problems.push_back("The point angle must be more than 0 and no more than 180 degrees.");
        }
        else
        {
            if (tool.fluteCount < 1)
                problems.push_back("The flute count must be 1 or more.");
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
