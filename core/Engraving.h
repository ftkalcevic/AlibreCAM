// Engraving.h : the Engrave operation's own settings. No Alibre or MFC code belongs here.

#pragma once

namespace camcore
{
    // How the tool gets from one cut level to the next along a line.
    enum class EngraveTraversal
    {
        ReturnToStart,  // lift at the line's end, go back to its start, and run the same way again
        StayDown        // stay down, go down to the next level, and run back (a closed loop carries on round)
    };

    // The Engraving tab.
    struct EngravingSettings
    {
        EngraveTraversal traversal = EngraveTraversal::ReturnToStart;
    };
}
