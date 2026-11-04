// BoundaryTests.cpp : tests for closing outlines into a polygon.

#include <doctest/doctest.h>

#include <cmath>

#include "Boundary.h"
#include "CamJob.h"

#include <atomic>
#include <chrono>

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
    facing.boundary = ClosePolygon({ { { 0, 0, 0 }, { 10, 0, 0 } }, { { 10, 0, 0 }, { 10, 6, 0 } } }, 4);
    group.children.push_back(facing);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    REQUIRE(loaded.status == LoadStatus::Ok);
    const Polyline& back = loaded.job.sequence[1].children[0].boundary;
    REQUIRE(back.size() == 3);
    CHECK(At(back[2], 10, 6));
    CHECK(back[0].z == 4);
}

TEST_CASE("an L of two edges closes into a triangle")
{
    const Polyline polygon = ClosePolygon({ { { 0, 0, 0 }, { 10, 0, 0 } }, { { 10, 0, 0 }, { 10, 6, 0 } } }, 0);
    REQUIRE(polygon.size() == 3);
    CHECK(SignedArea(polygon) == doctest::Approx(30));
}
