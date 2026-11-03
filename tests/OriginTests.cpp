// OriginTests.cpp : tests for CAM origins and Set Origin steps in the .cam file.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>

#include "CamJob.h"
#include "Origin.h"

namespace fs = std::filesystem;
using namespace camcore;

namespace
{
    const double kTolerance = 1e-9;

    bool Near(const Vec3& a, const Vec3& b)
    {
        return Length(a - b) < kTolerance;
    }

    // Checks the axes are unit length, square to each other, and right-handed.
    void CheckRightHanded(const Frame& frame)
    {
        CHECK(std::fabs(Length(frame.xAxis) - 1.0) < kTolerance);
        CHECK(std::fabs(Length(frame.yAxis) - 1.0) < kTolerance);
        CHECK(std::fabs(Length(frame.zAxis) - 1.0) < kTolerance);
        CHECK(std::fabs(Dot(frame.xAxis, frame.yAxis)) < kTolerance);
        CHECK(std::fabs(Dot(frame.xAxis, frame.zAxis)) < kTolerance);
        CHECK(Near(Cross(frame.xAxis, frame.yAxis), frame.zAxis));
    }

    PickedGeometry Face(const Vec3& point, const Vec3& normal)
    {
        PickedGeometry pick;
        pick.kind = PickKind::PlanarFace;
        pick.point = point;
        pick.direction = normal;
        pick.clicked = point;
        return pick;
    }

    PickedGeometry Line(const Vec3& start, const Vec3& end, const Vec3& clicked)
    {
        PickedGeometry pick;
        pick.kind = PickKind::LineEdge;
        pick.start = start;
        pick.end = end;
        pick.point = start;
        pick.direction = end - start;
        pick.clicked = clicked;
        return pick;
    }

    PickedGeometry Circle(const Vec3& centre, const Vec3& axis)
    {
        PickedGeometry pick;
        pick.kind = PickKind::CircleEdge;
        pick.point = centre;
        pick.direction = axis;
        pick.clicked = centre + Vec3{ 3.0, 0.0, 0.0 };
        return pick;
    }

    class TempFile
    {
    public:
        TempFile()
        {
            static std::atomic<int> counter{ 0 };
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                     ("camcore_origin_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ".cam");
        }

        ~TempFile()
        {
            std::error_code ignored;
            fs::remove(m_path, ignored);
        }

        const fs::path& Path() const { return m_path; }

    private:
        fs::path m_path;
    };
}

TEST_CASE("a top face and a corner edge give an origin square to the assembly")
{
    OriginDefinition definition;
    definition.plane = Face({ 0, 0, 20 }, { 0, 0, 1 });
    definition.position = Line({ 0, 0, 20 }, { 50, 0, 20 }, { 2, 0, 20 });

    const FrameResult result = BuildFrame(definition);
    REQUIRE(result.ok);
    CHECK(result.message.empty());
    CHECK(Near(result.frame.origin, { 0, 0, 20 }));
    CHECK(Near(result.frame.zAxis, { 0, 0, 1 }));
    CHECK(Near(result.frame.xAxis, { 1, 0, 0 }));
    CHECK(Near(result.frame.yAxis, { 0, 1, 0 }));
}

TEST_CASE("a line edge gives the end nearer the click")
{
    Vec3 position;
    REQUIRE(PositionFromPick(Line({ 0, 0, 0 }, { 50, 0, 0 }, { 48, 0, 0 }), position));
    CHECK(Near(position, { 50, 0, 0 }));

    REQUIRE(PositionFromPick(Line({ 0, 0, 0 }, { 50, 0, 0 }, { 1, 0, 0 }), position));
    CHECK(Near(position, { 0, 0, 0 }));

    REQUIRE(PositionFromPick(Circle({ 5, 6, 7 }, { 0, 0, 1 }), position));
    CHECK(Near(position, { 5, 6, 7 }));

    CHECK_FALSE(PositionFromPick(Face({ 0, 0, 0 }, { 0, 0, 1 }), position));
}

TEST_CASE("a circle edge can set both the plane and the position")
{
    OriginDefinition definition;
    definition.plane = Circle({ 10, 20, 5 }, { 0, 0, 2 });

    const FrameResult result = BuildFrame(definition);
    REQUIRE(result.ok);
    CHECK(Near(result.frame.origin, { 10, 20, 5 }));
    CHECK(Near(result.frame.zAxis, { 0, 0, 1 }));
    CheckRightHanded(result.frame);
}

TEST_CASE("a face alone has no position")
{
    OriginDefinition definition;
    definition.plane = Face({ 0, 0, 0 }, { 0, 0, 1 });

    const FrameResult result = BuildFrame(definition);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.message.empty());
}

TEST_CASE("missing or unsuitable picks are refused")
{
    SUBCASE("no plane")
    {
        CHECK_FALSE(BuildFrame(OriginDefinition()).ok);
    }

    SUBCASE("a face as the position")
    {
        OriginDefinition definition;
        definition.plane = Face({ 0, 0, 0 }, { 0, 0, 1 });
        definition.position = Face({ 0, 0, 0 }, { 0, 0, 1 });
        CHECK_FALSE(BuildFrame(definition).ok);
    }

    SUBCASE("a circle as the X direction")
    {
        OriginDefinition definition;
        definition.plane = Circle({ 0, 0, 0 }, { 0, 0, 1 });
        definition.xDirection = Circle({ 0, 0, 0 }, { 1, 0, 0 });
        CHECK_FALSE(BuildFrame(definition).ok);
    }

    SUBCASE("an X edge along the tool axis")
    {
        OriginDefinition definition;
        definition.plane = Circle({ 0, 0, 0 }, { 0, 0, 1 });
        definition.xDirection = Line({ 0, 0, 0 }, { 0, 0, 10 }, { 0, 0, 1 });
        CHECK_FALSE(BuildFrame(definition).ok);
    }

    SUBCASE("a plane pick with no direction")
    {
        OriginDefinition definition;
        definition.plane = Circle({ 0, 0, 0 }, { 0, 0, 0 });
        CHECK_FALSE(BuildFrame(definition).ok);
    }
}

TEST_CASE("an X edge sets X, pointing away from the end nearer the click")
{
    OriginDefinition definition;
    definition.plane = Circle({ 0, 0, 0 }, { 0, 0, 1 });

    // A diagonal edge that also rises, so X has to be projected onto the plane.
    definition.xDirection = Line({ 10, 10, 0 }, { 0, 0, 5 }, { 9, 9, 0 });

    const FrameResult result = BuildFrame(definition);
    REQUIRE(result.ok);
    const double half = std::sqrt(0.5);
    CHECK(Near(result.frame.xAxis, { -half, -half, 0 }));
    CheckRightHanded(result.frame);
}

TEST_CASE("without an X edge, X follows assembly X, or assembly Y when X is the tool axis")
{
    SUBCASE("tilted plane")
    {
        OriginDefinition definition;
        definition.plane = Circle({ 0, 0, 0 }, { 1, 0, 1 });

        const FrameResult result = BuildFrame(definition);
        REQUIRE(result.ok);
        const double half = std::sqrt(0.5);
        CHECK(Near(result.frame.xAxis, { half, 0, -half }));
        CheckRightHanded(result.frame);
    }

    SUBCASE("side plane facing +X")
    {
        OriginDefinition definition;
        definition.plane = Circle({ 0, 0, 0 }, { 1, 0, 0 });

        const FrameResult result = BuildFrame(definition);
        REQUIRE(result.ok);
        CHECK(Near(result.frame.xAxis, { 0, 1, 0 }));
        CHECK(Near(result.frame.yAxis, { 0, 0, 1 }));
        CheckRightHanded(result.frame);
    }
}

TEST_CASE("flipping Z keeps the frame right-handed")
{
    OriginDefinition definition;
    definition.plane = Circle({ 0, 0, 0 }, { 0, 0, 1 });
    definition.options.flipZ = true;

    const FrameResult result = BuildFrame(definition);
    REQUIRE(result.ok);
    CHECK(Near(result.frame.zAxis, { 0, 0, -1 }));
    CHECK(Near(result.frame.xAxis, { 1, 0, 0 }));
    CHECK(Near(result.frame.yAxis, { 0, -1, 0 }));
    CheckRightHanded(result.frame);
}

TEST_CASE("quarter turns rotate X about Z")
{
    OriginDefinition definition;
    definition.plane = Circle({ 0, 0, 0 }, { 0, 0, 1 });

    const Vec3 expected[4] = { { 1, 0, 0 }, { 0, 1, 0 }, { -1, 0, 0 }, { 0, -1, 0 } };
    for (int turns = 0; turns < 4; ++turns)
    {
        CAPTURE(turns);
        definition.options.xQuarterTurns = turns;
        const FrameResult result = BuildFrame(definition);
        REQUIRE(result.ok);
        CHECK(Near(result.frame.xAxis, expected[turns]));
        CheckRightHanded(result.frame);
    }
}

TEST_CASE("offsets move the origin along its own axes")
{
    OriginDefinition definition;
    definition.plane = Circle({ 100, 0, 0 }, { 1, 0, 0 });     // X is assembly Y, Y is assembly Z
    definition.options.offset = { 1, 2, 3 };

    const FrameResult result = BuildFrame(definition);
    REQUIRE(result.ok);
    CHECK(Near(result.frame.origin, { 103, 1, 2 }));
}

TEST_CASE("the plane rectangle covers the box with a margin")
{
    Frame frame;
    frame.origin = { 0, 0, 10 };

    const PlaneRectangle rectangle = PlaneCoveringBox(frame, { 0, 0, 0 }, { 100, 50, 10 }, 0.1);
    CHECK(Near(rectangle.corners[0], { -10, -10, 10 }));
    CHECK(Near(rectangle.corners[1], { 110, -10, 10 }));
    CHECK(Near(rectangle.corners[2], { 110, 60, 10 }));
    CHECK(Near(rectangle.corners[3], { -10, 60, 10 }));
}

TEST_CASE("the plane rectangle covers a box seen from the side")
{
    OriginDefinition definition;
    definition.plane = Circle({ 0, 0, 0 }, { 1, 0, 0 });
    const Frame frame = BuildFrame(definition).frame;

    const PlaneRectangle rectangle = PlaneCoveringBox(frame, { -5, 0, 0 }, { 5, 40, 20 }, 0.0);
    for (const Vec3& corner : rectangle.corners)
        CHECK(std::fabs(corner.x) < kTolerance);
    CHECK(Near(rectangle.corners[0], { 0, 0, 0 }));
    CHECK(Near(rectangle.corners[2], { 0, 40, 20 }));
}

TEST_CASE("an empty box gives a small square plane")
{
    Frame frame;
    const PlaneRectangle rectangle = PlaneCoveringBox(frame, { 0, 0, 0 }, { 0, 0, 0 }, 0.1);
    CHECK(Near(rectangle.corners[0], { -10, -10, 0 }));
    CHECK(Near(rectangle.corners[2], { 10, 10, 0 }));
}

TEST_CASE("Set Origin steps load back unchanged")
{
    TempFile file;

    Job job;
    job.assembly.name = "Bracket";
    job.assembly.guid = "guid";

    SequenceStep step;
    step.id = NewStableId();
    step.name = "Set Origin 1";
    step.origin.plane = Face({ 1, 2, 3 }, { 0, 0, 1 });
    step.origin.plane.occurrenceKey = { 17, "0A0B" };
    step.origin.plane.targetKey = { 17, "FF00" };
    step.origin.plane.occurrenceName = "Plate<1>";
    step.origin.position = Line({ 0, 0, 3 }, { 9, 0, 3 }, { 1, 0, 3 });
    step.origin.options.flipZ = true;
    step.origin.options.xQuarterTurns = 3;
    step.origin.options.offset = { 0.5, -1, 2 };
    job.sequence.push_back(step);

    SequenceStep second;
    second.id = NewStableId();
    second.name = "Side";
    job.sequence.push_back(second);

    REQUIRE(SaveJob(job, file.Path()).ok);

    const LoadResult loaded = LoadJob(file.Path());
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.job.sequence.size() == 2);

    const SequenceStep& back = loaded.job.sequence[0];
    CHECK(back.type == StepType::SetOrigin);
    CHECK(back.id == step.id);
    CHECK(back.name == "Set Origin 1");
    CHECK(back.origin.plane.kind == PickKind::PlanarFace);
    CHECK(back.origin.plane.occurrenceKey.elementType == 17);
    CHECK(back.origin.plane.occurrenceKey.hex == "0A0B");
    CHECK(back.origin.plane.targetKey.hex == "FF00");
    CHECK(back.origin.plane.occurrenceName == "Plate<1>");
    CHECK(Near(back.origin.plane.point, { 1, 2, 3 }));
    CHECK(back.origin.position.kind == PickKind::LineEdge);
    CHECK(Near(back.origin.position.end, { 9, 0, 3 }));
    CHECK(Near(back.origin.position.clicked, { 1, 0, 3 }));
    CHECK(back.origin.xDirection.kind == PickKind::None);
    CHECK(back.origin.options.flipZ);
    CHECK(back.origin.options.xQuarterTurns == 3);
    CHECK(Near(back.origin.options.offset, { 0.5, -1, 2 }));

    CHECK(loaded.job.sequence[1].origin.plane.kind == PickKind::None);
}

TEST_CASE("a damaged Set Origin step stops the file loading")
{
    TempFile file;
    const auto write = [&](const std::string& step)
    {
        std::ofstream out(file.Path(), std::ios::binary);
        out << R"({"format":"AlibreCAM","formatVersion":2,"assembly":{"name":"a","guid":"b"},"sequence":[)" << step
            << "]}";
    };

    const std::string options = R"("options":{"flipZ":false,"xQuarterTurns":0,"offset":[0,0,0]})";

    write(R"({"type":"setOrigin","id":"x","name":"n",)" + options + "}");
    CHECK(LoadJob(file.Path()).status == LoadStatus::Ok);

    write(R"({"type":"drill","id":"x","name":"n",)" + options + "}");
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);

    write(R"({"type":"setOrigin","id":"","name":"n",)" + options + "}");
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);

    write(R"({"type":"setOrigin","id":"x","name":"n","plane":{"kind":"sphere"},)" + options + "}");
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);

    write(R"({"type":"setOrigin","id":"x","name":"n","options":{"flipZ":false,"xQuarterTurns":4,"offset":[0,0,0]}})");
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);
}

TEST_CASE("stable IDs are long and unique")
{
    CHECK(NewStableId().size() == 32);
    CHECK(NewStableId() != NewStableId());
}
