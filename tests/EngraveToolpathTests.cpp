// EngraveToolpathTests.cpp : tests for joining engraving lines and their toolpaths.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "CamJob.h"
#include "EngraveToolpath.h"

using namespace camcore;

namespace
{
    Path Line(Vec3 from, Vec3 to) { return PathFromPoints({ from, to }); }

    bool At(const Vec3& point, double x, double y)
    {
        return std::fabs(point.x - x) < 1e-6 && std::fabs(point.y - y) < 1e-6;
    }

    SequenceStep EngraveStep(double total, double perCut, EngraveTraversal traversal)
    {
        SequenceStep step;
        step.type = StepType::Engrave;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 10;
        step.feeds.plunge = 100;
        step.feeds.cut = 1000;
        step.cutLevels.totalDepth = total;
        step.cutLevels.roughDepth = total;
        step.cutLevels.finishDepth = 0;
        step.cutLevels.roughStep = perCut;
        step.engraving.traversal = traversal;
        return step;
    }

    ToolpathResult Generate(const SequenceStep& step, const std::vector<EngraveLine>& lines)
    {
        static Tool tool;
        tool.diameter = 3;
        EngraveToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        input.lines = lines;
        return GenerateEngraveToolpath(input);
    }

    // The feed moves' end points at a height.
    std::vector<Vec3> FeedsAt(const Toolpath& toolpath, double z)
    {
        std::vector<Vec3> points;
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.kind == MoveKind::Feed && std::fabs(move.to.z - z) < 1e-6)
                points.push_back(move.to);
        }
        return points;
    }

    int Rapids(const Toolpath& toolpath)
    {
        int count = 0;
        for (const ToolpathMove& move : toolpath.moves)
            count += (move.kind == MoveKind::Rapid) ? 1 : 0;
        return count;
    }
}

TEST_CASE("pieces whose ends meet join into one line, turned round where needed")
{
    // An L in two pieces, the second backwards, and a separate stroke.
    const std::vector<EngraveLine> lines = JoinEngraveLines(
        { Line({ 0, 0, 0 }, { 10, 0, 0 }), Line({ 10, 10, 0 }, { 10, 0, 0 }), Line({ 20, 0, 0 }, { 30, 0, 0 }) }, 2);
    REQUIRE(lines.size() == 2);
    CHECK_FALSE(lines[0].closed);
    REQUIRE(lines[0].path.size() == 3);
    CHECK(At(lines[0].path[0].point, 0, 0));
    CHECK(At(lines[0].path[1].point, 10, 0));
    CHECK(At(lines[0].path[2].point, 10, 10));
    CHECK(lines[0].path[0].point.z == 2);       // flat at the level
    CHECK(lines[1].path.size() == 2);
}

TEST_CASE("a line starts at a loose end, and four edges round a square close")
{
    // The first piece's start joins the second, so the line starts from its other, loose end.
    const std::vector<EngraveLine> open = JoinEngraveLines(
        { Line({ 10, 0, 0 }, { 0, 0, 0 }), Line({ 0, 0, 0 }, { 0, 10, 0 }) }, 0);
    REQUIRE(open.size() == 1);
    CHECK(At(open[0].path.front().point, 10, 0));
    CHECK(At(open[0].path.back().point, 0, 10));

    const std::vector<EngraveLine> square = JoinEngraveLines(
        { Line({ 0, 0, 0 }, { 10, 0, 0 }), Line({ 0, 10, 0 }, { 10, 10, 0 }), Line({ 10, 0, 0 }, { 10, 10, 0 }),
          Line({ 0, 0, 0 }, { 0, 10, 0 }) },
        0);
    REQUIRE(square.size() == 1);
    CHECK(square[0].closed);
    CHECK(square[0].path.size() == 4);          // no repeated first point
}

TEST_CASE("where three pieces meet, the line carries on along the earliest and the other starts its own")
{
    const std::vector<EngraveLine> lines = JoinEngraveLines(
        { Line({ 0, 0, 0 }, { 10, 0, 0 }), Line({ 10, 0, 0 }, { 20, 0, 0 }), Line({ 10, 0, 0 }, { 10, 10, 0 }) }, 0);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].path.size() == 3);
    CHECK(At(lines[0].path.back().point, 20, 0));
    CHECK(lines[1].path.size() == 2);
}

TEST_CASE("return to start runs every level the same way, lifting between")
{
    const SequenceStep step = EngraveStep(1, 0.5, EngraveTraversal::ReturnToStart);
    const ToolpathResult result = Generate(step, JoinEngraveLines({ Line({ 0, 0, 0 }, { 10, 0, 0 }) }, 0));
    REQUIRE_MESSAGE(result.ok, result.problem);

    // Down at the start and along to the end, at -0.5 and then -1.
    for (double z : { -0.5, -1.0 })
    {
        const std::vector<Vec3> points = FeedsAt(result.toolpath, z);
        REQUIRE(points.size() == 2);
        CHECK(At(points[0], 0, 0));
        CHECK(At(points[1], 10, 0));
    }
    CHECK(Rapids(result.toolpath) == 4);        // over, up, back over, up
}

TEST_CASE("stay down goes straight down and runs back the other way")
{
    const SequenceStep step = EngraveStep(1, 0.5, EngraveTraversal::StayDown);
    const ToolpathResult result = Generate(step, JoinEngraveLines({ Line({ 0, 0, 0 }, { 10, 0, 0 }) }, 0));
    REQUIRE_MESSAGE(result.ok, result.problem);

    const std::vector<Vec3> second = FeedsAt(result.toolpath, -1.0);
    REQUIRE(second.size() == 2);
    CHECK(At(second[0], 10, 0));                // straight down at the end
    CHECK(At(second[1], 0, 0));                 // and back
    CHECK(Rapids(result.toolpath) == 2);        // over at the start, up at the end
}

TEST_CASE("a closed loop that stays down carries on round the same way")
{
    const SequenceStep step = EngraveStep(1, 0.5, EngraveTraversal::StayDown);
    const std::vector<EngraveLine> square = JoinEngraveLines(
        { Line({ 0, 0, 0 }, { 10, 0, 0 }), Line({ 10, 0, 0 }, { 10, 10, 0 }), Line({ 10, 10, 0 }, { 0, 10, 0 }),
          Line({ 0, 10, 0 }, { 0, 0, 0 }) },
        0);
    const ToolpathResult result = Generate(step, square);
    REQUIRE_MESSAGE(result.ok, result.problem);

    const std::vector<Vec3> second = FeedsAt(result.toolpath, -1.0);
    REQUIRE(second.size() == 5);                // down at the start, then round
    CHECK(At(second[1], 10, 0));
    CHECK(At(second[4], 0, 0));
}

TEST_CASE("an engrave operation saves its traversal")
{
    CHECK(IsOperation(StepType::Engrave));
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "AlibreCAM-engrave-test.cam";

    Job job;
    job.assembly.name = "Plate";
    job.assembly.guid = "{guid}";
    SequenceStep origin;
    origin.type = StepType::SetOrigin;
    origin.id = NewStableId();
    origin.name = "Set Origin 1";
    job.sequence.push_back(origin);
    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "Folder 1";
    SequenceStep engrave = EngraveStep(1, 0.5, EngraveTraversal::StayDown);
    engrave.id = NewStableId();
    engrave.name = "Engrave 1";
    group.children.push_back(engrave);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(back.type == StepType::Engrave);
    CHECK(back.engraving.traversal == EngraveTraversal::StayDown);
    CHECK(back.cutLevels.totalDepth == 1);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("an engraving needs a tool and lines")
{
    const SequenceStep step = EngraveStep(1, 0.5, EngraveTraversal::ReturnToStart);
    CHECK_FALSE(Generate(step, {}).ok);
    EngraveToolpathInput input;
    input.operation = &step;
    input.lines = JoinEngraveLines({ Line({ 0, 0, 0 }, { 10, 0, 0 }) }, 0);
    CHECK_FALSE(GenerateEngraveToolpath(input).ok);
}
