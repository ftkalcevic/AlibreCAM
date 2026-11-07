// DropCutter.h : how low a tool's tip can come down over the parts' triangles. No Alibre or MFC code belongs here.

#pragma once

#include <vector>

#include "Origin.h"
#include "Simulation.h"     // ToolProfile

namespace camcore
{
    // Drops a tool pointing straight down (along -Z) onto a mesh of triangles,
    // exactly for the tool's own shape: its face, edges and corners against each
    // triangle's face, edges and corners. The tool is grown by the stock to leave
    // as the waterline operations grow it: at distance d from its axis it's
    // HeightForRadius(d - s) - s above its tip, out to its radius plus s.
    class DropCutter
    {
    public:
        // triangles: three points to a triangle, in the frame the tool points down in.
        // faces, if not empty: the face each triangle is on (-1 for none known).
        DropCutter(const std::vector<Vec3>& triangles, const ToolProfile& profile, double stockToLeave,
                   const std::vector<int>& faces = {});

        // The lowest the tip may go with the tool's axis through (x, y), or false
        // if no triangle is within reach.
        bool TipAt(double x, double y, double& tip) const;

        // As TipAt, and which face the tool touches (-1 if not known), and how low
        // the tip may go for every other face, where that's within 0.5 mm of the tip
        // (otherwise anything at least 0.5 below it, or lowest() if none is within reach).
        struct Contact
        {
            double tip = 0.0;
            int face = -1;
            double others = 0.0;
        };
        bool ContactAt(double x, double y, Contact& contact) const;

        // The tool's height above its tip at distance d from its axis (grown by
        // the stock to leave), for d from 0 to Reach().
        double Rise(double d) const;
        double Reach() const { return m_reach; }

    private:
        struct Triangle
        {
            Vec3 p[3];
            double lowX, highX, lowY, highY, highZ;
            bool flat;              // a face seen from above (not edge on)
            double z0, gx, gy;      // its plane: z = z0 + gx x + gy y
            int face;
        };

        double Against(const Triangle& t, double x, double y) const;
        double AlongEdge(const Vec3& a, const Vec3& b, double x, double y) const;
        double ContactDistance(double slope) const;

        ToolProfile m_profile;
        double m_s = 0.0;
        double m_reach = 0.0;
        bool m_haveFaces = false;
        std::vector<Triangle> m_triangles;
        // Buckets of triangles over XY, each a reach wide.
        double m_bucket = 1.0;
        double m_xLow = 0.0, m_yLow = 0.0;
        int m_nx = 0, m_ny = 0;
        std::vector<std::vector<int>> m_buckets;
    };
}
