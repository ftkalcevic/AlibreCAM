// CamViewCommand.cpp : the regular command that draws CAM graphics and receives picks.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "CamViewCommand.h"
#include "CamTabCommand.h"
#include "DisplaySettings.h"
#include "Log.h"

#include <cmath>
#include <vector>

using camcore::Vec3;

namespace
{
    // The sample add-ons pass 1-based arrays, so these do too.
    template <class T>
    SAFEARRAY* MakeArray(VARTYPE type, const std::vector<T>& values)
    {
        SAFEARRAY* pArray = SafeArrayCreateVector(type, 1, static_cast<ULONG>(values.size()));
        if (pArray == nullptr)
            return nullptr;

        void* pData = nullptr;
        if (FAILED(SafeArrayAccessData(pArray, &pData)))
        {
            SafeArrayDestroy(pArray);
            return nullptr;
        }
        memcpy(pData, values.data(), values.size() * sizeof(T));
        SafeArrayUnaccessData(pArray);
        return pArray;
    }

    // Canvas coordinates are in the assembly's model units.
    void AddPoint(std::vector<float>& values, const Vec3& mm, double millimetresPerUnit)
    {
        values.push_back(static_cast<float>(mm.x / millimetresPerUnit));
        values.push_back(static_cast<float>(mm.y / millimetresPerUnit));
        values.push_back(static_cast<float>(mm.z / millimetresPerUnit));
    }

    __int64 AddSegment(IADAddOnCanvasDisplay* pCanvas, const wchar_t* name, const DisplayColour& colour,
                       float lineWeight, bool foreground)
    {
        __int64 segment = 0;
        _bstr_t segmentName(name);
        pCanvas->AddSubSegment(0, segmentName, &segment);
        pCanvas->SetSegmentColor(segment, colour.red, colour.green, colour.blue, colour.alpha);
        pCanvas->SetLineWeight(segment, lineWeight);
        pCanvas->ToggleForegroundRendering(segment, foreground ? VARIANT_TRUE : VARIANT_FALSE);
        return segment;
    }

    void DrawPolyline(IADAddOnCanvasDisplay* pCanvas, __int64 segment, const std::vector<float>& points)
    {
        SAFEARRAY* pArray = MakeArray(VT_R4, points);
        if (pArray == nullptr)
            return;

        __int64 result = 0;
        const HRESULT hr = pCanvas->DrawPolyline(segment, &pArray, &result);
        if (FAILED(hr))
            Log(L"[view] DrawPolyline failed hr=0x%08X", static_cast<unsigned>(hr));
        SafeArrayDestroy(pArray);
    }

    // A shaded mesh: three floats to a vertex and a normal, and each triangle as 3
    // followed by its vertex numbers, as the canvas takes it.
    void DrawMesh(IADAddOnCanvasDisplay* pCanvas, __int64 segment, const camcore::StockMesh& mesh,
                  double millimetresPerUnit)
    {
        if (mesh.faces.empty())
            return;
        std::vector<float> vertices(mesh.vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i)
            vertices[i] = static_cast<float>(mesh.vertices[i] / millimetresPerUnit);
        SAFEARRAY* pVertices = MakeArray(VT_R4, vertices);
        SAFEARRAY* pNormals = MakeArray(VT_R4, mesh.normals);
        SAFEARRAY* pFaces = MakeArray(VT_I4, mesh.faces);
        if (pVertices != nullptr && pNormals != nullptr && pFaces != nullptr)
        {
            __int64 result = 0;
            const HRESULT hr = pCanvas->DrawMesh(segment, &pVertices, &pNormals, &pFaces, &result);
            if (FAILED(hr))
                Log(L"[view] DrawMesh failed hr=0x%08X", static_cast<unsigned>(hr));
        }
        for (SAFEARRAY* pArray : { pVertices, pNormals, pFaces })
        {
            if (pArray != nullptr)
                SafeArrayDestroy(pArray);
        }
    }

    // The tool as lines: its outline up four sides, and rings round it.
    void DrawTool(IADAddOnCanvasDisplay* pCanvas, const CamScene& scene, double unit)
    {
        Vec3 w;
        if (!camcore::Normalise(scene.simAxis, w))
            return;
        const Vec3 other = (std::fabs(w.x) < 0.9) ? Vec3{ 1.0, 0.0, 0.0 } : Vec3{ 0.0, 1.0, 0.0 };
        Vec3 u;
        camcore::Normalise(camcore::Cross(w, other), u);
        const Vec3 v = camcore::Cross(w, u);

        const camcore::ToolProfile& profile = scene.simProfile;
        const double top = (std::min)(profile.length, 4.0 * profile.radius + 10.0);
        const int samples = 12;
        const __int64 segment = AddSegment(pCanvas, L"Simulated tool", kSimToolColour, kSimToolLineWeight, true);
        for (int side = 0; side < 4; ++side)
        {
            const double angle = side * 3.14159265358979323846 / 2.0;
            const Vec3 out = u * std::cos(angle) + v * std::sin(angle);
            std::vector<float> points;
            for (int i = 0; i <= samples; ++i)
            {
                const double h = top * i / samples;
                const double r = (std::max)(0.0, profile.RadiusAt((std::min)(h, profile.length)));
                AddPoint(points, scene.simTip + w * h + out * r, unit);
            }
            DrawPolyline(pCanvas, segment, points);
        }
        for (double h : { 0.0, profile.radius, top })
        {
            const double r = profile.RadiusAt((std::min)(h, profile.length));
            if (!(r > 0.01))
                continue;
            std::vector<float> ring;
            for (int i = 0; i <= 24; ++i)
            {
                const double angle = i * 2.0 * 3.14159265358979323846 / 24;
                AddPoint(ring, scene.simTip + w * h + (u * std::cos(angle) + v * std::sin(angle)) * r, unit);
            }
            DrawPolyline(pCanvas, segment, ring);
        }
    }

    void DrawLine(IADAddOnCanvasDisplay* pCanvas, __int64 segment, const Vec3& from, const Vec3& to,
                  double millimetresPerUnit)
    {
        std::vector<float> points;
        AddPoint(points, from, millimetresPerUnit);
        AddPoint(points, to, millimetresPerUnit);
        DrawPolyline(pCanvas, segment, points);
    }

    // The plane is an outline and grid lines rather than a shaded mesh. Alibre
    // picks add-on graphics before the model behind them, and a mesh would cover
    // everything behind the plane.
    void DrawPlane(IADAddOnCanvasDisplay* pCanvas, const camcore::Frame& frame, const camcore::PlaneRectangle& plane,
                   bool foreground, double millimetresPerUnit)
    {
        const __int64 grid = AddSegment(pCanvas, L"Origin plane grid", kOriginGridColour, kOriginGridLineWeight,
                                        foreground);
        for (const camcore::LineSegment& line : camcore::PlaneGrid(frame, plane, kOriginGridDivisions))
            DrawLine(pCanvas, grid, line.from, line.to, millimetresPerUnit);

        const __int64 outline = AddSegment(pCanvas, L"Origin plane outline", kOriginPlaneColour,
                                           kOriginOutlineLineWeight, foreground);
        std::vector<float> points;
        for (int i = 0; i <= 4; ++i)
            AddPoint(points, plane.corners[i % 4], millimetresPerUnit);
        DrawPolyline(pCanvas, outline, points);
    }
}

CCamViewCommand::CCamViewCommand(CCamTabCommand* pTab)
    : CCommandBase(L"view")
    , m_pTab(pTab)
    , m_reportedRender(false)
{
    m_pTab->AddRef();
}

CCamViewCommand::~CCamViewCommand()
{
    if (m_pTab != nullptr)
        m_pTab->Release();
}

void CCamViewCommand::Refresh()
{
    if (m_site != nullptr)
        m_site->InvalidateCanvas();
}

STDMETHODIMP CCamViewCommand::OnComplete()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());
    Log(L"[view] OnComplete");
    Refresh();
    return S_OK;
}

STDMETHODIMP CCamViewCommand::OnSelectionChange()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());
    if (m_pTab != nullptr)
        m_pTab->HandleSelection();
    return S_OK;
}

STDMETHODIMP CCamViewCommand::OnTerminate()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());
    Log(L"[view] OnTerminate: CAM graphics are no longer drawn");

    if (m_pTab != nullptr)
    {
        CCamTabCommand* pTab = m_pTab;
        m_pTab = nullptr;
        pTab->OnViewTerminated(this);
        pTab->Release();
    }
    m_site = nullptr;
    return S_OK;
}

STDMETHODIMP CCamViewCommand::On3DRender()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (m_site == nullptr || m_pTab == nullptr)
        return S_OK;

    // Alibre loses the graphics on pan, zoom and hover unless they're rebuilt
    // on every render.
    IUnknown* pContext = nullptr;
    const HRESULT hr = m_site->Begin3DDisplay(VARIANT_TRUE, &pContext);
    if (!m_reportedRender)
    {
        Log(L"[view] first On3DRender: Begin3DDisplay hr=0x%08X", static_cast<unsigned>(hr));
        m_reportedRender = true;
    }
    if (FAILED(hr))
        return S_OK;

    if (pContext != nullptr)
    {
        IADAddOnCanvasDisplayPtr canvas;
        pContext->QueryInterface(__uuidof(IADAddOnCanvasDisplay), reinterpret_cast<void**>(&canvas));
        pContext->Release();

        if (canvas != nullptr)
        {
            try
            {
                DrawScene(canvas, m_pTab->GetScene());
            }
            catch (const _com_error& error)
            {
                Log(L"[view] drawing failed: hr=0x%08X", static_cast<unsigned>(error.Error()));
            }
        }
    }

    m_site->End3DDisplay();
    return S_OK;
}

void CCamViewCommand::DrawScene(IADAddOnCanvasDisplay* pCanvas, const CamScene& scene)
{
    const double unit = scene.millimetresPerUnit;

    // The simulated stock in place of the stock's edges, the tool, and a rapid that cut.
    if (scene.simulating)
    {
        if (scene.simMesh != nullptr && scene.stockShown)
        {
            const __int64 segment = AddSegment(pCanvas, L"Simulated stock", kSimStockColour, 1.0f, scene.foreground);
            DrawMesh(pCanvas, segment, *scene.simMesh, unit);
        }
        if (scene.simToolShown)
            DrawTool(pCanvas, scene, unit);
        if (scene.simWarning)
        {
            const __int64 segment =
                AddSegment(pCanvas, L"Rapid into stock", kSimWarningColour, kSimWarningLineWeight, true);
            DrawLine(pCanvas, segment, scene.simWarningFrom, scene.simWarningTo, unit);
        }
    }

    // The stock's edges. Hidden while picking, as graphics take part in it.
    if (!scene.simulating && scene.showStock && scene.stockShown && !scene.stockEdges.empty())
    {
        const __int64 segment = AddSegment(pCanvas, L"Stock", kStockColour, kStockLineWeight, scene.foreground);
        for (const camcore::LineSegment& edge : scene.stockEdges)
        {
            std::vector<float> points;
            points.reserve(6);
            AddPoint(points, edge.from, unit);
            AddPoint(points, edge.to, unit);
            DrawPolyline(pCanvas, segment, points);
        }
    }

    // Hidden while picking, like the toolpaths, as graphics take part in it.
    if (scene.showToolpaths && !scene.highlights.empty())
    {
        const __int64 segment = AddSegment(pCanvas, L"Operation features", kFeatureHighlightColour,
                                           kFeatureHighlightLineWeight, scene.foreground);
        for (const std::vector<Vec3>& outline : scene.highlights)
        {
            std::vector<float> points;
            points.reserve(outline.size() * 3);
            for (const Vec3& point : outline)
                AddPoint(points, point, unit);
            DrawPolyline(pCanvas, segment, points);
        }
    }

    if (scene.showToolpaths && !scene.boundaries.empty())
    {
        const __int64 segment = AddSegment(pCanvas, L"Facing boundaries", kBoundaryColour, kBoundaryLineWeight,
                                           scene.foreground);
        for (const std::vector<Vec3>& boundary : scene.boundaries)
        {
            std::vector<float> points;
            points.reserve(boundary.size() * 3);
            for (const Vec3& point : boundary)
                AddPoint(points, point, unit);
            DrawPolyline(pCanvas, segment, points);
        }
    }

    if (scene.showToolpaths && scene.toolpathsShown)
    {
        for (const CamScene::Toolpath& toolpath : scene.toolpaths)
        {
            // A suppressed operation's toolpath is a thin grey line, a little bolder when selected.
            const float weight = toolpath.suppressed ? (toolpath.selected ? kToolpathLineWeight
                                                                          : kSuppressedToolpathLineWeight)
                                 : toolpath.selected ? kSelectedToolpathLineWeight
                                                     : kToolpathLineWeight;
            const struct
            {
                const wchar_t* name;
                const DisplayColour& colour;
                const std::vector<std::vector<Vec3>>& runs;
            } kinds[] =
            {
                { L"Toolpath rapids", toolpath.suppressed ? kSuppressedColour : kRapidColour, toolpath.rapids },
                { L"Toolpath feeds", toolpath.suppressed ? kSuppressedColour : kFeedColour, toolpath.feeds },
                { L"Toolpath arcs", toolpath.suppressed ? kSuppressedColour : kArcColour, toolpath.arcs }
            };

            for (const auto& kind : kinds)
            {
                if (kind.runs.empty())
                    continue;
                const __int64 segment = AddSegment(pCanvas, kind.name, kind.colour, weight, scene.foreground);
                for (const std::vector<Vec3>& run : kind.runs)
                {
                    std::vector<float> points;
                    points.reserve(run.size() * 3);
                    for (const Vec3& point : run)
                        AddPoint(points, point, unit);
                    DrawPolyline(pCanvas, segment, points);
                }
            }
        }
    }

    if (!scene.hasOrigin || !scene.originShown)
        return;

    const camcore::Frame& frame = scene.origin;

    if (scene.showPlane)
        DrawPlane(pCanvas, frame, scene.plane, scene.foreground, unit);

    const struct
    {
        const wchar_t* name;
        const DisplayColour& colour;
        const Vec3& axis;
    } axes[] =
    {
        { L"Origin X", kOriginXColour, frame.xAxis },
        { L"Origin Y", kOriginYColour, frame.yAxis },
        { L"Origin Z", kOriginZColour, frame.zAxis }
    };

    for (const auto& axis : axes)
    {
        const __int64 segment = AddSegment(pCanvas, axis.name, axis.colour, kOriginLineWeight, scene.foreground);
        DrawLine(pCanvas, segment, frame.origin, frame.origin + axis.axis * kOriginAxisLengthMm, unit);
    }
}
