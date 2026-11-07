// PlungeRoughToolpath.h : calculates a Plunge Roughing operation's toolpath. No Alibre or MFC code belongs here.
//
// Points are in the operation's origin frame, in millimetres, so Z is the tool axis.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "CamJob.h"
#include "ProfileToolpath.h"    // ProfileChain
#include "Simulation.h"         // ToolProfile
#include "Toolpath.h"
#include "ToolLibrary.h"

namespace camcore
{
    // Everything a plunge roughing toolpath is worked out from.
    struct PlungeRoughToolpathInput
    {
        const SequenceStep* operation = nullptr;
        const Tool* tool = nullptr;                 // in millimetres, from ToolInMillimetres

        // What's projected onto the frame's XY plane to make the plunge area: each
        // picked face's facets (three points to a triangle), and closed loops of
        // picked edges (or a face's outline, when its facets couldn't be read).
        std::vector<std::vector<Vec3>> faces;
        std::vector<ProfileChain> outlines;         // each closed, in the operation's frame
        std::vector<Vec3> partTriangles;            // the parts, three points to a triangle, in the frame

        // The parts' highest point along the frame's Z, for the clearance plane.
        bool havePartMaxZ = false;
        double partMaxZ = 0.0;

        // The stock's highest and lowest points along the frame's Z, when there's a stock.
        bool haveStockMaxZ = false;
        double stockMaxZ = 0.0;
        bool haveStockMinZ = false;
        double stockMinZ = 0.0;
    };

    // The parts' highest point over each cell of a grid in the frame's XY: from
    // each triangle's inside, and its edges sampled, so steep and upright faces
    // count too.
    class PartHeightField
    {
    public:
        void Build(const std::vector<Vec3>& triangles, double xLow, double yLow, double xHigh, double yHigh,
                   double cell);

        // The highest the tool's tip can go down to, centred at (x, y), touching
        // nothing within stockToLeave of the parts. False if no part is under it.
        bool TipHeight(const ToolProfile& profile, double stockToLeave, double x, double y, double& tip) const;

        // True if any part is over the cell holding (x, y).
        bool HasPart(double x, double y) const;

        // The parts' highest point over the cell holding (x, y). False if none is.
        bool HeightAt(double x, double y, double& height) const;

        double Cell() const { return m_cell; }

    private:
        void Raise(double x, double y, double z);

        double m_xLow = 0.0;
        double m_yLow = 0.0;
        double m_cell = 1.0;
        int m_nx = 0;
        int m_ny = 0;
        std::vector<float> m_height;    // lowest() where nothing is
    };

    // Where the tool may plunge, seen from above: faces and closed loops projected
    // onto the frame's XY plane as a grid of cells, merged where they touch or
    // overlap. A face covers its facets, with any holes inside it filled. Each
    // cell knows how far it is from the area's edge, so a plunge can be kept
    // wholly inside.
    class PlungeArea
    {
    public:
        // An empty grid over the box, with a margin of empty cells round it at
        // least `padding` wide.
        void Reset(double xLow, double yLow, double xHigh, double yHigh, double cell, double padding = 0.0);
        void AddLoop(const Path& loop);
        void AddFace(const std::vector<Vec3>& triangles);

        // Works out the distances. Call after adding everything. Without
        // `blocked`, everything outside the area is in the tool's way. With it,
        // only outside places it says are blocked, more than `margin` from the
        // area (nearer is taken to be the area's own edge), so the tool may reach
        // over an open edge up to its centre.
        void Finish(const std::function<bool(double x, double y)>& blocked = {}, double margin = 0.0);

        bool Empty() const { return m_count == 0; }
        double Cell() const { return m_cell; }

        // The grid: its cells, the centre of each, and whether it's in the area.
        int CellsX() const { return m_nx; }
        int CellsY() const { return m_ny; }
        double X(int i) const { return m_xLow + (i + 0.5) * m_cell; }
        double Y(int j) const { return m_yLow + (j + 0.5) * m_cell; }
        bool Inside(int i, int j) const { return m_in[static_cast<size_t>(j) * m_nx + i] != 0; }

        // How far (x, y) is from anything in the tool's way, a little short to be
        // safe; negative outside the area, as the tool's centre stays inside it.
        double Clearance(double x, double y) const;

        // The lowest and highest of Dot(centre, direction) over the cells at least
        // `clearance` inside the edge. False if there are none.
        bool Extent(const Vec3& direction, double clearance, double& low, double& high) const;

    private:
        double m_xLow = 0.0;
        double m_yLow = 0.0;
        double m_cell = 1.0;
        int m_nx = 0;
        int m_ny = 0;
        size_t m_count = 0;
        std::vector<unsigned char> m_in;
        std::vector<float> m_distance;  // from each cell's centre to the nearest blocked cell's centre, mm
    };

    // Squared distances in cells from each cell of an nx by ny grid (row by row)
    // to the nearest cell holding 0; the others must hold a large number, such as 1e12.
    void SquaredDistanceTransform(std::vector<double>& grid, int nx, int ny);

    // The plunge centres, row by row in the order they're cut. Each plunge's tool,
    // reach (its radius plus the stock to leave) wide, stays inside the area.
    // They pack as circles do: rows along the angle of cuts, alternate rows
    // shifted half a pitch, the grid centred along the rows.
    // A pitch of at least the tool's diameter means plunges that don't overlap:
    // rows exactly (pitch times root 3 over 2) apart, centred across the area,
    // and a row's end gets a plunge of its own only where it's at least a tool
    // diameter from every other plunge.
    // A smaller pitch overlaps anyway, so it clears to the edges instead: rows
    // spread evenly at most that far apart, and a plunge at a row's end wherever
    // the grid leaves more than half a pitch.
    std::vector<std::vector<Vec3>> PlungeRows(const PlungeArea& area, double reach, double pitch,
                                              double toolDiameter, const PlungeRoughingSettings& settings);

    // The faces and loops projected and merged into one area, then its plunges row by row: down
    // at rapid to the approach height (the approach distance above the stock's
    // top, or the parts' without a stock), feeding down at the plunge feed to
    // the stock to leave above the parts under the tool (or to the stock's
    // bottom where no part is), and back at the retract feed. It moves between
    // plunges at the approach height, and starts and ends at the clearance plane.
    ToolpathResult GeneratePlungeRoughToolpath(const PlungeRoughToolpathInput& input);
}
