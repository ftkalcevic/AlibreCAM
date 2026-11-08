// ToolDrawing.cpp : the Tool dialog's drawing of what each measurement means. Plain
// GDI, in design units with Y up from the tool's tip and X out from its axis.

#include "stdafx.h"
#include "ToolDrawing.h"

#include <algorithm>
#include <cmath>
#include <vector>

using camcore::ToolType;

namespace
{
    const double kPi = 3.14159265358979323846;

    // The drawing's proportions, which aren't the tool's own.
    const double kDesignWidth = 200.0;
    const double kAxisX = 90.0;             // the tool's axis, from the drawing's left
    const double kRadius = 36.0;            // the cutting diameter's half
    const double kShankRadius = 56.0;       // a reduced neck's wide shank
    const double kFlute = 130.0;
    const double kNeck = 250.0;
    const double kLength = 380.0;
    const double kDesignTop = kLength + 44.0;
    const double kDesignBottom = -96.0;
    const double kDiameterY = -80.0;        // where the diameter's dimension runs

    const int kSupersample = 3;             // drawn larger then shrunk, to smooth the lines

    const COLORREF kPaper = RGB(255, 255, 255);
    const COLORREF kToolFill = RGB(226, 230, 236);
    const COLORREF kToolLine = RGB(40, 40, 40);
    const COLORREF kFluteLine = RGB(150, 156, 166);
    const COLORREF kDimension = RGB(0, 84, 166);

    struct P
    {
        double x, y;
    };

    P Polar(const P& centre, double radius, double degrees)
    {
        const double a = degrees * kPi / 180.0;
        return { centre.x + radius * std::cos(a), centre.y + radius * std::sin(a) };
    }

    class Canvas
    {
    public:
        Canvas(HDC hdc, int width, int height) : m_hdc(hdc)
        {
            m_scale = (std::min)(width / kDesignWidth, height / (kDesignTop - kDesignBottom));
            m_left = (width - kDesignWidth * m_scale) / 2.0 + kAxisX * m_scale;
            m_top = (height - (kDesignTop - kDesignBottom) * m_scale) / 2.0 + kDesignTop * m_scale;
            m_thin = kSupersample;
            m_thick = m_thin * 2;
            m_font = CreateFontW(-static_cast<int>(16.0 * m_scale), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Arial");
        }

        ~Canvas()
        {
            DeleteObject(m_font);
        }

        POINT At(const P& p) const
        {
            return { static_cast<LONG>(std::lround(m_left + p.x * m_scale)),
                     static_cast<LONG>(std::lround(m_top - p.y * m_scale)) };
        }

        void Polyline(const std::vector<P>& points, COLORREF colour, bool thick = false)
        {
            std::vector<POINT> device;
            for (const P& p : points)
                device.push_back(At(p));
            HPEN pen = CreatePen(PS_SOLID, thick ? m_thick : m_thin, colour);
            HGDIOBJ old = SelectObject(m_hdc, pen);
            ::Polyline(m_hdc, device.data(), static_cast<int>(device.size()));
            SelectObject(m_hdc, old);
            DeleteObject(pen);
        }

        void Line(const P& a, const P& b, COLORREF colour = kDimension)
        {
            Polyline({ a, b }, colour);
        }

        // A filled arrowhead with its point at tip, coming from the direction of `from`.
        void Arrow(const P& tip, const P& from)
        {
            const double dx = from.x - tip.x, dy = from.y - tip.y;
            const double length = std::hypot(dx, dy);
            if (length <= 0.0)
                return;
            const double ux = dx / length, uy = dy / length;
            const double size = 9.0, half = 3.2;
            const POINT points[3] = {
                At(tip),
                At({ tip.x + ux * size - uy * half, tip.y + uy * size + ux * half }),
                At({ tip.x + ux * size + uy * half, tip.y + uy * size - ux * half }),
            };
            HBRUSH brush = CreateSolidBrush(kDimension);
            HPEN pen = CreatePen(PS_SOLID, 1, kDimension);
            HGDIOBJ oldBrush = SelectObject(m_hdc, brush);
            HGDIOBJ oldPen = SelectObject(m_hdc, pen);
            ::Polygon(m_hdc, points, 3);
            SelectObject(m_hdc, oldBrush);
            SelectObject(m_hdc, oldPen);
            DeleteObject(brush);
            DeleteObject(pen);
        }

        // The letter centred at p, on a patch of paper so lines don't run through it.
        void Letter(const P& p, wchar_t letter)
        {
            HGDIOBJ oldFont = SelectObject(m_hdc, m_font);
            SIZE size = {};
            GetTextExtentPoint32W(m_hdc, &letter, 1, &size);
            const POINT at = At(p);
            const int pad = kSupersample * 2;
            const RECT patch = { at.x - size.cx / 2 - pad, at.y - size.cy / 2, at.x + size.cx / 2 + pad,
                                 at.y + size.cy / 2 };
            HBRUSH paper = CreateSolidBrush(kPaper);
            FillRect(m_hdc, &patch, paper);
            DeleteObject(paper);
            SetBkMode(m_hdc, TRANSPARENT);
            SetTextColor(m_hdc, kDimension);
            TextOutW(m_hdc, at.x - size.cx / 2, at.y - size.cy / 2, &letter, 1);
            SelectObject(m_hdc, oldFont);
        }

        // A dimension from a to b, arrows at both ends, with the letter half way along.
        void Dimension(const P& a, const P& b, wchar_t letter)
        {
            Line(a, b);
            Arrow(a, b);
            Arrow(b, a);
            Letter({ (a.x + b.x) / 2.0, (a.y + b.y) / 2.0 }, letter);
        }

        // An arc about centre, from one angle to another in degrees, with its letter
        // at the middle and, if there's room, arrows at its ends.
        void Angle(const P& centre, double radius, double from, double to, wchar_t letter, bool arrows = true)
        {
            std::vector<P> arc;
            for (int i = 0; i <= 32; ++i)
                arc.push_back(Polar(centre, radius, from + (to - from) * i / 32));
            Polyline(arc, kDimension);
            if (arrows)
            {
                Arrow(arc.front(), arc[4]);
                Arrow(arc.back(), arc[arc.size() - 5]);
            }
            if (letter != 0)
                Letter(Polar(centre, radius, (from + to) / 2.0), letter);
        }

        // The tool: filled, with flute lines below kFlute, then its outline.
        void Tool(const std::vector<P>& right, bool slantedFlutes)
        {
            std::vector<P> outline = right;
            for (auto it = right.rbegin(); it != right.rend(); ++it)
                outline.push_back({ -it->x, it->y });

            std::vector<POINT> device;
            for (const P& p : outline)
                device.push_back(At(p));

            HBRUSH brush = CreateSolidBrush(kToolFill);
            HPEN none = CreatePen(PS_NULL, 0, 0);
            HGDIOBJ oldBrush = SelectObject(m_hdc, brush);
            HGDIOBJ oldPen = SelectObject(m_hdc, none);
            ::Polygon(m_hdc, device.data(), static_cast<int>(device.size()));
            SelectObject(m_hdc, oldBrush);
            SelectObject(m_hdc, oldPen);
            DeleteObject(brush);
            DeleteObject(none);

            if (slantedFlutes)
            {
                HRGN shape = CreatePolygonRgn(device.data(), static_cast<int>(device.size()), WINDING);
                const POINT a = At({ -kShankRadius * 2.0, kFlute });
                const POINT b = At({ kShankRadius * 2.0, kDesignBottom });
                HRGN below = CreateRectRgn(a.x, a.y, b.x, b.y);
                CombineRgn(shape, shape, below, RGN_AND);
                SelectClipRgn(m_hdc, shape);
                for (double y = -80.0; y < kFlute + 20.0; y += 26.0)
                    Polyline({ { -kRadius - 4.0, y }, { kRadius + 4.0, y + 36.0 } }, kFluteLine);
                SelectClipRgn(m_hdc, nullptr);
                DeleteObject(shape);
                DeleteObject(below);
            }

            outline.push_back(outline.front());
            Polyline(outline, kToolLine, true);
        }

    private:
        HDC m_hdc;
        HFONT m_font = nullptr;
        double m_scale = 1.0, m_left = 0.0, m_top = 0.0;
        int m_thin = 1, m_thick = 2;
    };

    // The included angle across a point: its flanks carried on past the (perhaps
    // cut off) apex, and the angle between them underneath.
    void DrawPointAngle(Canvas& canvas, double tip, double halfAngle, wchar_t letter)
    {
        const double a = halfAngle * kPi / 180.0;
        const P apex = { 0.0, -tip / std::tan(a) };
        const double radius = 32.0;
        for (double side : { -1.0, 1.0 })
        {
            const P end = { side * (radius + 8.0) * std::sin(a), apex.y - (radius + 8.0) * std::cos(a) };
            canvas.Line({ side * tip, 0.0 }, end);
        }
        canvas.Angle(apex, radius, -90.0 - halfAngle, -90.0 + halfAngle, letter);
    }

    // The tip's width, by a leader from the left.
    void DrawTipLeader(Canvas& canvas, double tip)
    {
        const P label = { -kRadius - 20.0, 0.0 };
        canvas.Line({ -tip, 0.0 }, label);
        canvas.Arrow({ -tip, 0.0 }, label);
        canvas.Letter(label, L'g');
    }
}

void DrawToolDiagram(HDC hdc, const RECT& rect, ToolType type, bool reducedNeck)
{
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
        return;

    HDC memory = CreateCompatibleDC(hdc);
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, width * kSupersample, height * kSupersample);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    const RECT whole = { 0, 0, width * kSupersample, height * kSupersample };
    HBRUSH paper = CreateSolidBrush(kPaper);
    FillRect(memory, &whole, paper);
    DeleteObject(paper);

    {
        Canvas canvas(memory, whole.right, whole.bottom);

        // The right side of the tool, tip to top.
        std::vector<P> right;
        double tip = 0.0;               // half the width at y = 0
        double widestY = 0.0;           // where the cutting diameter is first reached
        const double cornerRadius = 12.0;
        const double taperTip = 10.0;
        const double threadPitch = kFlute / 5.0, threadRoot = 18.0;
        const double vHalf = 30.0, chamferHalf = 45.0, drillHalf = 59.0;
        const double vTip = 5.0, chamferTip = 14.0;

        const auto point = [&](double t, double h) {
            tip = t;
            const double rise = (kRadius - t) / std::tan(h * kPi / 180.0);
            right.push_back({ t, 0.0 });
            right.push_back({ kRadius, rise });
            widestY = rise;
        };
        switch (type)
        {
        case ToolType::BallMill:
            for (int i = 0; i <= 24; ++i)
            {
                const double a = i * (kPi / 2.0) / 24;
                right.push_back({ kRadius * std::sin(a), kRadius - kRadius * std::cos(a) });
            }
            widestY = kRadius;
            break;
        case ToolType::CornerRadiusMill:
            tip = kRadius - cornerRadius;
            for (int i = 0; i <= 16; ++i)
            {
                const double a = i * (kPi / 2.0) / 16;
                right.push_back({ tip + cornerRadius * std::sin(a), cornerRadius - cornerRadius * std::cos(a) });
            }
            widestY = cornerRadius;
            break;
        case ToolType::VMill:
            point(vTip, vHalf);
            break;
        case ToolType::ChamferMill:
            point(chamferTip, chamferHalf);
            break;
        case ToolType::Drill:
            point(0.0, drillHalf);
            break;
        case ToolType::TaperMill:
            tip = taperTip;
            right.push_back({ taperTip, 0.0 });
            right.push_back({ kRadius, kFlute });
            widestY = kFlute;
            break;
        case ToolType::ThreadMill:
            tip = threadRoot;
            for (int i = 0; i < 5; ++i)
            {
                right.push_back({ threadRoot, i * threadPitch });
                right.push_back({ kRadius, (i + 0.5) * threadPitch });
            }
            right.push_back({ threadRoot, kFlute });
            right.push_back({ kRadius, kFlute });
            widestY = threadPitch / 2.0;
            break;
        default:
            tip = kRadius;
            right.push_back({ kRadius, 0.0 });
            break;
        }
        const double topRadius = reducedNeck ? kShankRadius : kRadius;
        if (reducedNeck)
        {
            right.push_back({ kRadius, kNeck });
            right.push_back({ kShankRadius, kNeck });
        }
        right.push_back({ topRadius, kLength });

        canvas.Tool(right, type != ToolType::ThreadMill);

        // The diameter, below the tool.
        for (double side : { -1.0, 1.0 })
            canvas.Line({ side * kRadius, widestY }, { side * kRadius, kDiameterY - 5.0 });
        canvas.Dimension({ -kRadius, kDiameterY }, { kRadius, kDiameterY }, L'a');

        // The lengths, all from the tip, up the right side.
        const double fluteX = 70.0;
        const double neckX = 86.0;
        const double overallX = reducedNeck ? 102.0 : 86.0;
        canvas.Line({ tip, 0.0 }, { overallX + 5.0, 0.0 });
        canvas.Line({ kRadius, kFlute }, { fluteX + 5.0, kFlute });
        canvas.Dimension({ fluteX, 0.0 }, { fluteX, kFlute }, L'b');
        canvas.Line({ topRadius, kLength }, { overallX + 5.0, kLength });
        canvas.Dimension({ overallX, 0.0 }, { overallX, kLength }, L'c');
        if (reducedNeck)
        {
            canvas.Line({ kShankRadius, kNeck }, { neckX + 5.0, kNeck });
            canvas.Dimension({ neckX, 0.0 }, { neckX, kNeck }, L'd');

            const double y = kLength + 24.0;
            for (double side : { -1.0, 1.0 })
                canvas.Line({ side * kShankRadius, kLength }, { side * kShankRadius, y + 5.0 });
            canvas.Dimension({ -kShankRadius, y }, { kShankRadius, y }, L'e');
        }

        // Each type's own.
        switch (type)
        {
        case ToolType::CornerRadiusMill:
        {
            const double a = kPi / 4.0;
            const P on = { -(tip + cornerRadius * std::sin(a)), cornerRadius - cornerRadius * std::cos(a) };
            const P label = { -kRadius - 24.0, -24.0 };
            canvas.Line(on, label);
            canvas.Arrow(on, label);
            canvas.Letter(label, L'f');
            break;
        }
        case ToolType::VMill:
            DrawTipLeader(canvas, vTip);
            DrawPointAngle(canvas, vTip, vHalf, L'h');
            break;
        case ToolType::ChamferMill:
            DrawTipLeader(canvas, chamferTip);
            DrawPointAngle(canvas, chamferTip, chamferHalf, L'h');
            break;
        case ToolType::Drill:
            DrawPointAngle(canvas, 0.0, drillHalf, L'j');
            break;
        case ToolType::TaperMill:
        {
            DrawTipLeader(canvas, taperTip);
            // The flank carried on past the tip, against the tool's axis direction.
            const double taper = std::atan2(kRadius - taperTip, kFlute) * 180.0 / kPi;
            const P corner = { -taperTip, 0.0 };
            const double radius = 60.0;
            canvas.Line(corner, Polar(corner, radius + 8.0, -90.0));
            canvas.Line(corner, Polar(corner, radius + 8.0, -90.0 + taper));
            canvas.Angle(corner, radius, -90.0, -90.0 + taper, 0, false);
            canvas.Letter({ corner.x - 13.0, -radius }, L'i');
            break;
        }
        case ToolType::ThreadMill:
        {
            const double x = -kRadius - 14.0;
            const double y0 = 1.5 * threadPitch, y1 = 2.5 * threadPitch;
            canvas.Line({ -kRadius, y0 }, { x - 5.0, y0 });
            canvas.Line({ -kRadius, y1 }, { x - 5.0, y1 });
            canvas.Line({ x, y0 }, { x, y1 });
            canvas.Arrow({ x, y0 }, { x, y1 });
            canvas.Arrow({ x, y1 }, { x, y0 });
            canvas.Letter({ x - 13.0, (y0 + y1) / 2.0 }, L'k');

            const P crest = { kRadius, 3.5 * threadPitch };
            const double half = std::atan2(threadPitch / 2.0, kRadius - threadRoot) * 180.0 / kPi;
            canvas.Line(crest, Polar(crest, 26.0, half));
            canvas.Line(crest, Polar(crest, 26.0, -half));
            canvas.Angle(crest, 20.0, -half, half, L'l', false);
            break;
        }
        default:
            break;
        }
    }

    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);
    StretchBlt(hdc, rect.left, rect.top, width, height, memory, 0, 0, whole.right, whole.bottom, SRCCOPY);

    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
}
