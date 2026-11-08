// ScriptedPost.cpp : turns toolpaths into G-code by following a plain text post script.

#include "ScriptedPost.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

#include "Features.h"

namespace camcore
{
    namespace
    {
        const std::set<std::string> kSections = { "program start", "tool change", "operation start", "rapid",
                                                  "feed", "arc cw", "arc ccw", "dwell", "plane xy", "plane zx", "plane yz", "plane xz",
                                                  "operation end",
                                                  "program end" };

        const std::set<std::string> kWords = { "X", "Y", "Z", "I", "J", "K", "F" };
        const std::set<std::string> kValues = { "TOOL", "TOOLNAME", "TOOLDIA", "SPINDLE", "SECONDS",
                                                "PROGRAM", "OPERATION", "ORIGIN", "DATE" };

        std::string Trim(const std::string& text)
        {
            const size_t first = text.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                return std::string();
            const size_t last = text.find_last_not_of(" \t\r");
            return text.substr(first, last - first + 1);
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // One {...}: what's written in front, and the name.
        struct Field
        {
            std::string prefix;
            std::string name;
        };

        // Splits "{ X}" into " " and "X". The name is the capitals at the end.
        bool SplitField(const std::string& inside, Field& field)
        {
            size_t start = inside.size();
            while (start > 0 && std::isupper(static_cast<unsigned char>(inside[start - 1])))
                --start;
            if (start == inside.size())
                return false;
            field.prefix = inside.substr(0, start);
            field.name = inside.substr(start);
            return true;
        }

        bool ReadSetting(const std::string& key, const std::string& value, PostScript& script, std::string& problem)
        {
            auto number = [&](int& target, int low, int high) {
                char* end = nullptr;
                const long parsed = std::strtol(value.c_str(), &end, 10);
                if (value.empty() || *end != '\0' || parsed < low || parsed > high)
                {
                    problem = key + " must be a whole number from " + std::to_string(low) + " to " +
                              std::to_string(high) + ".";
                    return false;
                }
                target = static_cast<int>(parsed);
                return true;
            };
            auto yesNo = [&](bool& target) {
                const std::string word = Lower(value);
                if (word != "yes" && word != "no")
                {
                    problem = key + " must be yes or no.";
                    return false;
                }
                target = (word == "yes");
                return true;
            };

            if (key == "name")
                script.name = value;
            else if (key == "extension")
                script.extension = value;
            else if (key == "axes")
            {
                if (!number(script.axes, 3, 3))
                {
                    problem = "axes can only be 3 so far.";
                    return false;
                }
            }
            else if (key == "decimals")
                return number(script.decimals, 0, 6);
            else if (key == "feed decimals")
                return number(script.feedDecimals, 0, 6);
            else if (key == "trim zeros")
                return yesNo(script.trimZeros);
            else if (key == "line numbers")
                return yesNo(script.lineNumbers);
            else if (key == "line number start")
                return number(script.lineNumberStart, 0, 1000000);
            else if (key == "line number step")
                return number(script.lineNumberStep, 1, 1000);
            else
            {
                problem = "There's no setting called \"" + key + "\".";
                return false;
            }
            return true;
        }

        // Fills in a script's {...} as the program is written.
        class Writer
        {
        public:
            explicit Writer(const PostScript& script) : m_script(script), m_line(script.lineNumberStart) {}

            std::string text;

            // The values {...} fill in from, set as the program goes.
            std::map<std::string, double> words;            // X Y Z I J K F for this move
            std::map<std::string, std::string> values;      // TOOL, OPERATION and the rest

            // Forgets the last X, Y, Z and F, so the next ones are written.
            void Forget() { m_last.clear(); }

            // Writes a section. A motion section's line that moves nothing is skipped.
            void Section(const std::string& name, bool motion = false)
            {
                const auto found = m_script.sections.find(name);
                if (found == m_script.sections.end())
                    return;
                for (const std::string& line : found->second)
                    Line(line, motion);
            }

        private:
            std::string Number(double value, int decimals) const
            {
                const double smallest = 0.5 * std::pow(10.0, -decimals);
                if (std::fabs(value) < smallest)
                    value = 0.0;
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
                std::string out = buffer;
                if (m_script.trimZeros && out.find('.') != std::string::npos)
                {
                    while (!out.empty() && out.back() == '0')
                        out.pop_back();
                    if (!out.empty() && out.back() == '.')
                        out.pop_back();
                    if (out == "-0")
                        out = "0";
                }
                return out;
            }

            static std::string Safe(std::string value)
            {
                std::replace(value.begin(), value.end(), '(', '[');
                std::replace(value.begin(), value.end(), ')', ']');
                return value;
            }

            void Line(const std::string& line, bool motion)
            {
                std::string out;
                bool moved = false;
                std::map<std::string, std::string> written;
                size_t at = 0;
                while (at < line.size())
                {
                    const size_t open = line.find('{', at);
                    if (open == std::string::npos)
                    {
                        out += line.substr(at);
                        break;
                    }
                    out += line.substr(at, open - at);
                    const size_t close = line.find('}', open);
                    Field field;
                    SplitField(line.substr(open + 1, close - open - 1), field);
                    at = close + 1;

                    if (kWords.count(field.name) != 0)
                    {
                        const auto word = words.find(field.name);
                        if (word == words.end())
                            continue;
                        const bool feed = field.name == "F";
                        const std::string number = Number(word->second, feed ? m_script.feedDecimals : m_script.decimals);
                        const bool modal = field.name == "X" || field.name == "Y" || field.name == "Z" || feed;
                        const auto last = m_last.find(field.name);
                        if (modal && last != m_last.end() && last->second == number)
                            continue;
                        out += field.prefix + field.name + number;
                        written[field.name] = number;
                        if (field.name == "X" || field.name == "Y" || field.name == "Z")
                            moved = true;
                    }
                    else
                    {
                        const auto value = values.find(field.name);
                        if (value != values.end())
                            out += field.prefix + Safe(value->second);
                    }
                }

                if (motion && !moved)
                    return;
                for (const auto& word : written)
                    m_last[word.first] = word.second;

                const bool numbered = m_script.lineNumbers && !out.empty() && out[0] != '%' && out[0] != '(';
                if (numbered)
                {
                    out = "N" + std::to_string(m_line) + " " + out;
                    m_line += m_script.lineNumberStep;
                }
                text += out + "\n";
            }

            const PostScript& m_script;
            std::map<std::string, std::string> m_last;
            int m_line;
        };
    }

    PostScriptRead ReadPostScript(const std::string& text)
    {
        PostScriptRead result;
        std::istringstream lines(text);
        std::string raw;
        std::string section;
        int number = 0;
        auto refuse = [&](const std::string& problem) {
            PostScriptRead failed;
            failed.problem = "Line " + std::to_string(number) + ": " + problem;
            return failed;
        };

        while (std::getline(lines, raw))
        {
            ++number;
            if (!raw.empty() && raw.back() == '\r')
                raw.pop_back();
            const std::string trimmed = Trim(raw);
            if (!trimmed.empty() && trimmed[0] == ';')
                continue;

            if (!trimmed.empty() && trimmed[0] == '[' && trimmed.back() == ']')
            {
                section = Lower(Trim(trimmed.substr(1, trimmed.size() - 2)));
                if (section != "settings" && kSections.count(section) == 0)
                    return refuse("There's no section called [" + section + "].");
                result.script.sections[section];
                continue;
            }
            if (section.empty())
            {
                if (trimmed.empty())
                    continue;
                return refuse("Text before the first [section].");
            }

            if (section == "settings")
            {
                if (trimmed.empty())
                    continue;
                std::string line = trimmed;
                const size_t note = line.find(';');
                if (note != std::string::npos)
                    line = Trim(line.substr(0, note));
                const size_t equals = line.find('=');
                if (equals == std::string::npos)
                    return refuse("A setting needs a name = value.");
                std::string problem;
                if (!ReadSetting(Lower(Trim(line.substr(0, equals))), Trim(line.substr(equals + 1)), result.script,
                                 problem))
                    return refuse(problem);
                continue;
            }

            // A section's line. Blank lines aren't written; others are, filled in.
            if (trimmed.empty())
                continue;
            size_t at = 0;
            while ((at = raw.find('{', at)) != std::string::npos)
            {
                const size_t close = raw.find('}', at);
                if (close == std::string::npos)
                    return refuse("A { has no }.");
                Field field;
                if (!SplitField(raw.substr(at + 1, close - at - 1), field) ||
                    (kWords.count(field.name) == 0 && kValues.count(field.name) == 0))
                    return refuse("There's nothing called {" + raw.substr(at + 1, close - at - 1) + "}.");
                at = close + 1;
            }
            result.script.sections[section].push_back(Trim(raw));
        }

        if (result.script.name.empty())
        {
            number = 0;
            return refuse("The [settings] need a name.");
        }
        result.ok = true;
        return result;
    }

    PostOutput WriteScriptedPost(const PostScript& script, const std::string& jobName, const Frame& workZero,
                                 const std::vector<PostOperation>& operations, const std::string& date,
                                 const ArcFitting& arcs)
    {
        const auto has = [&](const char* name) { return script.sections.count(name) != 0; };
        const bool arcSections = has("arc cw") && has("arc ccw");
        const char* zxSection = has("plane zx") ? "plane zx" : "plane xz";    // [plane xz] before it was renamed
        const bool planeSections = has("plane xy") && has(zxSection) && has("plane yz");
        ArcPlane currentPlane = ArcPlane::XY;       // program start sets G17
        PostOutput output;

        // A 3 axis machine can only cut along the work zero's Z.
        std::string tilted;
        for (const PostOperation& entry : operations)
        {
            if (entry.operation != nullptr && Dot(entry.frame.zAxis, workZero.zAxis) < 1.0 - 1e-6)
                tilted += "\n" + entry.operation->name;
        }
        if (!tilted.empty())
        {
            output.problem = "This post is for a 3 axis machine, so it can't cut operations whose Set Origin "
                             "tilts the tool away from the work zero's Z:" + tilted;
            return output;
        }

        Writer writer(script);
        writer.values["PROGRAM"] = jobName;
        writer.values["DATE"] = date;
        writer.Section("program start");

        std::string currentTool;
        bool firstTool = true;
        for (const PostOperation& entry : operations)
        {
            if (entry.operation == nullptr)
                continue;
            const SequenceStep& operation = *entry.operation;
            writer.values["OPERATION"] = operation.name;
            writer.values["ORIGIN"] = (entry.origin != nullptr) ? entry.origin->name : std::string();

            const std::string toolKey = (entry.tool != nullptr) ? entry.tool->id : std::string();
            if (firstTool || toolKey != currentTool)
            {
                firstTool = false;
                currentTool = toolKey;
                writer.values["TOOL"] = (entry.tool != nullptr) ? std::to_string(entry.tool->number) : "0";
                writer.values["TOOLNAME"] = (entry.tool != nullptr) ? entry.tool->name : "unknown tool";
                char diameter[32];
                std::snprintf(diameter, sizeof(diameter), "%g", (entry.tool != nullptr) ? entry.tool->diameter : 0.0);
                writer.values["TOOLDIA"] = diameter;
                writer.words.clear();
                writer.Section("tool change");
                writer.Forget();        // where the tool is after a change isn't known
            }

            char speed[32];
            std::snprintf(speed, sizeof(speed), "%.0f", operation.feeds.spindleRpm);
            writer.values["SPINDLE"] = speed;
            writer.words.clear();
            writer.Section("operation start");

            // Every point from the work zero.
            auto place = [&](const Vec3& local) { return WorldToFrame(workZero, FramePointToWorld(entry.frame, local)); };
            Vec3 at;
            Vec3 atLocal;       // the same, in the operation's frame
            bool haveAt = false;
            const Vec3 zero = place({ 0.0, 0.0, 0.0 });
            for (const ToolpathMove& move : operation.toolpath.moves)
            {
                writer.words.clear();
                if (move.kind == MoveKind::Dwell)
                {
                    char seconds[32];
                    std::snprintf(seconds, sizeof(seconds), "%g", move.seconds);
                    writer.values["SECONDS"] = seconds;
                    writer.Section("dwell");
                    continue;
                }

                const Vec3 to = place(move.to);
                writer.words["X"] = to.x;
                writer.words["Y"] = to.y;
                writer.words["Z"] = to.z;
                if (move.kind == MoveKind::Rapid)
                {
                    writer.Section("rapid", true);
                }
                else if (move.arc && haveAt)
                {
                    // The arc's plane, seen from the work zero: its normal along Z, Y or X.
                    const Vec3 local = (move.plane == ArcPlane::XY)   ? Vec3{ 0.0, 0.0, 1.0 }
                                       : (move.plane == ArcPlane::ZX) ? Vec3{ 0.0, 1.0, 0.0 }
                                                                      : Vec3{ 1.0, 0.0, 0.0 };
                    const Vec3 normal = place(local) - zero;
                    ArcPlane plane = ArcPlane::XY;
                    double along = 0.0;
                    bool lined = false;
                    if (std::fabs(normal.z) > 1.0 - 1e-9)
                    {
                        plane = ArcPlane::XY;
                        along = normal.z;
                        lined = true;
                    }
                    else if (std::fabs(normal.y) > 1.0 - 1e-9)
                    {
                        plane = ArcPlane::ZX;
                        along = normal.y;
                        lined = true;
                    }
                    else if (std::fabs(normal.x) > 1.0 - 1e-9)
                    {
                        plane = ArcPlane::YZ;
                        along = normal.x;
                        lined = true;
                    }
                    const bool asArc = lined && arcSections && arcs.Allows(plane) &&
                                       (plane == ArcPlane::XY || planeSections);
                    if (asArc)
                    {
                        if (plane != currentPlane && planeSections)
                        {
                            writer.words.clear();
                            writer.Section(plane == ArcPlane::XY ? "plane xy" : plane == ArcPlane::ZX ? zxSection
                                                                                                       : "plane yz");
                            currentPlane = plane;
                        }
                        writer.words.clear();
                        writer.words["X"] = to.x;
                        writer.words["Y"] = to.y;
                        writer.words["Z"] = to.z;
                        const Vec3 centre = place(move.centre);
                        if (plane != ArcPlane::YZ)
                            writer.words["I"] = centre.x - at.x;
                        if (plane != ArcPlane::ZX)
                            writer.words["J"] = centre.y - at.y;
                        if (plane != ArcPlane::XY)
                            writer.words["K"] = centre.z - at.z;
                        writer.words["F"] = move.feed;
                        // Seen from the other side, it turns the other way.
                        const bool clockwise = (along > 0.0) ? move.clockwise : !move.clockwise;
                        writer.Section(clockwise ? "arc cw" : "arc ccw");
                    }
                    else
                    {
                        // Straight moves within the tolerance.
                        double fu, fv, fw, cu, cv, cw;
                        ToPlane(move.plane, atLocal, fu, fv, fw);
                        ToPlane(move.plane, move.centre, cu, cv, cw);
                        const double radius = std::hypot(fu - cu, fv - cv);
                        const double tolerance = (std::max)(1e-4, arcs.tolerance);
                        const double step = (radius > tolerance) ? 2.0 * std::acos(1.0 - tolerance / radius)
                                                                 : 3.14159265358979323846 / 4.0;
                        for (const Vec3& point : ArcMovePoints(atLocal, move, step))
                        {
                            const Vec3 p = place(point);
                            writer.words.clear();
                            writer.words["X"] = p.x;
                            writer.words["Y"] = p.y;
                            writer.words["Z"] = p.z;
                            writer.words["F"] = move.feed;
                            writer.Section("feed", true);
                        }
                    }
                }
                else
                {
                    writer.words["F"] = move.feed;
                    writer.Section("feed", true);
                }
                at = to;
                atLocal = move.to;
                haveAt = true;
            }

            writer.words.clear();
            writer.Section("operation end");
        }

        writer.words.clear();
        writer.Section("program end");
        output.ok = true;
        output.text = writer.text;
        return output;
    }
}
