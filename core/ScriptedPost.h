// ScriptedPost.h : turns toolpaths into G-code by following a plain text post script.
// No Alibre or MFC code belongs here.
//
// A post script (a .post file) is a template for one kind of machine:
//
//   ; A line starting with a semicolon is a note, and isn't written.
//   [settings]
//   name = LinuxCNC (3 axis, mm)    ; shown in the Post dialog
//   extension = ngc                  ; the output file's extension
//   axes = 3                         ; 3: every operation must cut along the work zero's Z
//   decimals = 4                     ; for X, Y, Z, I, J and K
//   feed decimals = 0                ; for F
//   trim zeros = yes                 ; 12.5 rather than 12.5000
//   line numbers = no                ; yes puts N words on the lines
//   line number start = 10
//   line number step = 10
//
//   [program start]     written once, first
//   [tool change]       before the first operation, and whenever the tool changes
//   [operation start]   before each operation's moves
//   [rapid] [feed] [arc cw] [arc ccw] [dwell]    once for each move
//   [plane xy] [plane zx] [plane yz]   before an arc in a different plane from the
//                       last (G17, G18, G19). Program start is taken as XY. Without
//                       them, arcs in ZX and YZ are written as straight moves; without
//                       [arc cw] and [arc ccw], every arc is.
//   [operation end]     after each operation's moves
//   [program end]       written once, last
//
// Any section can be left out or empty. Its lines are written as they are,
// filling in {...}. The name is the capitals at the end of the braces, and
// anything before it is written in front of the value, so "G1{ X}{ Y}{ F}"
// writes "G1 X10 F500" when only X and F change.
//   X Y Z     the move's end, from the work zero (the first Set Origin), in mm.
//             Written only when changed since last written, as are F and the
//             first move after a tool change.
//   I J K     an arc's centre from where it starts: I and J in XY, I and K in ZX,
//             J and K in YZ. Always written, the third left out.
//   F         the feed, in mm/min. Written only when it changes.
//   TOOL TOOLNAME TOOLDIA   the tool's number, name and diameter.
//   SPINDLE   the operation's spindle speed, in rpm.
//   SECONDS   a dwell's time.
//   PROGRAM OPERATION ORIGIN   the job's, the operation's and its Set Origin's names.
//   DATE      today's date, as yyyy-mm-dd.
// Names are written with ( and ) turned to [ and ], so they're safe in G-code comments.
// A [rapid] or [feed] line that would move no axis isn't written.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "ArcFit.h"         // ArcFitting
#include "PseudoPost.h"     // PostOperation

namespace camcore
{
    // A post script, read and checked.
    struct PostScript
    {
        std::string name;
        std::string extension = "ngc";
        int axes = 3;
        int decimals = 4;
        int feedDecimals = 0;
        bool trimZeros = true;
        bool lineNumbers = false;
        int lineNumberStart = 10;
        int lineNumberStep = 10;
        std::map<std::string, std::vector<std::string>> sections;      // by name, such as "rapid"
    };

    struct PostScriptRead
    {
        bool ok = false;
        std::string problem;        // with its line number, when not ok
        PostScript script;
    };

    // Reads a post script. Unknown sections, settings or {names} are problems,
    // so mistakes show up when it's read rather than in the G-code.
    PostScriptRead ReadPostScript(const std::string& text);

    struct PostOutput
    {
        bool ok = false;
        std::string text;
        std::string problem;
    };

    // The whole program for these operations, from the work zero. A 3 axis
    // post refuses operations whose origin's Z isn't the work zero's, naming them.
    // date is what {DATE} writes. Arcs go out as arcs in the work zero's XY, ZX or
    // YZ plane where the machine allows that plane; otherwise (or where an arc's
    // plane isn't one of those, as when its origin is turned about Z) as straight
    // moves within the machine's arc tolerance.
    PostOutput WriteScriptedPost(const PostScript& script, const std::string& jobName, const Frame& workZero,
                                 const std::vector<PostOperation>& operations, const std::string& date,
                                 const ArcFitting& arcs = ArcFitting());
}
