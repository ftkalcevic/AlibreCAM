// PseudoPost.cpp : the built-in post, which writes toolpaths as APT CL data.

#include "PseudoPost.h"

#include <cmath>
#include <cstdio>

#include "Features.h"

namespace camcore
{
    namespace
    {
        // APT style: a fixed 4 decimals, and never "-0.0000".
        std::string Number(double value)
        {
            if (std::fabs(value) < 0.00005)
                value = 0.0;

            char text[64];
            std::snprintf(text, sizeof(text), "%.4f", value);
            return text;
        }

        // A direction in the work zero's axes.
        Vec3 DirectionInFrame(const Frame& frame, const Vec3& world)
        {
            return { Dot(world, frame.xAxis), Dot(world, frame.yAxis), Dot(world, frame.zAxis) };
        }
    }

    std::string WriteAptClData(const std::string& jobName, const Frame& workZero,
                               const std::vector<PostOperation>& operations)
    {
        std::string out;
        const auto line = [&](const std::string& text) { out += text + "\n"; };

        line("PARTNO " + jobName);
        line("$$ CAMForAlibre APT CL data. Points are in mm from the work zero, the first Set Origin.");
        line("UNITS/MM");

        const SequenceStep* currentOrigin = nullptr;
        std::string currentTool;
        bool firstTool = true;
        double currentFeed = -1.0;

        for (const PostOperation& entry : operations)
        {
            if (entry.operation == nullptr)
                continue;
            const SequenceStep& operation = *entry.operation;

            if (entry.origin != currentOrigin)
            {
                currentOrigin = entry.origin;
                line("$$ ORIGIN " + (entry.origin != nullptr ? entry.origin->name : std::string("?")));
            }

            line("$$ OPERATION " + operation.name);

            const std::string toolKey = (entry.tool != nullptr) ? entry.tool->id : "";
            if (firstTool || toolKey != currentTool)
            {
                firstTool = false;
                currentTool = toolKey;
                if (entry.tool != nullptr)
                {
                    line("$$ TOOL " + entry.tool->name);
                    line("CUTTER/" + Number(entry.tool->diameter));
                    line("LOADTL/" + std::to_string(entry.tool->number));
                }
                else
                {
                    line("$$ TOOL ? This operation's tool isn't in the library.");
                }
            }

            line("SPINDL/RPM," + Number(operation.feeds.spindleRpm) + ",CLW");

            // The tool axis is the operation's origin Z, seen from the work zero.
            const Vec3 axis = DirectionInFrame(workZero, entry.frame.zAxis);
            const std::string axisText = Number(axis.x) + "," + Number(axis.y) + "," + Number(axis.z);

            Vec3 from;      // where the tool is, in the operation's frame
            bool haveFrom = false;
            for (const ToolpathMove& move : operation.toolpath.moves)
            {
                if (move.kind == MoveKind::Dwell)
                {
                    line("DELAY/" + Number(move.seconds));
                    continue;
                }

                if (move.kind == MoveKind::Rapid)
                {
                    line("RAPID");
                }
                else if (move.feed != currentFeed)
                {
                    currentFeed = move.feed;
                    line("FEDRAT/MMPM," + Number(move.feed));
                }

                // An arc is a CIRCLE/ of its centre, axis and radius, then the GOTO/
                // to its end. The axis is its plane's normal (the tool axis for XY)
                // anticlockwise, reversed clockwise.
                if (move.kind == MoveKind::Feed && move.arc && haveFrom)
                {
                    const Vec3 centre = WorldToFrame(workZero, FramePointToWorld(entry.frame, move.centre));
                    const Vec3 local = (move.plane == ArcPlane::XY)   ? entry.frame.zAxis
                                       : (move.plane == ArcPlane::ZX) ? entry.frame.yAxis
                                                                      : entry.frame.xAxis;
                    const Vec3 normal = DirectionInFrame(workZero, local);
                    const Vec3 turn = move.clockwise ? normal * -1.0 : normal;
                    double fu, fv, fw, cu, cv, cw;
                    ToPlane(move.plane, from, fu, fv, fw);
                    ToPlane(move.plane, move.centre, cu, cv, cw);
                    const double radius = std::hypot(fu - cu, fv - cv);
                    line("CIRCLE/" + Number(centre.x) + "," + Number(centre.y) + "," + Number(centre.z) + "," +
                         Number(turn.x) + "," + Number(turn.y) + "," + Number(turn.z) + "," + Number(radius));
                }

                const Vec3 point = WorldToFrame(workZero, FramePointToWorld(entry.frame, move.to));
                line("GOTO/" + Number(point.x) + "," + Number(point.y) + "," + Number(point.z) + "," + axisText);
                from = move.to;
                haveFrom = true;
            }
        }

        line("SPINDL/OFF");
        line("END");
        line("FINI");
        return out;
    }
}
