// PostTests.cpp : tests for the pseudo-code post.

#include <doctest/doctest.h>

#include <string>

#include "PseudoPost.h"

using namespace camcore;

namespace
{
    SequenceStep Origin(const std::string& name)
    {
        SequenceStep step;
        step.name = name;
        return step;
    }

    SequenceStep Drill(const std::string& name)
    {
        SequenceStep step;
        step.type = StepType::Drill;
        step.name = name;
        step.feeds.spindleRpm = 12000;

        ToolpathMove rapid;
        rapid.to = { 10, 0, 20 };
        step.toolpath.moves.push_back(rapid);

        ToolpathMove feed;
        feed.kind = MoveKind::Feed;
        feed.to = { 10, 0, -10 };
        feed.feed = 100;
        step.toolpath.moves.push_back(feed);

        ToolpathMove dwell;
        dwell.kind = MoveKind::Dwell;
        dwell.to = feed.to;
        dwell.seconds = 0.5;
        step.toolpath.moves.push_back(dwell);
        return step;
    }

    Tool MakeTool(const std::string& id, int number)
    {
        Tool tool;
        tool.id = id;
        tool.number = number;
        tool.name = "5 mm drill";
        tool.diameter = 5;
        return tool;
    }
}

TEST_CASE("the pseudo post writes the program, origin, tool and moves")
{
    const SequenceStep origin = Origin("Top");
    const SequenceStep drill = Drill("Drill 1");
    const Tool tool = MakeTool("a", 3);

    PostOperation entry;
    entry.origin = &origin;
    entry.frame.origin = { 1, 2, 3 };
    entry.operation = &drill;
    entry.tool = &tool;

    const std::string text = WritePseudoPost("Bracket", { entry });
    CHECK(text.find("PROGRAM START \"Bracket\"\n") != std::string::npos);
    CHECK(text.find("ORIGIN \"Top\" AT (1, 2, 3) X (1, 0, 0) Y (0, 1, 0) Z (0, 0, 1)\n") != std::string::npos);
    CHECK(text.find("TOOL T3 \"5 mm drill\" DIAMETER 5\n") != std::string::npos);
    CHECK(text.find("OPERATION \"Drill 1\" TYPE drill\n") != std::string::npos);
    CHECK(text.find("SPINDLE 12000 RPM\n") != std::string::npos);
    CHECK(text.find("  RAPID X10 Y0 Z20\n") != std::string::npos);
    CHECK(text.find("  FEED X10 Y0 Z-10 F100\n") != std::string::npos);
    CHECK(text.find("  DWELL 0.5 S\n") != std::string::npos);
    CHECK(text.find("PROGRAM END\n") != std::string::npos);
}

TEST_CASE("origin and tool lines appear only when they change")
{
    const SequenceStep top = Origin("Top");
    const SequenceStep side = Origin("Side");
    const SequenceStep first = Drill("D1");
    const SequenceStep second = Drill("D2");
    const SequenceStep third = Drill("D3");
    const Tool small = MakeTool("a", 1);
    const Tool large = MakeTool("b", 2);

    std::vector<PostOperation> entries(3);
    entries[0].origin = &top;
    entries[0].operation = &first;
    entries[0].tool = &small;
    entries[1].origin = &top;
    entries[1].operation = &second;
    entries[1].tool = &small;
    entries[2].origin = &side;
    entries[2].operation = &third;
    entries[2].tool = &large;

    const std::string text = WritePseudoPost("Job", entries);
    const auto count = [&](const std::string& word)
    {
        size_t found = 0;
        for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
            ++found;
        return found;
    };
    CHECK(count("ORIGIN ") == 2);
    CHECK(count("TOOL T") == 2);
    CHECK(count("OPERATION END") == 3);

    // The side origin and second tool come before D3.
    CHECK(text.find("ORIGIN \"Side\"") < text.find("OPERATION \"D3\""));
    CHECK(text.find("TOOL T2") < text.find("OPERATION \"D3\""));
}

TEST_CASE("a missing tool is written as unknown, and numbers stay short")
{
    const SequenceStep origin = Origin("Top");
    SequenceStep drill = Drill("D1");
    drill.toolpath.moves[0].to = { 1.23456789, -0.00001, 2.5 };

    PostOperation entry;
    entry.origin = &origin;
    entry.operation = &drill;

    const std::string text = WritePseudoPost("Job", { entry });
    CHECK(text.find("TOOL ? ") != std::string::npos);
    CHECK(text.find("  RAPID X1.2346 Y0 Z2.5\n") != std::string::npos);
}
