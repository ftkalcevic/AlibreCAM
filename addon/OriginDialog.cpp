// OriginDialog.cpp : the modeless dialog that edits one Set Origin step.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "OriginDialog.h"
#include "Help.h"
#include "CamTabCommand.h"
#include "ModelReader.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

using namespace camcore;

namespace
{
    // Where the dialog was last closed, so it reopens there. Empty until then.
    CPoint g_lastPosition(-1, -1);

    std::wstring FormatNumber(double value)
    {
        wchar_t text[64] = {};
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.10g", value);
        return text;
    }

    std::wstring FormatVec3(const Vec3& v, const wchar_t* format)
    {
        wchar_t text[128] = {};
        _snwprintf_s(text, _countof(text), _TRUNCATE, format, v.x, v.y, v.z);
        return text;
    }

    std::wstring Describe(const PickedGeometry& pick)
    {
        const wchar_t* kind = L"";
        switch (pick.kind)
        {
        case PickKind::PlanarFace:      kind = L"Flat face"; break;
        case PickKind::CylindricalFace: kind = L"Cylindrical face"; break;
        case PickKind::LineEdge:        kind = L"Straight edge"; break;
        case PickKind::CircleEdge:      kind = L"Circular edge"; break;
        case PickKind::DesignPlane:     kind = L"Plane"; break;
        case PickKind::DesignAxis:      kind = L"Axis"; break;
        case PickKind::DesignPoint:     kind = L"Point"; break;
        default:                        return L"Not picked";
        }
        if (pick.kind == PickKind::DesignPlane || pick.kind == PickKind::DesignAxis || pick.kind == PickKind::DesignPoint)
        {
            const std::wstring text = std::wstring(kind) + L" " + FromUtf8(pick.targetName);
            return pick.occurrenceName.empty() ? text : text + L" on " + FromUtf8(pick.occurrenceName);
        }
        return std::wstring(kind) + L" on " + FromUtf8(pick.occurrenceName);
    }

    bool ReadNumber(CWnd* pDialog, int id, double& value)
    {
        CString text;
        pDialog->GetDlgItemText(id, text);
        text.Trim();

        wchar_t* pEnd = nullptr;
        const double parsed = wcstod(text, &pEnd);
        if (text.IsEmpty() || pEnd == nullptr || *pEnd != L'\0')
            return false;

        value = parsed;
        return true;
    }
}

BEGIN_MESSAGE_MAP(COriginDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &COriginDlg::OnHelpButton)
    ON_BN_CLICKED(IDC_ORIGIN_PLANE_PICK, &COriginDlg::OnPickPlane)
    ON_BN_CLICKED(IDC_ORIGIN_POSITION_PICK, &COriginDlg::OnPickPosition)
    ON_BN_CLICKED(IDC_ORIGIN_X_PICK, &COriginDlg::OnPickX)
    ON_BN_CLICKED(IDC_ORIGIN_POSITION_CLEAR, &COriginDlg::OnClearPosition)
    ON_BN_CLICKED(IDC_ORIGIN_X_CLEAR, &COriginDlg::OnClearX)
    ON_BN_CLICKED(IDC_ORIGIN_FLIP_Z, &COriginDlg::OnOptionChanged)
    ON_CBN_SELCHANGE(IDC_ORIGIN_X_ROTATION, &COriginDlg::OnOptionChanged)
    ON_EN_CHANGE(IDC_ORIGIN_OFFSET_X, &COriginDlg::OnOptionChanged)
    ON_EN_CHANGE(IDC_ORIGIN_OFFSET_Y, &COriginDlg::OnOptionChanged)
    ON_EN_CHANGE(IDC_ORIGIN_OFFSET_Z, &COriginDlg::OnOptionChanged)
    ON_WM_CLOSE()
END_MESSAGE_MAP()

void COriginDlg::OnHelpButton()
{
    ShowHelp(L"set-origin");
}

COriginDlg::COriginDlg(CCamTabCommand* pTab, const SequenceStep& step)
    : CDialog(IDD_SET_ORIGIN, nullptr)
    , m_pTab(pTab)
    , m_step(step)
    , m_pickSlot(PickSlot::None)
    , m_offsetsValid(true)
    , m_filling(false)
{
}

bool COriginDlg::CreateModeless(CWnd* pParent)
{
    return Create(IDD_SET_ORIGIN, pParent) != FALSE;
}

BOOL COriginDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    m_filling = true;

    SetDlgItemText(IDC_ORIGIN_NAME, FromUtf8(m_step.name).c_str());

    m_rotation.SubclassDlgItem(IDC_ORIGIN_X_ROTATION, this);
    m_rotation.AddString(L"0\u00B0");
    m_rotation.AddString(L"90\u00B0");
    m_rotation.AddString(L"180\u00B0");
    m_rotation.AddString(L"270\u00B0");
    m_rotation.SetCurSel(m_step.origin.options.xQuarterTurns);

    CheckDlgButton(IDC_ORIGIN_FLIP_Z, m_step.origin.options.flipZ ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemText(IDC_ORIGIN_OFFSET_X, FormatNumber(m_step.origin.options.offset.x).c_str());
    SetDlgItemText(IDC_ORIGIN_OFFSET_Y, FormatNumber(m_step.origin.options.offset.y).c_str());
    SetDlgItemText(IDC_ORIGIN_OFFSET_Z, FormatNumber(m_step.origin.options.offset.z).c_str());

    m_filling = false;

    if (g_lastPosition.x >= 0)
        SetWindowPos(nullptr, g_lastPosition.x, g_lastPosition.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    ShowPicks();
    UpdatePreview();

    // A new step waits for the user to click Pick, so nothing is picked by accident.
    if (m_step.origin.plane.kind == PickKind::None)
        SetDlgItemText(IDC_ORIGIN_PROMPT, L"Click Pick beside Plane (Z) to choose the plane.");
    return TRUE;
}

void COriginDlg::PostNcDestroy()
{
    delete this;
}

void COriginDlg::ShowPicks()
{
    SetDlgItemText(IDC_ORIGIN_PLANE_TEXT, Describe(m_step.origin.plane).c_str());

    const bool positionFromPlane = m_step.origin.position.kind == PickKind::None &&
                                   (m_step.origin.plane.kind == PickKind::LineEdge ||
                                    m_step.origin.plane.kind == PickKind::CircleEdge);
    SetDlgItemText(IDC_ORIGIN_POSITION_TEXT,
                   positionFromPlane ? L"From the plane's edge" : Describe(m_step.origin.position).c_str());

    SetDlgItemText(IDC_ORIGIN_X_TEXT, m_step.origin.xDirection.kind == PickKind::None
                                          ? L"Calculated"
                                          : Describe(m_step.origin.xDirection).c_str());
    UpdatePickButtons();
}

void COriginDlg::UpdatePickButtons()
{
    SetDlgItemText(IDC_ORIGIN_PLANE_PICK, m_pickSlot == PickSlot::Plane ? L"Stop" : L"Pick");
    SetDlgItemText(IDC_ORIGIN_POSITION_PICK, m_pickSlot == PickSlot::Position ? L"Stop" : L"Pick");
    SetDlgItemText(IDC_ORIGIN_X_PICK, m_pickSlot == PickSlot::XDirection ? L"Stop" : L"Pick");

    GetDlgItem(IDC_ORIGIN_POSITION_CLEAR)->EnableWindow(m_step.origin.position.kind != PickKind::None);
    GetDlgItem(IDC_ORIGIN_X_CLEAR)->EnableWindow(m_step.origin.xDirection.kind != PickKind::None);
}

void COriginDlg::StartPicking(PickSlot slot)
{
    if (m_pickSlot == slot)
    {
        // The same button again stops picking.
        StopPicking();
        if (m_pTab != nullptr)
            m_pTab->EndPick();
        return;
    }

    m_pickSlot = slot;
    UpdatePickButtons();

    switch (slot)
    {
    case PickSlot::Plane:
        SetDlgItemText(IDC_ORIGIN_PROMPT, L"Click a face, an edge, or a design plane or axis. A flat face sets the "
                                          L"plane, a circular edge the plane and the centre, a plane its normal.");
        break;
    case PickSlot::Position:
        SetDlgItemText(IDC_ORIGIN_PROMPT, L"Click an edge or a design point. A straight edge gives the end nearer "
                                          L"the click, a circular edge its centre.");
        break;
    default:
        SetDlgItemText(IDC_ORIGIN_PROMPT, L"Click a straight edge or a design axis. X points along an edge away "
                                          L"from the end nearer the click, or along an axis.");
        break;
    }

    if (m_pTab != nullptr)
        m_pTab->BeginPick(slot);
}

void COriginDlg::StopPicking()
{
    m_pickSlot = PickSlot::None;
    UpdatePickButtons();
    SetDlgItemText(IDC_ORIGIN_PROMPT, L"");
}

void COriginDlg::SetPick(PickSlot slot, const PickedGeometry& pick)
{
    switch (slot)
    {
    case PickSlot::Plane:      m_step.origin.plane = pick; break;
    case PickSlot::Position:   m_step.origin.position = pick; break;
    case PickSlot::XDirection: m_step.origin.xDirection = pick; break;
    default:                   return;
    }

    StopPicking();
    ShowPicks();
    UpdatePreview();
}

void COriginDlg::ShowPickProblem(const std::wstring& problem)
{
    SetDlgItemText(IDC_ORIGIN_PROMPT, problem.c_str());
}

void COriginDlg::OnPickPlane()
{
    StartPicking(PickSlot::Plane);
}

void COriginDlg::OnPickPosition()
{
    StartPicking(PickSlot::Position);
}

void COriginDlg::OnPickX()
{
    StartPicking(PickSlot::XDirection);
}

void COriginDlg::OnClearPosition()
{
    m_step.origin.position = PickedGeometry();
    ShowPicks();
    UpdatePreview();
}

void COriginDlg::OnClearX()
{
    m_step.origin.xDirection = PickedGeometry();
    ShowPicks();
    UpdatePreview();
}

void COriginDlg::OnOptionChanged()
{
    if (m_filling)
        return;

    m_step.origin.options.flipZ = IsDlgButtonChecked(IDC_ORIGIN_FLIP_Z) == BST_CHECKED;
    const int selection = m_rotation.GetCurSel();
    m_step.origin.options.xQuarterTurns = (selection >= 0) ? selection : 0;
    UpdatePreview();
}

bool COriginDlg::ReadOffsets(std::wstring& problem)
{
    Vec3 offset;
    if (!ReadNumber(this, IDC_ORIGIN_OFFSET_X, offset.x) || !ReadNumber(this, IDC_ORIGIN_OFFSET_Y, offset.y) ||
        !ReadNumber(this, IDC_ORIGIN_OFFSET_Z, offset.z))
    {
        problem = L"Each offset must be a number.";
        return false;
    }

    m_step.origin.options.offset = offset;
    return true;
}

void COriginDlg::UpdatePreview()
{
    std::wstring problem;
    m_offsetsValid = ReadOffsets(problem);

    if (m_offsetsValid)
    {
        const FrameResult result = BuildFrame(m_step.origin);
        if (result.ok)
        {
            const Frame& frame = result.frame;
            SetDlgItemText(IDC_ORIGIN_RESULT,
                           (FormatVec3(frame.origin, L"Origin (%.3f, %.3f, %.3f) mm") +
                            FormatVec3(frame.zAxis, L"\nZ (%.4f, %.4f, %.4f)  ") +
                            FormatVec3(frame.xAxis, L"X (%.4f, %.4f, %.4f)"))
                               .c_str());
        }
        else
        {
            SetDlgItemText(IDC_ORIGIN_RESULT, FromUtf8(result.message).c_str());
        }
    }
    else
    {
        SetDlgItemText(IDC_ORIGIN_RESULT, problem.c_str());
    }

    if (m_pTab != nullptr)
        m_pTab->OnOriginPreviewChanged();
}

void COriginDlg::OnOK()
{
    CString name;
    GetDlgItemText(IDC_ORIGIN_NAME, name);
    name.Trim();

    std::wstring problem;
    if (name.IsEmpty())
    {
        problem = L"The step needs a name.";
    }
    else if (!ReadOffsets(problem))
    {
        // problem is set
    }
    else
    {
        const FrameResult result = BuildFrame(m_step.origin);
        if (!result.ok)
            problem = FromUtf8(result.message);
    }

    if (!problem.empty())
    {
        MessageBoxW(problem.c_str(), L"Set Origin", MB_OK | MB_ICONWARNING);
        return;
    }

    m_step.name = ToUtf8(static_cast<LPCWSTR>(name));
    Finish(true);
}

void COriginDlg::OnCancel()
{
    Finish(false);
}

void COriginDlg::OnClose()
{
    Finish(false);
}

void COriginDlg::Finish(bool apply)
{
    CRect rect;
    GetWindowRect(&rect);
    g_lastPosition = rect.TopLeft();

    if (m_pTab != nullptr)
    {
        CCamTabCommand* pTab = m_pTab;
        m_pTab = nullptr;
        pTab->OnOriginDialogDone(this, apply);
    }
    DestroyWindow();
}
