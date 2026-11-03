// DrillToolpath.cpp : calculates a Drill operation's toolpath.

#include "DrillToolpath.h"

#include <algorithm>
#include <cmath>

#include "Features.h"

namespace camcore
{
    namespace
    {
        const double kPi = 3.14159265358979323846;

        ToolpathResult Failure(const std::string& problem)
        {
            ToolpathResult result;
            result.problem = problem;
            return result;
        }

        class PathBuilder
        {
        public:
            explicit PathBuilder(Toolpath& toolpath) : m_toolpath(toolpath) {}

            void Rapid(const Vec3& to) { Add(MoveKind::Rapid, to, 0.0, 0.0); }
            void Feed(const Vec3& to, double feed) { Add(MoveKind::Feed, to, feed, 0.0); }
            void Dwell(double seconds) { Add(MoveKind::Dwell, m_at, 0.0, seconds); }

            const Vec3& At() const { return m_at; }

        private:
            void Add(MoveKind kind, const Vec3& to, double feed, double seconds)
            {
                ToolpathMove move;
                move.kind = kind;
                move.to = to;
                move.feed = feed;
                move.seconds = seconds;
                m_toolpath.moves.push_back(move);
                m_at = to;
            }

            Toolpath& m_toolpath;
            Vec3 m_at;
        };
    }

    double DrillPointLength(const Tool& tool)
    {
        if (tool.type != ToolType::Drill || !(tool.pointAngle > 0.0) || !(tool.pointAngle < 180.0))
            return 0.0;
        return (tool.diameter / 2.0) / std::tan(tool.pointAngle / 2.0 * kPi / 180.0);
    }

    bool ClearanceHeight(const ClearanceSettings& clearance, bool havePartMaxZ, double partMaxZ, double& height,
                         std::string& problem)
    {
        switch (clearance.mode)
        {
        case ClearanceMode::AbsoluteZ:
            height = clearance.absoluteZ;
            return true;

        case ClearanceMode::StockMaxZ:
            problem = "The clearance plane is set from the stock, and there's no stock yet.";
            return false;

        case ClearanceMode::PartMaxZ:
        case ClearanceMode::Automatic:
        default:
            if (!havePartMaxZ)
            {
                problem = "The parts' height couldn't be read for the clearance plane.";
                return false;
            }
            height = partMaxZ + (clearance.mode == ClearanceMode::PartMaxZ ? clearance.partDistance
                                                                            : kAutomaticClearanceAboveParts);
            return true;
        }
    }

    ToolpathResult GenerateDrillToolpath(const DrillToolpathInput& input)
    {
        const SequenceStep* pOperation = input.operation;
        if (pOperation == nullptr || pOperation->type != StepType::Drill)
            return Failure("This isn't a drill operation.");
        if (input.tool == nullptr)
            return Failure("Choose a tool on the Tool tab.");
        if (pOperation->features.empty())
            return Failure("There are no features to drill.");

        const DrillSettings& drill = pOperation->drill;
        const std::vector<std::string> drillProblems = ValidateDrillSettings(drill);
        if (!drillProblems.empty())
            return Failure(drillProblems.front());
        const std::vector<std::string> feedProblems = ValidateFeeds(pOperation->feeds);
        if (!feedProblems.empty())
            return Failure(feedProblems.front());

        double clearance = 0.0;
        std::string clearanceProblem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, clearanceProblem))
            return Failure(clearanceProblem);

        const FeedSettings& feeds = pOperation->feeds;
        const double extra = drill.addTipToDepth ? DrillPointLength(*input.tool) : 0.0;

        ToolpathResult result;
        PathBuilder path(result.toolpath);
        bool first = true;

        for (const PickedGeometry& feature : pOperation->features)
        {
            const Vec3 point = WorldToFrame(input.frame, FeaturePoint(input.frame, feature));

            // The picked geometry is the hole's top or bottom. Depth runs down from the top.
            const double top = (drill.location == GeometryLocation::AtBottom) ? point.z + drill.depth : point.z;
            const double bottom = top - drill.depth - extra;
            const double approach = top + drill.approachDistance;

            if (approach > clearance)
            {
                return Failure("The clearance plane is below a hole's approach height. Raise the clearance plane, "
                               "or shorten the approach distance.");
            }

            // Across to the hole at the clearance plane.
            const Vec3 above{ point.x, point.y, clearance };
            if (first || feeds.transferAtRapid)
                path.Rapid(above);
            else
                path.Feed(above, feeds.transfer);
            first = false;

            path.Rapid({ point.x, point.y, approach });

            if (drill.pecking)
            {
                // Each peck goes down at the plunge feed, then all the way back to the approach height.
                double reached = top;
                while (reached > bottom)
                {
                    const double next = (std::max)(reached - drill.peckDepth, bottom);
                    if (reached < top)
                        path.Rapid({ point.x, point.y, (std::min)(reached + kPeckReentryGap, approach) });
                    path.Feed({ point.x, point.y, next }, feeds.plunge);
                    reached = next;
                    if (reached > bottom)
                        path.Rapid({ point.x, point.y, approach });
                }
            }
            else
            {
                path.Feed({ point.x, point.y, bottom }, feeds.plunge);
            }

            if (drill.dwell == DwellMode::Seconds)
                path.Dwell(drill.dwellSeconds);
            else if (drill.dwell == DwellMode::Revolutions)
                path.Dwell(drill.dwellRevolutions / feeds.spindleRpm * 60.0);

            path.Feed({ point.x, point.y, approach }, feeds.retract);
            path.Rapid(above);
        }

        result.ok = true;
        return result;
    }
}
