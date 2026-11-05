// ProfileToolpath.h : calculates a Profiling operation's toolpath. No Alibre or MFC code belongs here.
//
// Points are in the operation's origin frame, in millimetres. Profiles lie flat
// at one level, and arcs turn about Z.

#pragma once

#include <string>
#include <vector>

#include "CamJob.h"
#include "ToolLibrary.h"
#include "Toolpath.h"

namespace camcore
{
    // Edge ends closer than this join.
    constexpr double kProfileJoinTolerance = 0.01;

    // The profile the features make, in one direction.
    struct ProfileChain
    {
        bool ok = false;
        std::string problem;
        Path path;              // closed paths don't repeat their first point
        bool closed = false;
    };

    // Joins the features' pieces end to end into one profile. pieces[f] are what
    // feature f gave, in the Features list's order. The edges must join with no
    // gaps or branches, though the profile needn't close. An open profile starts
    // at the free end belonging to the earliest feature in the list. A closed one
    // starts at the first feature, going towards whichever neighbour comes earlier
    // in the list. Every point is put at level.
    ProfileChain ChainProfile(const std::vector<std::vector<Path>>& pieces, double level);

    // A stretch of an offset profile the tool can follow without cutting into the profile.
    struct ProfileRun
    {
        Path path;              // closed paths don't repeat their first point
        bool closed = false;
    };

    // The profile offset by distance, positive to its left seen from +Z. Outside
    // corners are rounded with arcs about the corner. Anything that would come
    // closer to the profile than the distance, such as inside corners' overlaps
    // and places too narrow for the tool, is left out, which can split the result
    // into several runs. An open profile's offset ends square to its ends.
    std::vector<ProfileRun> OffsetProfile(const Path& profile, bool closed, double distance);

    // Everything a profiling toolpath is worked out from.
    struct ProfileToolpathInput
    {
        const SequenceStep* operation = nullptr;
        const Tool* tool = nullptr;     // in millimetres, from ToolInMillimetres
        ProfileChain profile;           // from ChainProfile, in the operation's frame

        // The parts' highest point along the frame's Z, for the clearance plane.
        bool havePartMaxZ = false;
        double partMaxZ = 0.0;
    };

    // The tool runs beside the profile on the chosen side, its edge the stock to
    // leave away. Climb keeps the profile on the cutter's right (clockwise
    // spindle) and Conventional on its left, by choosing the direction to run.
    // Mixed runs the profile's own way. With a total cut width, passes start the
    // width further out and step in, no more than Step/Cut apart. Every pass at
    // every cut level, top down, is a cut of its own in the same direction, with
    // the Entry/Exit tab's motions, lifting to the clearance plane between. Tabs
    // leave uncut material of the tab length on the levels below the tab top
    // (the bottom of the cut plus the tab height): the tool goes straight up to
    // the tab top and back down at the plunge feed.
    ToolpathResult GenerateProfileToolpath(const ProfileToolpathInput& input);
}
