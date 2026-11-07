// PlungeRoughToolpath.h : calculates a Plunge Roughing operation's toolpath. No Alibre or MFC code belongs here.
//
// Points are in the operation's origin frame, in millimetres, so Z is the tool axis.

#pragma once

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

    // The plunge centres for one closed outline, row by row in the order they're
    // cut. Each plunge's tool, reach (its radius plus the stock to leave) wide,
    // stays inside the outline. Rows run along the angle of cuts, (pitch times
    // root 3 over 2) apart at most, alternate rows shifted half a pitch, so the
    // plunges pack as circles do; a row's ends get a plunge of their own when the
    // grid leaves more than half a pitch there.
    std::vector<std::vector<Vec3>> PlungeRows(const Path& outline, double reach, double pitch,
                                              const PlungeRoughingSettings& settings);

    // Each closed outline's plunges, in the outlines' order and row by row: down
    // at rapid to the approach height (the approach distance above the stock's
    // top, or the parts' without a stock), feeding down at the plunge feed to
    // the stock to leave above the parts under the tool (or to the stock's
    // bottom where no part is), and back at the retract feed. It moves between
    // plunges at the approach height, and starts and ends at the clearance plane.
    ToolpathResult GeneratePlungeRoughToolpath(const PlungeRoughToolpathInput& input);
}
