// ScriptedPostTests.cpp : tests for reading post scripts and writing G-code with them.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "ScriptedPost.h"

using namespace camcore;

namespace
{
    // The LinuxCNC post shipped with the add-on, in the repository's posts folder.
    std::string LinuxCncPost()
    {
        const std::filesystem::path path =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "posts" / "LinuxCNC.post";
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    ToolpathMove Move(MoveKind kind, Vec3 to, double feed = 0)
    {
        ToolpathMove move;
        move.kind = kind;
        move.to = to;
        move.feed = feed;
        return move;
    }

    // A drill down and up, a dwell, then an anticlockwise half circle.
    SequenceStep Operation(const std::string& name)
    {
        SequenceStep step;
        step.type = StepType::Drill;
        step.name = name;
        step.feeds.spindleRpm = 12000;
        step.toolpath.moves = {
            Move(MoveKind::Rapid, { 10, 0, 20 }),
            Move(MoveKind::Rapid, { 10, 0, 20 }),       // goes nowhere, so isn't written
            Move(MoveKind::Feed, { 10, 0, -10 }, 100),
            Move(MoveKind::Dwell, { 10, 0, -10 }),
            Move(MoveKind::Feed, { 10, 0, -5 }, 100),
        };
        step.toolpath.moves[3].seconds = 0.5;
        ToolpathMove arc = Move(MoveKind::Feed, { -10, 0, -5 }, 100);
        arc.arc = true;
        arc.centre = { 0, 0, -5 };
        step.toolpath.moves.push_back(arc);
        return step;
    }

    Tool MakeTool(const std::string& id, int number)
    {
        Tool tool;
        tool.id = id;
        tool.number = number;
        tool.name = "6 mm (2 flute) end mill";
        tool.diameter = 6;
        return tool;
    }

    bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}

TEST_CASE("the LinuxCNC post reads cleanly")
{
    const PostScriptRead read = ReadPostScript(LinuxCncPost());
    REQUIRE_MESSAGE(read.ok, read.problem);
    CHECK(read.script.name == "LinuxCNC (3 axis router, mm)");
    CHECK(read.script.extension == "ngc");
    CHECK(read.script.decimals == 4);
    CHECK_FALSE(read.script.lineNumbers);
}

TEST_CASE("the LinuxCNC post writes a program from the work zero")
{
    const PostScript script = ReadPostScript(LinuxCncPost()).script;
    SequenceStep origin;
    origin.name = "Top";
    const SequenceStep operation = Operation("Drill (holes)");
    const Tool tool = MakeTool("a", 3);
    PostOperation entry;
    entry.origin = &origin;
    entry.operation = &operation;
    entry.tool = &tool;

    const PostOutput out = WriteScriptedPost(script, "Bracket", Frame(), { entry }, "2026-11-06");
    REQUIRE(out.ok);
    const std::string& g = out.text;
    CHECK(g.rfind("%\n(Bracket)\n(Posted by AlibreCAM on 2026-11-06)\nG21 G90 G17 G94", 0) == 0);
    CHECK(Has(g, "M5\nT3 M6\n(6 mm [2 flute] end mill, 6 mm)\n"));
    CHECK(Has(g, "(Drill [holes])\nS12000 M3\n"));
    CHECK(Has(g, "G0 X10 Y0 Z20\nG1 Z-10 F100\nG4 P0.5\nG1 Z-5\n"));
    CHECK(Has(g, "G3 X-10 I-10 J0\n"));
    CHECK(g.find("G0 X10 Y0 Z20") == g.rfind("G0 X10 Y0 Z20"));     // the second rapid went nowhere
    const std::string ending = "M5\nM30\n%\n";
    CHECK(g.compare(g.size() - ending.size(), ending.size(), ending) == 0);
}

TEST_CASE("points come from the work zero, and the tool changes only when it does")
{
    const PostScript script = ReadPostScript(LinuxCncPost()).script;
    const SequenceStep first = Operation("First");
    const SequenceStep second = Operation("Second");
    const Tool tool = MakeTool("a", 3);

    // The second runs under an origin 100 along X from the work zero, turned a quarter about Z.
    Frame moved;
    moved.origin = { 100, 0, 0 };
    moved.xAxis = { 0, 1, 0 };
    moved.yAxis = { -1, 0, 0 };
    std::vector<PostOperation> entries(2);
    entries[0].operation = &first;
    entries[0].tool = &tool;
    entries[1].operation = &second;
    entries[1].tool = &tool;
    entries[1].frame = moved;

    const PostOutput out = WriteScriptedPost(script, "Job", Frame(), entries, "");
    REQUIRE(out.ok);
    size_t changes = 0;
    for (size_t at = out.text.find("M6"); at != std::string::npos; at = out.text.find("M6", at + 1))
        ++changes;
    CHECK(changes == 1);
    CHECK(Has(out.text, "(Second)\nS12000 M3\nG0 X100 Y10 Z20\n"));
    CHECK(Has(out.text, "G3 Y-10 I0 J-10\n"));          // X stays 100; still anticlockwise, turned with the origin
}

TEST_CASE("a 3 axis post refuses operations tilted away from the work zero's Z")
{
    const PostScript script = ReadPostScript(LinuxCncPost()).script;
    const SequenceStep operation = Operation("Side drill");
    PostOperation entry;
    entry.operation = &operation;
    entry.frame.zAxis = { 1, 0, 0 };
    entry.frame.xAxis = { 0, 1, 0 };
    entry.frame.yAxis = { 0, 0, 1 };
    const PostOutput out = WriteScriptedPost(script, "Job", Frame(), { entry }, "");
    CHECK_FALSE(out.ok);
    CHECK(Has(out.problem, "Side drill"));
}

TEST_CASE("line numbers and untrimmed numbers")
{
    const PostScriptRead read = ReadPostScript("[settings]\nname = Test\ntrim zeros = no\ndecimals = 3\n"
                                               "line numbers = yes\nline number start = 100\nline number step = 5\n"
                                               "[program start]\n%\nG21\n[rapid]\nG0{ X}{ Y}{ Z}\n");
    REQUIRE_MESSAGE(read.ok, read.problem);
    const SequenceStep operation = Operation("Op");
    PostOperation entry;
    entry.operation = &operation;
    const PostOutput out = WriteScriptedPost(read.script, "Job", Frame(), { entry }, "");
    REQUIRE(out.ok);
    CHECK(out.text.rfind("%\nN100 G21\nN105 G0 X10.000 Y0.000 Z20.000\n", 0) == 0);
}

TEST_CASE("mistakes in a post script are found when it's read")
{
    CHECK_FALSE(ReadPostScript("[settings]\nname = A\n[warp drive]\n").ok);
    CHECK_FALSE(ReadPostScript("[settings]\nname = A\n[rapid]\nG0{ W}\n").ok);
    CHECK_FALSE(ReadPostScript("[settings]\nname = A\n[rapid]\nG0{ X\n").ok);
    CHECK_FALSE(ReadPostScript("[settings]\nname = A\ndecimals = lots\n").ok);
    CHECK_FALSE(ReadPostScript("[settings]\nname = A\ncolour = red\n").ok);
    CHECK_FALSE(ReadPostScript("[settings]\nextension = nc\n").ok);         // no name
    CHECK_FALSE(ReadPostScript("G0 X1\n[settings]\nname = A\n").ok);       // before any section

    const PostScriptRead bad = ReadPostScript("[settings]\nname = A\n[feed]\nG1{ X}{ SPEED}\n");
    CHECK_FALSE(bad.ok);
    CHECK(Has(bad.problem, "Line 4"));
}
