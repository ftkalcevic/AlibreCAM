// EngraveToolpath.cpp : Engrave's lines and toolpath.

#include "EngraveToolpath.h"

#include <cmath>

#include "Clearance.h"
#include "CutLevels.h"
#include "PathBuilder.h"

namespace camcore
{
    namespace
    {
        constexpr double kJoinTolerance = 0.01;
        constexpr double kTiny = 1e-9;

        ToolpathResult Failure(const std::string& problem)
        {
            ToolpathResult result;
            result.problem = problem;
            return result;
        }

        bool Meet(const Vec3& a, const Vec3& b)
        {
            return std::hypot(a.x - b.x, a.y - b.y) <= kJoinTolerance;
        }

        // Adds a piece to the end of a line, the piece starting where the line ends.
        void Append(Path& line, const Path& piece)
        {
            line.back() = piece.front();
            line.insert(line.end(), piece.begin() + 1, piece.end());
        }

        // How many of the unused pieces, other than skip, have an end at the point.
        int EndsAt(const std::vector<Path>& pieces, const std::vector<bool>& used, size_t skip, const Vec3& point)
        {
            int count = 0;
            for (size_t i = 0; i < pieces.size(); ++i)
            {
                if (i == skip || used[i])
                    continue;
                count += Meet(pieces[i].front().point, point) ? 1 : 0;
                count += Meet(pieces[i].back().point, point) ? 1 : 0;
            }
            return count;
        }

        // Carries the line on from its end along the earliest unused piece that meets it.
        void Extend(Path& line, const std::vector<Path>& pieces, std::vector<bool>& used)
        {
            for (bool grew = true; grew;)
            {
                grew = false;
                for (size_t i = 0; i < pieces.size() && !grew; ++i)
                {
                    if (used[i])
                        continue;
                    if (Meet(pieces[i].front().point, line.back().point))
                        Append(line, pieces[i]);
                    else if (Meet(pieces[i].back().point, line.back().point))
                        Append(line, ReversePath(pieces[i], false));
                    else
                        continue;
                    used[i] = true;
                    grew = true;
                }
            }
        }

        // The way an open path runs at a vertex, along the segment that starts there.
        Vec3 DirectionFrom(const PathVertex& from, const Vec3& to)
        {
            Vec3 direction{ to.x - from.point.x, to.y - from.point.y, 0.0 };
            if (from.arc)
            {
                // Anticlockwise about the axis: the tangent is the axis crossed with the radius.
                const Vec3 radius{ from.point.x - from.centre.x, from.point.y - from.centre.y, 0.0 };
                const double sense = (from.axis.z < 0.0) ? -1.0 : 1.0;
                direction = { -radius.y * sense, radius.x * sense, 0.0 };
            }
            Vec3 unit;
            return Normalise(direction, unit) ? unit : Vec3{ 1.0, 0.0, 0.0 };
        }

        // The way an open path runs as it reaches to, along the segment from `from`.
        Vec3 DirectionInto(const PathVertex& from, const Vec3& to)
        {
            Vec3 direction{ to.x - from.point.x, to.y - from.point.y, 0.0 };
            if (from.arc)
            {
                const Vec3 radius{ to.x - from.centre.x, to.y - from.centre.y, 0.0 };
                const double sense = (from.axis.z < 0.0) ? -1.0 : 1.0;
                direction = { -radius.y * sense, radius.x * sense, 0.0 };
            }
            Vec3 unit;
            return Normalise(direction, unit) ? unit : Vec3{ 1.0, 0.0, 0.0 };
        }

        Vec3 LeftOf(const Vec3& direction) { return { -direction.y, direction.x, 0.0 }; }

        // The tool along a line at the builder's cut Z, from its first vertex.
        void Follow(PathBuilder& builder, const Path& path, bool closed)
        {
            const size_t count = path.size();
            const size_t segments = closed ? count : count - 1;
            for (size_t i = 0; i < segments; ++i)
            {
                const PathVertex& from = path[i];
                const Vec3& to = path[(i + 1) % count].point;
                if (from.arc)
                    builder.ArcTo(to, from.centre, from.axis.z < 0.0);
                else
                    builder.Cut(to);
            }
        }

        // Where a run along the line starts and ends, and which way it's going there.
        struct Ends
        {
            Vec3 start, startDirection;
            Vec3 end, endDirection;
        };

        Ends EndsOf(const Path& path, bool closed)
        {
            Ends ends;
            ends.start = path.front().point;
            ends.startDirection = DirectionFrom(path.front(), path[1 % path.size()].point);
            if (closed)
            {
                ends.end = ends.start;
                ends.endDirection = DirectionInto(path.back(), path.front().point);
            }
            else
            {
                ends.end = path.back().point;
                ends.endDirection = DirectionInto(path[path.size() - 2], path.back().point);
            }
            return ends;
        }
    }

    std::vector<EngraveLine> JoinEngraveLines(const std::vector<Path>& input, double level)
    {
        std::vector<Path> pieces;
        for (const Path& piece : input)
        {
            if (piece.size() < 2)
                continue;
            Path flat = FlattenPath(piece, level);
            if (flat.size() >= 2 && LengthXY(flat) > kTiny)
                pieces.push_back(flat);
        }

        std::vector<EngraveLine> lines;
        std::vector<bool> used(pieces.size(), false);
        for (size_t first = 0; first < pieces.size(); ++first)
        {
            if (used[first])
                continue;
            used[first] = true;

            // Start at the piece's loose end if it has one.
            Path line = pieces[first];
            const bool startJoins = EndsAt(pieces, used, first, line.front().point) > 0;
            const bool endJoins = EndsAt(pieces, used, first, line.back().point) > 0;
            if (startJoins && !endJoins)
                line = ReversePath(line, false);

            // On from its end, then on from its start the other way, if that end wasn't loose.
            Extend(line, pieces, used);
            if (EndsAt(pieces, used, pieces.size(), line.front().point) > 0)
            {
                line = ReversePath(line, false);
                Extend(line, pieces, used);
                line = ReversePath(line, false);
            }

            EngraveLine engrave;
            engrave.closed = line.size() > 2 && Meet(line.front().point, line.back().point);
            if (engrave.closed)
                line.pop_back();    // a closed path doesn't repeat its first point
            engrave.path = std::move(line);
            lines.push_back(std::move(engrave));
        }
        return lines;
    }

    ToolpathResult GenerateEngraveToolpath(const EngraveToolpathInput& input)
    {
        const SequenceStep* pOperation = input.operation;
        if (pOperation == nullptr || pOperation->type != StepType::Engrave)
            return Failure("This isn't an engrave operation.");
        if (input.tool == nullptr || !(input.tool->diameter > 0.0))
            return Failure("Choose a tool on the Tool tab.");
        if (input.lines.empty())
            return Failure("There are no lines to engrave.");
        for (const std::vector<std::string>& problems :
             { ValidateFeeds(pOperation->feeds), ValidateCutLevels(pOperation->cutLevels),
               ValidateEntryExit(pOperation->entryExit) })
        {
            if (!problems.empty())
                return Failure(problems.front());
        }

        // The lines are the cut geometry, all at one height.
        const double z = input.lines.front().path.front().point.z;
        const std::vector<CutLevel> levels = CutLevelHeights(pOperation->cutLevels, z);
        const EntryExitSettings& entryExit = pOperation->entryExit;
        double clearance = 0.0;
        std::string clearanceProblem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, clearanceProblem,
                             input.haveStockMaxZ, input.stockMaxZ))
            return Failure(clearanceProblem);
        if (!(clearance > CutTopZ(pOperation->cutLevels, z)))
            return Failure("The clearance plane must be above the top of the cut.");
        if (entryExit.entry == EntryMode::AlongPath3D && !(clearance > levels.front().z + entryExit.alongPathHeight))
            return Failure("The along path height reaches the clearance plane.");

        ToolpathResult result;

        // One line, at levels first to last: in, along, and out, with or without
        // staying down between them.
        auto cutLine = [&](const EngraveLine& line, size_t firstLevel, size_t lastLevel, bool stayDown) {
            Path path = line.path;
            for (size_t l = firstLevel; l <= lastLevel; ++l)
            {
                const bool firstHere = l == firstLevel;
                const bool lastHere = l == lastLevel;
                PathBuilder builder(result.toolpath, pOperation->feeds, clearance, levels[l].z);
                builder.SetEntryExit(&entryExit, entryExit.eachCutLevel || l == 0,
                                     entryExit.eachCutLevel || l + 1 == levels.size());

                const Ends ends = EndsOf(path, line.closed);
                if (!stayDown || firstHere)
                    builder.Enter(ends.start, ends.startDirection, LeftOf(ends.startDirection));
                else
                    builder.MoveToZ(levels[l].z);

                Follow(builder, path, line.closed);

                if (!stayDown || lastHere)
                    builder.Exit(ends.end, ends.endDirection, LeftOf(ends.endDirection));
                else if (!line.closed)
                    path = ReversePath(path, false);    // back the other way from where it stopped
            }
        };

        const bool stayDown = pOperation->engraving.traversal == EngraveTraversal::StayDown;
        if (pOperation->cutLevels.order == CutLevelOrder::LevelFirst)
        {
            for (size_t l = 0; l < levels.size(); ++l)
            {
                for (const EngraveLine& line : input.lines)
                {
                    if (line.path.size() >= 2)
                        cutLine(line, l, l, false);
                }
            }
        }
        else
        {
            for (const EngraveLine& line : input.lines)
            {
                if (line.path.size() >= 2)
                    cutLine(line, 0, levels.size() - 1, stayDown);
            }
        }

        result.ok = !result.toolpath.moves.empty();
        if (!result.ok)
            result.problem = "There are no lines to engrave.";
        return result;
    }
}
