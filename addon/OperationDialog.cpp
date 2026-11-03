// OperationDialog.cpp : the modeless dialog that edits one operation.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "OperationDialog.h"
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

    // The tab shown when the dialog last closed.
    int g_lastPage = 0;

    const wchar_t* OperationTitle(StepType type)
    {
        switch (type)
        {
        case StepType::Drill:  return L"Drill";
        case StepType::Facing: return L"Facing";
        default:               return L"Operation";
        }
    }

    std::wstring FormatNumber(double value)
    {
        wchar_t text[64] = {};
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.10g", value);
        return text;
    }

    void AppendProblem(std::wstring& problems, const std::wstring& problem)
    {
        if (!problems.empty())
            problems += L"\n";
        problems += problem;
    }
}

/////////////////////////////////////////////////////////////////////////////
// COperationPage

COperationPage::COperationPage(UINT templateId, COperationDlg* pOwner)
    : CDialog(templateId, nullptr)
    , m_pOwner(pOwner)
    , m_templateId(templateId)
{
}

bool COperationPage::CreateInTab(CWnd* pParent)
{
    return Create(m_templateId, pParent) != FALSE;
}

bool COperationPage::ReadNumber(int id, const wchar_t* label, double& value, std::wstring& problems,
                                bool blankAllowed, bool* pBlank)
{
    CString text;
    GetDlgItemText(id, text);
    text.Trim();

    if (pBlank != nullptr)
        *pBlank = text.IsEmpty();
    if (text.IsEmpty() && blankAllowed)
        return false;

    wchar_t* pEnd = nullptr;
    const double parsed = wcstod(text, &pEnd);
    if (text.IsEmpty() || pEnd == nullptr || *pEnd != L'\0')
    {
        AppendProblem(problems, std::wstring(label) + L" must be a number.");
        return false;
    }

    value = parsed;
    return true;
}

void COperationPage::SetNumber(int id, double value)
{
    SetDlgItemText(id, FormatNumber(value).c_str());
}

/////////////////////////////////////////////////////////////////////////////
// CFeaturesPage

namespace
{
    // The sort choices, kept between dialogs.
    int g_lastOrder = 0;
    int g_lastCorner = 0;

    enum FeatureColumn
    {
        kColumnNumber,
        kColumnType,
        kColumnPart,
        kColumnDiameter,
        kColumnX,
        kColumnY,
        kColumnZ
    };

    const UINT ID_FEATURE_DELETE = 1;

    const UINT WM_APP_ROWS_CHOSEN = WM_APP + 1;

    const wchar_t* FeatureTypeName(PickKind kind)
    {
        switch (kind)
        {
        case PickKind::CylindricalFace: return L"Hole";
        case PickKind::CircleEdge:      return L"Circle";
        case PickKind::DesignPoint:     return L"Point";
        case PickKind::PlanarFace:      return L"Face";
        case PickKind::LineEdge:        return L"Edge";
        default:                        return L"Other";
        }
    }

    std::wstring FormatCoordinate(double value)
    {
        wchar_t text[64] = {};
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.3f", value);
        return text;
    }
}

BEGIN_MESSAGE_MAP(CFeaturesPage, COperationPage)
    ON_BN_CLICKED(IDC_FEATURES_SELECT, &CFeaturesPage::OnSelectFeatures)
    ON_BN_CLICKED(IDC_FEATURES_DELETE, &CFeaturesPage::OnDelete)
    ON_BN_CLICKED(IDC_FEATURES_SORT, &CFeaturesPage::OnSort)
    ON_CBN_SELCHANGE(IDC_FEATURES_ORDER, &CFeaturesPage::OnOrderChanged)
    ON_NOTIFY(LVN_KEYDOWN, IDC_FEATURES_LIST, &CFeaturesPage::OnListKeyDown)
    ON_NOTIFY(NM_RCLICK, IDC_FEATURES_LIST, &CFeaturesPage::OnListRightClick)
    ON_NOTIFY(LVN_BEGINDRAG, IDC_FEATURES_LIST, &CFeaturesPage::OnListBeginDrag)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_FEATURES_LIST, &CFeaturesPage::OnListSelectionChanged)
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_CAPTURECHANGED()
    ON_MESSAGE(WM_APP_ROWS_CHOSEN, &CFeaturesPage::OnRowsChosen)
END_MESSAGE_MAP()

CFeaturesPage::CFeaturesPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_FEATURES, pOwner)
    , m_dragging(false)
    , m_dragRow(-1)
    , m_dropHighlight(-1)
    , m_filling(false)
    , m_rowsChosenPosted(false)
{
}

BOOL CFeaturesPage::OnInitDialog()
{
    COperationPage::OnInitDialog();

    m_list.SubclassDlgItem(IDC_FEATURES_LIST, this);
    m_list.SetExtendedStyle(m_list.GetExtendedStyle() | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_list.InsertColumn(kColumnNumber, L"#", LVCFMT_RIGHT, 28);
    m_list.InsertColumn(kColumnType, L"Type", LVCFMT_LEFT, 48);
    m_list.InsertColumn(kColumnPart, L"Part", LVCFMT_LEFT, 110);
    m_list.InsertColumn(kColumnDiameter, L"Dia.", LVCFMT_RIGHT, 54);
    m_list.InsertColumn(kColumnX, L"X", LVCFMT_RIGHT, 64);
    m_list.InsertColumn(kColumnY, L"Y", LVCFMT_RIGHT, 64);
    m_list.InsertColumn(kColumnZ, L"Z", LVCFMT_RIGHT, 64);

    m_order.SubclassDlgItem(IDC_FEATURES_ORDER, this);
    m_order.AddString(L"Minimum distance");
    m_order.AddString(L"Directional, X first");
    m_order.AddString(L"Directional, Y first");
    m_order.SetCurSel(g_lastOrder);

    m_corner.SubclassDlgItem(IDC_FEATURES_CORNER, this);
    m_corner.AddString(L"Top left");
    m_corner.AddString(L"Top right");
    m_corner.AddString(L"Bottom left");
    m_corner.AddString(L"Bottom right");
    m_corner.SetCurSel(g_lastCorner);

    ShowSelecting(false);
    OnOrderChanged();
    return TRUE;
}

void CFeaturesPage::Load(const SequenceStep& step)
{
    m_features = step.features;
    FillList();
}

bool CFeaturesPage::Save(SequenceStep& step, std::wstring& /*problems*/)
{
    step.features = m_features;
    return true;
}

void CFeaturesPage::FillList()
{
    camcore::Frame frame;
    const bool haveFrame = m_pOwner->GetFrame(frame);

    m_filling = true;
    m_list.SetRedraw(FALSE);
    m_list.DeleteAllItems();
    for (size_t i = 0; i < m_features.size(); ++i)
    {
        const PickedGeometry& feature = m_features[i];
        const int row = m_list.InsertItem(static_cast<int>(i), std::to_wstring(i + 1).c_str());
        m_list.SetItemText(row, kColumnType, FeatureTypeName(feature.kind));

        std::wstring part = FromUtf8(feature.occurrenceName);
        if (feature.kind == PickKind::DesignPoint)
            part = part.empty() ? FromUtf8(feature.targetName) : part + L": " + FromUtf8(feature.targetName);
        m_list.SetItemText(row, kColumnPart, part.c_str());

        if (feature.radius > 0.0)
            m_list.SetItemText(row, kColumnDiameter, FormatCoordinate(feature.radius * 2.0).c_str());

        // Shown in the operation's origin frame, the way it's machined.
        const Vec3 point = haveFrame ? WorldToFrame(frame, FeaturePoint(frame, feature)) : feature.point;
        m_list.SetItemText(row, kColumnX, FormatCoordinate(point.x).c_str());
        m_list.SetItemText(row, kColumnY, FormatCoordinate(point.y).c_str());
        m_list.SetItemText(row, kColumnZ, FormatCoordinate(point.z).c_str());
    }
    m_list.SetRedraw(TRUE);
    m_list.Invalidate();
    m_filling = false;
    UpdateButtons();
}

void CFeaturesPage::SelectRow(int row)
{
    for (int i = 0; i < m_list.GetItemCount(); ++i)
        m_list.SetItemState(i, i == row ? LVIS_SELECTED | LVIS_FOCUSED : 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (row >= 0)
        m_list.EnsureVisible(row, FALSE);
}

void CFeaturesPage::UpdateButtons()
{
    GetDlgItem(IDC_FEATURES_DELETE)->EnableWindow(m_list.GetSelectedCount() > 0);
    GetDlgItem(IDC_FEATURES_SORT)->EnableWindow(m_features.size() > 1);
}

void CFeaturesPage::ForgetViewSelection()
{
    m_viewSelection.clear();
}

void CFeaturesPage::ShowSelecting(bool selecting)
{
    SetDlgItemText(IDC_FEATURES_SELECT, selecting ? L"Done" : L"Select Features");
    ShowNote(selecting ? L"Select features in the view. Ctrl+click adds or removes one." : L"");
}

void CFeaturesPage::ShowNote(const std::wstring& text)
{
    SetDlgItemText(IDC_FEATURES_NOTE, text.c_str());
}

void CFeaturesPage::SetSelectedFeatures(const std::vector<PickedGeometry>& selected, int ignored)
{
    // Only what changed in the view counts, so clearing and re-selecting the list
    // when selecting starts doesn't empty it.
    m_features = ApplySelectionChange(m_features, m_viewSelection, selected);
    m_viewSelection = selected;
    FillList();

    std::wstring note = std::to_wstring(m_features.size()) + (m_features.size() == 1 ? L" feature." : L" features.");
    if (ignored > 0)
    {
        note += L" " + std::to_wstring(ignored) + (ignored == 1 ? L" selected item isn't" : L" selected items aren't") +
                (m_pOwner->GetFeatureFilter() == FeatureFilter::FacingArea ? L" a flat face or an edge"
                                                                           : L" a circle, hole or point") +
                L", and was left out.";
    }
    ShowNote(note);
}

void CFeaturesPage::ChangedByUser()
{
    FillList();
}

void CFeaturesPage::OnSelectFeatures()
{
    if (m_pOwner->IsSelectingFeatures())
        m_pOwner->StopSelectingFeatures();
    else
        m_pOwner->StartSelectingFeatures();
}

void CFeaturesPage::OnDelete()
{
    // Deleting from the end keeps the earlier rows' numbers right.
    std::vector<int> rows;
    POSITION position = m_list.GetFirstSelectedItemPosition();
    while (position != nullptr)
        rows.push_back(m_list.GetNextSelectedItem(position));
    if (rows.empty())
        return;

    std::sort(rows.rbegin(), rows.rend());
    for (int row : rows)
    {
        if (row >= 0 && row < static_cast<int>(m_features.size()))
            m_features.erase(m_features.begin() + row);
    }

    // While selecting, the view's selection follows the list.
    if (m_pOwner->IsSelectingFeatures())
        m_pOwner->StartSelectingFeatures();

    ChangedByUser();
    SelectRow((std::min)(rows.back(), static_cast<int>(m_features.size()) - 1));
}

void CFeaturesPage::OnOrderChanged()
{
    // The corner is only for the directional orders.
    GetDlgItem(IDC_FEATURES_CORNER)->EnableWindow(m_order.GetCurSel() > 0);
    g_lastOrder = (std::max)(m_order.GetCurSel(), 0);
}

void CFeaturesPage::OnSort()
{
    camcore::Frame frame;
    if (!m_pOwner->GetFrame(frame))
    {
        ShowNote(L"There's no complete Set Origin above this folder, so the features can't be sorted.");
        return;
    }

    g_lastOrder = (std::max)(m_order.GetCurSel(), 0);
    g_lastCorner = (std::max)(m_corner.GetCurSel(), 0);

    std::vector<Vec3> points;
    for (const PickedGeometry& feature : m_features)
        points.push_back(WorldToFrame(frame, FeaturePoint(frame, feature)));

    const FeatureOrder order = (g_lastOrder == 1)   ? FeatureOrder::RowsXFirst
                               : (g_lastOrder == 2) ? FeatureOrder::RowsYFirst
                                                    : FeatureOrder::MinimumDistance;
    static const StartCorner corners[] =
    {
        StartCorner::TopLeft, StartCorner::TopRight, StartCorner::BottomLeft, StartCorner::BottomRight
    };

    std::vector<PickedGeometry> sorted;
    for (size_t index : SortFeatures(points, order, corners[g_lastCorner % 4]))
        sorted.push_back(m_features[index]);
    m_features = sorted;

    ChangedByUser();
    ShowNote(L"Sorted.");
}

void CFeaturesPage::OnListKeyDown(NMHDR* pNotify, LRESULT* pResult)
{
    *pResult = 0;
    const WORD key = reinterpret_cast<NMLVKEYDOWN*>(pNotify)->wVKey;
    if (key == VK_DELETE)
    {
        OnDelete();
    }
    else if (key == 'A' && (::GetKeyState(VK_CONTROL) & 0x8000) != 0)
    {
        // Ctrl+A chooses every row. The view follows, as for any change.
        for (int row = 0; row < m_list.GetItemCount(); ++row)
            m_list.SetItemState(row, LVIS_SELECTED, LVIS_SELECTED);
    }
}

void CFeaturesPage::OnListRightClick(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;
    if (m_list.GetSelectedCount() == 0)
        return;

    CMenu menu;
    menu.CreatePopupMenu();
    menu.AppendMenu(MF_STRING, ID_FEATURE_DELETE, L"Delete");

    CPoint point;
    ::GetCursorPos(&point);
    if (menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y,
                            this) == ID_FEATURE_DELETE)
        OnDelete();
}

void CFeaturesPage::OnListSelectionChanged(NMHDR* pNotify, LRESULT* pResult)
{
    *pResult = 0;
    UpdateButtons();

    // Choosing several rows changes each one, so they're shown in the view once
    // the changes have all arrived.
    const NMLISTVIEW* pInfo = reinterpret_cast<NMLISTVIEW*>(pNotify);
    const bool selectionChanged = (pInfo->uChanged & LVIF_STATE) != 0 &&
                                  ((pInfo->uOldState ^ pInfo->uNewState) & (LVIS_SELECTED | LVIS_FOCUSED)) != 0;
    if (selectionChanged && !m_filling && !m_rowsChosenPosted)
    {
        m_rowsChosenPosted = true;
        PostMessage(WM_APP_ROWS_CHOSEN);
    }
}

LRESULT CFeaturesPage::OnRowsChosen(WPARAM, LPARAM)
{
    m_rowsChosenPosted = false;
    ShowChosenRows();
    return 0;
}

void CFeaturesPage::ShowChosenRows()
{
    // The focused row comes first, so it's the one highlighted while selecting.
    std::vector<PickedGeometry> chosen;
    const int focused = m_list.GetNextItem(-1, LVNI_FOCUSED);
    if (focused >= 0 && focused < static_cast<int>(m_features.size()) &&
        (m_list.GetItemState(focused, LVIS_SELECTED) & LVIS_SELECTED) != 0)
        chosen.push_back(m_features[focused]);

    POSITION position = m_list.GetFirstSelectedItemPosition();
    while (position != nullptr)
    {
        const int row = m_list.GetNextSelectedItem(position);
        if (row != focused && row >= 0 && row < static_cast<int>(m_features.size()))
            chosen.push_back(m_features[row]);
    }
    m_pOwner->ShowFeaturesInView(chosen);
}

void CFeaturesPage::OnListBeginDrag(NMHDR* pNotify, LRESULT* pResult)
{
    *pResult = 0;
    m_dragRow = reinterpret_cast<NMLISTVIEW*>(pNotify)->iItem;
    if (m_dragRow < 0 || m_dragRow >= static_cast<int>(m_features.size()))
        return;

    m_dragging = true;
    SetCapture();
}

int CFeaturesPage::DropRow(CPoint pagePoint) const
{
    CPoint point = pagePoint;
    ::MapWindowPoints(m_hWnd, m_list.GetSafeHwnd(), &point, 1);

    LVHITTESTINFO hit = {};
    hit.pt = point;
    const int row = const_cast<CListCtrl&>(m_list).HitTest(&hit);
    if (row >= 0)
        return row;

    // Below the last row means the end. Above the first means the start.
    const int count = m_list.GetItemCount();
    CRect rect;
    if (count > 0 && m_list.GetItemRect(count - 1, &rect, LVIR_BOUNDS) && point.y >= rect.bottom)
        return count;
    return 0;
}

void CFeaturesPage::OnMouseMove(UINT flags, CPoint point)
{
    if (!m_dragging)
    {
        COperationPage::OnMouseMove(flags, point);
        return;
    }

    const int row = DropRow(point);
    const int highlight = (row < m_list.GetItemCount()) ? row : -1;
    if (highlight != m_dropHighlight)
    {
        if (m_dropHighlight >= 0)
            m_list.SetItemState(m_dropHighlight, 0, LVIS_DROPHILITED);
        if (highlight >= 0)
        {
            m_list.SetItemState(highlight, LVIS_DROPHILITED, LVIS_DROPHILITED);
            m_list.EnsureVisible(highlight, FALSE);
        }
        m_dropHighlight = highlight;
    }
}

void CFeaturesPage::OnLButtonUp(UINT flags, CPoint point)
{
    if (!m_dragging)
    {
        COperationPage::OnLButtonUp(flags, point);
        return;
    }

    // The dragged feature goes before the row it's dropped on.
    const int from = m_dragRow;
    int to = DropRow(point);
    EndDrag();

    if (from < 0 || from >= static_cast<int>(m_features.size()) || to == from || to == from + 1)
        return;

    PickedGeometry feature = m_features[from];
    m_features.erase(m_features.begin() + from);
    if (to > from)
        --to;
    m_features.insert(m_features.begin() + to, feature);

    ChangedByUser();
    SelectRow(to);
}

void CFeaturesPage::OnCaptureChanged(CWnd* pWnd)
{
    if (m_dragging)
        EndDrag();
    COperationPage::OnCaptureChanged(pWnd);
}

void CFeaturesPage::EndDrag()
{
    if (!m_dragging)
        return;

    m_dragging = false;
    if (m_dropHighlight >= 0)
        m_list.SetItemState(m_dropHighlight, 0, LVIS_DROPHILITED);
    m_dropHighlight = -1;
    if (GetCapture() == this)
        ReleaseCapture();
}

/////////////////////////////////////////////////////////////////////////////
// CToolPage

namespace
{
    const wchar_t* ToolTypeName(ToolType type)
    {
        return (type == ToolType::Drill) ? L"Drill" : L"Flat end mill";
    }

    std::wstring Length(double value, ToolUnit unit)
    {
        return FormatNumber(value) + (unit == ToolUnit::Inches ? L" in" : L" mm");
    }
}

BEGIN_MESSAGE_MAP(CToolPage, COperationPage)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_TOOL_LIST, &CToolPage::OnToolChanged)
END_MESSAGE_MAP()

CToolPage::CToolPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_TOOL, pOwner)
{
}

BOOL CToolPage::OnInitDialog()
{
    COperationPage::OnInitDialog();

    m_tools.SubclassDlgItem(IDC_TOOL_LIST, this);
    m_tools.SetExtendedStyle(m_tools.GetExtendedStyle() | LVS_EX_FULLROWSELECT);
    m_tools.InsertColumn(0, L"T", LVCFMT_RIGHT, 32);
    m_tools.InsertColumn(1, L"Name", LVCFMT_LEFT, 130);
    m_tools.InsertColumn(2, L"Type", LVCFMT_LEFT, 80);

    m_details.SubclassDlgItem(IDC_TOOL_DETAILS, this);
    m_details.SetExtendedStyle(m_details.GetExtendedStyle() | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_details.InsertColumn(0, L"Property", LVCFMT_LEFT, 90);
    m_details.InsertColumn(1, L"Value", LVCFMT_LEFT, 100);
    return TRUE;
}

void CToolPage::Load(const SequenceStep& step)
{
    m_library = m_pOwner->GetToolLibrary();
    m_toolId = step.toolId;

    // Later, each operation lists only the tools that suit it.
    m_tools.DeleteAllItems();
    int chosen = -1;
    for (size_t i = 0; i < m_library.tools.size(); ++i)
    {
        const Tool& tool = m_library.tools[i];
        const int row = m_tools.InsertItem(static_cast<int>(i), std::to_wstring(tool.number).c_str());
        m_tools.SetItemText(row, 1, FromUtf8(tool.name).c_str());
        m_tools.SetItemText(row, 2, ToolTypeName(tool.type));
        m_tools.SetItemData(row, static_cast<DWORD_PTR>(i));
        if (tool.id == m_toolId)
            chosen = row;
    }

    if (chosen >= 0)
    {
        m_tools.SetItemState(chosen, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        m_tools.EnsureVisible(chosen, FALSE);
    }

    if (m_library.tools.empty())
        SetDlgItemText(IDC_TOOL_NOTE, L"The tool library is empty. Double-click Tools on the CAM tab to add tools.");
    else if (!m_toolId.empty() && chosen < 0)
        SetDlgItemText(IDC_TOOL_NOTE, L"This operation's tool isn't in the library. Choose another.");
    else if (chosen < 0)
        SetDlgItemText(IDC_TOOL_NOTE, L"Choose a tool.");
    else
        SetDlgItemText(IDC_TOOL_NOTE, L"");

    ShowDetails(ChosenTool());
}

bool CToolPage::Save(SequenceStep& step, std::wstring& /*problems*/)
{
    // With nothing chosen, a tool missing from the library stays, so it's still known.
    if (const Tool* pTool = ChosenTool())
        step.toolId = pTool->id;
    else
        step.toolId = m_toolId;
    return true;
}

const Tool* CToolPage::ChosenTool() const
{
    const int row = m_tools.GetNextItem(-1, LVNI_SELECTED);
    if (row < 0)
        return nullptr;
    const size_t index = static_cast<size_t>(m_tools.GetItemData(row));
    return (index < m_library.tools.size()) ? &m_library.tools[index] : nullptr;
}

void CToolPage::ShowDetails(const Tool* pTool)
{
    m_details.DeleteAllItems();
    if (pTool == nullptr)
        return;

    const auto add = [&](const wchar_t* property, const std::wstring& value)
    {
        const int row = m_details.InsertItem(m_details.GetItemCount(), property);
        m_details.SetItemText(row, 1, value.c_str());
    };

    // In the tool's own unit, as the library holds it.
    add(L"Number", L"T" + std::to_wstring(pTool->number));
    add(L"Name", FromUtf8(pTool->name));
    add(L"Type", ToolTypeName(pTool->type));
    add(L"Units", pTool->unit == ToolUnit::Inches ? L"Inches" : L"Millimetres");
    add(L"Diameter", Length(pTool->diameter, pTool->unit));
    add(L"Flute length", Length(pTool->fluteLength, pTool->unit));
    add(L"Overall length", Length(pTool->overallLength, pTool->unit));
    if (pTool->type == ToolType::Drill)
        add(L"Point angle", FormatNumber(pTool->pointAngle) + L"\u00B0");
    else
        add(L"Flute count", std::to_wstring(pTool->fluteCount));
}

void CToolPage::OnToolChanged(NMHDR* pNotify, LRESULT* pResult)
{
    *pResult = 0;
    const NMLISTVIEW* pInfo = reinterpret_cast<NMLISTVIEW*>(pNotify);
    if ((pInfo->uChanged & LVIF_STATE) != 0 && ((pInfo->uOldState ^ pInfo->uNewState) & LVIS_SELECTED) != 0)
    {
        ShowDetails(ChosenTool());
        if (ChosenTool() != nullptr)
            SetDlgItemText(IDC_TOOL_NOTE, L"");
    }
}

/////////////////////////////////////////////////////////////////////////////
// CFeedsPage

BEGIN_MESSAGE_MAP(CFeedsPage, COperationPage)
    ON_BN_CLICKED(IDC_FEEDS_TRANSFER_RAPID, &CFeedsPage::OnTransferChanged)
    ON_BN_CLICKED(IDC_FEEDS_TRANSFER_SET, &CFeedsPage::OnTransferChanged)
END_MESSAGE_MAP()

CFeedsPage::CFeedsPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_FEEDS, pOwner)
{
}

void CFeedsPage::Load(const SequenceStep& step)
{
    const FeedSettings& feeds = step.feeds;
    SetNumber(IDC_FEEDS_SPINDLE, feeds.spindleRpm);
    SetNumber(IDC_FEEDS_PLUNGE, feeds.plunge);
    SetNumber(IDC_FEEDS_APPROACH, feeds.approach);
    SetNumber(IDC_FEEDS_ENGAGE, feeds.engage);
    SetNumber(IDC_FEEDS_CUT, feeds.cut);
    SetNumber(IDC_FEEDS_RETRACT, feeds.retract);
    SetNumber(IDC_FEEDS_DEPARTURE, feeds.departure);
    SetNumber(IDC_FEEDS_TRANSFER, feeds.transfer);
    CheckRadioButton(IDC_FEEDS_TRANSFER_RAPID, IDC_FEEDS_TRANSFER_SET,
                     feeds.transferAtRapid ? IDC_FEEDS_TRANSFER_RAPID : IDC_FEEDS_TRANSFER_SET);
    UpdateFields();
}

bool CFeedsPage::Save(SequenceStep& step, std::wstring& problems)
{
    FeedSettings feeds = step.feeds;
    std::wstring found;

    ReadNumber(IDC_FEEDS_SPINDLE, L"The spindle speed", feeds.spindleRpm, found);
    ReadNumber(IDC_FEEDS_PLUNGE, L"The plunge feed", feeds.plunge, found);
    ReadNumber(IDC_FEEDS_APPROACH, L"The approach feed", feeds.approach, found);
    ReadNumber(IDC_FEEDS_ENGAGE, L"The engage feed", feeds.engage, found);
    ReadNumber(IDC_FEEDS_CUT, L"The cut feed", feeds.cut, found);
    ReadNumber(IDC_FEEDS_RETRACT, L"The retract feed", feeds.retract, found);
    ReadNumber(IDC_FEEDS_DEPARTURE, L"The departure feed", feeds.departure, found);

    // The transfer feed is only read when it's used.
    feeds.transferAtRapid = IsDlgButtonChecked(IDC_FEEDS_TRANSFER_SET) != BST_CHECKED;
    if (!feeds.transferAtRapid)
        ReadNumber(IDC_FEEDS_TRANSFER, L"The transfer feed", feeds.transfer, found);

    if (found.empty())
    {
        for (const std::string& problem : ValidateFeeds(feeds))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.feeds = feeds;
    return true;
}

void CFeedsPage::UpdateFields()
{
    GetDlgItem(IDC_FEEDS_TRANSFER)->EnableWindow(IsDlgButtonChecked(IDC_FEEDS_TRANSFER_SET) == BST_CHECKED);
}

void CFeedsPage::OnTransferChanged()
{
    UpdateFields();
}

/////////////////////////////////////////////////////////////////////////////
// CClearancePage

BEGIN_MESSAGE_MAP(CClearancePage, COperationPage)
    ON_BN_CLICKED(IDC_CLEARANCE_AUTOMATIC, &CClearancePage::OnModeChanged)
    ON_BN_CLICKED(IDC_CLEARANCE_PART, &CClearancePage::OnModeChanged)
    ON_BN_CLICKED(IDC_CLEARANCE_STOCK, &CClearancePage::OnModeChanged)
    ON_BN_CLICKED(IDC_CLEARANCE_ABSOLUTE, &CClearancePage::OnModeChanged)
END_MESSAGE_MAP()

CClearancePage::CClearancePage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_CLEARANCE, pOwner)
{
}

void CClearancePage::Load(const SequenceStep& step)
{
    const ClearanceSettings& clearance = step.clearance;

    const int mode = (clearance.mode == ClearanceMode::PartMaxZ)    ? IDC_CLEARANCE_PART
                     : (clearance.mode == ClearanceMode::StockMaxZ) ? IDC_CLEARANCE_STOCK
                     : (clearance.mode == ClearanceMode::AbsoluteZ) ? IDC_CLEARANCE_ABSOLUTE
                                                                    : IDC_CLEARANCE_AUTOMATIC;
    CheckRadioButton(IDC_CLEARANCE_AUTOMATIC, IDC_CLEARANCE_ABSOLUTE, mode);

    SetNumber(IDC_CLEARANCE_PART_DISTANCE, clearance.partDistance);
    SetNumber(IDC_CLEARANCE_STOCK_DISTANCE, clearance.stockDistance);
    SetNumber(IDC_CLEARANCE_ABSOLUTE_Z, clearance.absoluteZ);
    UpdateFields();
}

bool CClearancePage::Save(SequenceStep& step, std::wstring& problems)
{
    ClearanceSettings clearance = step.clearance;
    std::wstring found;

    // Only the box for the chosen mode is read, so a stray entry in another doesn't block OK.
    switch (GetCheckedRadioButton(IDC_CLEARANCE_AUTOMATIC, IDC_CLEARANCE_ABSOLUTE))
    {
    case IDC_CLEARANCE_PART:
        clearance.mode = ClearanceMode::PartMaxZ;
        ReadNumber(IDC_CLEARANCE_PART_DISTANCE, L"The distance above the parts", clearance.partDistance, found);
        break;
    case IDC_CLEARANCE_STOCK:
        clearance.mode = ClearanceMode::StockMaxZ;
        ReadNumber(IDC_CLEARANCE_STOCK_DISTANCE, L"The distance above the stock", clearance.stockDistance, found);
        break;
    case IDC_CLEARANCE_ABSOLUTE:
        clearance.mode = ClearanceMode::AbsoluteZ;
        ReadNumber(IDC_CLEARANCE_ABSOLUTE_Z, L"The absolute Z", clearance.absoluteZ, found);
        break;
    default:
        clearance.mode = ClearanceMode::Automatic;
        break;
    }

    if (found.empty())
    {
        for (const std::string& problem : ValidateClearance(clearance))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.clearance = clearance;
    return true;
}

void CClearancePage::UpdateFields()
{
    const int mode = GetCheckedRadioButton(IDC_CLEARANCE_AUTOMATIC, IDC_CLEARANCE_ABSOLUTE);
    GetDlgItem(IDC_CLEARANCE_PART_DISTANCE)->EnableWindow(mode == IDC_CLEARANCE_PART);
    GetDlgItem(IDC_CLEARANCE_STOCK_DISTANCE)->EnableWindow(mode == IDC_CLEARANCE_STOCK);
    GetDlgItem(IDC_CLEARANCE_ABSOLUTE_Z)->EnableWindow(mode == IDC_CLEARANCE_ABSOLUTE);
}

void CClearancePage::OnModeChanged()
{
    UpdateFields();
}

/////////////////////////////////////////////////////////////////////////////
// CDrillingPage

BEGIN_MESSAGE_MAP(CDrillingPage, COperationPage)
    ON_BN_CLICKED(IDC_DRILL_LOCATE, &CDrillingPage::OnLocate)
    ON_BN_CLICKED(IDC_DWELL_OFF, &CDrillingPage::OnDwellChanged)
    ON_BN_CLICKED(IDC_DWELL_TIME, &CDrillingPage::OnDwellChanged)
    ON_BN_CLICKED(IDC_DWELL_REVOLUTIONS, &CDrillingPage::OnDwellChanged)
END_MESSAGE_MAP()

CDrillingPage::CDrillingPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_DRILLING, pOwner)
{
}

BOOL CDrillingPage::OnInitDialog()
{
    COperationPage::OnInitDialog();
    ShowLocating(false);
    return TRUE;
}

void CDrillingPage::Load(const SequenceStep& step)
{
    const DrillSettings& drill = step.drill;

    SetNumber(IDC_DRILL_DEPTH, drill.depth);
    CheckDlgButton(IDC_DRILL_ADD_TIP, drill.addTipToDepth ? BST_CHECKED : BST_UNCHECKED);

    CheckRadioButton(IDC_DRILL_AT_TOP, IDC_DRILL_AT_BOTTOM,
                     drill.location == GeometryLocation::AtBottom ? IDC_DRILL_AT_BOTTOM : IDC_DRILL_AT_TOP);

    const int dwell = (drill.dwell == DwellMode::Seconds)       ? IDC_DWELL_TIME
                      : (drill.dwell == DwellMode::Revolutions) ? IDC_DWELL_REVOLUTIONS
                                                                : IDC_DWELL_OFF;
    CheckRadioButton(IDC_DWELL_OFF, IDC_DWELL_REVOLUTIONS, dwell);
    SetNumber(IDC_DWELL_SECONDS, drill.dwellSeconds);
    SetNumber(IDC_DWELL_REVOLUTION_COUNT, drill.dwellRevolutions);

    SetNumber(IDC_DRILL_APPROACH, drill.approachDistance);

    if (drill.pecking)
        SetNumber(IDC_DRILL_PECK, drill.peckDepth);
    else
        SetDlgItemText(IDC_DRILL_PECK, L"");

    UpdateDwellFields();
}

bool CDrillingPage::Save(SequenceStep& step, std::wstring& problems)
{
    DrillSettings drill = step.drill;
    std::wstring found;

    ReadNumber(IDC_DRILL_DEPTH, L"The depth", drill.depth, found);
    drill.addTipToDepth = IsDlgButtonChecked(IDC_DRILL_ADD_TIP) == BST_CHECKED;
    drill.location = (GetCheckedRadioButton(IDC_DRILL_AT_TOP, IDC_DRILL_AT_BOTTOM) == IDC_DRILL_AT_BOTTOM)
                         ? GeometryLocation::AtBottom
                         : GeometryLocation::AtTop;

    switch (GetCheckedRadioButton(IDC_DWELL_OFF, IDC_DWELL_REVOLUTIONS))
    {
    case IDC_DWELL_TIME:
        drill.dwell = DwellMode::Seconds;
        ReadNumber(IDC_DWELL_SECONDS, L"The dwell time", drill.dwellSeconds, found);
        break;
    case IDC_DWELL_REVOLUTIONS:
        drill.dwell = DwellMode::Revolutions;
        ReadNumber(IDC_DWELL_REVOLUTION_COUNT, L"The dwell revolutions", drill.dwellRevolutions, found);
        break;
    default:
        drill.dwell = DwellMode::Off;
        break;
    }

    ReadNumber(IDC_DRILL_APPROACH, L"The approach distance", drill.approachDistance, found);

    bool blank = false;
    if (ReadNumber(IDC_DRILL_PECK, L"The peck depth", drill.peckDepth, found, true, &blank))
        drill.pecking = true;
    else if (blank)
        drill.pecking = false;

    // Range checks only make sense once every number has been read.
    if (found.empty())
    {
        for (const std::string& problem : ValidateDrillSettings(drill))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.drill = drill;
    return true;
}

void CDrillingPage::UpdateDwellFields()
{
    const int dwell = GetCheckedRadioButton(IDC_DWELL_OFF, IDC_DWELL_REVOLUTIONS);
    GetDlgItem(IDC_DWELL_SECONDS)->EnableWindow(dwell == IDC_DWELL_TIME);
    GetDlgItem(IDC_DWELL_REVOLUTION_COUNT)->EnableWindow(dwell == IDC_DWELL_REVOLUTIONS);
}

void CDrillingPage::OnDwellChanged()
{
    UpdateDwellFields();
}

void CDrillingPage::OnLocate()
{
    if (m_pOwner->IsLocating())
        m_pOwner->StopLocating();
    else
        m_pOwner->StartLocating();
}

void CDrillingPage::ShowLocating(bool locating)
{
    SetDlgItemText(IDC_DRILL_LOCATE, locating ? L"Stop" : L"Locate");
    ShowLocateNote(locating ? L"Click a point, plane, face or edge to measure its distance from the origin along Z."
                            : L"");
}

void CDrillingPage::SetDepth(double depth)
{
    SetNumber(IDC_DRILL_DEPTH, depth);
}

void CDrillingPage::ShowLocateNote(const std::wstring& text)
{
    SetDlgItemText(IDC_DRILL_LOCATE_NOTE, text.c_str());
}

/////////////////////////////////////////////////////////////////////////////
// COperationDlg

BEGIN_MESSAGE_MAP(COperationDlg, CDialog)
    ON_NOTIFY(TCN_SELCHANGE, IDC_OP_TABS, &COperationDlg::OnTabChanged)
    ON_WM_CLOSE()
END_MESSAGE_MAP()

COperationDlg::COperationDlg(CCamTabCommand* pTab, const SequenceStep& step)
    : CDialog(IDD_OPERATION, nullptr)
    , m_pTab(pTab)
    , m_step(step)
    , m_pFeatures(nullptr)
    , m_pDrilling(nullptr)
    , m_locating(false)
    , m_selectingFeatures(false)
    , m_haveFrame(false)
{
}

bool COperationDlg::CreateModeless(CWnd* pParent)
{
    return Create(IDD_OPERATION, pParent) != FALSE;
}

BOOL COperationDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    SetWindowText(OperationTitle(m_step.type));
    SetDlgItemText(IDC_OP_NAME, FromUtf8(m_step.name).c_str());

    m_tabs.SubclassDlgItem(IDC_OP_TABS, this);
    if (m_pTab != nullptr)
        m_haveFrame = m_pTab->GetOperationFrame(m_frame);

    // Every operation has these. The empty ones are still to be built.
    auto features = std::make_unique<CFeaturesPage>(this);
    CFeaturesPage* pFeatures = features.get();
    AddPage(L"Features", std::move(features));
    if (pFeatures->GetSafeHwnd() != nullptr)
        m_pFeatures = pFeatures;

    AddPage(L"Tool", std::make_unique<CToolPage>(this));
    AddPage(L"Feeds && Speeds", std::make_unique<CFeedsPage>(this));
    AddPage(L"Clearance", std::make_unique<CClearancePage>(this));

    // Drill and Facing have no Sorting tab.
    if (m_step.type != StepType::Drill && m_step.type != StepType::Facing)
        AddPage(L"Sorting", std::make_unique<COperationPage>(IDD_OP_PAGE, this));

    // Then its own.
    if (m_step.type == StepType::Facing)
    {
        AddPage(L"Facing", std::make_unique<COperationPage>(IDD_OP_PAGE, this));
    }
    else if (m_step.type == StepType::Drill)
    {
        auto drilling = std::make_unique<CDrillingPage>(this);
        CDrillingPage* pDrilling = drilling.get();
        AddPage(L"Drilling", std::move(drilling));
        if (pDrilling->GetSafeHwnd() != nullptr)
            m_pDrilling = pDrilling;
    }

    for (const auto& page : m_pages)
        page->Load(m_step);

    if (g_lastPosition.x >= 0)
        SetWindowPos(nullptr, g_lastPosition.x, g_lastPosition.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    const int page = (g_lastPage < static_cast<int>(m_pages.size())) ? g_lastPage : 0;
    m_tabs.SetCurSel(page);
    ShowPage(page);
    return TRUE;
}

void COperationDlg::AddPage(const wchar_t* title, std::unique_ptr<COperationPage> page)
{
    if (!page->CreateInTab(this))
    {
        Log(L"[operation] could not create the %s tab", title);
        return;
    }

    // A tab's label shows a single & as an underline, so titles double it.
    m_tabs.InsertItem(static_cast<int>(m_pages.size()), title);

    // Place the page over the tab control's display area, in front of it.
    CRect area;
    m_tabs.GetWindowRect(&area);
    ScreenToClient(&area);
    m_tabs.AdjustRect(FALSE, &area);
    page->SetWindowPos(&CWnd::wndTop, area.left, area.top, area.Width(), area.Height(), SWP_HIDEWINDOW);

    m_pages.push_back(std::move(page));
}

void COperationDlg::ShowPage(int index)
{
    for (size_t i = 0; i < m_pages.size(); ++i)
        m_pages[i]->ShowWindow(static_cast<int>(i) == index ? SW_SHOW : SW_HIDE);
}

void COperationDlg::OnTabChanged(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;
    ShowPage(m_tabs.GetCurSel());
}

void COperationDlg::StartLocating()
{
    if (m_pDrilling == nullptr || m_pTab == nullptr)
        return;

    StopSelectingFeatures();

    m_locating = true;
    m_pDrilling->ShowLocating(true);
    m_pTab->BeginLocate();
}

void COperationDlg::StopLocating()
{
    if (!m_locating)
        return;

    m_locating = false;
    if (m_pDrilling != nullptr)
        m_pDrilling->ShowLocating(false);
    if (m_pTab != nullptr)
        m_pTab->EndLocate();
}

void COperationDlg::SetLocatedDepth(double depth)
{
    StopLocating();
    if (m_pDrilling != nullptr)
    {
        m_pDrilling->SetDepth(depth);
        m_pDrilling->ShowLocateNote(depth > 0.0 ? L"The depth was measured from the picked item."
                                                : L"That item measures as 0. It's on the origin's XY plane, runs "
                                                  L"along the drill direction, or is too far away.");
    }
}

void COperationDlg::ShowLocateProblem(const std::wstring& problem)
{
    if (m_pDrilling != nullptr)
        m_pDrilling->ShowLocateNote(problem);
}

FeatureFilter COperationDlg::GetFeatureFilter() const
{
    // Each operation type has its own filter. Drill needs points, and Facing faces and edges.
    return (m_step.type == StepType::Facing) ? FeatureFilter::FacingArea : FeatureFilter::Points;
}

void COperationDlg::StartSelectingFeatures()
{
    if (m_pFeatures == nullptr || m_pTab == nullptr)
        return;

    // Locate and Select Features both use Alibre's selection, so only one runs.
    StopLocating();

    // Starting again while selecting puts the view's selection back in step with the list.
    m_selectingFeatures = true;
    m_pFeatures->ForgetViewSelection();
    m_pFeatures->ShowSelecting(true);
    m_pTab->BeginFeatureSelect();
}

void COperationDlg::StopSelectingFeatures()
{
    if (!m_selectingFeatures)
        return;

    m_selectingFeatures = false;
    if (m_pFeatures != nullptr)
        m_pFeatures->ShowSelecting(false);
    if (m_pTab != nullptr)
        m_pTab->EndFeatureSelect();

    // The view's selection was the whole list. Now it follows the chosen rows again.
    if (m_pFeatures != nullptr && m_pTab != nullptr)
        m_pFeatures->ShowChosenRows();
}

std::vector<PickedGeometry> COperationDlg::GetFeatures() const
{
    return (m_pFeatures != nullptr) ? m_pFeatures->GetFeatures() : std::vector<PickedGeometry>();
}

void COperationDlg::SetSelectedFeatures(const std::vector<PickedGeometry>& selected, int ignored)
{
    if (m_pFeatures != nullptr)
        m_pFeatures->SetSelectedFeatures(selected, ignored);
}

void COperationDlg::ShowFeaturesInView(const std::vector<PickedGeometry>& features)
{
    if (m_pTab != nullptr)
        m_pTab->ShowFeaturesInView(features);
}

void COperationDlg::ShowFeatureNote(const std::wstring& text)
{
    if (m_pFeatures != nullptr)
        m_pFeatures->ShowNote(text);
}

ToolLibrary COperationDlg::GetToolLibrary() const
{
    return (m_pTab != nullptr) ? m_pTab->GetToolLibrary() : ToolLibrary();
}

bool COperationDlg::GetFrame(camcore::Frame& frame) const
{
    if (m_haveFrame)
        frame = m_frame;
    return m_haveFrame;
}

void COperationDlg::PostNcDestroy()
{
    delete this;
}

void COperationDlg::OnOK()
{
    CString name;
    GetDlgItemText(IDC_OP_NAME, name);
    name.Trim();

    std::wstring problems;
    if (name.IsEmpty())
        AppendProblem(problems, L"The operation needs a name.");

    // Each tab reads its own controls. The first tab with a problem is shown.
    SequenceStep step = m_step;
    int firstBad = -1;
    for (size_t i = 0; i < m_pages.size(); ++i)
    {
        if (!m_pages[i]->Save(step, problems) && firstBad < 0)
            firstBad = static_cast<int>(i);
    }

    if (!problems.empty())
    {
        if (firstBad >= 0)
        {
            m_tabs.SetCurSel(firstBad);
            ShowPage(firstBad);
        }
        MessageBoxW(problems.c_str(), OperationTitle(m_step.type), MB_OK | MB_ICONWARNING);
        return;
    }

    step.name = ToUtf8(static_cast<LPCWSTR>(name));
    m_step = step;
    Finish(true);
}

void COperationDlg::OnCancel()
{
    Finish(false);
}

void COperationDlg::OnClose()
{
    Finish(false);
}

void COperationDlg::Finish(bool apply)
{
    CRect rect;
    GetWindowRect(&rect);
    g_lastPosition = rect.TopLeft();
    g_lastPage = m_tabs.GetCurSel();

    // Selecting features leaves Alibre's selection full, so it's cleared here.
    StopSelectingFeatures();
    m_locating = false;
    if (m_pTab != nullptr)
    {
        CCamTabCommand* pTab = m_pTab;
        m_pTab = nullptr;
        pTab->OnOperationDialogDone(this, apply);
    }
    DestroyWindow();
}
