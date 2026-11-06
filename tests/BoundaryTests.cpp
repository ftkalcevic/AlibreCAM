// BoundaryTests.cpp : tests for closing outlines into a polygon.

#include <doctest/doctest.h>

#include <cmath>

#include "Boundary.h"
#include "CamJob.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>
#include <iterator>

using namespace camcore;

namespace
{
    bool At(const Vec3& point, double x, double y)
    {
        return std::fabs(point.x - x) < 1e-9 && std::fabs(point.y - y) < 1e-9;
    }
}

TEST_CASE("a face's loop edges, in any order and direction, make its outline")
{
    // A 10 by 5 rectangle's four edges, shuffled and some reversed.
    const std::vector<Polyline> edges =
    {
        { { 10, 5, 0 }, { 0, 5, 0 } },
        { { 0, 0, 0 }, { 10, 0, 0 } },
        { { 0, 0, 0 }, { 0, 5, 0 } },
        { { 10, 0, 0 }, { 10, 5, 0 } }
    };

    const Polyline polygon = ClosePolygon(edges, 2);
    REQUIRE(polygon.size() == 4);
    CHECK(SignedArea(polygon) == doctest::Approx(50));
    for (const Vec3& point : polygon)
        CHECK(point.z == 2);
}

TEST_CASE("open edges are joined across their gaps and closed")
{
    // Two separate lines, like the near and far sides of an area.
    const std::vector<Polyline> edges =
    {
        { { 0, 0, 3 }, { 20, 0, 3 } },
        { { 0, 10, 3 }, { 20, 10, 3 } }
    };

    const Polyline polygon = ClosePolygon(edges, 3);
    REQUIRE(polygon.size() == 4);

    // The second line is turned round to join the first's end, making a rectangle.
    CHECK(SignedArea(polygon) == doctest::Approx(200));
    CHECK(At(polygon[0], 0, 0));
    CHECK(At(polygon[1], 20, 0));
    CHECK(At(polygon[2], 20, 10));
    CHECK(At(polygon[3], 0, 10));
}

TEST_CASE("a closed curve's points stay, and a clockwise outline is turned anticlockwise")
{
    Polyline circle;
    for (int i = 0; i <= 36; ++i)
    {
        const double angle = -i * 3.14159265358979 * 2 / 36;   // clockwise, ending on its start
        circle.push_back({ 5 * std::cos(angle), 5 * std::sin(angle), 0 });
    }

    const Polyline polygon = ClosePolygon({ circle }, 0);
    CHECK(polygon.size() == 36);
    CHECK(SignedArea(polygon) > 0);
    CHECK(SignedArea(polygon) == doctest::Approx(3.14159265 * 25).epsilon(0.01));
}

TEST_CASE("too little to enclose anything gives no polygon")
{
    CHECK(ClosePolygon({}, 0).empty());
    CHECK(ClosePolygon({ { { 0, 0, 0 }, { 10, 0, 0 } } }, 0).empty());
    CHECK(ClosePolygon({ { { 0, 0, 0 }, { 0, 0, 0 } }, { { 0, 0, 0 } } }, 0).empty());
}

TEST_CASE("a facing's boundary loads back unchanged")
{
    static std::atomic<int> counter{ 0 };
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("camcore_boundary_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ".cam");

    Job job;
    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "O1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";

    SequenceStep facing;
    facing.type = StepType::Facing;
    facing.id = NewStableId();
    facing.name = "Facing 1";
    Path boundary = PathFromPoints(ClosePolygon({ { { 0, 0, 0 }, { 10, 0, 0 } }, { { 10, 0, 0 }, { 10, 6, 0 } } }, 4));

    // The closing side, from (10, 6) back to (0, 0), becomes an arc.
    boundary[2].arc = true;
    boundary[2].centre = { 5, 3, 4 };
    boundary[2].axis = { 0, 0, 1 };
    facing.boundaries = { boundary, PathFromPoints({ { 20, 0, 4 }, { 30, 0, 4 }, { 30, 5, 4 } }) };

    // An arc move goes with it.
    ToolpathMove arc;
    arc.kind = MoveKind::Feed;
    arc.to = { 1, 2, 3 };
    arc.feed = 500;
    arc.arc = true;
    arc.centre = { 0, 2, 3 };
    arc.clockwise = true;
    facing.toolpath.moves.push_back(arc);
    group.children.push_back(facing);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.job.sequence[1].children[0].boundaries.size() == 2);
    CHECK(loaded.job.sequence[1].children[0].boundaries[1].size() == 3);
    const Path& back = loaded.job.sequence[1].children[0].boundaries[0];
    REQUIRE(back.size() == 3);
    CHECK(At(back[2].point, 10, 6));
    CHECK(back[0].point.z == 4);
    CHECK_FALSE(back[0].arc);
    CHECK(back[2].arc);
    CHECK(At(back[2].centre, 5, 3));
    CHECK(back[2].axis.z == 1);

    const std::vector<ToolpathMove>& moves = loaded.job.sequence[1].children[0].toolpath.moves;
    REQUIRE(moves.size() == 1);
    CHECK(moves[0].kind == MoveKind::Feed);
    CHECK(moves[0].arc);
    CHECK(moves[0].clockwise);
    CHECK(At(moves[0].centre, 0, 2));
    CHECK(moves[0].feed == 500);
}

TEST_CASE("a version 2 file, with only points and straight moves, still loads")
{
    static std::atomic<int> counter{ 0 };
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("camcore_v2_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ".cam");

    Job job;
    job.assembly.name = "Test";
    job.assembly.guid = "{00000000-0000-0000-0000-000000000001}";
    SequenceStep origin;
    origin.type = StepType::SetOrigin;
    origin.id = NewStableId();
    origin.name = "Origin 1";
    job.sequence.push_back(origin);
    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "Folder 1";
    SequenceStep facing;
    facing.type = StepType::Facing;
    facing.id = NewStableId();
    facing.name = "Facing 1";
    facing.boundaries = { PathFromPoints({ { 0, 0, 1 }, { 10, 0, 1 }, { 10, 6, 1 } }) };
    ToolpathMove move;
    move.kind = MoveKind::Feed;
    move.to = { 1, 2, 3 };
    facing.toolpath.moves.push_back(move);
    group.children.push_back(facing);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);

    // Later versions write these just as version 2 did, so only the number differs.
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string current = "\"formatVersion\": " + std::to_string(kCamFormatVersion);
    const size_t at = text.find(current);
    REQUIRE(at != std::string::npos);
    text.replace(at, current.size(), "\"formatVersion\": 2");

    // Before several areas, the one boundary was saved on its own.
    nlohmann::json root = nlohmann::json::parse(text);
    nlohmann::json& saved = root["sequence"][1]["children"][0];
    REQUIRE(saved["boundaries"].size() == 1);
    saved["boundary"] = saved["boundaries"][0];
    saved.erase("boundaries");
    text = root.dump(2);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    const LoadResult loaded = LoadJob(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.fileVersion == 2);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    REQUIRE(back.boundaries.size() == 1);
    REQUIRE(back.boundaries[0].size() == 3);
    CHECK_FALSE(back.boundaries[0][1].arc);
    REQUIRE(back.toolpath.moves.size() == 1);
    CHECK_FALSE(back.toolpath.moves[0].arc);
}

TEST_CASE("a rounded outline of lines and arcs closes with its arcs kept")
{
    // A 20 by 10 slot: two straight sides and two half circles of radius 5,
    // given in any order, one arc backwards.
    auto arcPiece = [](Vec3 from, Vec3 to, Vec3 centre, double axisZ) {
        PathVertex start;
        start.point = from;
        start.arc = true;
        start.centre = centre;
        start.axis = { 0, 0, axisZ };
        PathVertex end;
        end.point = to;
        return Path{ start, end };
    };
    const std::vector<Path> pieces = {
        PathFromPoints({ { 0, 0, 0 }, { 20, 0, 0 } }),
        arcPiece({ 0, 10, 0 }, { 0, 0, 0 }, { 0, 5, 0 }, 1),        // the left end, anticlockwise
        PathFromPoints({ { 20, 10, 0 }, { 0, 10, 0 } }),
        arcPiece({ 20, 10, 0 }, { 20, 0, 0 }, { 20, 5, 0 }, -1)     // the right end, given backwards
    };

    const Path path = ClosePath(pieces, 1);
    REQUIRE(path.size() == 4);
    CHECK(SignedArea(path) == doctest::Approx(200 + 3.14159265358979 * 25));

    size_t arcs = 0;
    for (const PathVertex& vertex : path)
    {
        CHECK(vertex.point.z == 1);
        if (vertex.arc)
        {
            ++arcs;
            CHECK(ArcIsAnticlockwise(vertex));      // the whole path turns anticlockwise
        }
    }
    CHECK(arcs == 2);

    // The right end reaches out to x 25, and across at y 5 the slot runs -5 to 25.
    double low = 0, high = 0;
    PathExtent(path, { 1, 0, 0 }, low, high);
    CHECK(low == doctest::Approx(-5));
    CHECK(high == doctest::Approx(25));
    std::vector<double> crossings = PathCrossings(path, { 1, 0, 0 }, { 0, 1, 0 }, 5);
    REQUIRE(crossings.size() >= 2);
    CHECK(*std::min_element(crossings.begin(), crossings.end()) == doctest::Approx(-5));
    CHECK(*std::max_element(crossings.begin(), crossings.end()) == doctest::Approx(25));

    // At y 9 the ends are 3 in from the arcs' widest.
    crossings = PathCrossings(path, { 1, 0, 0 }, { 0, 1, 0 }, 9);
    CHECK(*std::max_element(crossings.begin(), crossings.end()) == doctest::Approx(23));
}

TEST_CASE("a full circle from two half arcs is exact, and turned anticlockwise")
{
    PathVertex a;
    a.point = { 5, 0, 0 };
    a.arc = true;
    a.axis = { 0, 0, -1 };     // clockwise
    PathVertex b;
    b.point = { -5, 0, 0 };
    b.arc = true;
    b.axis = { 0, 0, -1 };
    const Path path = ClosePath({ Path{ a, b, a } }, 0);
    REQUIRE(path.size() == 2);
    CHECK(SignedArea(path) == doctest::Approx(3.14159265358979 * 25));
    CHECK(ArcIsAnticlockwise(path[0]));
    CHECK(ArcIsAnticlockwise(path[1]));

    // Drawn, it stays on the circle.
    for (const Vec3& point : PathPoints(path, true))
        CHECK(std::hypot(point.x, point.y) == doctest::Approx(5));
}

TEST_CASE("an L of two edges closes into a triangle")
{
    const Polyline polygon = ClosePolygon({ { { 0, 0, 0 }, { 10, 0, 0 } }, { { 10, 0, 0 }, { 10, 6, 0 } } }, 0);
    REQUIRE(polygon.size() == 3);
    CHECK(SignedArea(polygon) == doctest::Approx(30));
}
