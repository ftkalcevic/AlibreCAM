// PathBuilder.cpp : adds an operation's moves to a toolpath, with entry and exit motions.

#include "PathBuilder.h"

#include "AdaptiveClearing.h"     // AppendHelix

#include <cmath>

namespace camcore
{
    namespace
    {
        const double kPi = 3.14159265358979323846;

        Vec3 Plus(const Vec3& a, const Vec3& b, double scale) { return { a.x + b.x * scale, a.y + b.y * scale, a.z }; }

        // Seen from +Z, true if turning from a to b is anticlockwise.
        bool TurnsLeft(const Vec3& a, const Vec3& b) { return a.x * b.y - a.y * b.x > 0.0; }
    }

    PathBuilder::PathBuilder(Toolpath& toolpath, const FeedSettings& feeds, double clearance, double cutZ)
        : m_toolpath(toolpath), m_feeds(feeds), m_clearance(clearance), m_cutZ(cutZ)
    {
    }

    void PathBuilder::SetEntryExit(const EntryExitSettings* pSettings, bool entry, bool exit)
    {
        m_pEntryExit = pSettings;
        // A helix is for getting into the material, which every level needs.
        m_useEntry = (entry || (pSettings != nullptr && pSettings->entry == EntryMode::Helix)) && pSettings != nullptr &&
                     pSettings->entry != EntryMode::None;
        m_useExit = exit && pSettings != nullptr && pSettings->exit != ExitMode::None;
    }

    void PathBuilder::Enter(const Vec3& point, const Vec3& direction, const Vec3& away)
    {
        if (!m_useEntry)
        {
            ArriveAt(point);
            return;
        }

        const EntryExitSettings& settings = *m_pEntryExit;
        if (settings.entry == EntryMode::Helix)
        {
            // Down a helix whose circle passes through the start, from the path height
            // above the cut, its centre that far in from the start (up the room's
            // slope) where there's room round it, smaller where there isn't.
            const double h = (std::max)(0.01, 0.05 * m_toolRadius);
            double nx = 0.0, ny = 0.0;
            if (m_helixRoom)
            {
                nx = m_helixRoom({ point.x + h, point.y, point.z }) - m_helixRoom({ point.x - h, point.y, point.z });
                ny = m_helixRoom({ point.x, point.y + h, point.z }) - m_helixRoom({ point.x, point.y - h, point.z });
            }
            const double g = std::hypot(nx, ny);
            nx = (g > 1e-12) ? nx / g : 1.0;
            ny = (g > 1e-12) ? ny / g : 0.0;
            for (double radius = settings.helixPercent / 100.0 * m_toolRadius; m_toolRadius > 0.0 &&
                                                                                 radius >= 0.1 * m_toolRadius;
                 radius *= 0.5)
            {
                const Vec3 centre{ point.x + nx * radius, point.y + ny * radius, m_cutZ };
                if (m_helixRoom && m_helixRoom(centre) < radius - 0.01)
                    continue;
                const Vec3 first{ point.x, point.y, m_cutZ + settings.alongPathHeight };
                TransferTo(first);
                Add(MoveKind::Feed, first, m_feeds.plunge);
                AppendHelix(m_toolpath, first, centre, m_cutZ, settings.alongPathAngle, m_feeds.engage,
                            m_helixAnticlockwise);
                return;
            }
            ArriveAt(point);    // no room for one
            return;
        }
        if (settings.entry == EntryMode::AlongPath3D)
        {
            // Down a ramp in line with the cut, from the height above it.
            const double angle = settings.alongPathAngle * kPi / 180.0;
            const Vec3 top = Plus(point, direction, -settings.alongPathHeight / std::tan(angle));
            TransferTo(top);
            Add(MoveKind::Feed, { top.x, top.y, m_cutZ + settings.alongPathHeight }, m_feeds.plunge);
            Cut(point);
            return;
        }

        const LeadMotion& lead = settings.engage;
        if (lead.shape == LeadShape::Radial)
        {
            // A quarter circle, its centre the radius off to one side, ending
            // tangent to the cut. It always starts behind the cut.
            const Vec3 side = (lead.radius < 0.0) ? Vec3{ -away.x, -away.y, 0.0 } : away;
            const double radius = std::fabs(lead.radius);
            const Vec3 centre = Plus(point, side, radius);
            ArriveAt(Plus(centre, direction, -radius));
            ArcTo(point, centre, !TurnsLeft(direction, side));
            return;
        }

        // A straight line, the angle from the cut's line, positive to the away side.
        const double angle = lead.angle * kPi / 180.0;
        Vec3 start = Plus(point, direction, -lead.length * std::cos(angle));
        start = Plus(start, away, lead.length * std::sin(angle));
        ArriveAt(start);
        Cut(point);
    }

    void PathBuilder::Exit(const Vec3& point, const Vec3& direction, const Vec3& away)
    {
        if (m_useExit)
        {
            const LeadMotion& lead = m_pEntryExit->retract;
            if (lead.shape == LeadShape::Radial)
            {
                // A quarter circle on from the cut, curving to one side.
                const Vec3 side = (lead.radius < 0.0) ? Vec3{ -away.x, -away.y, 0.0 } : away;
                const double radius = std::fabs(lead.radius);
                const Vec3 centre = Plus(point, side, radius);
                ArcTo(Plus(centre, direction, radius), centre, !TurnsLeft(direction, side));
            }
            else
            {
                const double angle = lead.angle * kPi / 180.0;
                const Vec3 end = Plus(point, direction, lead.length * std::cos(angle));
                Cut(Plus(end, away, lead.length * std::sin(angle)));
            }
        }
        Leave();
    }

    void PathBuilder::TransferTo(const Vec3& point)
    {
        const Vec3 above{ point.x, point.y, m_clearance };
        if (m_toolpath.moves.empty() || m_feeds.transferAtRapid)
            Add(MoveKind::Rapid, above, 0.0);
        else
            Add(MoveKind::Feed, above, m_feeds.transfer);
    }

    void PathBuilder::ArriveAt(const Vec3& point)
    {
        TransferTo(point);
        Add(MoveKind::Feed, { point.x, point.y, m_cutZ }, m_feeds.plunge);
    }

    void PathBuilder::Cut(const Vec3& point)
    {
        Add(MoveKind::Feed, { point.x, point.y, m_cutZ }, m_feeds.cut);
    }

    void PathBuilder::ArcTo(const Vec3& point, const Vec3& centre, bool clockwise)
    {
        Add(MoveKind::Feed, { point.x, point.y, m_cutZ }, m_feeds.cut);
        ToolpathMove& move = m_toolpath.moves.back();
        move.arc = true;
        move.centre = { centre.x, centre.y, m_cutZ };
        move.clockwise = clockwise;
    }

    void PathBuilder::MoveToZ(double z)
    {
        m_cutZ = z;
        if (m_toolpath.moves.empty())
            return;
        const Vec3 at = m_toolpath.moves.back().to;
        if (at.z != z)
            Add(MoveKind::Feed, { at.x, at.y, z }, m_feeds.plunge);
    }

    void PathBuilder::Leave()
    {
        if (m_toolpath.moves.empty())
            return;
        const Vec3 at = m_toolpath.moves.back().to;
        Add(MoveKind::Rapid, { at.x, at.y, m_clearance }, 0.0);
    }

    void PathBuilder::Add(MoveKind kind, const Vec3& to, double feed)
    {
        ToolpathMove move;
        move.kind = kind;
        move.to = to;
        move.feed = feed;
        m_toolpath.moves.push_back(move);
    }
}
