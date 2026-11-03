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

// The selected origin's XY plane, shaded and translucent.
const DisplayColour kOriginPlaneColour = { 0, 120, 255, 64 };

// The plane reaches past the parts by this fraction of their size.
const double kOriginPlaneMargin = 0.1;

const DisplayColour kOriginXColour = { 230, 40, 40, 255 };
const DisplayColour kOriginYColour = { 40, 200, 40, 255 };
const DisplayColour kOriginZColour = { 40, 90, 255, 255 };

const double kOriginAxisLengthMm = 25.0;
const float kSelectedOriginLineWeight = 3.0f;
const float kOtherOriginLineWeight = 1.5f;
