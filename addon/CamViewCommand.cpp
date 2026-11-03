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

    void AddDirection(std::vector<float>& values, const Vec3& direction)
    {
        values.push_back(static_cast<float>(direction.x));
        values.push_back(static_cast<float>(direction.y));
        values.push_back(static_cast<float>(direction.z));
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

    // The plane is drawn twice, once facing each way, so it shows from both sides.
    void DrawPlane(IADAddOnCanvasDisplay* pCanvas, const camcore::PlaneRectangle& plane, const Vec3& normal,
                   bool foreground, double millimetresPerUnit)
    {
        std::vector<float> vertices;
        std::vector<float> normals;
        for (int side = 0; side < 2; ++side)
        {
            for (const Vec3& corner : plane.corners)
            {
                AddPoint(vertices, corner, millimetresPerUnit);
                AddDirection(normals, side == 0 ? normal : normal * -1.0);
            }
        }

        // Each face is its vertex count followed by the vertex indices.
        const std::vector<int> indices =
        {
            3, 0, 1, 2,   3, 0, 2, 3,
            3, 4, 6, 5,   3, 4, 7, 6
        };

        const __int64 segment = AddSegment(pCanvas, L"Origin plane", kOriginPlaneColour, 1.0f, foreground);

        SAFEARRAY* pVertices = MakeArray(VT_R4, vertices);
        SAFEARRAY* pNormals = MakeArray(VT_R4, normals);
        SAFEARRAY* pIndices = MakeArray(VT_I4, indices);
        if (pVertices != nullptr && pNormals != nullptr && pIndices != nullptr)
        {
            __int64 result = 0;
            const HRESULT hr = pCanvas->DrawMesh(segment, &pVertices, &pNormals, &pIndices, &result);
            if (FAILED(hr))
                Log(L"[view] DrawMesh failed hr=0x%08X", static_cast<unsigned>(hr));
        }
        if (pVertices != nullptr)
            SafeArrayDestroy(pVertices);
        if (pNormals != nullptr)
            SafeArrayDestroy(pNormals);
        if (pIndices != nullptr)
            SafeArrayDestroy(pIndices);

        // An opaque outline, so the plane's extent shows even if Alibre ignores the alpha.
        DisplayColour outlineColour = kOriginPlaneColour;
        outlineColour.alpha = 255;
        const __int64 outline = AddSegment(pCanvas, L"Origin plane outline", outlineColour, 1.0f, foreground);
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

    if (scene.hasPlane)
    {
        for (const CamScene::Origin& origin : scene.origins)
        {
            if (origin.selected)
                DrawPlane(pCanvas, scene.plane, origin.frame.zAxis, scene.foreground, unit);
        }
    }

    for (const CamScene::Origin& origin : scene.origins)
    {
        const camcore::Frame& frame = origin.frame;
        const float weight = origin.selected ? kSelectedOriginLineWeight : kOtherOriginLineWeight;

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
            const __int64 segment = AddSegment(pCanvas, axis.name, axis.colour, weight, scene.foreground);
            DrawLine(pCanvas, segment, frame.origin, frame.origin + axis.axis * kOriginAxisLengthMm, unit);
        }
    }
}
