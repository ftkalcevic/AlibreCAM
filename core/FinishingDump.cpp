// FinishingDump.cpp : saves and loads a finishing operation's whole input.

#include "FinishingDump.h"

#include <fstream>
#include <memory>
#include <sstream>

#include <nlohmann/json.hpp>

#include "FileIO.h"
#include "ToolJson.h"

namespace camcore
{
    namespace
    {
        using nlohmann::json;
        const int kDumpVersion = 1;

        json Points(const std::vector<Vec3>& points)
        {
            json values = json::array();
            for (const Vec3& p : points)
            {
                values.push_back(p.x);
                values.push_back(p.y);
                values.push_back(p.z);
            }
            return values;
        }

        std::vector<Vec3> PointsFrom(const json& values)
        {
            std::vector<Vec3> points;
            for (size_t k = 0; k + 2 < values.size(); k += 3)
                points.push_back({ values[k].get<double>(), values[k + 1].get<double>(), values[k + 2].get<double>() });
            return points;
        }

        json PointJson(const Vec3& p) { return json::array({ p.x, p.y, p.z }); }
        Vec3 PointFrom(const json& v) { return { v[0].get<double>(), v[1].get<double>(), v[2].get<double>() }; }

        json SurfaceJson(const AnalyticSurface& s)
        {
            json item = { { "kind", static_cast<int>(s.kind) },
                          { "centre", PointJson(s.centre) },
                          { "axis", PointJson(s.axis) },
                          { "radius", s.radius },
                          { "minorRadius", s.minorRadius },
                          { "slope", s.slope },
                          { "onFaceTolerance", s.onFaceTolerance } };
            if (s.nurbs != nullptr)
            {
                const NurbsSurface& n = *s.nurbs;
                item["nurbs"] = { { "orderU", n.orderU }, { "orderV", n.orderV }, { "countU", n.countU },
                                  { "countV", n.countV }, { "knotsU", n.knotsU }, { "knotsV", n.knotsV },
                                  { "points", Points(n.points) }, { "weights", n.weights } };
            }
            return item;
        }

        AnalyticSurface SurfaceFrom(const json& item)
        {
            AnalyticSurface s;
            s.kind = static_cast<SurfaceKind>(item.at("kind").get<int>());
            s.centre = PointFrom(item.at("centre"));
            s.axis = PointFrom(item.at("axis"));
            s.radius = item.at("radius").get<double>();
            s.minorRadius = item.at("minorRadius").get<double>();
            s.slope = item.at("slope").get<double>();
            s.onFaceTolerance = item.at("onFaceTolerance").get<double>();
            if (item.contains("nurbs"))
            {
                const json& n = item["nurbs"];
                auto nurbs = std::make_shared<NurbsSurface>();
                nurbs->orderU = n.at("orderU").get<int>();
                nurbs->orderV = n.at("orderV").get<int>();
                nurbs->countU = n.at("countU").get<int>();
                nurbs->countV = n.at("countV").get<int>();
                nurbs->knotsU = n.at("knotsU").get<std::vector<double>>();
                nurbs->knotsV = n.at("knotsV").get<std::vector<double>>();
                nurbs->points = PointsFrom(n.at("points"));
                nurbs->weights = n.at("weights").get<std::vector<double>>();
                nurbs->Prepare();
                s.nurbs = nurbs;
            }
            return s;
        }

        std::filesystem::path JobPath(const std::filesystem::path& path)
        {
            std::filesystem::path job = path;
            job += ".cam";
            return job;
        }
    }

    bool SaveFinishingInput(const WaterlineRoughToolpathInput& input, const std::filesystem::path& path,
                            std::string& problem)
    {
        problem.clear();
        if (input.operation == nullptr || input.tool == nullptr)
        {
            problem = "There's no operation or tool to save.";
            return false;
        }
        try
        {
            // The operation, as a job of its own.
            Job job;
            job.assembly.name = "Finishing dump";
            job.assembly.guid = "{dump}";
            SequenceStep origin;
            origin.type = StepType::SetOrigin;
            origin.id = NewStableId();
            origin.name = "Set Origin 1";
            job.sequence.push_back(origin);
            SequenceStep group;
            group.type = StepType::Group;
            group.id = NewStableId();
            group.name = "Folder 1";
            SequenceStep operation = *input.operation;
            operation.toolpath = Toolpath();
            group.children.push_back(operation);
            job.sequence.push_back(group);
            const SaveResult saved = SaveJob(job, JobPath(path));
            if (!saved.ok)
            {
                problem = "The operation couldn't be saved.";
                return false;
            }

            json root = { { "version", kDumpVersion },
                          { "tool", ToolToJson(*input.tool) },
                          { "partTriangles", Points(input.partTriangles) },
                          { "partTriangleFaces", input.partTriangleFaces },
                          { "havePartMaxZ", input.havePartMaxZ },
                          { "partMaxZ", input.partMaxZ },
                          { "havePartMinZ", input.havePartMinZ },
                          { "partMinZ", input.partMinZ },
                          { "haveStockMaxZ", input.haveStockMaxZ },
                          { "stockMaxZ", input.stockMaxZ },
                          { "haveStockMinZ", input.haveStockMinZ },
                          { "stockMinZ", input.stockMinZ } };
            json faces = json::array();
            for (const std::vector<Vec3>& face : input.faces)
                faces.push_back(Points(face));
            root["faces"] = faces;
            json outlines = json::array();
            for (const ProfileChain& chain : input.outlines)
            {
                json vertices = json::array();
                for (const PathVertex& v : chain.path)
                    vertices.push_back({ { "point", PointJson(v.point) }, { "arc", v.arc },
                                         { "centre", PointJson(v.centre) }, { "axis", PointJson(v.axis) } });
                outlines.push_back({ { "ok", chain.ok }, { "closed", chain.closed }, { "path", vertices } });
            }
            root["outlines"] = outlines;
            json surfaces = json::array();
            for (const AnalyticSurface& s : input.faceSurfaces)
                surfaces.push_back(SurfaceJson(s));
            root["faceSurfaces"] = surfaces;
            std::string message;
            if (!WriteFileAtomically(path, root.dump(), message))
            {
                problem = message;
                return false;
            }
            return true;
        }
        catch (const std::exception& error)
        {
            problem = error.what();
            return false;
        }
    }

    bool LoadFinishingInput(const std::filesystem::path& path, FinishingDump& dump, std::string& problem)
    {
        problem.clear();
        try
        {
            const LoadResult loaded = LoadJob(JobPath(path));
            if (loaded.status != LoadStatus::Ok || loaded.job.sequence.size() < 2 ||
                loaded.job.sequence[1].children.empty())
            {
                problem = "The operation couldn't be read.";
                return false;
            }
            dump.operation = loaded.job.sequence[1].children[0];

            std::ifstream file(path, std::ios::binary);
            std::stringstream text;
            text << file.rdbuf();
            const json root = json::parse(text.str());
            if (root.at("version").get<int>() != kDumpVersion)
            {
                problem = "The dump is from another version.";
                return false;
            }
            dump.tool = ToolFromJson(root.at("tool"));
            WaterlineRoughToolpathInput& input = dump.input;
            input = WaterlineRoughToolpathInput();
            input.operation = &dump.operation;
            input.tool = &dump.tool;
            input.partTriangles = PointsFrom(root.at("partTriangles"));
            input.partTriangleFaces = root.at("partTriangleFaces").get<std::vector<int>>();
            input.havePartMaxZ = root.at("havePartMaxZ").get<bool>();
            input.partMaxZ = root.at("partMaxZ").get<double>();
            input.havePartMinZ = root.at("havePartMinZ").get<bool>();
            input.partMinZ = root.at("partMinZ").get<double>();
            input.haveStockMaxZ = root.at("haveStockMaxZ").get<bool>();
            input.stockMaxZ = root.at("stockMaxZ").get<double>();
            input.haveStockMinZ = root.at("haveStockMinZ").get<bool>();
            input.stockMinZ = root.at("stockMinZ").get<double>();
            for (const json& face : root.at("faces"))
                input.faces.push_back(PointsFrom(face));
            for (const json& outline : root.at("outlines"))
            {
                ProfileChain chain;
                chain.ok = outline.at("ok").get<bool>();
                chain.closed = outline.at("closed").get<bool>();
                for (const json& v : outline.at("path"))
                {
                    PathVertex vertex;
                    vertex.point = PointFrom(v.at("point"));
                    vertex.arc = v.at("arc").get<bool>();
                    vertex.centre = PointFrom(v.at("centre"));
                    vertex.axis = PointFrom(v.at("axis"));
                    chain.path.push_back(vertex);
                }
                input.outlines.push_back(chain);
            }
            for (const json& s : root.at("faceSurfaces"))
                input.faceSurfaces.push_back(SurfaceFrom(s));
            return true;
        }
        catch (const std::exception& error)
        {
            problem = error.what();
            return false;
        }
    }
}
