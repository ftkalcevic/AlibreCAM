// CamViewCommand.cpp : the regular command that draws CAM graphics and receives picks.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "CamViewCommand.h"
#include "CamTabCommand.h"
#include "DisplaySettings.h"
#include "Log.h"

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

    if (scene.showToolpaths)
    {
        for (const CamScene::Toolpath& toolpath : scene.toolpaths)
        {
            const float weight = toolpath.selected ? kSelectedToolpathLineWeight : kToolpathLineWeight;
            const struct
            {
                const wchar_t* name;
                const DisplayColour& colour;
                const std::vector<std::vector<Vec3>>& runs;
            } kinds[] =
            {
                { L"Toolpath rapids", kRapidColour, toolpath.rapids },
                { L"Toolpath feeds", kFeedColour, toolpath.feeds }
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

    if (!scene.hasOrigin)
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
