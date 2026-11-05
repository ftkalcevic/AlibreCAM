// PathBuilder.h : adds an operation's moves to a toolpath, with entry and exit motions.
// No Alibre or MFC code belongs here.
//
// Points are in the operation's origin frame, in millimetres. Directions are
// unit vectors in XY.

#pragma once

#include "EntryExit.h"
#include "Feeds.h"
#include "Toolpath.h"

namespace camcore
{
    class PathBuilder
    {
    public:
        PathBuilder(Toolpath& toolpath, const FeedSettings& feeds, double clearance, double cutZ);

        // The entry and exit motions, and whether this level uses each.
        void SetEntryExit(const EntryExitSettings* pSettings, bool entry, bool exit);

        // The Z that cuts, plunges and leads go to.
        void SetCutZ(double z) { m_cutZ = z; }
        double CutZ() const { return m_cutZ; }

        // Into a cut at point, which runs along direction. away is square to it,
        // pointing away from the geometry. A positive lead angle or radius puts the
        // lead on that side, and a negative one on the other.
        void Enter(const Vec3& point, const Vec3& direction, const Vec3& away);

        // Out of a cut that ended at point, running along direction, then up to
        // the clearance plane.
        void Exit(const Vec3& point, const Vec3& direction, const Vec3& away);

        // Across at the clearance plane to above the point.
        void TransferTo(const Vec3& point);

        // Across at the clearance plane to above the point, then down to cut.
        void ArriveAt(const Vec3& point);

        // Straight to point at the cut Z, at the cut feed.
        void Cut(const Vec3& point);

        // Round an arc about centre to point at the cut Z, at the cut feed.
        void ArcTo(const Vec3& point, const Vec3& centre, bool clockwise);

        // Straight up or down to z at the plunge feed, which then becomes the cut Z.
        void MoveToZ(double z);

        // Straight up to the clearance plane.
        void Leave();

    private:
        void Add(MoveKind kind, const Vec3& to, double feed);

        Toolpath& m_toolpath;
        const FeedSettings& m_feeds;
        double m_clearance;
        double m_cutZ;
        const EntryExitSettings* m_pEntryExit = nullptr;
        bool m_useEntry = false;
        bool m_useExit = false;
    };
}
