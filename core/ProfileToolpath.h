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

    // The features' pieces sorted into the separate profiles they make, each
    // joined as ChainProfile does, in the order their first feature comes in the
    // list. Pieces belong together when their ends meet.
    struct ProfileChains
    {
        bool ok = false;
        std::string problem;
        std::vector<ProfileChain> chains;
    };
    ProfileChains ChainProfiles(const std::vector<std::vector<Path>>& pieces, double level);

    // The features' outlines sorted into areas: each face's pieces on their own,
    // and edges grouped where their ends meet, in the order of each area's first
    // feature in the list. Only XY matters: everything is put flat at the
    // highest point of the outlines, which At Top and At Bottom cut levels are
    // measured from, and pieces with no length seen from above are left out.
    struct AreaGeometry
    {
        bool ok = false;
        std::string problem;
        double level = 0.0;
        std::vector<std::vector<std::vector<Path>>> areas;     // each area's pieces, by feature
    };
    AreaGeometry GatherAreas(const std::vector<std::vector<Path>>& outlines, const std::vector<bool>& isFace);

    // An area's pieces in one list, for ClosePath.
    std::vector<Path> AreaPieces(const std::vector<std::vector<Path>>& area);

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
        std::vector<ProfileChain> profiles;     // from ChainProfile, in the operation's frame, all at one level

        // The parts' highest point along the frame's Z, for the clearance plane.
        bool havePartMaxZ = false;
        double partMaxZ = 0.0;

        // The stock's highest point along the frame's Z, when there's a stock.
        bool haveStockMaxZ = false;
        double stockMaxZ = 0.0;
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
    // the tab top and back down at the plunge feed. Each profile is cut in turn,
    // to its full depth before the next (Depth First) or each level in every
    // profile before the next level (Level First).
    ToolpathResult GenerateProfileToolpath(const ProfileToolpathInput& input);

    // A chamfer cut with a V-mill or chamfer mill, whose half angle is the
    // chamfer's angle. Its depth is the width over the tangent of the half angle.
    double ChamferDepth(const Tool& tool, const ChamferSettings& chamfer);

    // How far the tool's centre runs from the edge so its cone lies on the
    // chamfer face when the tip is the tooltip clearance below the chamfer: the
    // tip radius plus the clearance times the tangent of the half angle, plus
    // the stock to leave.
    double ChamferOffset(const Tool& tool, const ChamferSettings& chamfer, double stockToLeave);

    // A Chamfer operation's toolpath: Profiling's, following each profile at the
    // chamfer offset. The chamfer's depth is cut in equal passes no deeper than
    // Max Depth/Cut, each with the tip the tooltip clearance below that depth.
    // The edge is the profiles' height (the selected geometry's highest point).
    // Stepover passes work as in Profiling, there are no tabs, and each profile
    // is cut to its full depth before the next.
    ToolpathResult GenerateChamferToolpath(const ProfileToolpathInput& input);
}
