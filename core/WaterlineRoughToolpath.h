// WaterlineRoughToolpath.h : calculates a Waterline Roughing operation's toolpath. No Alibre or MFC code belongs here.
//
// Points are in the operation's origin frame, in millimetres, so Z is the tool axis.

#pragma once

#include <functional>
#include <vector>

#include "Boundary.h"
#include "CamJob.h"
#include "PlungeRoughToolpath.h"    // ProfileChain, the area and the parts' heights
#include "SurfaceGeometry.h"
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

        // Parallel Finishing with a ball mill only, both optional: the part face
        // each triangle is on (-1 for none), and the closest point on a face's
        // true surface to p (both in the frame), with whether it's on the face
        // itself (not the surface past its edges). It's called from one thread.
        std::vector<int> partTriangleFaces;
        std::function<bool(int face, const Vec3& p, Vec3& closest, bool& onFace)> closestPoint;
        // Each face's surface where it's a sphere, cylinder, cone or torus (in the
        // frame), so its closest points are worked out here, on all cores, without
        // asking closestPoint. Indexed by face; a kind of None, or past the end, asks.
        std::vector<AnalyticSurface> faceSurfaces;

        // Told how far along the calculation is (0 to 1 within each stage) and what
        // it's doing, from the calling thread only, often.
        std::function<void(double fraction, const std::string& stage)> progress;

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

    // Parallel Finishing: straight rows a stepover apart along the angle of cuts,
    // the tool following the parts' surface with its own shape, over the area (its
    // centre up to the edge, and past it by the overtravel), in one pass down to the
    // bottom: the Cut Levels tab's stepdown isn't used, only its Top and Bottom
    // (rows only where the surface is below the top). Climb and Conventional cut every
    // row the same way, lifting between; Mixed zigzags, staying down from row to
    // row when the step is short and in the area. A Scallop stepover needs a ball mill.
    // The heights come from the tool dropped onto the triangles; with a ball mill
    // and closestPoint, each contact is then moved onto the face's true surface.
    ToolpathResult GenerateParallelFinishToolpath(const WaterlineRoughToolpathInput& input);
}
