// Nurbs.h : NURBS surfaces, evaluated and searched for closest points. No Alibre or MFC code belongs here.

#pragma once

#include <vector>

#include "Origin.h"

namespace camcore
{
    // A NURBS surface. Control point (i, j), i along u and j along v, is
    // points[i + j * countU]. Weights are one a control point, or empty for a
    // surface that isn't rational. Orders are degree + 1, and each knot vector
    // has count + order knots.
    struct NurbsSurface
    {
        int orderU = 0;
        int orderV = 0;
        int countU = 0;
        int countV = 0;
        std::vector<double> knotsU;
        std::vector<double> knotsV;
        std::vector<Vec3> points;
        std::vector<double> weights;

        // Whether the sizes agree and the knots never go down.
        bool Validate() const;

        double LowU() const { return knotsU[static_cast<size_t>(orderU - 1)]; }
        double HighU() const { return knotsU[static_cast<size_t>(countU)]; }
        double LowV() const { return knotsV[static_cast<size_t>(orderV - 1)]; }
        double HighV() const { return knotsV[static_cast<size_t>(countV)]; }

        // The point at (u, v) and, where asked for, its first and second derivatives.
        void Evaluate(double u, double v, Vec3& point, Vec3* du = nullptr, Vec3* dv = nullptr, Vec3* duu = nullptr,
                      Vec3* duv = nullptr, Vec3* dvv = nullptr) const;

        // Samples the surface for ClosestPoint's first guesses. Call once the
        // control points are final; after this the surface is safe on any thread.
        void Prepare();

        // The closest point on the surface (within its parameter range) to p: the
        // nearest sample, then Newton's method on (u, v). False if not prepared.
        bool ClosestPoint(const Vec3& p, Vec3& closest, double* u = nullptr, double* v = nullptr) const;

    private:
        struct Sample
        {
            Vec3 point;
            double u, v;
        };
        std::vector<Sample> m_samples;
        std::vector<int> m_tree;        // the samples as a k-d tree, by index
        int Nearest(const Vec3& p) const;
        void Build(int first, int last, int depth);
    };
}
