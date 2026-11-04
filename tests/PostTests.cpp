// PostTests.cpp : tests for the built-in post's APT CL data.

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

        ToolpathMove up = feed;
        up.to = { 10, 0, 2 };
        up.feed = 400;
        step.toolpath.moves.push_back(up);
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

    size_t Count(const std::string& text, const std::string& word)
    {
        size_t found = 0;
        for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
            ++found;
        return found;
    }
}

TEST_CASE("APT CL data has the program, tool, spindle and moves")
{
    const SequenceStep origin = Origin("Top");
    const SequenceStep drill = Drill("Drill 1");
    const Tool tool = MakeTool("a", 3);

    PostOperation entry;
    entry.origin = &origin;
    entry.operation = &drill;
    entry.tool = &tool;

    const std::string text = WriteAptClData("Bracket", Frame(), { entry });
    CHECK(text.rfind("PARTNO Bracket\n", 0) == 0);
    CHECK(text.find("UNITS/MM\n") != std::string::npos);
    CHECK(text.find("$$ ORIGIN Top\n") != std::string::npos);
    CHECK(text.find("$$ OPERATION Drill 1\n") != std::string::npos);
    CHECK(text.find("CUTTER/5.0000\nLOADTL/3\n") != std::string::npos);
    CHECK(text.find("SPINDL/RPM,12000.0000,CLW\n") != std::string::npos);
    CHECK(text.find("RAPID\nGOTO/10.0000,0.0000,20.0000,0.0000,0.0000,1.0000\n") != std::string::npos);
    CHECK(text.find("FEDRAT/MMPM,100.0000\nGOTO/10.0000,0.0000,-10.0000,0.0000,0.0000,1.0000\n") != std::string::npos);
    CHECK(text.find("DELAY/0.5000\n") != std::string::npos);
    CHECK(text.find("FEDRAT/MMPM,400.0000\nGOTO/10.0000,0.0000,2.0000,") != std::string::npos);

    const std::string ending = "SPINDL/OFF\nEND\nFINI\n";
    CHECK(text.compare(text.size() - ending.size(), ending.size(), ending) == 0);
}

TEST_CASE("FEDRAT is written only when the feed changes, and RAPID before every rapid")
{
    const SequenceStep origin = Origin("Top");
    const SequenceStep first = Drill("D1");
    const SequenceStep second = Drill("D2");

    std::vector<PostOperation> entries(2);
    entries[0].origin = &origin;
    entries[0].operation = &first;
    entries[1].origin = &origin;
    entries[1].operation = &second;

    const std::string text = WriteAptClData("Job", Frame(), entries);
    CHECK(Count(text, "RAPID\n") == 2);
    CHECK(Count(text, "FEDRAT/MMPM,100.0000") == 2);       // 100, 400, then 100 again in D2
    CHECK(Count(text, "FEDRAT/MMPM,400.0000") == 2);
    CHECK(Count(text, "$$ ORIGIN") == 1);
}

TEST_CASE("tool and origin changes are written where they happen")
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

    const std::string text = WriteAptClData("Job", Frame(), entries);
    CHECK(Count(text, "LOADTL/") == 2);
    CHECK(Count(text, "$$ ORIGIN") == 2);
    CHECK(text.find("$$ ORIGIN Side") < text.find("$$ OPERATION D3"));
    CHECK(text.find("LOADTL/2") > text.find("$$ OPERATION D3"));
}

TEST_CASE("points and the tool axis are given from the work zero")
{
    // A side origin at x = 100 with its Z along assembly +X; the work zero is the assembly.
    const SequenceStep side = Origin("Side");
    const SequenceStep drill = Drill("D1");

    PostOperation entry;
    entry.origin = &side;
    entry.operation = &drill;
    entry.frame.origin = { 100, 0, 0 };
    entry.frame.xAxis = { 0, 1, 0 };
    entry.frame.yAxis = { 0, 0, 1 };
    entry.frame.zAxis = { 1, 0, 0 };

    // The first move, (10, 0, 20) in the side frame, is (120, 10, 0) from the work zero.
    const std::string text = WriteAptClData("Job", Frame(), { entry });
    CHECK(text.find("GOTO/120.0000,10.0000,0.0000,1.0000,0.0000,0.0000\n") != std::string::npos);

    // With the work zero raised 5 mm, every Z drops by 5.
    Frame raised;
    raised.origin = { 0, 0, 5 };
    const std::string shifted = WriteAptClData("Job", raised, { entry });
    CHECK(shifted.find("GOTO/120.0000,10.0000,-5.0000,1.0000,0.0000,0.0000\n") != std::string::npos);
}

TEST_CASE("a missing tool is noted, and tiny numbers aren't negative zero")
{
    const SequenceStep origin = Origin("Top");
    SequenceStep drill = Drill("D1");
    drill.toolpath.moves[0].to = { 1.23456789, -0.00001, 2.5 };

    PostOperation entry;
    entry.origin = &origin;
    entry.operation = &drill;

    const std::string text = WriteAptClData("Job", Frame(), { entry });
    CHECK(text.find("$$ TOOL ?") != std::string::npos);
    CHECK(text.find("LOADTL") == std::string::npos);
    CHECK(text.find("GOTO/1.2346,0.0000,2.5000,") != std::string::npos);
}
