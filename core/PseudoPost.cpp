// PseudoPost.cpp : writes toolpaths as readable pseudo code, before real posts exist.

#include "PseudoPost.h"

#include <cmath>
#include <cstdio>

namespace camcore
{
    namespace
    {
        // Up to 4 decimals, without trailing zeros, and never "-0".
        std::string Number(double value)
        {
            if (std::fabs(value) < 0.00005)
                value = 0.0;

            char text[64];
            std::snprintf(text, sizeof(text), "%.4f", value);
            std::string result = text;
            const size_t point = result.find('.');
            if (point != std::string::npos)
            {
                result.erase(result.find_last_not_of('0') + 1);
                if (result.back() == '.')
                    result.pop_back();
            }
            return result;
        }

        std::string Point(const Vec3& v)
        {
            return "(" + Number(v.x) + ", " + Number(v.y) + ", " + Number(v.z) + ")";
        }

        std::string Quoted(const std::string& text)
        {
            return "\"" + text + "\"";
        }

        const char* OperationType(StepType type)
        {
            switch (type)
            {
            case StepType::Drill:  return "drill";
            case StepType::Facing: return "facing";
            default:               return "operation";
            }
        }
    }

    std::string WritePseudoPost(const std::string& jobName, const std::vector<PostOperation>& operations)
    {
        std::string out;
        const auto line = [&](const std::string& text) { out += text + "\n"; };

        line("; AlibreCAM pseudo code. Coordinates are in each origin's frame, in mm.");
        line("PROGRAM START " + Quoted(jobName));

        const SequenceStep* currentOrigin = nullptr;
        std::string currentTool;
        bool firstTool = true;

        for (const PostOperation& entry : operations)
        {
            if (entry.operation == nullptr)
                continue;
            const SequenceStep& operation = *entry.operation;

            if (entry.origin != currentOrigin)
            {
                currentOrigin = entry.origin;
                const Frame& frame = entry.frame;
                line("ORIGIN " + Quoted(entry.origin != nullptr ? entry.origin->name : "") + " AT " +
                     Point(frame.origin) + " X " + Point(frame.xAxis) + " Y " + Point(frame.yAxis) + " Z " +
                     Point(frame.zAxis));
            }

            const std::string toolKey = (entry.tool != nullptr) ? entry.tool->id : "";
            if (firstTool || toolKey != currentTool)
            {
                firstTool = false;
                currentTool = toolKey;
                if (entry.tool != nullptr)
                {
                    line("TOOL T" + std::to_string(entry.tool->number) + " " + Quoted(entry.tool->name) +
                         " DIAMETER " + Number(entry.tool->diameter));
                }
                else
                {
                    line("TOOL ? ; this operation's tool isn't in the library");
                }
            }

            line("OPERATION " + Quoted(operation.name) + " TYPE " + OperationType(operation.type));
            line("SPINDLE " + Number(operation.feeds.spindleRpm) + " RPM");

            for (const ToolpathMove& move : operation.toolpath.moves)
            {
                const std::string at = "X" + Number(move.to.x) + " Y" + Number(move.to.y) + " Z" + Number(move.to.z);
                switch (move.kind)
                {
                case MoveKind::Rapid: line("  RAPID " + at); break;
                case MoveKind::Feed:  line("  FEED " + at + " F" + Number(move.feed)); break;
                case MoveKind::Dwell: line("  DWELL " + Number(move.seconds) + " S"); break;
                }
            }

            line("SPINDLE OFF");
            line("OPERATION END");
        }

        line("PROGRAM END");
        return out;
    }
}
