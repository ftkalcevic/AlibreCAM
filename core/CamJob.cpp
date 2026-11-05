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
            case PickKind::DesignPoint:     return "designPoint";
            case PickKind::CurveEdge:       return "curveEdge";
            default:                        return "none";
            }
        }

        bool PickKindFromName(const std::string& name, PickKind& kind)
        {
            static const PickKind kinds[] =
            {
                PickKind::PlanarFace, PickKind::CylindricalFace, PickKind::LineEdge, PickKind::CircleEdge,
                PickKind::DesignPoint, PickKind::CurveEdge
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
            if (pick.kind == PickKind::LineEdge || pick.kind == PickKind::CylindricalFace ||
                pick.kind == PickKind::CurveEdge)
            {
                item["start"] = WriteVec3(pick.start);
                item["end"] = WriteVec3(pick.end);
            }
            if (pick.kind == PickKind::CircleEdge || pick.kind == PickKind::CylindricalFace)
                item["radius"] = pick.radius;
            if (pick.kind == PickKind::DesignPoint)
                item["targetName"] = pick.targetName;
            return item;
        }

        bool ReadPickObject(const json& item, PickedGeometry& pick)
        {
            pick = PickedGeometry();
            if (!item.is_object())
                return false;

            std::string kind;
            if (!ReadString(item, "kind", kind) || !PickKindFromName(kind, pick.kind))
                return false;

            if (!ReadKey(item, "occurrence", pick.occurrenceKey) || !ReadKey(item, "target", pick.targetKey) ||
                !ReadString(item, "occurrenceName", pick.occurrenceName) || !ReadVec3(item, "clicked", pick.clicked) ||
                !ReadVec3(item, "point", pick.point) || !ReadVec3(item, "direction", pick.direction))
                return false;

            switch (pick.kind)
            {
            case PickKind::LineEdge:
            case PickKind::CurveEdge:
                return ReadVec3(item, "start", pick.start) && ReadVec3(item, "end", pick.end);

            case PickKind::CylindricalFace:
                // Origins picked before the ends were recorded have neither.
                if (!ReadVec3(item, "start", pick.start) || !ReadVec3(item, "end", pick.end))
                    pick.start = pick.end = pick.point;
                break;

            case PickKind::DesignPoint:
                return ReadString(item, "targetName", pick.targetName);

            default:
                break;
            }

            // Picks saved before the radius was recorded have none.
            const auto radius = item.find("radius");
            if (radius != item.end())
            {
                if (!radius->is_number())
                    return false;
                pick.radius = radius->get<double>();
            }
            return true;
        }

        bool ReadPick(const json& parent, const char* name, PickedGeometry& pick)
        {
            pick = PickedGeometry();

            const auto item = parent.find(name);
            if (item == parent.end() || item->is_null())
                return true;
            return ReadPickObject(*item, pick);
        }

        const char* ClearanceModeName(ClearanceMode mode)
        {
            switch (mode)
            {
            case ClearanceMode::PartMaxZ:  return "partMaxZ";
            case ClearanceMode::StockMaxZ: return "stockMaxZ";
            case ClearanceMode::AbsoluteZ: return "absoluteZ";
            default:                       return "automatic";
            }
        }

        json WriteClearance(const ClearanceSettings& clearance)
        {
            return
            {
                { "mode", ClearanceModeName(clearance.mode) },
                { "partDistance", clearance.partDistance },
                { "stockDistance", clearance.stockDistance },
                { "absoluteZ", clearance.absoluteZ }
            };
        }

        bool ReadNumber(const json& parent, const char* name, double& value)
        {
            const auto item = parent.find(name);
            if (item == parent.end() || !item->is_number())
                return false;
            value = item->get<double>();
            return true;
        }

        // Operations saved before the Clearance tab existed have the defaults.
        bool ReadClearance(const json& step, ClearanceSettings& clearance)
        {
            clearance = ClearanceSettings();
            const auto item = step.find("clearance");
            if (item == step.end())
                return true;
            if (!item->is_object())
                return false;

            std::string mode;
            if (!ReadString(*item, "mode", mode) || !ReadNumber(*item, "partDistance", clearance.partDistance) ||
                !ReadNumber(*item, "stockDistance", clearance.stockDistance) ||
                !ReadNumber(*item, "absoluteZ", clearance.absoluteZ))
                return false;

            static const ClearanceMode modes[] =
            {
                ClearanceMode::Automatic, ClearanceMode::PartMaxZ, ClearanceMode::StockMaxZ, ClearanceMode::AbsoluteZ
            };
            for (ClearanceMode candidate : modes)
            {
                if (mode == ClearanceModeName(candidate))
                {
                    clearance.mode = candidate;
                    return true;
                }
            }
            return false;
        }

        json WriteFeeds(const FeedSettings& feeds)
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

        // Operations saved before the Feeds & Speeds tab existed have the defaults.
        bool ReadFeeds(const json& step, FeedSettings& feeds)
        {
            feeds = FeedSettings();
            const auto item = step.find("feeds");
            if (item == step.end())
                return true;
            return item->is_object() && ReadNumber(*item, "spindleRpm", feeds.spindleRpm) &&
                   ReadNumber(*item, "plunge", feeds.plunge) && ReadNumber(*item, "approach", feeds.approach) &&
                   ReadNumber(*item, "engage", feeds.engage) && ReadNumber(*item, "cut", feeds.cut) &&
                   ReadNumber(*item, "retract", feeds.retract) && ReadNumber(*item, "departure", feeds.departure) &&
                   ReadBool(*item, "transferAtRapid", feeds.transferAtRapid) &&
                   ReadNumber(*item, "transfer", feeds.transfer);
        }

        // Each move is [kind, x, y, z, feed, seconds], kind 0 rapid, 1 feed, 2 dwell.
        // An arc feed is kind 3, followed by its centre's x, y, z and 1 if clockwise.
        json WriteToolpath(const Toolpath& toolpath)
        {
            json moves = json::array();
            for (const ToolpathMove& move : toolpath.moves)
            {
                if (move.kind == MoveKind::Feed && move.arc)
                {
                    moves.push_back(json::array({ 3, move.to.x, move.to.y, move.to.z, move.feed, move.seconds,
                                                  move.centre.x, move.centre.y, move.centre.z,
                                                  move.clockwise ? 1 : 0 }));
                    continue;
                }
                const int kind = (move.kind == MoveKind::Feed) ? 1 : (move.kind == MoveKind::Dwell) ? 2 : 0;
                moves.push_back(json::array({ kind, move.to.x, move.to.y, move.to.z, move.feed, move.seconds }));
            }
            return moves;
        }

        // Operations never generated have no toolpath.
        bool ReadToolpath(const json& step, Toolpath& toolpath)
        {
            toolpath = Toolpath();
            const auto item = step.find("toolpath");
            if (item == step.end())
                return true;
            if (!item->is_array())
                return false;

            for (const json& entry : *item)
            {
                if (!entry.is_array() || entry.empty())
                    return false;
                for (const json& number : entry)
                {
                    if (!number.is_number())
                        return false;
                }

                const int kind = entry[0].get<int>();
                if (kind < 0 || kind > 3 || entry.size() != (kind == 3 ? 10u : 6u))
                    return false;

                ToolpathMove move;
                move.kind = (kind == 1 || kind == 3) ? MoveKind::Feed : (kind == 2) ? MoveKind::Dwell : MoveKind::Rapid;
                move.to = { entry[1].get<double>(), entry[2].get<double>(), entry[3].get<double>() };
                move.feed = entry[4].get<double>();
                move.seconds = entry[5].get<double>();
                if (kind == 3)
                {
                    move.arc = true;
                    move.centre = { entry[6].get<double>(), entry[7].get<double>(), entry[8].get<double>() };
                    move.clockwise = entry[9].get<double>() != 0.0;
                }
                toolpath.moves.push_back(move);
            }
            return true;
        }

        const char* CutLocationName(CutGeometryLocation location)
        {
            switch (location)
            {
            case CutGeometryLocation::AtBottom: return "bottom";
            case CutGeometryLocation::PickTop:  return "pickTop";
            default:                            return "top";
            }
        }

        json WriteCutLevels(const CutLevelSettings& levels)
        {
            return
            {
                { "geometryLocation", CutLocationName(levels.location) },
                { "topZ", levels.topZ },
                { "totalDepth", levels.totalDepth },
                { "roughDepth", levels.roughDepth },
                { "finishDepth", levels.finishDepth },
                { "roughStep", levels.roughStep },
                { "finishStep", levels.finishStep },
                { "clearIslandTops", levels.clearIslandTops },
                { "useModelForDepth", levels.useModelForDepth },
                { "order", levels.order == CutLevelOrder::LevelFirst ? "levelFirst" : "depthFirst" }
            };
        }

        // Operations saved before the Cut Levels tab existed have the defaults.
        bool ReadCutLevels(const json& step, CutLevelSettings& levels)
        {
            levels = CutLevelSettings();
            const auto item = step.find("cutLevels");
            if (item == step.end())
                return true;
            if (!item->is_object())
                return false;

            std::string location, order;
            if (!ReadString(*item, "geometryLocation", location) || !ReadNumber(*item, "topZ", levels.topZ) ||
                !ReadNumber(*item, "totalDepth", levels.totalDepth) ||
                !ReadNumber(*item, "roughDepth", levels.roughDepth) ||
                !ReadNumber(*item, "finishDepth", levels.finishDepth) ||
                !ReadNumber(*item, "roughStep", levels.roughStep) ||
                !ReadNumber(*item, "finishStep", levels.finishStep) ||
                !ReadBool(*item, "clearIslandTops", levels.clearIslandTops) ||
                !ReadBool(*item, "useModelForDepth", levels.useModelForDepth) || !ReadString(*item, "order", order))
                return false;

            if (location == CutLocationName(CutGeometryLocation::AtTop))
                levels.location = CutGeometryLocation::AtTop;
            else if (location == CutLocationName(CutGeometryLocation::AtBottom))
                levels.location = CutGeometryLocation::AtBottom;
            else if (location == CutLocationName(CutGeometryLocation::PickTop))
                levels.location = CutGeometryLocation::PickTop;
            else
                return false;

            if (order == "depthFirst")
                levels.order = CutLevelOrder::DepthFirst;
            else if (order == "levelFirst")
                levels.order = CutLevelOrder::LevelFirst;
            else
                return false;
            return true;
        }

        json WriteLead(const LeadMotion& lead)
        {
            return
            {
                { "shape", lead.shape == LeadShape::Radial ? "radial" : "linear" },
                { "length", lead.length },
                { "angle", lead.angle },
                { "radius", lead.radius }
            };
        }

        bool ReadLead(const json& parent, const char* name, LeadMotion& lead)
        {
            const auto item = parent.find(name);
            if (item == parent.end() || !item->is_object())
                return false;

            std::string shape;
            if (!ReadString(*item, "shape", shape) || !ReadNumber(*item, "length", lead.length) ||
                !ReadNumber(*item, "angle", lead.angle) || !ReadNumber(*item, "radius", lead.radius))
                return false;
            if (shape == "linear")
                lead.shape = LeadShape::Linear;
            else if (shape == "radial")
                lead.shape = LeadShape::Radial;
            else
                return false;
            return true;
        }

        const char* EntryModeName(EntryMode mode)
        {
            switch (mode)
            {
            case EntryMode::Entry2D:     return "2d";
            case EntryMode::AlongPath3D: return "alongPath";
            default:                     return "none";
            }
        }

        json WriteEntryExit(const EntryExitSettings& settings)
        {
            return
            {
                { "entry", EntryModeName(settings.entry) },
                { "engage", WriteLead(settings.engage) },
                { "alongPathAngle", settings.alongPathAngle },
                { "alongPathHeight", settings.alongPathHeight },
                { "exit", settings.exit == ExitMode::Exit2D ? "2d" : "none" },
                { "retract", WriteLead(settings.retract) },
                { "eachCutLevel", settings.eachCutLevel }
            };
        }

        // Operations saved before the Entry/Exit tab existed have the defaults.
        bool ReadEntryExit(const json& step, EntryExitSettings& settings)
        {
            settings = EntryExitSettings();
            const auto item = step.find("entryExit");
            if (item == step.end())
                return true;
            if (!item->is_object())
                return false;

            std::string entry, exit;
            if (!ReadString(*item, "entry", entry) || !ReadLead(*item, "engage", settings.engage) ||
                !ReadNumber(*item, "alongPathAngle", settings.alongPathAngle) ||
                !ReadNumber(*item, "alongPathHeight", settings.alongPathHeight) || !ReadString(*item, "exit", exit) ||
                !ReadLead(*item, "retract", settings.retract) || !ReadBool(*item, "eachCutLevel", settings.eachCutLevel))
                return false;

            if (entry == EntryModeName(EntryMode::None))
                settings.entry = EntryMode::None;
            else if (entry == EntryModeName(EntryMode::Entry2D))
                settings.entry = EntryMode::Entry2D;
            else if (entry == EntryModeName(EntryMode::AlongPath3D))
                settings.entry = EntryMode::AlongPath3D;
            else
                return false;

            if (exit == "none")
                settings.exit = ExitMode::None;
            else if (exit == "2d")
                settings.exit = ExitMode::Exit2D;
            else
                return false;
            return true;
        }

        json WriteSize(const ToolRelativeSize& size)
        {
            return { { "usePercent", size.usePercent }, { "percent", size.percent }, { "distance", size.distance } };
        }

        bool ReadSize(const json& parent, const char* name, ToolRelativeSize& size)
        {
            const auto item = parent.find(name);
            return item != parent.end() && item->is_object() && ReadBool(*item, "usePercent", size.usePercent) &&
                   ReadNumber(*item, "percent", size.percent) && ReadNumber(*item, "distance", size.distance);
        }

        const char* CutDirectionName(CutDirection direction)
        {
            switch (direction)
            {
            case CutDirection::Conventional: return "conventional";
            case CutDirection::Mixed:        return "mixed";
            default:                         return "climb";
            }
        }

        const char* CornerName(StartCorner corner)
        {
            switch (corner)
            {
            case StartCorner::TopRight:    return "topRight";
            case StartCorner::BottomLeft:  return "bottomLeft";
            case StartCorner::BottomRight: return "bottomRight";
            default:                       return "topLeft";
            }
        }

        json WriteFacing(const FacingSettings& facing)
        {
            return
            {
                { "direction", CutDirectionName(facing.direction) },
                { "stepDistance", WriteSize(facing.stepDistance) },
                { "overtravel", WriteSize(facing.overtravel) },
                { "cutType", facing.cutType == FacingCutType::Spiral ? "spiral" : "linear" },
                { "cutAngle", facing.cutAngle },
                { "start", CornerName(facing.start) },
                { "arcStepOver", facing.arcStepOver }
            };
        }

        // Facings saved before these settings existed keep the defaults.
        bool ReadFacingPattern(const json& item, FacingSettings& facing)
        {
            const auto cutType = item.find("cutType");
            if (cutType != item.end())
            {
                if (*cutType == "linear")
                    facing.cutType = FacingCutType::Linear;
                else if (*cutType == "spiral")
                    facing.cutType = FacingCutType::Spiral;
                else
                    return false;
            }

            const auto arcs = item.find("arcStepOver");
            if (arcs != item.end())
            {
                if (!arcs->is_boolean())
                    return false;
                facing.arcStepOver = arcs->get<bool>();
            }

            const auto angle = item.find("cutAngle");
            if (angle != item.end())
            {
                if (!angle->is_number())
                    return false;
                facing.cutAngle = angle->get<double>();
            }

            const auto start = item.find("start");
            if (start != item.end())
            {
                if (!start->is_string())
                    return false;
                static const StartCorner corners[] =
                {
                    StartCorner::TopLeft, StartCorner::TopRight, StartCorner::BottomLeft, StartCorner::BottomRight
                };
                bool known = false;
                for (StartCorner corner : corners)
                {
                    if (start->get<std::string>() == CornerName(corner))
                    {
                        facing.start = corner;
                        known = true;
                    }
                }
                if (!known)
                    return false;
            }
            return true;
        }

        // Each vertex is [x, y, z], or [x, y, z, cx, cy, cz, ax, ay, az] when an arc
        // about that centre and axis goes on from it.
        json WritePath(const Path& path)
        {
            json list = json::array();
            for (const PathVertex& vertex : path)
            {
                if (vertex.arc)
                    list.push_back(json::array({ vertex.point.x, vertex.point.y, vertex.point.z, vertex.centre.x,
                                                 vertex.centre.y, vertex.centre.z, vertex.axis.x, vertex.axis.y,
                                                 vertex.axis.z }));
                else
                    list.push_back(WriteVec3(vertex.point));
            }
            return list;
        }

        // Facings never generated have no boundary. Older files have only points.
        bool ReadBoundary(const json& step, Path& boundary)
        {
            boundary.clear();
            const auto item = step.find("boundary");
            if (item == step.end())
                return true;
            if (!item->is_array())
                return false;

            for (const json& entry : *item)
            {
                if (!entry.is_array() || (entry.size() != 3 && entry.size() != 9))
                    return false;
                for (const json& number : entry)
                {
                    if (!number.is_number())
                        return false;
                }

                PathVertex vertex;
                vertex.point = { entry[0].get<double>(), entry[1].get<double>(), entry[2].get<double>() };
                if (entry.size() == 9)
                {
                    vertex.arc = true;
                    vertex.centre = { entry[3].get<double>(), entry[4].get<double>(), entry[5].get<double>() };
                    vertex.axis = { entry[6].get<double>(), entry[7].get<double>(), entry[8].get<double>() };
                }
                boundary.push_back(vertex);
            }
            return true;
        }

        json WriteProfiling(const ProfilingSettings& profiling)
        {
            return
            {
                { "stockToLeave", profiling.stockToLeave },
                { "direction", CutDirectionName(profiling.direction) },
                { "startSide", profiling.startSide == ProfileSide::Left ? "left" : "right" },
                { "useClosedSide", profiling.useClosedSide },
                { "closedSide", profiling.closedSide == ClosedProfileSide::Inside ? "inside" : "outside" },
                { "useModelForSide", profiling.useModelForSide },
                { "cutWidth", profiling.cutWidth },
                { "stepPerCut", profiling.stepPerCut },
                { "tabs",
                  {
                      { "use", profiling.useTabs },
                      { "height", profiling.tabHeight },
                      { "length", profiling.tabLength },
                      { "spacing", profiling.tabSpacing == TabSpacing::Distance ? "distance" : "count" },
                      { "count", profiling.tabCount },
                      { "distance", profiling.tabDistance }
                  } }
            };
        }

        // Reads a name into one of two choices, leaving the value alone if it's missing.
        template <typename T>
        bool ReadChoice(const json& parent, const char* name, const char* firstName, T first, const char* secondName,
                        T second, T& value)
        {
            const auto item = parent.find(name);
            if (item == parent.end())
                return true;
            if (!item->is_string())
                return false;
            const std::string text = item->get<std::string>();
            if (text == firstName)
                value = first;
            else if (text == secondName)
                value = second;
            else
                return false;
            return true;
        }

        // Reads a value only if it's there, so settings added later keep their defaults.
        bool ReadOptionalNumber(const json& parent, const char* name, double& value)
        {
            const auto item = parent.find(name);
            return item == parent.end() || ReadNumber(parent, name, value);
        }

        bool ReadOptionalBool(const json& parent, const char* name, bool& value)
        {
            const auto item = parent.find(name);
            return item == parent.end() || ReadBool(parent, name, value);
        }

        // Profiling saved before the Profiling tab existed has the defaults.
        bool ReadProfiling(const json& step, ProfilingSettings& profiling)
        {
            profiling = ProfilingSettings();
            const auto item = step.find("profiling");
            if (item == step.end())
                return true;
            if (!item->is_object())
                return false;

            const auto direction = item->find("direction");
            if (direction != item->end())
            {
                if (!direction->is_string())
                    return false;
                const std::string name = direction->get<std::string>();
                if (name == CutDirectionName(CutDirection::Climb))
                    profiling.direction = CutDirection::Climb;
                else if (name == CutDirectionName(CutDirection::Conventional))
                    profiling.direction = CutDirection::Conventional;
                else if (name == CutDirectionName(CutDirection::Mixed))
                    profiling.direction = CutDirection::Mixed;
                else
                    return false;
            }

            if (!ReadOptionalNumber(*item, "stockToLeave", profiling.stockToLeave) ||
                !ReadChoice(*item, "startSide", "right", ProfileSide::Right, "left", ProfileSide::Left,
                            profiling.startSide) ||
                !ReadOptionalBool(*item, "useClosedSide", profiling.useClosedSide) ||
                !ReadChoice(*item, "closedSide", "outside", ClosedProfileSide::Outside, "inside",
                            ClosedProfileSide::Inside, profiling.closedSide) ||
                !ReadOptionalBool(*item, "useModelForSide", profiling.useModelForSide) ||
                !ReadOptionalNumber(*item, "cutWidth", profiling.cutWidth) ||
                !ReadOptionalNumber(*item, "stepPerCut", profiling.stepPerCut))
                return false;

            const auto tabs = item->find("tabs");
            if (tabs == item->end())
                return true;
            if (!tabs->is_object())
                return false;
            const auto count = tabs->find("count");
            if (count != tabs->end())
            {
                if (!count->is_number_integer())
                    return false;
                profiling.tabCount = count->get<int>();
            }
            return ReadOptionalBool(*tabs, "use", profiling.useTabs) &&
                   ReadOptionalNumber(*tabs, "height", profiling.tabHeight) &&
                   ReadOptionalNumber(*tabs, "length", profiling.tabLength) &&
                   ReadChoice(*tabs, "spacing", "count", TabSpacing::Count, "distance", TabSpacing::Distance,
                              profiling.tabSpacing) &&
                   ReadOptionalNumber(*tabs, "distance", profiling.tabDistance);
        }

        // Facings saved before the Facing tab existed have the defaults.
        bool ReadFacing(const json& step, FacingSettings& facing)
        {
            facing = FacingSettings();
            const auto item = step.find("facing");
            if (item == step.end())
                return true;
            if (!item->is_object())
                return false;

            std::string direction;
            if (!ReadString(*item, "direction", direction) || !ReadSize(*item, "stepDistance", facing.stepDistance) ||
                !ReadSize(*item, "overtravel", facing.overtravel) || !ReadFacingPattern(*item, facing))
                return false;

            static const CutDirection directions[] =
            {
                CutDirection::Climb, CutDirection::Conventional, CutDirection::Mixed
            };
            for (CutDirection candidate : directions)
            {
                if (direction == CutDirectionName(candidate))
                {
                    facing.direction = candidate;
                    return true;
                }
            }
            return false;
        }

        // Operations saved before the Tool tab existed have no tool.
        bool ReadTool(const json& step, std::string& toolId)
        {
            toolId.clear();
            const auto item = step.find("tool");
            if (item == step.end())
                return true;
            if (!item->is_string())
                return false;
            toolId = item->get<std::string>();
            return true;
        }

        json WriteFeatures(const std::vector<PickedGeometry>& features)
        {
            json list = json::array();
            for (const PickedGeometry& feature : features)
                list.push_back(WritePick(feature));
            return list;
        }

        // Operations saved before the Features tab existed have none.
        bool ReadFeatures(const json& item, std::vector<PickedGeometry>& features)
        {
            features.clear();
            const auto list = item.find("features");
            if (list == item.end())
                return true;
            if (!list->is_array())
                return false;

            for (const json& entry : *list)
            {
                PickedGeometry feature;
                if (!ReadPickObject(entry, feature))
                    return false;
                features.push_back(feature);
            }
            return true;
        }

        const char* DwellName(DwellMode mode)
        {
            switch (mode)
            {
            case DwellMode::Seconds:     return "seconds";
            case DwellMode::Revolutions: return "revolutions";
            default:                     return "off";
            }
        }

        json WriteDrilling(const DrillSettings& drill)
        {
            json peck = nullptr;
            if (drill.pecking)
                peck = drill.peckDepth;

            return
            {
                { "depth", drill.depth },
                { "addDrillTip", drill.addTipToDepth },
                { "geometryLocation", drill.location == GeometryLocation::AtBottom ? "bottom" : "top" },
                { "dwell", DwellName(drill.dwell) },
                { "dwellSeconds", drill.dwellSeconds },
                { "dwellRevolutions", drill.dwellRevolutions },
                { "approachDistance", drill.approachDistance },
                { "peckDepth", peck }
            };
        }

        bool ReadDrilling(const json& item, DrillSettings& drill)
        {
            if (!item.is_object())
                return false;

            std::string location, dwell;
            if (!ReadNumber(item, "depth", drill.depth) || !ReadBool(item, "addDrillTip", drill.addTipToDepth) ||
                !ReadString(item, "geometryLocation", location) ||
                !ReadString(item, "dwell", dwell) || !ReadNumber(item, "dwellSeconds", drill.dwellSeconds) ||
                !ReadNumber(item, "dwellRevolutions", drill.dwellRevolutions) ||
                !ReadNumber(item, "approachDistance", drill.approachDistance))
                return false;

            if (location == "top")
                drill.location = GeometryLocation::AtTop;
            else if (location == "bottom")
                drill.location = GeometryLocation::AtBottom;
            else
                return false;

            if (dwell == DwellName(DwellMode::Off))
                drill.dwell = DwellMode::Off;
            else if (dwell == DwellName(DwellMode::Seconds))
                drill.dwell = DwellMode::Seconds;
            else if (dwell == DwellName(DwellMode::Revolutions))
                drill.dwell = DwellMode::Revolutions;
            else
                return false;

            // A null peck depth means no pecking.
            const auto peck = item.find("peckDepth");
            if (peck == item.end())
                return false;
            drill.pecking = !peck->is_null();
            if (drill.pecking)
            {
                if (!peck->is_number())
                    return false;
                drill.peckDepth = peck->get<double>();
            }
            return true;
        }

        json WriteStep(const SequenceStep& step);

        json WriteStepBody(const SequenceStep& step)
        {
            if (step.type == StepType::Group)
            {
                json children = json::array();
                for (const SequenceStep& child : step.children)
                    children.push_back(WriteStep(child));

                return { { "type", "group" }, { "id", step.id }, { "name", step.name }, { "children", children } };
            }

            // The other tabs' settings join this as each is built.
            // Facing settings join this when the Facing tab is built.
            if (step.type == StepType::Facing)
                return { { "type", "facing" }, { "id", step.id }, { "name", step.name }, { "tool", step.toolId },
                         { "feeds", WriteFeeds(step.feeds) }, { "toolpath", WriteToolpath(step.toolpath) },
                         { "features", WriteFeatures(step.features) }, { "clearance", WriteClearance(step.clearance) },
                         { "cutLevels", WriteCutLevels(step.cutLevels) }, { "entryExit", WriteEntryExit(step.entryExit) },
                         { "facing", WriteFacing(step.facing) }, { "boundary", WritePath(step.boundary) } };

            if (step.type == StepType::Profiling)
                return { { "type", "profiling" }, { "id", step.id }, { "name", step.name }, { "tool", step.toolId },
                         { "feeds", WriteFeeds(step.feeds) }, { "toolpath", WriteToolpath(step.toolpath) },
                         { "features", WriteFeatures(step.features) }, { "clearance", WriteClearance(step.clearance) },
                         { "cutLevels", WriteCutLevels(step.cutLevels) }, { "entryExit", WriteEntryExit(step.entryExit) },
                         { "profiling", WriteProfiling(step.profiling) } };

            if (step.type == StepType::Drill)
                return { { "type", "drill" }, { "id", step.id }, { "name", step.name }, { "tool", step.toolId },
                         { "feeds", WriteFeeds(step.feeds) }, { "toolpath", WriteToolpath(step.toolpath) },
                         { "features", WriteFeatures(step.features) }, { "clearance", WriteClearance(step.clearance) },
                         { "drilling", WriteDrilling(step.drill) } };

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

        bool ReadStep(const json& item, SequenceStep& step);

        bool ReadStepBody(const json& item, SequenceStep& step)
        {
            std::string type;
            if (!item.is_object() || !ReadString(item, "type", type))
                return false;
            if (!ReadString(item, "id", step.id) || step.id.empty() || !ReadString(item, "name", step.name))
                return false;

            if (type == "group")
            {
                step.type = StepType::Group;

                const auto children = item.find("children");
                if (children == item.end() || !children->is_array())
                    return false;

                for (const json& childItem : *children)
                {
                    // Groups hold only operations.
                    SequenceStep child;
                    if (!ReadStep(childItem, child) || !IsOperation(child.type))
                        return false;
                    step.children.push_back(child);
                }
                return true;
            }

            if (type == "facing")
            {
                step.type = StepType::Facing;
                return ReadTool(item, step.toolId) && ReadFeeds(item, step.feeds) && ReadToolpath(item, step.toolpath) &&
                       ReadFeatures(item, step.features) && ReadCutLevels(item, step.cutLevels) &&
                       ReadEntryExit(item, step.entryExit) && ReadFacing(item, step.facing) &&
                       ReadBoundary(item, step.boundary) &&
                       ReadClearance(item, step.clearance);
            }

            if (type == "profiling")
            {
                step.type = StepType::Profiling;
                return ReadTool(item, step.toolId) && ReadFeeds(item, step.feeds) && ReadToolpath(item, step.toolpath) &&
                       ReadFeatures(item, step.features) && ReadCutLevels(item, step.cutLevels) &&
                       ReadEntryExit(item, step.entryExit) && ReadProfiling(item, step.profiling) &&
                       ReadClearance(item, step.clearance);
            }

            if (type == "drill")
            {
                // Drills saved before the Drilling tab existed have no settings yet.
                step.type = StepType::Drill;
                if (!ReadTool(item, step.toolId) || !ReadFeeds(item, step.feeds) || !ReadToolpath(item, step.toolpath) ||
                    !ReadFeatures(item, step.features) ||
                    !ReadClearance(item, step.clearance))
                    return false;
                const auto drilling = item.find("drilling");
                return drilling == item.end() || ReadDrilling(*drilling, step.drill);
            }

            if (type != "setOrigin")
                return false;
            step.type = StepType::SetOrigin;

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

        json WriteStep(const SequenceStep& step)
        {
            json item = WriteStepBody(step);
            if (step.suppressed)
                item["suppressed"] = true;
            return item;
        }

        // Steps saved before suppressing existed aren't suppressed.
        bool ReadStep(const json& item, SequenceStep& step)
        {
            if (!ReadStepBody(item, step))
                return false;
            const auto suppressed = item.find("suppressed");
            if (suppressed == item.end())
                return true;
            if (!suppressed->is_boolean())
                return false;
            step.suppressed = suppressed->get<bool>();
            return true;
        }
    }

    bool IsOperation(StepType type)
    {
        return type == StepType::Drill || type == StepType::Facing || type == StepType::Profiling;
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

        // Version 3 added arcs to boundaries and toolpaths, which version 2 files
        // simply don't have, so they read as they are.

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
                // Operations only go in groups.
                SequenceStep step;
                if (!ReadStep(item, step) || IsOperation(step.type))
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

    bool SameSettings(const SequenceStep& a, const SequenceStep& b)
    {
        SequenceStep left = a;
        SequenceStep right = b;
        left.toolpath = Toolpath();
        right.toolpath = Toolpath();
        left.boundary.clear();
        right.boundary.clear();
        return WriteStep(left) == WriteStep(right);
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

    size_t ActiveOriginIndex(const Job& job, size_t stepIndex)
    {
        if (stepIndex >= job.sequence.size())
            return kNoStepIndex;

        // A Set Origin is its own, even suppressed. Steps below skip suppressed ones.
        for (size_t i = stepIndex + 1; i-- > 0;)
        {
            const SequenceStep& step = job.sequence[i];
            if (step.type == StepType::SetOrigin && (i == stepIndex || !step.suppressed))
                return i;
        }
        return kNoStepIndex;
    }

    size_t WorkZeroIndex(const Job& job)
    {
        for (size_t i = 0; i < job.sequence.size(); ++i)
        {
            if (job.sequence[i].type == StepType::SetOrigin && !job.sequence[i].suppressed)
                return i;
        }
        return kNoStepIndex;
    }

    bool IsOperationSuppressed(const Job& job, size_t folder, size_t operation)
    {
        if (folder >= job.sequence.size() || operation >= job.sequence[folder].children.size())
            return false;
        return job.sequence[folder].suppressed || job.sequence[folder].children[operation].suppressed;
    }

    bool MoveStep(Job& job, size_t from, size_t insertBefore, size_t& newIndex)
    {
        std::vector<SequenceStep>& sequence = job.sequence;
        if (from >= sequence.size() || insertBefore > sequence.size())
            return false;

        // Inserting just before or after itself leaves it where it is.
        const size_t target = (insertBefore > from) ? insertBefore - 1 : insertBefore;
        if (target == from)
            return false;

        std::vector<SequenceStep> moved = sequence;
        SequenceStep step = std::move(moved[from]);
        moved.erase(moved.begin() + static_cast<ptrdiff_t>(from));
        moved.insert(moved.begin() + static_cast<ptrdiff_t>(target), std::move(step));

        if (moved.front().type != StepType::SetOrigin)
            return false;

        sequence = std::move(moved);
        newIndex = target;
        return true;
    }

    bool MoveOperation(Job& job, size_t fromGroup, size_t fromIndex, size_t toGroup, size_t insertBefore,
                       size_t& newIndex)
    {
        std::vector<SequenceStep>& sequence = job.sequence;
        if (fromGroup >= sequence.size() || toGroup >= sequence.size() ||
            sequence[fromGroup].type != StepType::Group || sequence[toGroup].type != StepType::Group)
            return false;

        std::vector<SequenceStep>& source = sequence[fromGroup].children;
        std::vector<SequenceStep>& destination = sequence[toGroup].children;
        if (fromIndex >= source.size() || insertBefore > destination.size())
            return false;

        size_t target = insertBefore;
        if (fromGroup == toGroup)
        {
            target = (insertBefore > fromIndex) ? insertBefore - 1 : insertBefore;
            if (target == fromIndex)
                return false;
        }

        SequenceStep operation = std::move(source[fromIndex]);
        source.erase(source.begin() + static_cast<ptrdiff_t>(fromIndex));
        destination.insert(destination.begin() + static_cast<ptrdiff_t>(target), std::move(operation));
        newIndex = target;
        return true;
    }

    bool DuplicateStep(Job& job, size_t step, size_t operation, size_t& newIndex)
    {
        if (step >= job.sequence.size())
            return false;

        std::vector<SequenceStep>& list = (operation == kNoStepIndex) ? job.sequence : job.sequence[step].children;
        const size_t index = (operation == kNoStepIndex) ? step : operation;
        if (index >= list.size())
            return false;

        SequenceStep copy = list[index];
        copy.id = NewStableId();
        copy.name += " (copy)";
        for (SequenceStep& child : copy.children)
            child.id = NewStableId();

        list.insert(list.begin() + static_cast<ptrdiff_t>(index + 1), std::move(copy));
        newIndex = index + 1;
        return true;
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
