// Nurbs.cpp : NURBS surfaces, evaluated and searched for closest points.

#include "Nurbs.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace camcore
{
    namespace
    {
        // The knot span holding t (Piegl and Tiller A2.1), for degree p and n + 1 control points.
        int FindSpan(int n, int p, double t, const std::vector<double>& knots)
        {
            if (t >= knots[static_cast<size_t>(n + 1)])
            {
                // At the top end: the last span that isn't empty.
                int span = n;
                while (span > p && knots[static_cast<size_t>(span)] >= knots[static_cast<size_t>(span + 1)])
                    --span;
                return span;
            }
            if (t <= knots[static_cast<size_t>(p)])
            {
                int span = p;
                while (span < n && knots[static_cast<size_t>(span + 1)] <= t)
                    ++span;
                return span;
            }
            int low = p, high = n + 1;
            int mid = (low + high) / 2;
            while (t < knots[static_cast<size_t>(mid)] || t >= knots[static_cast<size_t>(mid + 1)])
            {
                if (t < knots[static_cast<size_t>(mid)])
                    high = mid;
                else
                    low = mid;
                mid = (low + high) / 2;
            }
            return mid;
        }

        // The basis functions that aren't zero at t and their derivatives up to the
        // second (Piegl and Tiller A2.3): ders[k][j] is the k-th derivative of
        // N(span - p + j). p is at most 15.
        void BasisDerivatives(int span, double t, int p, const std::vector<double>& knots, double ders[3][16])
        {
            double ndu[16][16];
            double left[16], right[16];
            ndu[0][0] = 1.0;
            for (int j = 1; j <= p; ++j)
            {
                left[j] = t - knots[static_cast<size_t>(span + 1 - j)];
                right[j] = knots[static_cast<size_t>(span + j)] - t;
                double saved = 0.0;
                for (int r = 0; r < j; ++r)
                {
                    ndu[j][r] = right[r + 1] + left[j - r];
                    const double temp = (ndu[j][r] != 0.0) ? ndu[r][j - 1] / ndu[j][r] : 0.0;
                    ndu[r][j] = saved + right[r + 1] * temp;
                    saved = left[j - r] * temp;
                }
                ndu[j][j] = saved;
            }
            for (int j = 0; j <= p; ++j)
                ders[0][j] = ndu[j][p];
            const int n = (std::min)(2, p);
            for (int k = 1; k <= 2; ++k)
            {
                for (int j = 0; j <= p; ++j)
                    ders[k][j] = 0.0;
            }
            double a[2][16];
            for (int r = 0; r <= p; ++r)
            {
                int s1 = 0, s2 = 1;
                a[0][0] = 1.0;
                for (int k = 1; k <= n; ++k)
                {
                    double d = 0.0;
                    const int rk = r - k, pk = p - k;
                    if (r >= k)
                    {
                        a[s2][0] = (ndu[pk + 1][rk] != 0.0) ? a[s1][0] / ndu[pk + 1][rk] : 0.0;
                        d = a[s2][0] * ndu[rk][pk];
                    }
                    const int j1 = (rk >= -1) ? 1 : -rk;
                    const int j2 = (r - 1 <= pk) ? k - 1 : p - r;
                    for (int j = j1; j <= j2; ++j)
                    {
                        a[s2][j] = (ndu[pk + 1][rk + j] != 0.0) ? (a[s1][j] - a[s1][j - 1]) / ndu[pk + 1][rk + j] : 0.0;
                        d += a[s2][j] * ndu[rk + j][pk];
                    }
                    if (r <= pk)
                    {
                        a[s2][k] = (ndu[pk + 1][r] != 0.0) ? -a[s1][k - 1] / ndu[pk + 1][r] : 0.0;
                        d += a[s2][k] * ndu[r][pk];
                    }
                    ders[k][r] = d;
                    std::swap(s1, s2);
                }
            }
            int factor = p;
            for (int k = 1; k <= n; ++k)
            {
                for (int j = 0; j <= p; ++j)
                    ders[k][j] *= factor;
                factor *= (p - k);
            }
        }
    }

    bool NurbsSurface::Validate() const
    {
        if (orderU < 1 || orderV < 1 || orderU > 16 || orderV > 16 || countU < orderU || countV < orderV)
            return false;
        if (knotsU.size() != static_cast<size_t>(countU + orderU) ||
            knotsV.size() != static_cast<size_t>(countV + orderV))
            return false;
        if (points.size() != static_cast<size_t>(countU) * static_cast<size_t>(countV))
            return false;
        if (!weights.empty() && weights.size() != points.size())
            return false;
        for (const std::vector<double>* knots : { &knotsU, &knotsV })
        {
            for (size_t k = 1; k < knots->size(); ++k)
            {
                if ((*knots)[k] < (*knots)[k - 1] || !std::isfinite((*knots)[k]))
                    return false;
            }
        }
        for (double w : weights)
        {
            if (!(w > 0.0) || !std::isfinite(w))
                return false;
        }
        return LowU() < HighU() && LowV() < HighV();
    }

    void NurbsSurface::Evaluate(double u, double v, Vec3& point, Vec3* du, Vec3* dv, Vec3* duu, Vec3* duv,
                                Vec3* dvv) const
    {
        const int pu = orderU - 1, pv = orderV - 1;
        u = (std::max)(LowU(), (std::min)(HighU(), u));
        v = (std::max)(LowV(), (std::min)(HighV(), v));
        const int su = FindSpan(countU - 1, pu, u, knotsU);
        const int sv = FindSpan(countV - 1, pv, v, knotsV);
        double nu[3][16], nv[3][16];
        BasisDerivatives(su, u, pu, knotsU, nu);
        BasisDerivatives(sv, v, pv, knotsV, nv);

        // The weighted sums A(k, l) (point times weight) and W(k, l), for the
        // derivatives k times in u and l in v, k + l at most 2.
        Vec3 a[3][3] = {};
        double w[3][3] = {};
        const bool rational = !weights.empty();
        for (int j = 0; j <= pv; ++j)
        {
            const int cj = sv - pv + j;
            for (int i = 0; i <= pu; ++i)
            {
                const int ci = su - pu + i;
                const size_t index = static_cast<size_t>(ci) + static_cast<size_t>(cj) * countU;
                const double weight = rational ? weights[index] : 1.0;
                const Vec3 pw = points[index] * weight;
                for (int k = 0; k <= 2; ++k)
                {
                    for (int l = 0; l + k <= 2; ++l)
                    {
                        const double b = nu[k][i] * nv[l][j];
                        a[k][l] = a[k][l] + pw * b;
                        w[k][l] += weight * b;
                    }
                }
            }
        }
        // The quotient rule for S = A / W.
        const double w0 = (w[0][0] != 0.0) ? w[0][0] : 1.0;
        const Vec3 s = a[0][0] * (1.0 / w0);
        point = s;
        const Vec3 su1 = (a[1][0] - s * w[1][0]) * (1.0 / w0);
        const Vec3 sv1 = (a[0][1] - s * w[0][1]) * (1.0 / w0);
        if (du != nullptr)
            *du = su1;
        if (dv != nullptr)
            *dv = sv1;
        if (duu != nullptr)
            *duu = (a[2][0] - su1 * (2.0 * w[1][0]) - s * w[2][0]) * (1.0 / w0);
        if (duv != nullptr)
            *duv = (a[1][1] - sv1 * w[1][0] - su1 * w[0][1] - s * w[1][1]) * (1.0 / w0);
        if (dvv != nullptr)
            *dvv = (a[0][2] - sv1 * (2.0 * w[0][1]) - s * w[0][2]) * (1.0 / w0);
    }

    void NurbsSurface::Prepare()
    {
        m_samples.clear();
        m_tree.clear();
        if (!Validate())
            return;
        // Several samples a knot span, at least 48 and at most 256 each way.
        const auto steps = [](int count, int order) {
            return (std::max)(48, (std::min)(256, 6 * (count - order + 1)));
        };
        const int nu = steps(countU, orderU), nv = steps(countV, orderV);
        for (int j = 0; j <= nv; ++j)
        {
            const double v = LowV() + (HighV() - LowV()) * j / nv;
            for (int i = 0; i <= nu; ++i)
            {
                const double u = LowU() + (HighU() - LowU()) * i / nu;
                Sample sample;
                Evaluate(u, v, sample.point);
                sample.u = u;
                sample.v = v;
                m_samples.push_back(sample);
            }
        }
        m_tree.resize(m_samples.size());
        for (size_t k = 0; k < m_tree.size(); ++k)
            m_tree[k] = static_cast<int>(k);
        Build(0, static_cast<int>(m_tree.size()), 0);
    }

    // A k-d tree in place: each range's middle splits it along x, y, z in turn.
    void NurbsSurface::Build(int first, int last, int depth)
    {
        if (last - first <= 1)
            return;
        const int axis = depth % 3;
        const int middle = (first + last) / 2;
        const auto key = [&](int index) {
            const Vec3& p = m_samples[static_cast<size_t>(index)].point;
            return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
        };
        std::nth_element(m_tree.begin() + first, m_tree.begin() + middle, m_tree.begin() + last,
                         [&](int a, int b) { return key(a) < key(b); });
        Build(first, middle, depth + 1);
        Build(middle + 1, last, depth + 1);
    }

    int NurbsSurface::Nearest(const Vec3& p) const
    {
        int best = -1;
        double bestDistance = std::numeric_limits<double>::max();
        struct Range
        {
            int first, last, depth;
        };
        Range stack[64];
        int top = 0;
        stack[top++] = { 0, static_cast<int>(m_tree.size()), 0 };
        while (top > 0)
        {
            const Range r = stack[--top];
            if (r.last <= r.first)
                continue;
            const int middle = (r.first + r.last) / 2;
            const int index = m_tree[static_cast<size_t>(middle)];
            const Vec3& q = m_samples[static_cast<size_t>(index)].point;
            const Vec3 d = p - q;
            const double distance = Dot(d, d);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = index;
            }
            const int axis = r.depth % 3;
            const double offset = axis == 0 ? d.x : axis == 1 ? d.y : d.z;
            const Range nearSide = (offset < 0.0) ? Range{ r.first, middle, r.depth + 1 }
                                                  : Range{ middle + 1, r.last, r.depth + 1 };
            const Range farSide = (offset < 0.0) ? Range{ middle + 1, r.last, r.depth + 1 }
                                                 : Range{ r.first, middle, r.depth + 1 };
            // The far side only if the splitting plane is nearer than the best so far.
            if (offset * offset < bestDistance && top < 63)
                stack[top++] = farSide;
            if (top < 63)
                stack[top++] = nearSide;
        }
        return best;
    }

    bool NurbsSurface::ClosestPoint(const Vec3& p, Vec3& closest, double* uOut, double* vOut) const
    {
        if (m_samples.empty())
            return false;
        const int seed = Nearest(p);
        if (seed < 0)
            return false;
        double u = m_samples[static_cast<size_t>(seed)].u, v = m_samples[static_cast<size_t>(seed)].v;
        Vec3 s, su, sv, suu, suv, svv;
        Evaluate(u, v, s, &su, &sv, &suu, &suv, &svv);
        double best = Length(s - p);
        Vec3 bestPoint = s;
        double bestU = u, bestV = v;
        // Newton's method on the squared distance, damped when it doesn't help
        // (Levenberg), clamped to the parameter range.
        double damping = 0.0;
        for (int k = 0; k < 40; ++k)
        {
            const Vec3 r = s - p;
            const double g0 = Dot(r, su), g1 = Dot(r, sv);
            double h00 = Dot(su, su) + Dot(r, suu), h01 = Dot(su, sv) + Dot(r, suv), h11 = Dot(sv, sv) + Dot(r, svv);
            const double scale = (std::max)(1e-30, Dot(su, su) + Dot(sv, sv));
            h00 += damping * scale;
            h11 += damping * scale;
            const double det = h00 * h11 - h01 * h01;
            double stepU = 0.0, stepV = 0.0;
            if (det > 1e-30 * scale * scale && h00 > 0.0)
            {
                stepU = -(h11 * g0 - h01 * g1) / det;
                stepV = -(-h01 * g0 + h00 * g1) / det;
            }
            else
            {
                // Not a minimum here: a gradient step.
                stepU = -g0 / scale;
                stepV = -g1 / scale;
            }
            const double nu = (std::max)(LowU(), (std::min)(HighU(), u + stepU));
            const double nv = (std::max)(LowV(), (std::min)(HighV(), v + stepV));
            Vec3 ns, nsu, nsv, nsuu, nsuv, nsvv;
            Evaluate(nu, nv, ns, &nsu, &nsv, &nsuu, &nsuv, &nsvv);
            const double distance = Length(ns - p);
            if (distance <= best + 1e-12)
            {
                const bool tiny = std::fabs(nu - u) < 1e-12 * (HighU() - LowU() + 1.0) &&
                                  std::fabs(nv - v) < 1e-12 * (HighV() - LowV() + 1.0);
                u = nu;
                v = nv;
                s = ns;
                su = nsu;
                sv = nsv;
                suu = nsuu;
                suv = nsuv;
                svv = nsvv;
                const double gain = best - distance;
                best = distance;
                bestPoint = ns;
                bestU = u;
                bestV = v;
                damping *= 0.25;
                if (tiny || gain < 1e-12)
                    break;
            }
            else
            {
                damping = (damping == 0.0) ? 1e-3 : damping * 10.0;
                if (damping > 1e6)
                    break;
            }
        }
        closest = bestPoint;
        if (uOut != nullptr)
            *uOut = bestU;
        if (vOut != nullptr)
            *vOut = bestV;
        return true;
    }
}
