// AdaptiveClearing.h : adaptive (constant engagement) clearing of an area at one level. No Alibre or MFC code belongs here.
//
// The area is a grid of cells. The tool walks through the material in short
// steps, turning at each so it removes about the same amount for each step (the
// optimal load times the step), so it never takes a full-width cut. It starts from
// air (outside the stock) where there is some, and otherwise helixes down.
// Points are in the operation's origin frame, in millimetres.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Boundary.h"
#include "Feeds.h"
#include "Origin.h"
#include "RampEntry.h"
#include "Toolpath.h"

namespace camcore
{
    // The area at one level, as cells nx by ny, row by row, each centred at
    // (xLow + i cell, yLow + j cell).
    struct AdaptiveGrid
    {
        double xLow = 0.0;
        double yLow = 0.0;
        double cell = 0.1;
        int nx = 0;
        int ny = 0;
        std::vector<unsigned char> free;        // the tool's centre may be here
        std::vector<unsigned char> material;    // stock still to be cut at this level

        size_t Index(int i, int j) const { return static_cast<size_t>(j) * static_cast<size_t>(nx) + static_cast<size_t>(i); }
        Vec3 Centre(int i, int j, double z = 0.0) const { return { xLow + i * cell, yLow + j * cell, z }; }
    };

    struct AdaptiveSettings
    {
        double toolRadius = 3.0;
        double optimalLoad = 0.6;       // the stepover the tool's side cuts at, mm
        bool climb = true;              // the material on the tool's right
        RampEntry ramp;
        bool wallLaps = true;           // last, laps along the edge of the free cells where material's left
        double simplify = 0.01;         // how closely each cut is simplified, mm
    };

    // One cut: how it's entered, then its points at the level.
    struct AdaptiveCut
    {
        enum class Entry
        {
            Link,       // from the last cut's end, staying down, straight to the first point
            Plunge,     // down from above onto the first point, where nothing is left to cut
            Helix       // down a helix round helixCentre, of helixRadius, ending at the first point
        };
        Entry entry = Entry::Plunge;
        Vec3 helixCentre;
        double helixRadius = 0.0;
        Polyline link;                  // with Link: points to go through first, round what's in the way
        Polyline points;                // at least one; z is 0
    };

    struct AdaptiveResult
    {
        std::vector<AdaptiveCut> cuts;
        double leftArea = 0.0;          // mm^2 of material the tool could reach but didn't cut
        size_t passes = 0;
        size_t helixes = 0;
        size_t plunges = 0;             // straight down into material: no room for a helix
    };

    // Clears the grid's material that the tool can reach (its centre in free
    // cells), material it can't reach being left. Progress goes from 0 to 1.
    AdaptiveResult AdaptiveClear(AdaptiveGrid grid, const AdaptiveSettings& settings,
                                 const std::function<void(double)>& progress = nullptr);

    // A cell size for adaptive clearing: about a fortieth of the tool, fine enough
    // for the load, and no more than about 8 million cells over width x height.
    double AdaptiveCell(double toolDiameter, double load, double width, double height);

    // A grid from closed outlines (inside by the even-odd rule is the material).
    // A positive inset keeps the tool's centre that far inside the outlines (a
    // pocket's wall); a negative one lets it go that far outside them (facing's
    // overtravel). The free cells err on the safe side by most of a cell.
    AdaptiveGrid AdaptiveGridFromOutlines(const std::vector<Path>& outlines, double cell, double inset, double toolRadius);

    // A helix round centre, from start (on its circle, at its Z) down to bottom at
    // the ramp angle, in half turns, then once round flat, ending back at start's
    // angle at bottom. Anticlockwise (seen from +Z) for climb with a clockwise spindle.
    void AppendHelix(Toolpath& toolpath, const Vec3& start, const Vec3& centre, double bottom, double rampAngle,
                     double feed, bool anticlockwise);

    // How the cuts are written at a level.
    struct AdaptiveWriting
    {
        double z = 0.0;             // the level
        double top = 0.0;           // the top of the material here, where a helix starts
        double enter = 0.0;         // reached at rapid before feeding down (the approach)
        double clearance = 10.0;
        FeedSettings feeds;         // plunge down, engage round the helix, cut along the cuts
        bool climb = true;
        double rampAngle = 2.0;
    };

    // Appends the cuts' moves to the toolpath: entering each by its entry
    // (lifting to the clearance plane first for a plunge or helix), cutting
    // along it, and lifting to the clearance plane at the end.
    void WriteAdaptiveCuts(const std::vector<AdaptiveCut>& cuts, const AdaptiveWriting& writing, Toolpath& toolpath);
}
