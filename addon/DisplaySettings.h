// DisplaySettings.h : how CAM graphics look in the view.
//
// These become program settings later.

#pragma once

struct DisplayColour
{
    long red;
    long green;
    long blue;
    long alpha;     // 255 is opaque
};

// The active origin's XY plane, drawn as an outline with grid lines inside.
const DisplayColour kOriginPlaneColour = { 0, 120, 255, 255 };
const DisplayColour kOriginGridColour = { 0, 120, 255, 96 };    // alpha may be ignored
const float kOriginOutlineLineWeight = 1.5f;
const float kOriginGridLineWeight = 1.0f;

// About this many grid squares along the plane's longer side.
const int kOriginGridDivisions = 10;

// The plane reaches past the parts by this fraction of their size.
const double kOriginPlaneMargin = 0.1;

const DisplayColour kOriginXColour = { 230, 40, 40, 255 };
const DisplayColour kOriginYColour = { 40, 200, 40, 255 };
const DisplayColour kOriginZColour = { 40, 90, 255, 255 };

const double kOriginAxisLengthMm = 25.0;
const float kOriginLineWeight = 3.0f;
