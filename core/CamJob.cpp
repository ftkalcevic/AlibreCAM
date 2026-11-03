// CamJob.cpp : reads and writes the .cam file.

#include "CamJob.h"

#include <cctype>
#include <cstdio>
#include <exception>
#include <random>
#include <system_error>

#include <nlohmann/json.hpp>

#include "FileIO.h"

namespace camcore
{
    namespace
    {
        namespace fs = std::filesystem;
        using nlohmann::json;

        const char* const kFormatName = "AlibreCAM";

        LoadResult LoadFailure(LoadStatus status, const std::string& message, int fileVersion = 0)
        {
            LoadResult result;
            result.status = status;
            result.message = message;
            result.fileVersion = fileVersion;
            return result;
        }

        bool EqualIgnoringCase(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;

            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }

        std::string PathText(const fs::path& path)
        {
            return path.u8string();
        }

        // Each of these reads one value and returns false if it's missing or
        // the wrong type, so a damaged step is reported rather than guessed at.

        bool ReadVec3(const json& parent, const char* name, Vec3& value)
        {
            const auto item = parent.find(name);
            if (item == parent.end() || !item->is_array() || item->size() != 3)
                return false;
            for (const json& number : *item)
            {
                if (!number.is_number())
                    return false;
            }
            value = { (*item)[0].get<double>(), (*item)[1].get<double>(), (*item)[2].get<double>() };
            return true;
        }

        bool ReadString(const json& parent, const char* name, std::string& value)
        {
            const auto item = parent.find(name);
            if (item == parent.end() || !item->is_string())
                return false;
            value = item->get<std::string>();
            return true;
        }

        bool ReadBool(const json& parent, const char* name, bool& value)
        {
            const auto item = parent.find(name);
            if (item == parent.end() || !item->is_boolean())
                return false;
            value = item->get<bool>();
            return true;
        }

        bool ReadInt(const json& parent, const char* name, int& value)
        {
            const auto item = parent.find(name);
            if (item == parent.end() || !item->is_number_integer())
                return false;
            value = item->get<int>();
            return true;
        }

        json WriteVec3(const Vec3& value)
        {
            return json::array({ value.x, value.y, value.z });
        }

        const char* PickKindName(PickKind kind)
        {
            switch (kind)
            {
            case PickKind::PlanarFace:      return "planarFace";
            case PickKind::CylindricalFace: return "cylindricalFace";
            case PickKind::LineEdge:        return "lineEdge";
            case PickKind::CircleEdge:      return "circleEdge";
            default:                        return "none";
            }
        }

        bool PickKindFromName(const std::string& name, PickKind& kind)
        {
            static const PickKind kinds[] =
            {
                PickKind::PlanarFace, PickKind::CylindricalFace, PickKind::LineEdge, PickKind::CircleEdge
            };
            for (PickKind candidate : kinds)
            {
                if (name == PickKindName(candidate))
                {
                    kind = candidate;
                    return true;
                }
            }
            return false;
        }

        json WriteKey(const PersistentKey& key)
        {
            return { { "type", key.elementType }, { "bytes", key.hex } };
        }

        bool ReadKey(const json& parent, const char* name, PersistentKey& key)
        {
            const auto item = parent.find(name);
            return item != parent.end() && item->is_object() && ReadInt(*item, "type", key.elementType) &&
                   ReadString(*item, "bytes", key.hex);
        }

        // An empty pick is written as null.
        json WritePick(const PickedGeometry& pick)
        {
            if (pick.kind == PickKind::None)
                return nullptr;

            json item =
            {
                { "kind", PickKindName(pick.kind) },
                { "occurrence", WriteKey(pick.occurrenceKey) },
                { "target", WriteKey(pick.targetKey) },
                { "occurrenceName", pick.occurrenceName },
                { "clicked", WriteVec3(pick.clicked) },
                { "point", WriteVec3(pick.point) },
                { "direction", WriteVec3(pick.direction) }
            };
            if (pick.kind == PickKind::LineEdge)
            {
                item["start"] = WriteVec3(pick.start);
                item["end"] = WriteVec3(pick.end);
            }
            return item;
        }

        bool ReadPick(const json& parent, const char* name, PickedGeometry& pick)
        {
            pick = PickedGeometry();

            const auto item = parent.find(name);
            if (item == parent.end() || item->is_null())
                return true;
            if (!item->is_object())
                return false;

            std::string kind;
            if (!ReadString(*item, "kind", kind) || !PickKindFromName(kind, pick.kind))
                return false;

            if (!ReadKey(*item, "occurrence", pick.occurrenceKey) || !ReadKey(*item, "target", pick.targetKey) ||
                !ReadString(*item, "occurrenceName", pick.occurrenceName) || !ReadVec3(*item, "clicked", pick.clicked) ||
                !ReadVec3(*item, "point", pick.point) || !ReadVec3(*item, "direction", pick.direction))
                return false;

            if (pick.kind == PickKind::LineEdge)
                return ReadVec3(*item, "start", pick.start) && ReadVec3(*item, "end", pick.end);
            return true;
        }

        json WriteStep(const SequenceStep& step)
        {
            const OriginDefinition& origin = step.origin;
            return
            {
                { "type", "setOrigin" },
                { "id", step.id },
                { "name", step.name },
                { "plane", WritePick(origin.plane) },
                { "position", WritePick(origin.position) },
                { "xDirection", WritePick(origin.xDirection) },
                { "options",
                  {
                      { "flipZ", origin.options.flipZ },
                      { "xQuarterTurns", origin.options.xQuarterTurns },
                      { "offset", WriteVec3(origin.options.offset) }
                  } }
            };
        }

        bool ReadStep(const json& item, SequenceStep& step)
        {
            std::string type;
            if (!item.is_object() || !ReadString(item, "type", type) || type != "setOrigin")
                return false;

            step.type = StepType::SetOrigin;
            if (!ReadString(item, "id", step.id) || step.id.empty() || !ReadString(item, "name", step.name))
                return false;

            OriginDefinition& origin = step.origin;
            if (!ReadPick(item, "plane", origin.plane) || !ReadPick(item, "position", origin.position) ||
                !ReadPick(item, "xDirection", origin.xDirection))
                return false;

            const auto options = item.find("options");
            return options != item.end() && options->is_object() &&
                   ReadBool(*options, "flipZ", origin.options.flipZ) &&
                   ReadInt(*options, "xQuarterTurns", origin.options.xQuarterTurns) &&
                   origin.options.xQuarterTurns >= 0 && origin.options.xQuarterTurns <= 3 &&
                   ReadVec3(*options, "offset", origin.options.offset);
        }
    }

    std::string NewStableId()
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

    LoadResult LoadJob(const fs::path& path)
    {
        std::error_code error;
        if (!fs::exists(path, error))
            return LoadFailure(LoadStatus::NotFound, "File not found: " + PathText(path));

        std::string text;
        if (!ReadWholeFile(path, text))
            return LoadFailure(LoadStatus::Unreadable, "Could not read " + PathText(path));

        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded())
            return LoadFailure(LoadStatus::Unreadable, "Not valid JSON: " + PathText(path));

        if (!root.is_object())
            return LoadFailure(LoadStatus::NotACamFile, "Not a .cam file: " + PathText(path));

        const auto format = root.find("format");
        if (format == root.end() || !format->is_string() || format->get<std::string>() != kFormatName)
            return LoadFailure(LoadStatus::NotACamFile, "Not a .cam file: " + PathText(path));

        const auto version = root.find("formatVersion");
        if (version == root.end() || !version->is_number_integer() || version->get<int>() < 1)
            return LoadFailure(LoadStatus::NotACamFile, "The format version is missing or invalid: " + PathText(path));

        const int fileVersion = version->get<int>();
        if (fileVersion > kCamFormatVersion)
        {
            return LoadFailure(LoadStatus::NewerVersion,
                               "This file uses format version " + std::to_string(fileVersion) +
                                   ", but this add-on supports up to version " +
                                   std::to_string(kCamFormatVersion) + ".",
                               fileVersion);
        }

        // Older formats will be migrated here when the format version changes.

        const auto assembly = root.find("assembly");
        if (assembly == root.end() || !assembly->is_object())
            return LoadFailure(LoadStatus::NotACamFile, "The assembly section is missing: " + PathText(path), fileVersion);

        const auto name = assembly->find("name");
        const auto guid = assembly->find("guid");
        if (name == assembly->end() || !name->is_string() || guid == assembly->end() || !guid->is_string())
            return LoadFailure(LoadStatus::NotACamFile, "The assembly section is incomplete: " + PathText(path), fileVersion);

        LoadResult result;
        result.status = LoadStatus::Ok;
        result.fileVersion = fileVersion;
        result.job.assembly.name = name->get<std::string>();
        result.job.assembly.guid = guid->get<std::string>();

        // Files saved before tools existed don't have the tools section.
        const auto tools = root.find("tools");
        if (tools != root.end())
        {
            if (!tools->is_object())
                return LoadFailure(LoadStatus::NotACamFile, "The tools section is invalid: " + PathText(path), fileVersion);

            const auto library = tools->find("library");
            if (library != tools->end() && library->is_string())
                result.job.toolLibraryPath = library->get<std::string>();

            const auto selected = tools->find("selected");
            if (selected != tools->end() && selected->is_string())
                result.job.selectedToolId = selected->get<std::string>();
        }

        // Version 1 files always have an empty sequence.
        const auto sequence = root.find("sequence");
        if (sequence != root.end())
        {
            if (!sequence->is_array())
                return LoadFailure(LoadStatus::NotACamFile, "The sequence is invalid: " + PathText(path), fileVersion);

            for (const json& item : *sequence)
            {
                SequenceStep step;
                if (!ReadStep(item, step))
                {
                    return LoadFailure(LoadStatus::NotACamFile,
                                       "Step " + std::to_string(result.job.sequence.size() + 1) +
                                           " of the sequence is damaged: " + PathText(path),
                                       fileVersion);
                }
                result.job.sequence.push_back(step);
            }
        }
        return result;
    }

    SaveResult SaveJob(const Job& job, const fs::path& path)
    {
        SaveResult result;

        std::string text;
        try
        {
            json sequence = json::array();
            for (const SequenceStep& step : job.sequence)
                sequence.push_back(WriteStep(step));

            const json root =
            {
                { "format", kFormatName },
                { "formatVersion", kCamFormatVersion },
                { "assembly",
                  {
                      { "name", job.assembly.name },
                      { "guid", job.assembly.guid }
                  } },
                { "tools",
                  {
                      { "library", job.toolLibraryPath },
                      { "selected", job.selectedToolId }
                  } },
                { "setup", json::object() },
                { "sequence", sequence }
            };
            text = root.dump(2) + "\n";
        }
        catch (const std::exception& error)
        {
            result.message = std::string("Could not prepare the file: ") + error.what();
            return result;
        }

        result.ok = WriteFileAtomically(path, text, result.message);
        return result;
    }

    IdentityMatch CompareIdentity(const AssemblyIdentity& a, const AssemblyIdentity& b)
    {
        if (a.guid.empty() || b.guid.empty())
            return IdentityMatch::Unknown;

        return EqualIgnoringCase(a.guid, b.guid) ? IdentityMatch::Same : IdentityMatch::Different;
    }

    fs::path CamPathForAssembly(const fs::path& assemblyPath)
    {
        fs::path result = assemblyPath;
        if (!result.empty())
            result.replace_extension(".cam");
        return result;
    }
}
