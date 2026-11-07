// WaterlineRoughToolpath.h : calculates a Waterline Roughing operation's toolpath. No Alibre or MFC code belongs here.
//
// Points are in the operation's origin frame, in millimetres, so Z is the tool axis.

#pragma once

#include <vector>

#include "Boundary.h"
#include "CamJob.h"
#include "PlungeRoughToolpath.h"    // ProfileChain, the area and the parts' heights
#include "Toolpath.h"
#include "ToolLibrary.h"

namespace camcore
{
    // Everything a waterline roughing toolpath is worked out from.
    struct WaterlineRoughToolpathInput
    {
        const SequenceStep* operation = nullptr;
        const Tool* tool = nullptr;                 // in millimetres, from ToolInMillimetres

        // The area, as for Plunge Roughing: picked faces' facets and closed loops,
        // projected onto the frame's XY plane and merged.
        std::vector<std::vector<Vec3>> faces;
        std::vector<ProfileChain> outlines;
        std::vector<Vec3> partTriangles;            // the parts, three points to a triangle, in the frame

        bool havePartMaxZ = false;
        double partMaxZ = 0.0;
        bool havePartMinZ = false;
        double partMinZ = 0.0;
        bool haveStockMaxZ = false;
        double stockMaxZ = 0.0;
        bool haveStockMinZ = false;
        double stockMinZ = 0.0;
    };

    // The heights of the parts' flat faces that face up (within about a degree of
    // the frame's XY plane), highest first, those within 0.001 mm of each other as one.
    // Which way is up for a face comes from its triangles' order round it, turned
    // round for the whole mesh if its signed volume says they go the other way.
    std::vector<double> FlatHeights(const std::vector<Vec3>& triangles);

    // The levels cut, highest first, below top and down to bottom: one at each
    // flat face's height plus the stock to leave, and the gaps between split
    // evenly so none is deeper than step. A flat face closer than the step still
    // gets its level.
    std::vector<double> WaterlineLevels(double top, double bottom, const std::vector<double>& flatHeights,
                                        double stockToLeave, double step);

    // The closed loops where a grid of values (nx by ny, row by row, at cell
    // centres from (xLow, yLow) in cells of `cell`) crosses `level`, each going
    // round with the higher values on its left, as points in XY at z.
    std::vector<Polyline> IsoContours(const std::vector<float>& values, int nx, int ny, double xLow, double yLow,
                                      double cell, double level, double z);

    // At each level, the area the tool's centre may reach: inside the area, where
    // the tool (its own shape, grown by the stock to leave) can come down to the
    // level without touching a part. That's cleared with loops a stepover
    // apart, the outermost hugging its edge, cut climb from the outside in.
    // Each level is entered from 1 mm above the one before (or the stock's top)
    // at the plunge feed. The tool stays down from one loop to the next when the
    // move is short and inside the cleared area, and otherwise lifts to the
    // clearance plane. Levels go down to the stock's bottom, or the parts'
    // lowest point without a stock.
    ToolpathResult GenerateWaterlineRoughToolpath(const WaterlineRoughToolpathInput& input);

    // Waterline Finishing: one lap round the parts at each level, where the tool's
    // own shape (a ball, a corner radius, a flat end) comes within the stock to
    // leave of them, worked out from how low its tip can go at every place. The
    // levels are Waterline Roughing's (its Cut Levels tab), and more are added
    // between two wherever their laps would be further apart across than the
    // stepover, as on a shallow slope. Laps are cut climb (the part on the right),
    // conventional, or alternating level by level for mixed; level by level, or
    // each part's laps down to the bottom first. The tool stays down from one lap
    // to the next round the same part when the move is short and clear.
    ToolpathResult GenerateWaterlineFinishToolpath(const WaterlineRoughToolpathInput& input);
}
