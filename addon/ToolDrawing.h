// ToolDrawing.h : the Tool dialog's drawing of what each measurement means.

#pragma once

#include "ToolLibrary.h"

// Draws a schematic of the tool type, tip down and not to scale, filling rect.
// Each measurement carries the letter on its field's label in the Tool dialog:
// a diameter, b flute length, c overall length, d neck length, e shank diameter,
// f corner radius, g tip diameter, h included angle, i taper angle, j point angle,
// k pitch, l thread angle.
void DrawToolDiagram(HDC hdc, const RECT& rect, camcore::ToolType type, bool reducedNeck);
