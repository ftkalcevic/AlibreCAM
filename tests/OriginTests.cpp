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

    SequenceStep NamedStepForDrill(StepType type, const std::string& name)
    {
        SequenceStep step;
        step.type = type;
        step.id = NewStableId();
        step.name = name;
        return step;
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

TEST_CASE("grid spacing is a round number giving about the asked-for squares")
{
    CHECK(GridSpacing(100, 10) == doctest::Approx(10));
    CHECK(GridSpacing(120, 10) == doctest::Approx(20));
    CHECK(GridSpacing(300, 10) == doctest::Approx(50));
    CHECK(GridSpacing(7, 10) == doctest::Approx(1));
    CHECK(GridSpacing(0, 10) == 0);
}

TEST_CASE("the grid runs through the origin and stops inside the outline")
{
    Frame frame;
    frame.origin = { 0, 0, 10 };

    // -10 to 110 by -10 to 60: a 120 mm longer side gives 20 mm squares.
    const PlaneRectangle rectangle = PlaneCoveringBox(frame, { 0, 0, 0 }, { 100, 50, 10 }, 0.1);
    const std::vector<LineSegment> lines = PlaneGrid(frame, rectangle, 10);

    // X = 0, 20 ... 100 across, and Y = 0, 20, 40 along.
    REQUIRE(lines.size() == 9);
    CHECK(Near(lines[0].from, { 0, -10, 10 }));
    CHECK(Near(lines[0].to, { 0, 60, 10 }));
    CHECK(Near(lines[5].from, { 100, -10, 10 }));
    CHECK(Near(lines[6].from, { -10, 0, 10 }));
    CHECK(Near(lines[8].to, { 110, 40, 10 }));
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

TEST_CASE("groups load back in their place in the sequence")
{
    TempFile file;

    Job job;
    job.assembly.name = "Bracket";
    job.assembly.guid = "guid";

    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "Set Origin 1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "Roughing";
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, file.Path()).ok);

    const LoadResult loaded = LoadJob(file.Path());
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.job.sequence.size() == 2);
    CHECK(loaded.job.sequence[0].type == StepType::SetOrigin);
    CHECK(loaded.job.sequence[1].type == StepType::Group);
    CHECK(loaded.job.sequence[1].id == group.id);
    CHECK(loaded.job.sequence[1].name == "Roughing");
    CHECK(loaded.job.sequence[1].children.empty());
}

TEST_CASE("a group holding a Set Origin or another group stops the file loading")
{
    TempFile file;
    const auto write = [&](const std::string& children)
    {
        std::ofstream out(file.Path(), std::ios::binary);
        out << R"({"format":"AlibreCAM","formatVersion":2,"assembly":{"name":"a","guid":"b"},"sequence":[)"
            << R"({"type":"group","id":"g","name":"Roughing","children":)" << children << "}]}";
    };

    write("[]");
    CHECK(LoadJob(file.Path()).status == LoadStatus::Ok);

    write(R"([{"type":"group","id":"h","name":"Inner","children":[]}])");
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);

    write(R"([{"type":"setOrigin","id":"o","name":"n","options":{"flipZ":false,"xQuarterTurns":0,"offset":[0,0,0]}}])");
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);

    write("{}");
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);
}

TEST_CASE("drill operations load back inside their group")
{
    TempFile file;

    Job job;
    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "Set Origin 1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "Holes";

    SequenceStep drill;
    drill.type = StepType::Drill;
    drill.id = NewStableId();
    drill.name = "Drill 1";
    group.children.push_back(drill);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, file.Path()).ok);

    const LoadResult loaded = LoadJob(file.Path());
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.job.sequence.size() == 2);
    REQUIRE(loaded.job.sequence[1].children.size() == 1);
    CHECK(loaded.job.sequence[1].children[0].type == StepType::Drill);
    CHECK(loaded.job.sequence[1].children[0].id == drill.id);
    CHECK(loaded.job.sequence[1].children[0].name == "Drill 1");
}

TEST_CASE("drilling settings load back unchanged")
{
    TempFile file;

    Job job;
    job.sequence.push_back(NamedStepForDrill(StepType::SetOrigin, "O1"));
    SequenceStep group = NamedStepForDrill(StepType::Group, "F1");
    SequenceStep drill = NamedStepForDrill(StepType::Drill, "D1");
    drill.drill.depth = 12.5;
    drill.drill.addTipToDepth = true;
    drill.drill.location = GeometryLocation::AtBottom;
    drill.drill.dwell = DwellMode::Revolutions;
    drill.drill.dwellSeconds = 1.25;
    drill.drill.dwellRevolutions = 4;
    drill.drill.approachDistance = 3;
    drill.drill.pecking = true;
    drill.drill.peckDepth = 1.5;
    group.children.push_back(drill);

    SequenceStep plain = NamedStepForDrill(StepType::Drill, "D2");
    group.children.push_back(plain);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, file.Path()).ok);
    const LoadResult loaded = LoadJob(file.Path());
    REQUIRE(loaded.status == LoadStatus::Ok);

    const DrillSettings& back = loaded.job.sequence[1].children[0].drill;
    CHECK(back.depth == 12.5);
    CHECK(back.addTipToDepth);
    CHECK(back.location == GeometryLocation::AtBottom);
    CHECK(back.dwell == DwellMode::Revolutions);
    CHECK(back.dwellSeconds == 1.25);
    CHECK(back.dwellRevolutions == 4);
    CHECK(back.approachDistance == 3);
    CHECK(back.pecking);
    CHECK(back.peckDepth == 1.5);

    const DrillSettings& defaults = loaded.job.sequence[1].children[1].drill;
    CHECK_FALSE(defaults.pecking);
    CHECK_FALSE(defaults.addTipToDepth);
    CHECK(defaults.dwell == DwellMode::Off);
}

TEST_CASE("drills without drilling settings load with defaults, and damaged ones don't load")
{
    TempFile file;
    const auto write = [&](const std::string& drill)
    {
        std::ofstream out(file.Path(), std::ios::binary);
        out << R"({"format":"AlibreCAM","formatVersion":2,"assembly":{"name":"a","guid":"b"},"sequence":[)"
            << R"({"type":"group","id":"g","name":"F","children":[)" << drill << "]}]}";
    };

    write(R"({"type":"drill","id":"d","name":"D1"})");
    const LoadResult loaded = LoadJob(file.Path());
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.sequence[0].children[0].drill.approachDistance == DrillSettings().approachDistance);

    const std::string good = R"("depth":5,"addDrillTip":false,"geometryLocation":"top","dwell":"off","dwellSeconds":1,"dwellRevolutions":2,"approachDistance":2,"peckDepth":null)";
    write(R"({"type":"drill","id":"d","name":"D1","drilling":{)" + good + "}}");
    CHECK(LoadJob(file.Path()).status == LoadStatus::Ok);

    std::string bad = good;
    bad.replace(bad.find("\"top\""), 5, "\"middle\"");
    write(R"({"type":"drill","id":"d","name":"D1","drilling":{)" + bad + "}}");
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);

    bad = good;
    bad.replace(bad.find("null"), 4, "\"deep\"");
    write(R"({"type":"drill","id":"d","name":"D1","drilling":{)" + bad + "}}");
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);
}

TEST_CASE("drilling settings are checked")
{
    DrillSettings settings;
    CHECK(ValidateDrillSettings(settings).size() == 1);     // no depth yet

    settings.depth = 10;
    CHECK(ValidateDrillSettings(settings).empty());

    settings.dwell = DwellMode::Seconds;
    settings.dwellSeconds = 0;
    settings.dwellRevolutions = 0;      // not used while dwelling by time
    settings.approachDistance = -1;
    settings.pecking = true;
    settings.peckDepth = 0;
    CHECK(ValidateDrillSettings(settings).size() == 3);
}

TEST_CASE("Locate measures along the origin's Z")
{
    Frame frame;
    frame.origin = { 0, 0, 20 };

    LocatedGeometry point;
    point.point = { 5, 5, 8 };
    CHECK(DistanceAlongZ(frame, point) == doctest::Approx(12));

    LocatedGeometry plane;
    plane.kind = LocatedGeometry::Kind::Plane;
    plane.point = { 50, -3, 32 };
    plane.direction = { 0, 0, -1 };
    CHECK(DistanceAlongZ(frame, plane) == doctest::Approx(12));

    // A tilted plane is measured where the drill axis meets it: x + z = 10 meets it at z = 10.
    plane.point = { 10, 0, 0 };
    plane.direction = { 1, 0, 1 };
    CHECK(DistanceAlongZ(frame, plane) == doctest::Approx(10));

    // A wall runs along the drill direction, so it never meets the axis.
    plane.direction = { 1, 0, 0 };
    CHECK(DistanceAlongZ(frame, plane) == 0);

    // Nearly along it meets the axis kilometres away, which counts as none.
    plane.point = { 1000, 0, 0 };
    plane.direction = { 1, 0, 0.02 };
    CHECK(DistanceAlongZ(frame, plane) == 0);

    LocatedGeometry line;
    line.kind = LocatedGeometry::Kind::Line;
    line.point = { 0, 0, 15 };
    line.direction = { 1, 0, 0 };
    CHECK(DistanceAlongZ(frame, line) == doctest::Approx(5));

    // A tilted line is measured at its point nearest the axis: it crosses x = 0 at z = 10.
    line.point = { 5, 0, 15 };
    line.direction = { 1, 0, 1 };
    CHECK(DistanceAlongZ(frame, line) == doctest::Approx(10));

    line.direction = { 0, 0, 1 };       // along the drill direction
    CHECK(DistanceAlongZ(frame, line) == 0);

    // A side origin: Z is assembly X.
    OriginDefinition definition;
    definition.plane = Circle({ 100, 0, 0 }, { 1, 0, 0 });
    point.point = { 90, 7, 7 };
    CHECK(DistanceAlongZ(BuildFrame(definition).frame, point) == doctest::Approx(10));
}

TEST_CASE("an operation outside a group stops the file loading")
{
    TempFile file;
    {
        std::ofstream out(file.Path(), std::ios::binary);
        out << R"({"format":"AlibreCAM","formatVersion":2,"assembly":{"name":"a","guid":"b"},)"
            << R"("sequence":[{"type":"drill","id":"d","name":"Drill 1"}]})";
    }
    CHECK(LoadJob(file.Path()).status == LoadStatus::NotACamFile);
}

TEST_CASE("a step runs under the nearest Set Origin at or above it")
{
    Job job;
    const auto add = [&](StepType type)
    {
        SequenceStep step;
        step.type = type;
        job.sequence.push_back(step);
    };

    add(StepType::Group);       // 0: before any origin
    add(StepType::SetOrigin);   // 1
    add(StepType::SetOrigin);   // 2
    add(StepType::Group);       // 3
    add(StepType::SetOrigin);   // 4

    CHECK(ActiveOriginIndex(job, 0) == kNoStepIndex);
    CHECK(ActiveOriginIndex(job, 1) == 1);
    CHECK(ActiveOriginIndex(job, 2) == 2);
    CHECK(ActiveOriginIndex(job, 3) == 2);
    CHECK(ActiveOriginIndex(job, 4) == 4);
    CHECK(ActiveOriginIndex(job, 5) == kNoStepIndex);
    CHECK(ActiveOriginIndex(job, kNoStepIndex) == kNoStepIndex);
}

namespace
{
    SequenceStep NamedStep(StepType type, const std::string& name)
    {
        SequenceStep step;
        step.type = type;
        step.id = name;
        step.name = name;
        return step;
    }

    std::string Names(const std::vector<SequenceStep>& steps)
    {
        std::string names;
        for (const SequenceStep& step : steps)
            names += (names.empty() ? "" : " ") + step.name;
        return names;
    }

    // O1 O2 F1(D1 D2) F2(D3)
    Job JobToReorder()
    {
        Job job;
        job.sequence.push_back(NamedStep(StepType::SetOrigin, "O1"));
        job.sequence.push_back(NamedStep(StepType::SetOrigin, "O2"));

        SequenceStep first = NamedStep(StepType::Group, "F1");
        first.children.push_back(NamedStep(StepType::Drill, "D1"));
        first.children.push_back(NamedStep(StepType::Drill, "D2"));
        job.sequence.push_back(first);

        SequenceStep second = NamedStep(StepType::Group, "F2");
        second.children.push_back(NamedStep(StepType::Drill, "D3"));
        job.sequence.push_back(second);
        return job;
    }
}

TEST_CASE("top-level steps move, but a Set Origin must stay first")
{
    size_t newIndex = 0;

    SUBCASE("a folder moves up")
    {
        Job job = JobToReorder();
        REQUIRE(MoveStep(job, 3, 1, newIndex));
        CHECK(Names(job.sequence) == "O1 F2 O2 F1");
        CHECK(newIndex == 1);
        CHECK(Names(job.sequence[1].children) == "D3");
    }

    SUBCASE("a Set Origin moves to the end")
    {
        Job job = JobToReorder();
        REQUIRE(MoveStep(job, 1, 4, newIndex));
        CHECK(Names(job.sequence) == "O1 F1 F2 O2");
        CHECK(newIndex == 3);
    }

    SUBCASE("the two origins swap, so a Set Origin is still first")
    {
        Job job = JobToReorder();
        REQUIRE(MoveStep(job, 1, 0, newIndex));
        CHECK(Names(job.sequence) == "O2 O1 F1 F2");
    }

    SUBCASE("a folder can't go first")
    {
        Job job = JobToReorder();
        CHECK_FALSE(MoveStep(job, 2, 0, newIndex));
        CHECK(Names(job.sequence) == "O1 O2 F1 F2");
    }

    SUBCASE("the only origin before the folders can't leave first place")
    {
        Job job = JobToReorder();
        job.sequence.erase(job.sequence.begin() + 1);
        CHECK_FALSE(MoveStep(job, 0, 3, newIndex));
        CHECK(Names(job.sequence) == "O1 F1 F2");
    }

    SUBCASE("dropping just before or after itself changes nothing")
    {
        Job job = JobToReorder();
        CHECK_FALSE(MoveStep(job, 2, 2, newIndex));
        CHECK_FALSE(MoveStep(job, 2, 3, newIndex));
        CHECK_FALSE(MoveStep(job, 9, 0, newIndex));
        CHECK_FALSE(MoveStep(job, 0, 9, newIndex));
    }
}

TEST_CASE("operations move within a folder and between folders")
{
    size_t newIndex = 0;

    SUBCASE("within a folder")
    {
        Job job = JobToReorder();
        REQUIRE(MoveOperation(job, 2, 0, 2, 2, newIndex));
        CHECK(Names(job.sequence[2].children) == "D2 D1");
        CHECK(newIndex == 1);
    }

    SUBCASE("into another folder, at the end")
    {
        Job job = JobToReorder();
        REQUIRE(MoveOperation(job, 2, 1, 3, 1, newIndex));
        CHECK(Names(job.sequence[2].children) == "D1");
        CHECK(Names(job.sequence[3].children) == "D3 D2");
        CHECK(newIndex == 1);
    }

    SUBCASE("into another folder, first")
    {
        Job job = JobToReorder();
        REQUIRE(MoveOperation(job, 3, 0, 2, 0, newIndex));
        CHECK(Names(job.sequence[2].children) == "D3 D1 D2");
        CHECK(job.sequence[3].children.empty());
    }

    SUBCASE("never out of a folder")
    {
        Job job = JobToReorder();
        CHECK_FALSE(MoveOperation(job, 2, 0, 1, 0, newIndex));
        CHECK_FALSE(MoveOperation(job, 2, 0, 2, 0, newIndex));
        CHECK_FALSE(MoveOperation(job, 2, 0, 2, 1, newIndex));
        CHECK_FALSE(MoveOperation(job, 2, 5, 3, 0, newIndex));
        CHECK(Names(job.sequence[2].children) == "D1 D2");
    }
}

TEST_CASE("a duplicate goes just below the original, with new IDs")
{
    size_t newIndex = 0;

    SUBCASE("a folder and its operations")
    {
        Job job = JobToReorder();
        job.sequence[2].children[0].drill.depth = 7;
        REQUIRE(DuplicateStep(job, 2, kNoStepIndex, newIndex));
        CHECK(newIndex == 3);
        CHECK(Names(job.sequence) == "O1 O2 F1 F1 (copy) F2");

        const SequenceStep& copy = job.sequence[3];
        CHECK(copy.type == StepType::Group);
        CHECK(Names(copy.children) == "D1 D2");
        CHECK(copy.children[0].drill.depth == 7);
        CHECK(copy.id != job.sequence[2].id);
        CHECK(copy.children[0].id != job.sequence[2].children[0].id);
        CHECK(copy.children[1].id != job.sequence[2].children[1].id);
    }

    SUBCASE("a Set Origin")
    {
        Job job = JobToReorder();
        REQUIRE(DuplicateStep(job, 0, kNoStepIndex, newIndex));
        CHECK(Names(job.sequence) == "O1 O1 (copy) O2 F1 F2");
        CHECK(job.sequence[1].type == StepType::SetOrigin);
    }

    SUBCASE("an operation stays in its folder")
    {
        Job job = JobToReorder();
        REQUIRE(DuplicateStep(job, 2, 0, newIndex));
        CHECK(newIndex == 1);
        CHECK(Names(job.sequence[2].children) == "D1 D1 (copy) D2");
        CHECK(job.sequence[2].children[1].id != job.sequence[2].children[0].id);
        CHECK(Names(job.sequence) == "O1 O2 F1 F2");
    }

    SUBCASE("nothing to copy")
    {
        Job job = JobToReorder();
        CHECK_FALSE(DuplicateStep(job, 9, kNoStepIndex, newIndex));
        CHECK_FALSE(DuplicateStep(job, 2, 5, newIndex));
        CHECK(Names(job.sequence) == "O1 O2 F1 F2");
    }
}

TEST_CASE("stable IDs are long and unique")
{
    CHECK(NewStableId().size() == 32);
    CHECK(NewStableId() != NewStableId());
}
