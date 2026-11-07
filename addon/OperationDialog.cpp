// OperationDialog.cpp : the modeless dialog that edits one operation.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "OperationDialog.h"
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

    // The tab shown when the dialog last closed.
    int g_lastPage = 0;

    const wchar_t* OperationTitle(StepType type)
    {
        switch (type)
        {
        case StepType::Drill:  return L"Drill";
        case StepType::Facing: return L"Facing";
        case StepType::Profiling: return L"Profiling";
        case StepType::Pocketing: return L"Pocketing";
        case StepType::Chamfer: return L"Chamfer";
        case StepType::Engrave: return L"Engrave";
        case StepType::HolePocketing: return L"Hole Pocketing";
        case StepType::PlungeRoughing: return L"Plunge Roughing";
        case StepType::WaterlineRoughing: return L"Waterline Roughing";
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

namespace
{
    enum ControlKind
    {
        kEditControl = 1,
        kCheckControl,
        kRadioControl,
        kComboControl,
        kListControl
    };

    int KindOf(CWnd* pControl)
    {
        wchar_t name[32] = {};
        ::GetClassNameW(pControl->GetSafeHwnd(), name, _countof(name));
        if (_wcsicmp(name, L"Edit") == 0)
            return kEditControl;
        if (_wcsicmp(name, L"ComboBox") == 0)
            return kComboControl;
        if (_wcsicmp(name, WC_LISTVIEWW) == 0)
            return kListControl;
        if (_wcsicmp(name, L"Button") == 0)
        {
            switch (pControl->GetStyle() & BS_TYPEMASK)
            {
            case BS_CHECKBOX:
            case BS_AUTOCHECKBOX:
            case BS_3STATE:
            case BS_AUTO3STATE:
                return kCheckControl;
            case BS_RADIOBUTTON:
            case BS_AUTORADIOBUTTON:
                return kRadioControl;
            default:
                return 0;
            }
        }
        return 0;
    }
}

COperationPage::ControlState COperationPage::Snapshot() const
{
    ControlState state;
    for (CWnd* pControl = GetWindow(GW_CHILD); pControl != nullptr; pControl = pControl->GetWindow(GW_HWNDNEXT))
    {
        const int id = pControl->GetDlgCtrlID();
        const int kind = KindOf(pControl);
        if (id <= 0 || id == 0xFFFF || kind == 0)
            continue;

        ControlValue value;
        value.kind = kind;
        switch (kind)
        {
        case kEditControl:
        {
            CString text;
            pControl->GetWindowText(text);
            value.text = static_cast<LPCWSTR>(text);
            break;
        }
        case kCheckControl:
        case kRadioControl:
            value.number = static_cast<int>(pControl->SendMessage(BM_GETCHECK));
            break;
        case kComboControl:
            value.number = static_cast<int>(pControl->SendMessage(CB_GETCURSEL));
            break;
        case kListControl:
            value.number = static_cast<int>(pControl->SendMessage(LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
                                                                  LVNI_SELECTED));
            break;
        }
        state[id] = value;
    }
    return state;
}

void COperationPage::ShowMixed(const std::vector<ControlState>& states)
{
    if (states.empty())
        return;
    for (const auto& entry : states.front())
    {
        bool same = true;
        for (const ControlState& other : states)
        {
            const auto found = other.find(entry.first);
            same = same && found != other.end() && found->second == entry.second;
        }
        if (same)
            continue;

        CWnd* pControl = GetDlgItem(entry.first);
        if (pControl == nullptr)
            continue;
        switch (entry.second.kind)
        {
        case kEditControl:
            pControl->SetWindowText(L"");
            break;
        case kCheckControl:
            // Neither on nor off. Clicking it goes through on and off.
            pControl->ModifyStyle(BS_TYPEMASK, BS_AUTO3STATE);
            pControl->SendMessage(BM_SETCHECK, BST_INDETERMINATE);
            break;
        case kRadioControl:
            pControl->SendMessage(BM_SETCHECK, BST_UNCHECKED);
            break;
        case kComboControl:
            pControl->SendMessage(CB_SETCURSEL, static_cast<WPARAM>(-1));
            break;
        case kListControl:
        {
            LVITEMW item = {};
            item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            pControl->SendMessage(LVM_SETITEMSTATE, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&item));
            break;
        }
        }
    }
}

std::set<int> COperationPage::ChangedControls(const ControlState& before, const ControlState& after)
{
    std::set<int> changed;
    for (const auto& entry : after)
    {
        const auto found = before.find(entry.first);
        if (found != before.end() && found->second == entry.second)
            continue;
        changed.insert(entry.first);

        // A radio's choice is its whole group's.
        if (entry.second.kind == kRadioControl)
        {
            CWnd* pFirst = GetDlgItem(entry.first);
            CWnd* pControl = pFirst;
            for (int guard = 0; pControl != nullptr && guard < 64; ++guard)
            {
                const auto member = after.find(pControl->GetDlgCtrlID());
                if (member != after.end() && member->second.kind == kRadioControl)
                    changed.insert(member->first);
                pControl = GetNextDlgGroupItem(pControl, FALSE);
                if (pControl == pFirst)
                    break;
            }
        }
    }
    return changed;
}

void COperationPage::Overlay(const ControlState& state, const std::set<int>& ids)
{
    // Choices first, each with the notice a click or selection sends, so the
    // tab enables its boxes as it would for the user. Then the boxes.
    std::vector<CWnd*> clicked;
    for (int id : ids)
    {
        const auto found = state.find(id);
        CWnd* pControl = GetDlgItem(id);
        if (found == state.end() || pControl == nullptr)
            continue;
        const ControlValue& value = found->second;
        switch (value.kind)
        {
        case kCheckControl:
        case kRadioControl:
            pControl->SendMessage(BM_SETCHECK, static_cast<WPARAM>(value.number));
            if (value.kind == kCheckControl || value.number == BST_CHECKED)
                clicked.push_back(pControl);
            break;
        case kComboControl:
            pControl->SendMessage(CB_SETCURSEL, static_cast<WPARAM>(value.number));
            SendMessage(WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(pControl->GetSafeHwnd()));
            break;
        case kListControl:
            if (value.number >= 0)
            {
                LVITEMW item = {};
                item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                item.state = LVIS_SELECTED | LVIS_FOCUSED;
                pControl->SendMessage(LVM_SETITEMSTATE, static_cast<WPARAM>(value.number),
                                      reinterpret_cast<LPARAM>(&item));
            }
            break;
        default:
            break;
        }
    }
    for (CWnd* pControl : clicked)
        SendMessage(WM_COMMAND, MAKEWPARAM(pControl->GetDlgCtrlID(), BN_CLICKED),
                    reinterpret_cast<LPARAM>(pControl->GetSafeHwnd()));

    for (int id : ids)
    {
        const auto found = state.find(id);
        CWnd* pControl = GetDlgItem(id);
        if (found != state.end() && pControl != nullptr && found->second.kind == kEditControl)
            pControl->SetWindowText(found->second.text.c_str());
    }
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
        case PickKind::CurveEdge:       return L"Curve";
        case PickKind::SurfaceFace:     return L"Surface";
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
    ON_BN_CLICKED(IDC_FEATURES_STOCK_FOOTPRINT, &CFeaturesPage::OnStockFootprint)
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

    // Area operations can take the stock box's footprint too.
    const bool area = m_pOwner->GetFeatureFilter() != FeatureFilter::Points;
    GetDlgItem(IDC_FEATURES_STOCK_FOOTPRINT)->ShowWindow(area ? SW_SHOW : SW_HIDE);
    CheckDlgButton(IDC_FEATURES_STOCK_FOOTPRINT, (area && step.stockFootprint) ? BST_CHECKED : BST_UNCHECKED);
}

bool CFeaturesPage::Save(SequenceStep& step, std::wstring& /*problems*/)
{
    step.features = m_features;
    step.stockFootprint = m_pOwner->GetFeatureFilter() != FeatureFilter::Points &&
                          IsDlgButtonChecked(IDC_FEATURES_STOCK_FOOTPRINT) == BST_CHECKED;
    return true;
}

void CFeaturesPage::OnStockFootprint()
{
    ShowNote(IsDlgButtonChecked(IDC_FEATURES_STOCK_FOOTPRINT) == BST_CHECKED
                 ? L"The stock box's footprint is part of the area too."
                 : L"");
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
                (m_pOwner->GetFeatureFilter() == FeatureFilter::FacingArea      ? L" a flat face or an edge"
                 : m_pOwner->GetFeatureFilter() == FeatureFilter::ProjectedArea ? L" a face or an edge"
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
    std::wstring ToolTypeName(ToolType type)
    {
        return FromUtf8(ToolTypeTitle(type));
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
        m_tools.SetItemText(row, 2, ToolTypeName(tool.type).c_str());
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
        SetDlgItemText(IDC_TOOL_NOTE, L"The machine has no tools. Double-click Tools on the CAM tab to add tools.");
    else if (!m_toolId.empty() && chosen < 0)
        SetDlgItemText(IDC_TOOL_NOTE, L"This operation's tool isn't on the machine. Choose another.");
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

std::string CToolPage::ChosenToolId() const
{
    const Tool* pTool = ChosenTool();
    return (pTool != nullptr) ? pTool->id : m_toolId;
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
    const ToolFields fields = FieldsOf(pTool->type);
    const std::wstring degrees = L"\u00B0";
    if (fields.pointAngle)
        add(L"Point angle", FormatNumber(pTool->pointAngle) + degrees);
    if (fields.fluteCount)
        add(L"Flute count", std::to_wstring(pTool->fluteCount));
    if (fields.cornerRadius)
        add(L"Corner radius", Length(pTool->cornerRadius, pTool->unit));
    if (fields.includedAngle)
        add(L"Included angle", FormatNumber(pTool->includedAngle) + degrees);
    if (fields.tipDiameter)
        add(L"Tip diameter", Length(pTool->tipDiameter, pTool->unit));
    if (fields.taperAngle)
        add(L"Taper angle", FormatNumber(pTool->taperAngle) + degrees);
    if (fields.pitch)
        add(L"Pitch", Length(pTool->pitch, pTool->unit));
    if (fields.threadAngle)
        add(L"Thread angle", FormatNumber(pTool->threadAngle) + degrees);
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
    ON_BN_CLICKED(IDC_FEEDS_FROM_TOOL, &CFeedsPage::OnFromTool)
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

BOOL CFeedsPage::OnInitDialog()
{
    COperationPage::OnInitDialog();

    // From Tool belongs to an operation. Editing a tool's own feeds hosts this page without one.
    if (m_pOwner == nullptr)
        GetDlgItem(IDC_FEEDS_FROM_TOOL)->ShowWindow(SW_HIDE);
    return TRUE;
}

void CFeedsPage::OnFromTool()
{
    if (m_pOwner == nullptr)
        return;

    FeedSettings feeds;
    std::wstring problem;
    if (!m_pOwner->FeedsFromTool(feeds, problem))
    {
        MessageBox(problem.c_str(), L"AlibreCAM", MB_OK | MB_ICONINFORMATION);
        return;
    }
    SequenceStep step;
    step.feeds = feeds;
    Load(step);
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
// CCutLevelsPage

namespace
{
    // The step sliders run from 1% to 100% of their depth.
    const int kStepSliderMin = 1;
    const int kSliderMax = 100;
}

BEGIN_MESSAGE_MAP(CCutLevelsPage, COperationPage)
    ON_BN_CLICKED(IDC_CUT_AT_TOP, &CCutLevelsPage::OnLocationChanged)
    ON_BN_CLICKED(IDC_CUT_AT_BOTTOM, &CCutLevelsPage::OnLocationChanged)
    ON_BN_CLICKED(IDC_CUT_PICK_TOP, &CCutLevelsPage::OnLocationChanged)
    ON_BN_CLICKED(IDC_CUT_PICK_TOP_BUTTON, &CCutLevelsPage::OnPickTop)
    ON_BN_CLICKED(IDC_CUT_PICK_DEPTH, &CCutLevelsPage::OnPickDepth)
    ON_EN_KILLFOCUS(IDC_CUT_TOTAL, &CCutLevelsPage::OnTotalChanged)
    ON_EN_KILLFOCUS(IDC_CUT_ROUGH, &CCutLevelsPage::OnRoughChanged)
    ON_EN_KILLFOCUS(IDC_CUT_FINISH, &CCutLevelsPage::OnFinishChanged)
    ON_EN_KILLFOCUS(IDC_CUT_ROUGH_STEP, &CCutLevelsPage::OnStepChanged)
    ON_EN_KILLFOCUS(IDC_CUT_FINISH_STEP, &CCutLevelsPage::OnStepChanged)
    ON_WM_HSCROLL()
END_MESSAGE_MAP()

CCutLevelsPage::CCutLevelsPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_CUT_LEVELS, pOwner)
    , m_updating(false)
{
}

BOOL CCutLevelsPage::OnInitDialog()
{
    COperationPage::OnInitDialog();

    m_ratio.SubclassDlgItem(IDC_CUT_RATIO, this);
    m_ratio.SetRange(0, kSliderMax);
    m_roughSlider.SubclassDlgItem(IDC_CUT_ROUGH_STEP_SLIDER, this);
    m_roughSlider.SetRange(kStepSliderMin, kSliderMax);
    m_finishSlider.SubclassDlgItem(IDC_CUT_FINISH_STEP_SLIDER, this);
    m_finishSlider.SetRange(kStepSliderMin, kSliderMax);
    return TRUE;
}

void CCutLevelsPage::Load(const SequenceStep& step)
{
    m_levels = step.cutLevels;

    const int location = (m_levels.location == CutGeometryLocation::AtBottom) ? IDC_CUT_AT_BOTTOM
                         : (m_levels.location == CutGeometryLocation::PickTop) ? IDC_CUT_PICK_TOP
                                                                                 : IDC_CUT_AT_TOP;
    CheckRadioButton(IDC_CUT_AT_TOP, IDC_CUT_PICK_TOP, location);
    SetNumber(IDC_CUT_TOP_Z, m_levels.topZ);

    CheckDlgButton(IDC_CUT_CLEAR_ISLANDS, m_levels.clearIslandTops ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CUT_USE_MODEL, m_levels.useModelForDepth ? BST_CHECKED : BST_UNCHECKED);
    CheckRadioButton(IDC_CUT_DEPTH_FIRST, IDC_CUT_LEVEL_FIRST,
                     m_levels.order == CutLevelOrder::LevelFirst ? IDC_CUT_LEVEL_FIRST : IDC_CUT_DEPTH_FIRST);

    ShowDepths();
    OnLocationChanged();
    ShowLocating(LocateTarget::CutTop, false);
}

bool CCutLevelsPage::Save(SequenceStep& step, std::wstring& problems)
{
    // Take in anything typed but not yet left.
    OnTotalChanged();
    OnRoughChanged();
    OnStepChanged();

    CutLevelSettings levels = m_levels;
    std::wstring found;

    switch (GetCheckedRadioButton(IDC_CUT_AT_TOP, IDC_CUT_PICK_TOP))
    {
    case IDC_CUT_AT_BOTTOM: levels.location = CutGeometryLocation::AtBottom; break;
    case IDC_CUT_PICK_TOP:  levels.location = CutGeometryLocation::PickTop; break;
    default:                levels.location = CutGeometryLocation::AtTop; break;
    }
    if (levels.location == CutGeometryLocation::PickTop)
        ReadNumber(IDC_CUT_TOP_Z, L"The top Z", levels.topZ, found);

    levels.clearIslandTops = IsDlgButtonChecked(IDC_CUT_CLEAR_ISLANDS) == BST_CHECKED;
    levels.useModelForDepth = IsDlgButtonChecked(IDC_CUT_USE_MODEL) == BST_CHECKED;
    levels.order = (GetCheckedRadioButton(IDC_CUT_DEPTH_FIRST, IDC_CUT_LEVEL_FIRST) == IDC_CUT_LEVEL_FIRST)
                       ? CutLevelOrder::LevelFirst
                       : CutLevelOrder::DepthFirst;

    if (found.empty())
    {
        for (const std::string& problem : ValidateCutLevels(levels))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.cutLevels = levels;
    return true;
}

bool COperationPage::ReadValue(int id, double& value)
{
    CString text;
    GetDlgItemText(id, text);
    text.Trim();

    wchar_t* pEnd = nullptr;
    const double parsed = wcstod(text, &pEnd);
    if (text.IsEmpty() || pEnd == nullptr || *pEnd != L'\0')
        return false;
    value = parsed;
    return true;
}

int CCutLevelsPage::StepPosition(double step, double depth)
{
    if (!(depth > 0.0))
        return kSliderMax;
    const int position = static_cast<int>(std::lround(step / depth * kSliderMax));
    return (std::max)(kStepSliderMin, (std::min)(kSliderMax, position));
}

void CCutLevelsPage::ShowDepths()
{
    // Puts every depth box and slider in step with m_levels.
    m_updating = true;
    SetNumber(IDC_CUT_TOTAL, m_levels.totalDepth);
    SetNumber(IDC_CUT_ROUGH, m_levels.roughDepth);
    SetNumber(IDC_CUT_FINISH, m_levels.finishDepth);
    SetNumber(IDC_CUT_ROUGH_STEP, m_levels.roughStep);
    SetNumber(IDC_CUT_FINISH_STEP, m_levels.finishStep);

    const int ratio = (m_levels.totalDepth > 0.0)
                          ? static_cast<int>(std::lround(m_levels.roughDepth / m_levels.totalDepth * kSliderMax))
                          : kSliderMax;
    m_ratio.SetPos((std::max)(0, (std::min)(kSliderMax, ratio)));
    m_roughSlider.SetPos(StepPosition(m_levels.roughStep, m_levels.roughDepth));
    m_finishSlider.SetPos(StepPosition(m_levels.finishStep, m_levels.finishDepth));

    // A depth of 0 has no passes to size.
    const BOOL rough = m_levels.roughDepth > 0.0;
    const BOOL finish = m_levels.finishDepth > 0.0;
    GetDlgItem(IDC_CUT_ROUGH_STEP)->EnableWindow(rough);
    m_roughSlider.EnableWindow(rough);
    GetDlgItem(IDC_CUT_FINISH_STEP)->EnableWindow(finish);
    m_finishSlider.EnableWindow(finish);
    m_updating = false;
}

void CCutLevelsPage::FitSteps()
{
    // A pass can't take more than its depth.
    if (m_levels.roughStep > m_levels.roughDepth || !(m_levels.roughStep > 0.0))
        m_levels.roughStep = m_levels.roughDepth;
    if (m_levels.finishStep > m_levels.finishDepth || !(m_levels.finishStep > 0.0))
        m_levels.finishStep = m_levels.finishDepth;
}

void CCutLevelsPage::OnTotalChanged()
{
    double total = 0.0;
    if (m_updating || !ReadValue(IDC_CUT_TOTAL, total) || !(total >= 0.0) || total == m_levels.totalDepth)
        return;

    // The finish depth stays if it fits, and the rough depth takes the rest.
    m_levels.totalDepth = total;
    m_levels.finishDepth = (std::min)(m_levels.finishDepth, total);
    m_levels.roughDepth = total - m_levels.finishDepth;
    FitSteps();
    ShowDepths();
}

void CCutLevelsPage::OnRoughChanged()
{
    double rough = 0.0;
    if (m_updating || !ReadValue(IDC_CUT_ROUGH, rough) || rough == m_levels.roughDepth)
        return;

    m_levels.roughDepth = (std::max)(0.0, (std::min)(rough, m_levels.totalDepth));
    m_levels.finishDepth = m_levels.totalDepth - m_levels.roughDepth;
    FitSteps();
    ShowDepths();
}

void CCutLevelsPage::OnFinishChanged()
{
    double finish = 0.0;
    if (m_updating || !ReadValue(IDC_CUT_FINISH, finish) || finish == m_levels.finishDepth)
        return;

    m_levels.finishDepth = (std::max)(0.0, (std::min)(finish, m_levels.totalDepth));
    m_levels.roughDepth = m_levels.totalDepth - m_levels.finishDepth;
    FitSteps();
    ShowDepths();
}

void CCutLevelsPage::OnStepChanged()
{
    if (m_updating)
        return;

    double step = 0.0;
    if (ReadValue(IDC_CUT_ROUGH_STEP, step) && step > 0.0)
        m_levels.roughStep = (std::min)(step, m_levels.roughDepth);
    if (ReadValue(IDC_CUT_FINISH_STEP, step) && step > 0.0)
        m_levels.finishStep = (std::min)(step, m_levels.finishDepth);
    ShowDepths();
}

void CCutLevelsPage::OnHScroll(UINT code, UINT position, CScrollBar* pScrollBar)
{
    COperationPage::OnHScroll(code, position, pScrollBar);
    if (m_updating || pScrollBar == nullptr)
        return;

    const HWND hSlider = pScrollBar->GetSafeHwnd();
    if (hSlider == m_ratio.GetSafeHwnd())
    {
        // The ratio slider shares the total between rough and finish.
        m_levels.roughDepth = m_levels.totalDepth * m_ratio.GetPos() / kSliderMax;
        m_levels.finishDepth = m_levels.totalDepth - m_levels.roughDepth;
        FitSteps();
    }
    else if (hSlider == m_roughSlider.GetSafeHwnd())
    {
        m_levels.roughStep = m_levels.roughDepth * m_roughSlider.GetPos() / kSliderMax;
    }
    else if (hSlider == m_finishSlider.GetSafeHwnd())
    {
        m_levels.finishStep = m_levels.finishDepth * m_finishSlider.GetPos() / kSliderMax;
    }
    else
    {
        return;
    }
    ShowDepths();
}

void CCutLevelsPage::OnLocationChanged()
{
    const BOOL pickTop = GetCheckedRadioButton(IDC_CUT_AT_TOP, IDC_CUT_PICK_TOP) == IDC_CUT_PICK_TOP;
    GetDlgItem(IDC_CUT_TOP_Z)->EnableWindow(pickTop);
    GetDlgItem(IDC_CUT_PICK_TOP_BUTTON)->EnableWindow(pickTop);
}

void CCutLevelsPage::OnPickTop()
{
    if (m_pOwner->IsLocating())
        m_pOwner->StopLocating();
    else
        m_pOwner->StartLocating(LocateTarget::CutTop);
}

void CCutLevelsPage::OnPickDepth()
{
    if (m_pOwner->IsLocating())
        m_pOwner->StopLocating();
    else
        m_pOwner->StartLocating(LocateTarget::CutDepth);
}

void CCutLevelsPage::ShowLocating(LocateTarget target, bool locating)
{
    SetDlgItemText(IDC_CUT_PICK_TOP_BUTTON, (locating && target == LocateTarget::CutTop) ? L"Stop" : L"Pick");
    SetDlgItemText(IDC_CUT_PICK_DEPTH, (locating && target == LocateTarget::CutDepth) ? L"Stop" : L"Pick");
}

double CCutLevelsPage::ReferenceZ()
{
    // Pick Top gives the level. Otherwise it's the highest of the operation's features.
    double z = 0.0;
    if (GetCheckedRadioButton(IDC_CUT_AT_TOP, IDC_CUT_PICK_TOP) == IDC_CUT_PICK_TOP)
        return ReadValue(IDC_CUT_TOP_Z, z) ? z : m_levels.topZ;

    camcore::Frame frame;
    if (!m_pOwner->GetFrame(frame))
        return 0.0;

    bool first = true;
    for (const PickedGeometry& feature : m_pOwner->GetFeatures())
    {
        const double height = WorldToFrame(frame, FeaturePoint(frame, feature)).z;
        z = first ? height : (std::max)(z, height);
        first = false;
    }
    return z;
}

void CCutLevelsPage::SetTopZ(double z)
{
    m_levels.topZ = z;
    SetNumber(IDC_CUT_TOP_Z, z);
    CheckRadioButton(IDC_CUT_AT_TOP, IDC_CUT_PICK_TOP, IDC_CUT_PICK_TOP);
    OnLocationChanged();
}

void CCutLevelsPage::SetDepthToHeight(double z)
{
    const double total = std::fabs(ReferenceZ() - z);
    if (!(total > 0.0))
    {
        MessageBoxW(L"That item is at the cut geometry's level, so it gives no depth.", L"Cut Levels",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    m_levels.totalDepth = total;
    m_levels.finishDepth = (std::min)(m_levels.finishDepth, total);
    m_levels.roughDepth = total - m_levels.finishDepth;
    FitSteps();
    ShowDepths();
}

/////////////////////////////////////////////////////////////////////////////
// CEntryExitPage

const CEntryExitPage::LeadControls CEntryExitPage::kEngage =
{
    IDC_EE_ENGAGE_GROUP, IDC_EE_ENGAGE_LINEAR, IDC_EE_ENGAGE_RADIAL, IDC_EE_ENGAGE_LENGTH_LABEL,
    IDC_EE_ENGAGE_LENGTH, IDC_EE_ENGAGE_ANGLE_LABEL, IDC_EE_ENGAGE_ANGLE, IDC_EE_ENGAGE_RADIUS_LABEL,
    IDC_EE_ENGAGE_RADIUS
};

const CEntryExitPage::LeadControls CEntryExitPage::kRetract =
{
    IDC_EE_RETRACT_GROUP, IDC_EE_RETRACT_LINEAR, IDC_EE_RETRACT_RADIAL, IDC_EE_RETRACT_LENGTH_LABEL,
    IDC_EE_RETRACT_LENGTH, IDC_EE_RETRACT_ANGLE_LABEL, IDC_EE_RETRACT_ANGLE, IDC_EE_RETRACT_RADIUS_LABEL,
    IDC_EE_RETRACT_RADIUS
};

BEGIN_MESSAGE_MAP(CEntryExitPage, COperationPage)
    ON_BN_CLICKED(IDC_EE_ENTRY_NONE, &CEntryExitPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_EE_ENTRY_2D, &CEntryExitPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_EE_ENTRY_3D, &CEntryExitPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_EE_ENGAGE_LINEAR, &CEntryExitPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_EE_ENGAGE_RADIAL, &CEntryExitPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_EE_EXIT_NONE, &CEntryExitPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_EE_EXIT_2D, &CEntryExitPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_EE_RETRACT_LINEAR, &CEntryExitPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_EE_RETRACT_RADIAL, &CEntryExitPage::OnChoiceChanged)
END_MESSAGE_MAP()

CEntryExitPage::CEntryExitPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_ENTRY_EXIT, pOwner)
{
}

void CEntryExitPage::LoadLead(const LeadControls& ids, const LeadMotion& lead)
{
    CheckRadioButton(ids.linear, ids.radial, lead.shape == LeadShape::Radial ? ids.radial : ids.linear);
    SetNumber(ids.length, lead.length);
    SetNumber(ids.angle, lead.angle);
    SetNumber(ids.radius, lead.radius);
}

void CEntryExitPage::Load(const SequenceStep& step)
{
    const EntryExitSettings& settings = step.entryExit;

    const int entry = (settings.entry == EntryMode::Entry2D)       ? IDC_EE_ENTRY_2D
                      : (settings.entry == EntryMode::AlongPath3D) ? IDC_EE_ENTRY_3D
                                                                   : IDC_EE_ENTRY_NONE;
    CheckRadioButton(IDC_EE_ENTRY_NONE, IDC_EE_ENTRY_3D, entry);
    LoadLead(kEngage, settings.engage);
    SetNumber(IDC_EE_PATH_ANGLE, settings.alongPathAngle);
    SetNumber(IDC_EE_PATH_HEIGHT, settings.alongPathHeight);

    CheckRadioButton(IDC_EE_EXIT_NONE, IDC_EE_EXIT_2D,
                     settings.exit == ExitMode::Exit2D ? IDC_EE_EXIT_2D : IDC_EE_EXIT_NONE);
    LoadLead(kRetract, settings.retract);

    CheckDlgButton(IDC_EE_EACH_LEVEL, settings.eachCutLevel ? BST_CHECKED : BST_UNCHECKED);
    UpdateFields();
}

void CEntryExitPage::SaveLead(const LeadControls& ids, const wchar_t* name, LeadMotion& lead, std::wstring& found)
{
    // Only the boxes for the chosen shape are read.
    const std::wstring label(name);
    if (IsDlgButtonChecked(ids.radial) == BST_CHECKED)
    {
        lead.shape = LeadShape::Radial;
        ReadNumber(ids.radius, (L"The " + label + L" radius").c_str(), lead.radius, found);
    }
    else
    {
        lead.shape = LeadShape::Linear;
        ReadNumber(ids.length, (L"The " + label + L" length").c_str(), lead.length, found);
        ReadNumber(ids.angle, (L"The " + label + L" angle").c_str(), lead.angle, found);
    }
}

bool CEntryExitPage::Save(SequenceStep& step, std::wstring& problems)
{
    EntryExitSettings settings = step.entryExit;
    std::wstring found;

    switch (GetCheckedRadioButton(IDC_EE_ENTRY_NONE, IDC_EE_ENTRY_3D))
    {
    case IDC_EE_ENTRY_2D:
        settings.entry = EntryMode::Entry2D;
        SaveLead(kEngage, L"engage", settings.engage, found);
        break;
    case IDC_EE_ENTRY_3D:
        settings.entry = EntryMode::AlongPath3D;
        ReadNumber(IDC_EE_PATH_ANGLE, L"The along path angle", settings.alongPathAngle, found);
        ReadNumber(IDC_EE_PATH_HEIGHT, L"The along path height", settings.alongPathHeight, found);
        break;
    default:
        settings.entry = EntryMode::None;
        break;
    }

    settings.exit = (IsDlgButtonChecked(IDC_EE_EXIT_2D) == BST_CHECKED) ? ExitMode::Exit2D : ExitMode::None;
    if (settings.exit == ExitMode::Exit2D)
        SaveLead(kRetract, L"retract", settings.retract, found);

    settings.eachCutLevel = IsDlgButtonChecked(IDC_EE_EACH_LEVEL) == BST_CHECKED;

    if (found.empty())
    {
        for (const std::string& problem : ValidateEntryExit(settings))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.entryExit = settings;
    return true;
}

void CEntryExitPage::EnableLead(const LeadControls& ids, bool enabled)
{
    // Within an enabled group, only the chosen shape's boxes are.
    const bool radial = IsDlgButtonChecked(ids.radial) == BST_CHECKED;
    const BOOL linearOn = enabled && !radial;
    const BOOL radialOn = enabled && radial;

    GetDlgItem(ids.group)->EnableWindow(enabled);
    GetDlgItem(ids.linear)->EnableWindow(enabled);
    GetDlgItem(ids.radial)->EnableWindow(enabled);
    GetDlgItem(ids.lengthLabel)->EnableWindow(linearOn);
    GetDlgItem(ids.length)->EnableWindow(linearOn);
    GetDlgItem(ids.angleLabel)->EnableWindow(linearOn);
    GetDlgItem(ids.angle)->EnableWindow(linearOn);
    GetDlgItem(ids.radiusLabel)->EnableWindow(radialOn);
    GetDlgItem(ids.radius)->EnableWindow(radialOn);
}

void CEntryExitPage::UpdateFields()
{
    const int entry = GetCheckedRadioButton(IDC_EE_ENTRY_NONE, IDC_EE_ENTRY_3D);
    EnableLead(kEngage, entry == IDC_EE_ENTRY_2D);

    const BOOL path = (entry == IDC_EE_ENTRY_3D);
    for (int id : { IDC_EE_PATH_GROUP, IDC_EE_PATH_ANGLE_LABEL, IDC_EE_PATH_ANGLE, IDC_EE_PATH_HEIGHT_LABEL,
                    IDC_EE_PATH_HEIGHT })
        GetDlgItem(id)->EnableWindow(path);

    EnableLead(kRetract, IsDlgButtonChecked(IDC_EE_EXIT_2D) == BST_CHECKED);
}

void CEntryExitPage::OnChoiceChanged()
{
    UpdateFields();
}

/////////////////////////////////////////////////////////////////////////////
// CFacingPage

const CFacingPage::SizeControls CFacingPage::kStep =
{
    IDC_FACING_STEP_PERCENT_RADIO, IDC_FACING_STEP_PERCENT, IDC_FACING_STEP_DISTANCE_RADIO, IDC_FACING_STEP_DISTANCE
};

const CFacingPage::SizeControls CFacingPage::kOvertravel =
{
    IDC_FACING_OVER_PERCENT_RADIO, IDC_FACING_OVER_PERCENT, IDC_FACING_OVER_DISTANCE_RADIO, IDC_FACING_OVER_DISTANCE
};

BEGIN_MESSAGE_MAP(CFacingPage, COperationPage)
    ON_BN_CLICKED(IDC_FACING_STEP_PERCENT_RADIO, &CFacingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_FACING_STEP_DISTANCE_RADIO, &CFacingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_FACING_OVER_PERCENT_RADIO, &CFacingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_FACING_OVER_DISTANCE_RADIO, &CFacingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_FACING_CLIMB, &CFacingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_FACING_CONVENTIONAL, &CFacingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_FACING_MIXED, &CFacingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_FACING_LINEAR, &CFacingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_FACING_SPIRAL, &CFacingPage::OnChoiceChanged)
END_MESSAGE_MAP()

CFacingPage::CFacingPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_FACING, pOwner)
{
}

void CFacingPage::LoadSize(const SizeControls& ids, const ToolRelativeSize& size)
{
    CheckRadioButton(ids.percentRadio, ids.distanceRadio, size.usePercent ? ids.percentRadio : ids.distanceRadio);
    SetNumber(ids.percent, size.percent);
    SetNumber(ids.distance, size.distance);
}

void CFacingPage::Load(const SequenceStep& step)
{
    const FacingSettings& facing = step.facing;
    const int direction = (facing.direction == CutDirection::Conventional) ? IDC_FACING_CONVENTIONAL
                          : (facing.direction == CutDirection::Mixed)      ? IDC_FACING_MIXED
                                                                           : IDC_FACING_CLIMB;
    CheckRadioButton(IDC_FACING_CLIMB, IDC_FACING_MIXED, direction);
    LoadSize(kStep, facing.stepDistance);
    LoadSize(kOvertravel, facing.overtravel);

    CheckRadioButton(IDC_FACING_LINEAR, IDC_FACING_SPIRAL,
                     facing.cutType == FacingCutType::Spiral ? IDC_FACING_SPIRAL : IDC_FACING_LINEAR);
    SetNumber(IDC_FACING_CUT_ANGLE, facing.cutAngle);
    const int start = (facing.start == StartCorner::TopRight)      ? IDC_FACING_TOP_RIGHT
                      : (facing.start == StartCorner::BottomLeft)  ? IDC_FACING_BOTTOM_LEFT
                      : (facing.start == StartCorner::BottomRight) ? IDC_FACING_BOTTOM_RIGHT
                                                                   : IDC_FACING_TOP_LEFT;
    CheckRadioButton(IDC_FACING_TOP_LEFT, IDC_FACING_BOTTOM_RIGHT, start);
    CheckDlgButton(IDC_FACING_ARC_STEPOVER, facing.arcStepOver ? BST_CHECKED : BST_UNCHECKED);
    UpdateFields();
}

void CFacingPage::SaveSize(const SizeControls& ids, const wchar_t* name, ToolRelativeSize& size, std::wstring& found)
{
    // Only the chosen box is read.
    const std::wstring label(name);
    size.usePercent = IsDlgButtonChecked(ids.distanceRadio) != BST_CHECKED;
    if (size.usePercent)
        ReadNumber(ids.percent, (L"The " + label + L" percentage").c_str(), size.percent, found);
    else
        ReadNumber(ids.distance, (L"The " + label + L" distance").c_str(), size.distance, found);
}

bool CFacingPage::Save(SequenceStep& step, std::wstring& problems)
{
    FacingSettings facing = step.facing;
    std::wstring found;

    switch (GetCheckedRadioButton(IDC_FACING_CLIMB, IDC_FACING_MIXED))
    {
    case IDC_FACING_CONVENTIONAL: facing.direction = CutDirection::Conventional; break;
    case IDC_FACING_MIXED:        facing.direction = CutDirection::Mixed; break;
    default:                      facing.direction = CutDirection::Climb; break;
    }
    SaveSize(kStep, L"step", facing.stepDistance, found);
    SaveSize(kOvertravel, L"overtravel", facing.overtravel, found);

    facing.cutType = (IsDlgButtonChecked(IDC_FACING_SPIRAL) == BST_CHECKED) ? FacingCutType::Spiral
                                                                            : FacingCutType::Linear;
    ReadNumber(IDC_FACING_CUT_ANGLE, L"The cut angle", facing.cutAngle, found);
    facing.arcStepOver = IsDlgButtonChecked(IDC_FACING_ARC_STEPOVER) == BST_CHECKED;     // kept when greyed
    switch (GetCheckedRadioButton(IDC_FACING_TOP_LEFT, IDC_FACING_BOTTOM_RIGHT))
    {
    case IDC_FACING_TOP_RIGHT:    facing.start = StartCorner::TopRight; break;
    case IDC_FACING_BOTTOM_LEFT:  facing.start = StartCorner::BottomLeft; break;
    case IDC_FACING_BOTTOM_RIGHT: facing.start = StartCorner::BottomRight; break;
    default:                      facing.start = StartCorner::TopLeft; break;
    }

    if (found.empty())
    {
        for (const std::string& problem : ValidateFacing(facing))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.facing = facing;
    return true;
}

void CFacingPage::UpdateFields()
{
    for (const SizeControls* pIds : { &kStep, &kOvertravel })
    {
        const bool percent = IsDlgButtonChecked(pIds->distanceRadio) != BST_CHECKED;
        GetDlgItem(pIds->percent)->EnableWindow(percent);
        GetDlgItem(pIds->distance)->EnableWindow(!percent);
    }

    // Arc step-overs only mean something going backwards and forwards.
    const bool zigzag = IsDlgButtonChecked(IDC_FACING_MIXED) == BST_CHECKED &&
                        IsDlgButtonChecked(IDC_FACING_SPIRAL) != BST_CHECKED;
    GetDlgItem(IDC_FACING_ARC_STEPOVER)->EnableWindow(zigzag);
}

void CFacingPage::OnChoiceChanged()
{
    UpdateFields();
}

/////////////////////////////////////////////////////////////////////////////
// CProfilingPage

namespace
{
    // The Step/Cut slider runs from 1% to 100% of the total cut width.
    const int kWidthSliderMin = 1;
    const int kWidthSliderMax = 100;
}

BEGIN_MESSAGE_MAP(CProfilingPage, COperationPage)
    ON_BN_CLICKED(IDC_PROF_USE_CLOSED, &CProfilingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_PROF_USE_TABS, &CProfilingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_PROF_TAB_COUNT_RADIO, &CProfilingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_PROF_TAB_DISTANCE_RADIO, &CProfilingPage::OnChoiceChanged)
    ON_EN_KILLFOCUS(IDC_PROF_CUT_WIDTH, &CProfilingPage::OnWidthChanged)
    ON_EN_KILLFOCUS(IDC_PROF_STEP, &CProfilingPage::OnStepChanged)
    ON_WM_HSCROLL()
END_MESSAGE_MAP()

CProfilingPage::CProfilingPage(COperationDlg* pOwner)
    : CProfilingPage(IDD_OP_PROFILING, pOwner)
{
}

CProfilingPage::CProfilingPage(UINT templateId, COperationDlg* pOwner)
    : COperationPage(templateId, pOwner)
    , m_cutWidth(0.0)
    , m_stepPerCut(0.0)
    , m_updating(false)
{
}

/////////////////////////////////////////////////////////////////////////////
// CEngravingPage

CEngravingPage::CEngravingPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_ENGRAVING, pOwner)
{
}

void CEngravingPage::Load(const SequenceStep& step)
{
    CheckRadioButton(IDC_ENGRAVE_RETURN, IDC_ENGRAVE_STAY_DOWN,
                     step.engraving.traversal == EngraveTraversal::StayDown ? IDC_ENGRAVE_STAY_DOWN
                                                                             : IDC_ENGRAVE_RETURN);
}

bool CEngravingPage::Save(SequenceStep& step, std::wstring& /*problems*/)
{
    // In a bulk edit neither may be chosen, which keeps the operation's own.
    if (IsDlgButtonChecked(IDC_ENGRAVE_STAY_DOWN) == BST_CHECKED)
        step.engraving.traversal = EngraveTraversal::StayDown;
    else if (IsDlgButtonChecked(IDC_ENGRAVE_RETURN) == BST_CHECKED)
        step.engraving.traversal = EngraveTraversal::ReturnToStart;
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// CChamferingPage

CChamferingPage::CChamferingPage(COperationDlg* pOwner)
    : CProfilingPage(IDD_OP_CHAMFERING, pOwner)
{
}

void CChamferingPage::Load(const SequenceStep& step)
{
    CProfilingPage::Load(step);
    SetNumber(IDC_CHAMFER_WIDTH, step.chamfer.width);
    SetNumber(IDC_CHAMFER_TIP_CLEARANCE, step.chamfer.tipClearance);
    SetNumber(IDC_CHAMFER_MAX_DEPTH, step.chamfer.maxDepthPerCut);
}

bool CChamferingPage::Save(SequenceStep& step, std::wstring& problems)
{
    // The chamfer's own values, then Profiling's.
    ChamferSettings chamfer = step.chamfer;
    std::wstring found;
    ReadNumber(IDC_CHAMFER_WIDTH, L"The chamfer width", chamfer.width, found);
    ReadNumber(IDC_CHAMFER_TIP_CLEARANCE, L"The tool tip clearance", chamfer.tipClearance, found);
    ReadNumber(IDC_CHAMFER_MAX_DEPTH, L"The maximum depth per cut", chamfer.maxDepthPerCut, found);
    if (found.empty())
    {
        for (const std::string& problem : ValidateChamfer(chamfer))
            AppendProblem(found, FromUtf8(problem));
    }

    const bool profilingOk = CProfilingPage::Save(step, found);
    if (!found.empty() || !profilingOk)
    {
        AppendProblem(problems, found);
        return false;
    }
    step.chamfer = chamfer;
    return true;
}

BOOL CProfilingPage::OnInitDialog()
{
    COperationPage::OnInitDialog();
    m_stepSlider.SubclassDlgItem(IDC_PROF_STEP_SLIDER, this);
    m_stepSlider.SetRange(kWidthSliderMin, kWidthSliderMax);
    return TRUE;
}

void CProfilingPage::Load(const SequenceStep& step)
{
    const ProfilingSettings& profiling = step.profiling;
    SetNumber(IDC_PROF_STOCK, profiling.stockToLeave);

    const int direction = (profiling.direction == CutDirection::Climb)          ? IDC_PROF_CLIMB
                          : (profiling.direction == CutDirection::Conventional) ? IDC_PROF_CONVENTIONAL
                                                                                : IDC_PROF_MIXED;
    CheckRadioButton(IDC_PROF_CLIMB, IDC_PROF_MIXED, direction);
    CheckRadioButton(IDC_PROF_RIGHT, IDC_PROF_LEFT,
                     profiling.startSide == ProfileSide::Left ? IDC_PROF_LEFT : IDC_PROF_RIGHT);
    CheckDlgButton(IDC_PROF_USE_CLOSED, profiling.useClosedSide ? BST_CHECKED : BST_UNCHECKED);
    CheckRadioButton(IDC_PROF_OUTSIDE, IDC_PROF_INSIDE,
                     profiling.closedSide == ClosedProfileSide::Inside ? IDC_PROF_INSIDE : IDC_PROF_OUTSIDE);
    CheckDlgButton(IDC_PROF_USE_MODEL, profiling.useModelForSide ? BST_CHECKED : BST_UNCHECKED);

    m_cutWidth = profiling.cutWidth;
    m_stepPerCut = profiling.stepPerCut;
    ShowStepover();

    CheckDlgButton(IDC_PROF_USE_TABS, profiling.useTabs ? BST_CHECKED : BST_UNCHECKED);
    SetNumber(IDC_PROF_TAB_HEIGHT, profiling.tabHeight);
    SetNumber(IDC_PROF_TAB_LENGTH, profiling.tabLength);
    CheckRadioButton(IDC_PROF_TAB_COUNT_RADIO, IDC_PROF_TAB_DISTANCE_RADIO,
                     profiling.tabSpacing == TabSpacing::Distance ? IDC_PROF_TAB_DISTANCE_RADIO
                                                                  : IDC_PROF_TAB_COUNT_RADIO);
    SetNumber(IDC_PROF_TAB_COUNT, profiling.tabCount);
    SetNumber(IDC_PROF_TAB_DISTANCE, profiling.tabDistance);
    UpdateFields();
}

bool CProfilingPage::Save(SequenceStep& step, std::wstring& problems)
{
    // Take in anything typed but not yet left.
    OnWidthChanged();
    OnStepChanged();

    ProfilingSettings profiling = step.profiling;
    std::wstring found;

    ReadNumber(IDC_PROF_STOCK, L"The stock to leave", profiling.stockToLeave, found);
    switch (GetCheckedRadioButton(IDC_PROF_CLIMB, IDC_PROF_MIXED))
    {
    case IDC_PROF_CLIMB:        profiling.direction = CutDirection::Climb; break;
    case IDC_PROF_CONVENTIONAL: profiling.direction = CutDirection::Conventional; break;
    default:                    profiling.direction = CutDirection::Mixed; break;
    }
    profiling.startSide = (IsDlgButtonChecked(IDC_PROF_LEFT) == BST_CHECKED) ? ProfileSide::Left : ProfileSide::Right;
    profiling.useClosedSide = IsDlgButtonChecked(IDC_PROF_USE_CLOSED) == BST_CHECKED;
    profiling.closedSide = (IsDlgButtonChecked(IDC_PROF_INSIDE) == BST_CHECKED) ? ClosedProfileSide::Inside
                                                                               : ClosedProfileSide::Outside;
    profiling.useModelForSide = IsDlgButtonChecked(IDC_PROF_USE_MODEL) == BST_CHECKED;

    // The boxes must hold numbers even when they've been checked as the user typed.
    double typed = 0.0;
    if (ReadNumber(IDC_PROF_CUT_WIDTH, L"The total cut width", typed, found) &&
        ReadNumber(IDC_PROF_STEP, L"The step/cut", typed, found))
    {
        profiling.cutWidth = m_cutWidth;
        profiling.stepPerCut = m_stepPerCut;
    }

    // Tab values are read only when tabs are used, and only the chosen spacing.
    profiling.useTabs = IsDlgButtonChecked(IDC_PROF_USE_TABS) == BST_CHECKED;
    profiling.tabSpacing = (IsDlgButtonChecked(IDC_PROF_TAB_DISTANCE_RADIO) == BST_CHECKED) ? TabSpacing::Distance
                                                                                            : TabSpacing::Count;
    if (profiling.useTabs)
    {
        ReadNumber(IDC_PROF_TAB_HEIGHT, L"The tab height", profiling.tabHeight, found);
        ReadNumber(IDC_PROF_TAB_LENGTH, L"The tab length", profiling.tabLength, found);
        if (profiling.tabSpacing == TabSpacing::Count)
        {
            double count = 0.0;
            if (ReadNumber(IDC_PROF_TAB_COUNT, L"The number of tabs", count, found))
            {
                if (count != std::floor(count) || count > 10000.0)
                    AppendProblem(found, L"The number of tabs must be a whole number.");
                else
                    profiling.tabCount = static_cast<int>(count);
            }
        }
        else
        {
            ReadNumber(IDC_PROF_TAB_DISTANCE, L"The distance between tabs", profiling.tabDistance, found);
        }
    }

    if (found.empty())
    {
        for (const std::string& problem : ValidateProfiling(profiling))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.profiling = profiling;
    return true;
}

void CProfilingPage::ShowStepover()
{
    // Puts the width, step and slider in step with what's been edited.
    m_updating = true;
    SetNumber(IDC_PROF_CUT_WIDTH, m_cutWidth);
    SetNumber(IDC_PROF_STEP, m_stepPerCut);
    int position = kWidthSliderMax;
    if (m_cutWidth > 0.0)
        position = static_cast<int>(std::lround(m_stepPerCut / m_cutWidth * kWidthSliderMax));
    m_stepSlider.SetPos((std::max)(kWidthSliderMin, (std::min)(kWidthSliderMax, position)));

    // A width of 0 is one pass, so there's no step to set.
    const BOOL stepped = m_cutWidth > 0.0;
    GetDlgItem(IDC_PROF_STEP)->EnableWindow(stepped);
    m_stepSlider.EnableWindow(stepped);
    m_updating = false;
}

void CProfilingPage::OnWidthChanged()
{
    double width = 0.0;
    if (m_updating || !ReadValue(IDC_PROF_CUT_WIDTH, width) || !(width >= 0.0) || width == m_cutWidth)
        return;

    // The step stays if it fits, and becomes the whole width otherwise.
    m_cutWidth = width;
    if (!(m_stepPerCut > 0.0) || m_stepPerCut > m_cutWidth)
        m_stepPerCut = m_cutWidth;
    ShowStepover();
}

void CProfilingPage::OnStepChanged()
{
    double step = 0.0;
    if (m_updating || !ReadValue(IDC_PROF_STEP, step) || !(step > 0.0) || step == m_stepPerCut)
        return;

    m_stepPerCut = (std::min)(step, m_cutWidth);
    ShowStepover();
}

void CProfilingPage::OnHScroll(UINT code, UINT position, CScrollBar* pScrollBar)
{
    COperationPage::OnHScroll(code, position, pScrollBar);
    if (m_updating || pScrollBar == nullptr || pScrollBar->GetSafeHwnd() != m_stepSlider.GetSafeHwnd())
        return;

    m_stepPerCut = m_cutWidth * m_stepSlider.GetPos() / kWidthSliderMax;
    ShowStepover();
}

void CProfilingPage::UpdateFields()
{
    // Outside and Inside only when closed curves use them.
    const BOOL closed = IsDlgButtonChecked(IDC_PROF_USE_CLOSED) == BST_CHECKED;
    GetDlgItem(IDC_PROF_OUTSIDE)->EnableWindow(closed);
    GetDlgItem(IDC_PROF_INSIDE)->EnableWindow(closed);

    // The Chamfering tab has no Tabs group.
    if (GetDlgItem(IDC_PROF_USE_TABS) == nullptr)
        return;

    // The rest of the Tabs group only when tabs are used, and only the chosen spacing's box.
    const BOOL tabs = IsDlgButtonChecked(IDC_PROF_USE_TABS) == BST_CHECKED;
    const BOOL byCount = IsDlgButtonChecked(IDC_PROF_TAB_DISTANCE_RADIO) != BST_CHECKED;
    for (const int id : { IDC_PROF_TAB_HEIGHT, IDC_PROF_TAB_LENGTH, IDC_PROF_TAB_COUNT_RADIO,
                          IDC_PROF_TAB_DISTANCE_RADIO })
        GetDlgItem(id)->EnableWindow(tabs);
    GetDlgItem(IDC_PROF_TAB_COUNT)->EnableWindow(tabs && byCount);
    GetDlgItem(IDC_PROF_TAB_DISTANCE)->EnableWindow(tabs && !byCount);
}

void CProfilingPage::OnChoiceChanged()
{
    UpdateFields();
}

/////////////////////////////////////////////////////////////////////////////
// CWaterlineRoughingPage

CWaterlineRoughingPage::CWaterlineRoughingPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_WATERLINE_ROUGHING, pOwner)
{
}

void CWaterlineRoughingPage::Load(const SequenceStep& step)
{
    SetNumber(IDC_WATERLINE_STOCK, step.waterlineRoughing.stockToLeave);
    SetNumber(IDC_WATERLINE_STEP, step.waterlineRoughing.stepDown);
}

bool CWaterlineRoughingPage::Save(SequenceStep& step, std::wstring& problems)
{
    WaterlineRoughingSettings waterline = step.waterlineRoughing;
    std::wstring found;
    ReadNumber(IDC_WATERLINE_STOCK, L"The stock to leave", waterline.stockToLeave, found);
    ReadNumber(IDC_WATERLINE_STEP, L"The cut depth step", waterline.stepDown, found);
    if (found.empty())
    {
        for (const std::string& problem : ValidateWaterlineRoughing(waterline))
            AppendProblem(found, FromUtf8(problem));
    }
    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }
    step.waterlineRoughing = waterline;
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// CPlungeRoughingPage

BEGIN_MESSAGE_MAP(CPlungeRoughingPage, COperationPage)
    ON_BN_CLICKED(IDC_PLUNGE_STEP_PERCENT_RADIO, &CPlungeRoughingPage::OnStepoverChanged)
    ON_BN_CLICKED(IDC_PLUNGE_STEP_DISTANCE_RADIO, &CPlungeRoughingPage::OnStepoverChanged)
END_MESSAGE_MAP()

CPlungeRoughingPage::CPlungeRoughingPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_PLUNGE_ROUGHING, pOwner)
{
}

void CPlungeRoughingPage::Load(const SequenceStep& step)
{
    const PlungeRoughingSettings& plunge = step.plungeRoughing;
    SetNumber(IDC_PLUNGE_STOCK, plunge.stockToLeave);
    CheckRadioButton(IDC_PLUNGE_ZIG, IDC_PLUNGE_ZIGZAG,
                     plunge.direction == PlungePathDirection::Zig ? IDC_PLUNGE_ZIG : IDC_PLUNGE_ZIGZAG);
    CheckRadioButton(IDC_PLUNGE_BOTTOM, IDC_PLUNGE_TOP,
                     plunge.start == PlungeStartSide::Top ? IDC_PLUNGE_TOP : IDC_PLUNGE_BOTTOM);
    SetNumber(IDC_PLUNGE_ANGLE, plunge.cutAngle);
    CheckRadioButton(IDC_PLUNGE_STEP_PERCENT_RADIO, IDC_PLUNGE_STEP_DISTANCE_RADIO,
                     plunge.stepover.usePercent ? IDC_PLUNGE_STEP_PERCENT_RADIO : IDC_PLUNGE_STEP_DISTANCE_RADIO);
    SetNumber(IDC_PLUNGE_STEP_PERCENT, plunge.stepover.percent);
    SetNumber(IDC_PLUNGE_STEP_DISTANCE, plunge.stepover.distance);
    SetNumber(IDC_PLUNGE_APPROACH, plunge.approach);
    UpdateFields();
}

bool CPlungeRoughingPage::Save(SequenceStep& step, std::wstring& problems)
{
    PlungeRoughingSettings plunge = step.plungeRoughing;
    std::wstring found;

    ReadNumber(IDC_PLUNGE_STOCK, L"The stock to leave", plunge.stockToLeave, found);
    plunge.direction =
        IsDlgButtonChecked(IDC_PLUNGE_ZIG) == BST_CHECKED ? PlungePathDirection::Zig : PlungePathDirection::ZigZag;
    plunge.start = IsDlgButtonChecked(IDC_PLUNGE_TOP) == BST_CHECKED ? PlungeStartSide::Top : PlungeStartSide::Bottom;
    ReadNumber(IDC_PLUNGE_ANGLE, L"The angle of cuts", plunge.cutAngle, found);

    // Only the stepover box in use is read.
    plunge.stepover.usePercent = IsDlgButtonChecked(IDC_PLUNGE_STEP_DISTANCE_RADIO) != BST_CHECKED;
    if (plunge.stepover.usePercent)
        ReadNumber(IDC_PLUNGE_STEP_PERCENT, L"The stepover percentage", plunge.stepover.percent, found);
    else
        ReadNumber(IDC_PLUNGE_STEP_DISTANCE, L"The stepover distance", plunge.stepover.distance, found);
    ReadNumber(IDC_PLUNGE_APPROACH, L"The approach", plunge.approach, found);

    if (found.empty())
    {
        for (const std::string& problem : ValidatePlungeRoughing(plunge))
            AppendProblem(found, FromUtf8(problem));
    }
    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.plungeRoughing = plunge;
    return true;
}

void CPlungeRoughingPage::UpdateFields()
{
    const bool percent = IsDlgButtonChecked(IDC_PLUNGE_STEP_DISTANCE_RADIO) != BST_CHECKED;
    GetDlgItem(IDC_PLUNGE_STEP_PERCENT)->EnableWindow(percent);
    GetDlgItem(IDC_PLUNGE_STEP_DISTANCE)->EnableWindow(!percent);
}

void CPlungeRoughingPage::OnStepoverChanged()
{
    UpdateFields();
}

/////////////////////////////////////////////////////////////////////////////
// CPocketingPage

BEGIN_MESSAGE_MAP(CPocketingPage, COperationPage)
    ON_BN_CLICKED(IDC_POCK_OFFSET, &CPocketingPage::OnCutTypeChanged)
    ON_BN_CLICKED(IDC_POCK_LINEAR, &CPocketingPage::OnCutTypeChanged)
    ON_BN_CLICKED(IDC_POCK_SPIRAL, &CPocketingPage::OnCutTypeChanged)
    ON_BN_CLICKED(IDC_POCK_RADIAL, &CPocketingPage::OnCutTypeChanged)
    ON_BN_CLICKED(IDC_POCK_CLIMB, &CPocketingPage::OnDirectionChanged)
    ON_BN_CLICKED(IDC_POCK_CONVENTIONAL, &CPocketingPage::OnDirectionChanged)
    ON_BN_CLICKED(IDC_POCK_MIXED, &CPocketingPage::OnDirectionChanged)
    ON_BN_CLICKED(IDC_POCK_START_A, &CPocketingPage::OnStartChanged)
    ON_BN_CLICKED(IDC_POCK_START_B, &CPocketingPage::OnStartChanged)
    ON_BN_CLICKED(IDC_POCK_STEP_PERCENT_RADIO, &CPocketingPage::OnStepoverChanged)
    ON_BN_CLICKED(IDC_POCK_STEP_DISTANCE_RADIO, &CPocketingPage::OnStepoverChanged)
END_MESSAGE_MAP()

CPocketingPage::CPocketingPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_POCKETING, pOwner)
    , m_direction(CutDirection::Climb)
    , m_start(PocketStart::Inside)
    , m_linearStart(PocketLinearStart::Bottom)
{
}

PocketCutType CPocketingPage::ShownCutType() const
{
    switch (GetCheckedRadioButton(IDC_POCK_OFFSET, IDC_POCK_RADIAL))
    {
    case IDC_POCK_LINEAR: return PocketCutType::Linear;
    case IDC_POCK_SPIRAL: return PocketCutType::Spiral;
    case IDC_POCK_RADIAL: return PocketCutType::Radial;
    default:              return PocketCutType::Offset;
    }
}

void CPocketingPage::Load(const SequenceStep& step)
{
    const PocketingSettings& pocketing = step.pocketing;
    SetNumber(IDC_POCK_STOCK, pocketing.stockToLeave);
    const int type = (pocketing.cutType == PocketCutType::Linear)   ? IDC_POCK_LINEAR
                     : (pocketing.cutType == PocketCutType::Spiral) ? IDC_POCK_SPIRAL
                     : (pocketing.cutType == PocketCutType::Radial) ? IDC_POCK_RADIAL
                                                                    : IDC_POCK_OFFSET;
    CheckRadioButton(IDC_POCK_OFFSET, IDC_POCK_RADIAL, type);
    SetNumber(IDC_POCK_ANGLE, pocketing.cutAngle);

    m_direction = pocketing.direction;
    m_start = pocketing.start;
    m_linearStart = pocketing.linearStart;

    const ToolRelativeSize& stepover = pocketing.stepover;
    CheckRadioButton(IDC_POCK_STEP_PERCENT_RADIO, IDC_POCK_STEP_DISTANCE_RADIO,
                     stepover.usePercent ? IDC_POCK_STEP_PERCENT_RADIO : IDC_POCK_STEP_DISTANCE_RADIO);
    SetNumber(IDC_POCK_STEP_PERCENT, stepover.percent);
    SetNumber(IDC_POCK_STEP_DISTANCE, stepover.distance);
    UpdateFields();
}

bool CPocketingPage::Save(SequenceStep& step, std::wstring& problems)
{
    PocketingSettings pocketing = step.pocketing;
    std::wstring found;

    ReadNumber(IDC_POCK_STOCK, L"The stock to leave", pocketing.stockToLeave, found);
    pocketing.cutType = ShownCutType();
    pocketing.direction = m_direction;
    pocketing.start = m_start;
    pocketing.linearStart = m_linearStart;

    // Only the boxes in use are read.
    if (PocketUsesAngle(pocketing.cutType))
        ReadNumber(IDC_POCK_ANGLE, L"The angle of cuts", pocketing.cutAngle, found);
    pocketing.stepover.usePercent = IsDlgButtonChecked(IDC_POCK_STEP_DISTANCE_RADIO) != BST_CHECKED;
    if (pocketing.stepover.usePercent)
        ReadNumber(IDC_POCK_STEP_PERCENT, L"The stepover percentage", pocketing.stepover.percent, found);
    else
        ReadNumber(IDC_POCK_STEP_DISTANCE, L"The stepover distance", pocketing.stepover.distance, found);

    if (found.empty())
    {
        for (const std::string& problem : ValidatePocketing(pocketing))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.pocketing = pocketing;
    return true;
}

void CPocketingPage::UpdateFields()
{
    const PocketCutType type = ShownCutType();

    // Spiral can't be Mixed, so it shows Climb while Mixed is kept for the other types.
    const bool mixedAllowed = PocketAllowsMixed(type);
    GetDlgItem(IDC_POCK_MIXED)->EnableWindow(mixedAllowed);
    CutDirection shown = m_direction;
    if (shown == CutDirection::Mixed && !mixedAllowed)
        shown = CutDirection::Climb;
    const int direction = (shown == CutDirection::Conventional) ? IDC_POCK_CONVENTIONAL
                          : (shown == CutDirection::Mixed)      ? IDC_POCK_MIXED
                                                                : IDC_POCK_CLIMB;
    CheckRadioButton(IDC_POCK_CLIMB, IDC_POCK_MIXED, direction);

    // Only Linear Cuts have an angle.
    const bool angle = PocketUsesAngle(type);
    GetDlgItem(IDC_POCK_ANGLE)->EnableWindow(angle);
    GetDlgItem(IDC_POCK_ANGLE_LABEL)->EnableWindow(angle);

    // The start point is the bottom or top for Linear Cuts, and inside or outside otherwise.
    const bool linear = (type == PocketCutType::Linear);
    SetDlgItemText(IDC_POCK_START_A, linear ? L"Start at Bottom" : L"Inside");
    SetDlgItemText(IDC_POCK_START_B, linear ? L"Start at Top" : L"Outside");
    const bool second = linear ? (m_linearStart == PocketLinearStart::Top) : (m_start == PocketStart::Outside);
    CheckRadioButton(IDC_POCK_START_A, IDC_POCK_START_B, second ? IDC_POCK_START_B : IDC_POCK_START_A);

    // Only the chosen stepover's box.
    const bool percent = IsDlgButtonChecked(IDC_POCK_STEP_DISTANCE_RADIO) != BST_CHECKED;
    GetDlgItem(IDC_POCK_STEP_PERCENT)->EnableWindow(percent);
    GetDlgItem(IDC_POCK_STEP_DISTANCE)->EnableWindow(!percent);
}

void CPocketingPage::OnCutTypeChanged()
{
    UpdateFields();
}

void CPocketingPage::OnDirectionChanged()
{
    switch (GetCheckedRadioButton(IDC_POCK_CLIMB, IDC_POCK_MIXED))
    {
    case IDC_POCK_CONVENTIONAL: m_direction = CutDirection::Conventional; break;
    case IDC_POCK_MIXED:        m_direction = CutDirection::Mixed; break;
    default:                    m_direction = CutDirection::Climb; break;
    }
}

void CPocketingPage::OnStartChanged()
{
    // The buttons belong to whichever start the shown cut type uses.
    const bool second = IsDlgButtonChecked(IDC_POCK_START_B) == BST_CHECKED;
    if (ShownCutType() == PocketCutType::Linear)
        m_linearStart = second ? PocketLinearStart::Top : PocketLinearStart::Bottom;
    else
        m_start = second ? PocketStart::Outside : PocketStart::Inside;
}

void CPocketingPage::OnStepoverChanged()
{
    UpdateFields();
}

/////////////////////////////////////////////////////////////////////////////
// CHolePocketingPage

BEGIN_MESSAGE_MAP(CHolePocketingPage, COperationPage)
    ON_BN_CLICKED(IDC_HP_AT_TOP, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_AT_BOTTOM, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_PICK_TOP, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_USE_ARC, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_STEP_PERCENT_RADIO, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_STEP_DISTANCE_RADIO, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_DOWN_PERCENT_RADIO, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_DOWN_DISTANCE_RADIO, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_DOWN_LEVELS_RADIO, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_PITCH_ANGLE_RADIO, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_PITCH_HEIGHT_RADIO, &CHolePocketingPage::OnChoiceChanged)
    ON_BN_CLICKED(IDC_HP_PICK_TOP_BUTTON, &CHolePocketingPage::OnPickTop)
    ON_BN_CLICKED(IDC_HP_PICK_DEPTH, &CHolePocketingPage::OnPickDepth)
END_MESSAGE_MAP()

CHolePocketingPage::CHolePocketingPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_HOLE_POCKETING, pOwner)
{
}

void CHolePocketingPage::Load(const SequenceStep& step)
{
    const HolePocketingSettings& hole = step.holePocketing;
    const int location = (hole.location == CutGeometryLocation::AtBottom)  ? IDC_HP_AT_BOTTOM
                         : (hole.location == CutGeometryLocation::PickTop) ? IDC_HP_PICK_TOP
                                                                           : IDC_HP_AT_TOP;
    CheckRadioButton(IDC_HP_AT_TOP, IDC_HP_PICK_TOP, location);
    SetNumber(IDC_HP_TOP_Z, hole.topZ);

    SetNumber(IDC_HP_DEPTH, hole.depth);
    CheckDlgButton(IDC_HP_USE_MODEL, hole.useModelForDepth ? BST_CHECKED : BST_UNCHECKED);
    SetNumber(IDC_HP_DIAMETER, hole.diameter);
    CheckDlgButton(IDC_HP_USE_ARC, hole.useArcDiameter ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_HP_CLEAN_UP, hole.cleanUpEachLevel ? BST_CHECKED : BST_UNCHECKED);

    CheckRadioButton(IDC_HP_CLIMB, IDC_HP_CONVENTIONAL,
                     hole.direction == CutDirection::Conventional ? IDC_HP_CONVENTIONAL : IDC_HP_CLIMB);

    CheckRadioButton(IDC_HP_STEP_PERCENT_RADIO, IDC_HP_STEP_DISTANCE_RADIO,
                     hole.stepover.usePercent ? IDC_HP_STEP_PERCENT_RADIO : IDC_HP_STEP_DISTANCE_RADIO);
    SetNumber(IDC_HP_STEP_PERCENT, hole.stepover.percent);
    SetNumber(IDC_HP_STEP_DISTANCE, hole.stepover.distance);

    const int down = (hole.stepDown == HoleStepDown::Distance) ? IDC_HP_DOWN_DISTANCE_RADIO
                     : (hole.stepDown == HoleStepDown::Levels) ? IDC_HP_DOWN_LEVELS_RADIO
                                                               : IDC_HP_DOWN_PERCENT_RADIO;
    CheckRadioButton(IDC_HP_DOWN_PERCENT_RADIO, IDC_HP_DOWN_LEVELS_RADIO, down);
    SetNumber(IDC_HP_DOWN_PERCENT, hole.stepDownPercent);
    SetNumber(IDC_HP_DOWN_DISTANCE, hole.stepDownDistance);
    SetNumber(IDC_HP_DOWN_LEVELS, hole.stepDownLevels);

    SetNumber(IDC_HP_HELIX_DIA, hole.helixDiameter);
    CheckRadioButton(IDC_HP_PITCH_ANGLE_RADIO, IDC_HP_PITCH_HEIGHT_RADIO,
                     hole.pitch == HelixPitch::Height ? IDC_HP_PITCH_HEIGHT_RADIO : IDC_HP_PITCH_ANGLE_RADIO);
    SetNumber(IDC_HP_PITCH_ANGLE, hole.pitchAngle);
    SetNumber(IDC_HP_PITCH_HEIGHT, hole.pitchHeight);
    SetNumber(IDC_HP_APPROACH, hole.approachDistance);

    UpdateFields();
    ShowLocating(LocateTarget::HoleTop, false);
}

bool CHolePocketingPage::Save(SequenceStep& step, std::wstring& problems)
{
    HolePocketingSettings hole = step.holePocketing;
    std::wstring found;

    // Only the boxes the chosen options use are read.
    switch (GetCheckedRadioButton(IDC_HP_AT_TOP, IDC_HP_PICK_TOP))
    {
    case IDC_HP_AT_BOTTOM: hole.location = CutGeometryLocation::AtBottom; break;
    case IDC_HP_PICK_TOP:  hole.location = CutGeometryLocation::PickTop; break;
    default:               hole.location = CutGeometryLocation::AtTop; break;
    }
    if (hole.location == CutGeometryLocation::PickTop)
        ReadNumber(IDC_HP_TOP_Z, L"The top Z", hole.topZ, found);

    ReadNumber(IDC_HP_DEPTH, L"The hole depth", hole.depth, found);
    hole.useModelForDepth = IsDlgButtonChecked(IDC_HP_USE_MODEL) == BST_CHECKED;
    hole.useArcDiameter = IsDlgButtonChecked(IDC_HP_USE_ARC) == BST_CHECKED;
    if (!hole.useArcDiameter)
        ReadNumber(IDC_HP_DIAMETER, L"The hole diameter", hole.diameter, found);
    hole.cleanUpEachLevel = IsDlgButtonChecked(IDC_HP_CLEAN_UP) == BST_CHECKED;
    hole.direction = (IsDlgButtonChecked(IDC_HP_CONVENTIONAL) == BST_CHECKED) ? CutDirection::Conventional
                                                                             : CutDirection::Climb;

    hole.stepover.usePercent = IsDlgButtonChecked(IDC_HP_STEP_DISTANCE_RADIO) != BST_CHECKED;
    if (hole.stepover.usePercent)
        ReadNumber(IDC_HP_STEP_PERCENT, L"The stepover percentage", hole.stepover.percent, found);
    else
        ReadNumber(IDC_HP_STEP_DISTANCE, L"The stepover distance", hole.stepover.distance, found);

    switch (GetCheckedRadioButton(IDC_HP_DOWN_PERCENT_RADIO, IDC_HP_DOWN_LEVELS_RADIO))
    {
    case IDC_HP_DOWN_DISTANCE_RADIO:
        hole.stepDown = HoleStepDown::Distance;
        ReadNumber(IDC_HP_DOWN_DISTANCE, L"The step down distance", hole.stepDownDistance, found);
        break;
    case IDC_HP_DOWN_LEVELS_RADIO:
    {
        hole.stepDown = HoleStepDown::Levels;
        double levels = 0.0;
        if (ReadNumber(IDC_HP_DOWN_LEVELS, L"The number of levels", levels, found))
        {
            if (levels != std::floor(levels) || levels > 10000.0)
                AppendProblem(found, L"The number of levels must be a whole number.");
            else
                hole.stepDownLevels = static_cast<int>(levels);
        }
        break;
    }
    default:
        hole.stepDown = HoleStepDown::Percent;
        ReadNumber(IDC_HP_DOWN_PERCENT, L"The step down percentage", hole.stepDownPercent, found);
        break;
    }

    ReadNumber(IDC_HP_HELIX_DIA, L"The helix diameter", hole.helixDiameter, found);
    hole.pitch = (IsDlgButtonChecked(IDC_HP_PITCH_HEIGHT_RADIO) == BST_CHECKED) ? HelixPitch::Height : HelixPitch::Angle;
    if (hole.pitch == HelixPitch::Angle)
        ReadNumber(IDC_HP_PITCH_ANGLE, L"The helix angle", hole.pitchAngle, found);
    else
        ReadNumber(IDC_HP_PITCH_HEIGHT, L"The helix height", hole.pitchHeight, found);
    ReadNumber(IDC_HP_APPROACH, L"The approach distance", hole.approachDistance, found);

    if (found.empty())
    {
        for (const std::string& problem : ValidateHolePocketing(hole))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.holePocketing = hole;
    return true;
}

void CHolePocketingPage::UpdateFields()
{
    const BOOL pickTop = GetCheckedRadioButton(IDC_HP_AT_TOP, IDC_HP_PICK_TOP) == IDC_HP_PICK_TOP;
    GetDlgItem(IDC_HP_TOP_Z)->EnableWindow(pickTop);
    GetDlgItem(IDC_HP_PICK_TOP_BUTTON)->EnableWindow(pickTop);

    // Each picked circle's own diameter, or the one typed in.
    const BOOL typed = IsDlgButtonChecked(IDC_HP_USE_ARC) != BST_CHECKED;
    GetDlgItem(IDC_HP_DIAMETER)->EnableWindow(typed);
    GetDlgItem(IDC_HP_DIAMETER_LABEL)->EnableWindow(typed);

    // Only the chosen box of each set.
    const BOOL stepPercent = IsDlgButtonChecked(IDC_HP_STEP_DISTANCE_RADIO) != BST_CHECKED;
    GetDlgItem(IDC_HP_STEP_PERCENT)->EnableWindow(stepPercent);
    GetDlgItem(IDC_HP_STEP_DISTANCE)->EnableWindow(!stepPercent);

    const int down = GetCheckedRadioButton(IDC_HP_DOWN_PERCENT_RADIO, IDC_HP_DOWN_LEVELS_RADIO);
    GetDlgItem(IDC_HP_DOWN_PERCENT)->EnableWindow(down != IDC_HP_DOWN_DISTANCE_RADIO && down != IDC_HP_DOWN_LEVELS_RADIO);
    GetDlgItem(IDC_HP_DOWN_DISTANCE)->EnableWindow(down == IDC_HP_DOWN_DISTANCE_RADIO);
    GetDlgItem(IDC_HP_DOWN_LEVELS)->EnableWindow(down == IDC_HP_DOWN_LEVELS_RADIO);

    const BOOL byAngle = IsDlgButtonChecked(IDC_HP_PITCH_HEIGHT_RADIO) != BST_CHECKED;
    GetDlgItem(IDC_HP_PITCH_ANGLE)->EnableWindow(byAngle);
    GetDlgItem(IDC_HP_PITCH_HEIGHT)->EnableWindow(!byAngle);
}

void CHolePocketingPage::OnChoiceChanged()
{
    UpdateFields();
}

void CHolePocketingPage::OnPickTop()
{
    if (m_pOwner->IsLocating())
        m_pOwner->StopLocating();
    else
        m_pOwner->StartLocating(LocateTarget::HoleTop);
}

void CHolePocketingPage::OnPickDepth()
{
    if (m_pOwner->IsLocating())
        m_pOwner->StopLocating();
    else
        m_pOwner->StartLocating(LocateTarget::HoleDepth);
}

void CHolePocketingPage::ShowLocating(LocateTarget target, bool locating)
{
    SetDlgItemText(IDC_HP_PICK_TOP_BUTTON, (locating && target == LocateTarget::HoleTop) ? L"Stop" : L"Pick");
    SetDlgItemText(IDC_HP_PICK_DEPTH, (locating && target == LocateTarget::HoleDepth) ? L"Stop" : L"Pick");
}

double CHolePocketingPage::ReferenceZ()
{
    // As for Cut Levels: Pick Top gives the level, otherwise the highest of the operation's features.
    double z = 0.0;
    if (GetCheckedRadioButton(IDC_HP_AT_TOP, IDC_HP_PICK_TOP) == IDC_HP_PICK_TOP)
        return ReadValue(IDC_HP_TOP_Z, z) ? z : 0.0;

    camcore::Frame frame;
    if (!m_pOwner->GetFrame(frame))
        return 0.0;
    bool first = true;
    for (const PickedGeometry& feature : m_pOwner->GetFeatures())
    {
        const double height = WorldToFrame(frame, FeaturePoint(frame, feature)).z;
        z = first ? height : (std::max)(z, height);
        first = false;
    }
    return z;
}

void CHolePocketingPage::SetTopZ(double z)
{
    SetNumber(IDC_HP_TOP_Z, z);
    CheckRadioButton(IDC_HP_AT_TOP, IDC_HP_PICK_TOP, IDC_HP_PICK_TOP);
    UpdateFields();
}

void CHolePocketingPage::SetDepthToHeight(double z)
{
    const double depth = std::fabs(ReferenceZ() - z);
    if (!(depth > 0.0))
    {
        MessageBoxW(L"That item is at the cut geometry's level, so it gives no depth.", L"Hole Pocketing",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    SetNumber(IDC_HP_DEPTH, depth);
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
        m_pOwner->StartLocating(LocateTarget::DrillDepth);
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
    ON_BN_CLICKED(IDC_HELP_BUTTON, &COperationDlg::OnHelpButton)
    ON_NOTIFY(TCN_SELCHANGE, IDC_OP_TABS, &COperationDlg::OnTabChanged)
    ON_WM_CLOSE()
    ON_BN_CLICKED(IDC_OP_GENERATE, &COperationDlg::OnGenerate)
END_MESSAGE_MAP()

void COperationDlg::OnHelpButton()
{
    // The help for the tab that's showing.
    const int index = m_tabs.GetCurSel();
    const UINT page = (index >= 0 && static_cast<size_t>(index) < m_pages.size())
                          ? m_pages[static_cast<size_t>(index)]->TemplateId()
                          : 0;
    const wchar_t* section = (page == IDD_OP_FEATURES)         ? L"tab-features"
                             : (page == IDD_OP_TOOL)           ? L"tab-tool"
                             : (page == IDD_OP_FEEDS)          ? L"tab-feeds"
                             : (page == IDD_OP_CLEARANCE)      ? L"tab-clearance"
                             : (page == IDD_OP_PAGE)           ? L"tab-sorting"
                             : (page == IDD_OP_CUT_LEVELS)     ? L"tab-cut-levels"
                             : (page == IDD_OP_ENTRY_EXIT)     ? L"tab-entry-exit"
                             : (page == IDD_OP_FACING)         ? L"tab-facing"
                             : (page == IDD_OP_PROFILING)      ? L"tab-profiling"
                             : (page == IDD_OP_CHAMFERING)     ? L"tab-chamfering"
                             : (page == IDD_OP_ENGRAVING)      ? L"tab-engraving"
                             : (page == IDD_OP_POCKETING)      ? L"tab-pocketing"
                             : (page == IDD_OP_HOLE_POCKETING) ? L"tab-hole-pocketing"
                             : (page == IDD_OP_PLUNGE_ROUGHING) ? L"tab-plunge-roughing"
                             : (page == IDD_OP_WATERLINE_ROUGHING) ? L"tab-waterline-roughing"
                             : (page == IDD_OP_DRILLING)       ? L"tab-drilling"
                                                               : L"operation";
    ShowHelp(section);
}

COperationDlg::COperationDlg(CCamTabCommand* pTab, const SequenceStep& step)
    : CDialog(IDD_OPERATION, nullptr)
    , m_pTab(pTab)
    , m_step(step)
    , m_pFeatures(nullptr)
    , m_pDrilling(nullptr)
    , m_pCutLevels(nullptr)
    , m_pHolePocketing(nullptr)
    , m_pToolPage(nullptr)
    , m_locating(false)
    , m_locateTarget(LocateTarget::DrillDepth)
    , m_selectingFeatures(false)
    , m_haveFrame(false)
{
}

COperationDlg::COperationDlg(CCamTabCommand* pTab, const std::vector<SequenceStep>& steps)
    : COperationDlg(pTab, steps.empty() ? SequenceStep() : steps.front())
{
    m_bulk = steps;
}

std::unique_ptr<COperationPage> COperationDlg::MakePage(UINT templateId)
{
    switch (templateId)
    {
    case IDD_OP_TOOL:           return std::make_unique<CToolPage>(this);
    case IDD_OP_FEEDS:          return std::make_unique<CFeedsPage>(this);
    case IDD_OP_CLEARANCE:      return std::make_unique<CClearancePage>(this);
    case IDD_OP_CUT_LEVELS:     return std::make_unique<CCutLevelsPage>(this);
    case IDD_OP_ENTRY_EXIT:     return std::make_unique<CEntryExitPage>(this);
    case IDD_OP_FACING:         return std::make_unique<CFacingPage>(this);
    case IDD_OP_PROFILING:      return std::make_unique<CProfilingPage>(this);
    case IDD_OP_CHAMFERING:     return std::make_unique<CChamferingPage>(this);
    case IDD_OP_ENGRAVING:      return std::make_unique<CEngravingPage>(this);
    case IDD_OP_POCKETING:      return std::make_unique<CPocketingPage>(this);
    case IDD_OP_DRILLING:       return std::make_unique<CDrillingPage>(this);
    case IDD_OP_HOLE_POCKETING: return std::make_unique<CHolePocketingPage>(this);
    case IDD_OP_PLUNGE_ROUGHING: return std::make_unique<CPlungeRoughingPage>(this);
    case IDD_OP_WATERLINE_ROUGHING: return std::make_unique<CWaterlineRoughingPage>(this);
    default:                    return std::make_unique<COperationPage>(templateId, this);
    }
}

bool COperationDlg::CreateModeless(CWnd* pParent)
{
    return Create(IDD_OPERATION, pParent) != FALSE;
}

BOOL COperationDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    const bool bulk = IsBulk();
    if (bulk)
    {
        // Several operations: no name or Generate of their own.
        const std::wstring count = std::to_wstring(m_bulk.size());
        SetWindowText((L"Edit " + count + L" Operations").c_str());
        SetDlgItemText(IDC_OP_NAME, (count + L" operations").c_str());
        GetDlgItem(IDC_OP_NAME)->EnableWindow(FALSE);
        GetDlgItem(IDC_OP_GENERATE)->ShowWindow(SW_HIDE);
    }
    else
    {
        SetWindowText(OperationTitle(m_step.type));
        SetDlgItemText(IDC_OP_NAME, FromUtf8(m_step.name).c_str());
    }

    // The tabs every chosen operation has. One operation has all of its own.
    const auto isArea = [](StepType type) {
        return type == StepType::Facing || type == StepType::Profiling || type == StepType::Pocketing ||
               type == StepType::Chamfer || type == StepType::Engrave;
    };
    // Chamfer has no Cut Levels tab: its depth comes from the chamfer.
    const auto hasLevels = [](StepType type) {
        return type == StepType::Facing || type == StepType::Profiling || type == StepType::Pocketing ||
               type == StepType::Engrave;
    };
    bool sameType = true;
    bool allArea = isArea(m_step.type);
    bool allLevels = hasLevels(m_step.type);
    for (const SequenceStep& other : m_bulk)
    {
        sameType = sameType && other.type == m_step.type;
        allArea = allArea && isArea(other.type);
        allLevels = allLevels && hasLevels(other.type);
    }

    m_tabs.SubclassDlgItem(IDC_OP_TABS, this);
    if (m_pTab != nullptr)
        m_haveFrame = m_pTab->GetOperationFrame(m_frame);

    // Every operation has these. The empty ones are still to be built.
    // Features are each operation's own, so they aren't bulk edited.
    if (!bulk)
    {
        auto features = std::make_unique<CFeaturesPage>(this);
        CFeaturesPage* pFeatures = features.get();
        AddPage(L"Features", std::move(features));
        if (pFeatures->GetSafeHwnd() != nullptr)
            m_pFeatures = pFeatures;
    }

    auto toolPage = std::make_unique<CToolPage>(this);
    CToolPage* pToolPage = toolPage.get();
    AddPage(L"Tool", std::move(toolPage));
    if (pToolPage->GetSafeHwnd() != nullptr)
        m_pToolPage = pToolPage;
    AddPage(L"Feeds && Speeds", std::make_unique<CFeedsPage>(this));
    AddPage(L"Clearance", std::make_unique<CClearancePage>(this));

    // Drill, Hole Pocketing, Facing, Profiling and Pocketing have no Sorting tab.
    if (!bulk && m_step.type != StepType::Drill && m_step.type != StepType::Facing &&
        m_step.type != StepType::Profiling && m_step.type != StepType::Pocketing && m_step.type != StepType::Chamfer &&
        m_step.type != StepType::Engrave && m_step.type != StepType::PlungeRoughing &&
        m_step.type != StepType::WaterlineRoughing &&
        m_step.type != StepType::HolePocketing)
        AddPage(L"Sorting", std::make_unique<COperationPage>(IDD_OP_PAGE, this));

    // Then its own.
    if (allArea)
    {
        // Cut Levels is shared by the operations that cut in levels.
        if (allLevels)
        {
            auto cutLevels = std::make_unique<CCutLevelsPage>(this);
            CCutLevelsPage* pCutLevels = cutLevels.get();
            AddPage(L"Cut Levels", std::move(cutLevels));
            if (pCutLevels->GetSafeHwnd() != nullptr)
                m_pCutLevels = pCutLevels;
        }

        // Entry/Exit is shared with them too.
        AddPage(L"Entry/Exit", std::make_unique<CEntryExitPage>(this));

        // Then the operation's own tab, when they're all the same kind.
        if (sameType && m_step.type == StepType::Facing)
            AddPage(L"Facing", std::make_unique<CFacingPage>(this));
        else if (sameType && m_step.type == StepType::Profiling)
            AddPage(L"Profiling", std::make_unique<CProfilingPage>(this));
        else if (sameType && m_step.type == StepType::Chamfer)
            AddPage(L"Chamfering", std::make_unique<CChamferingPage>(this));
        else if (sameType && m_step.type == StepType::Pocketing)
            AddPage(L"Pocketing", std::make_unique<CPocketingPage>(this));
        else if (sameType && m_step.type == StepType::Engrave)
            AddPage(L"Engraving", std::make_unique<CEngravingPage>(this));
    }
    else if (!sameType)
    {
        // Mixed kinds share only the first tabs.
    }
    else if (m_step.type == StepType::Drill)
    {
        auto drilling = std::make_unique<CDrillingPage>(this);
        CDrillingPage* pDrilling = drilling.get();
        AddPage(L"Drilling", std::move(drilling));
        if (pDrilling->GetSafeHwnd() != nullptr)
            m_pDrilling = pDrilling;
    }
    else if (m_step.type == StepType::PlungeRoughing)
    {
        AddPage(L"Plunge Roughing", std::make_unique<CPlungeRoughingPage>(this));
    }
    else if (m_step.type == StepType::WaterlineRoughing)
    {
        AddPage(L"Waterline Roughing", std::make_unique<CWaterlineRoughingPage>(this));
    }
    else if (m_step.type == StepType::HolePocketing)
    {
        auto hole = std::make_unique<CHolePocketingPage>(this);
        CHolePocketingPage* pHole = hole.get();
        AddPage(L"Hole Pocketing", std::move(hole));
        if (pHole->GetSafeHwnd() != nullptr)
            m_pHolePocketing = pHole;
    }

    if (!bulk)
    {
        for (const auto& page : m_pages)
            page->Load(m_step);
    }
    else
    {
        // Each tab shows what the operations agree on, and the rest mixed. A
        // hidden copy of the tab applies the changes to each operation on OK.
        for (const auto& page : m_pages)
        {
            std::vector<COperationPage::ControlState> states;
            for (const SequenceStep& other : m_bulk)
            {
                page->Load(other);
                states.push_back(page->Snapshot());
            }
            page->Load(m_bulk.front());
            page->ShowMixed(states);
            m_baselines.push_back(page->Snapshot());

            std::unique_ptr<COperationPage> twin = MakePage(page->TemplateId());
            if (!twin->CreateInTab(this))
                Log(L"[operation] could not create a hidden tab for bulk editing");
            m_twins.push_back(std::move(twin));
        }
        g_lastPage = 0;
    }

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

void COperationDlg::StartLocating(LocateTarget target)
{
    const bool forDrilling = (target == LocateTarget::DrillDepth);
    const bool forHole = (target == LocateTarget::HoleTop || target == LocateTarget::HoleDepth);
    const bool havePage = forDrilling ? m_pDrilling != nullptr
                          : forHole   ? m_pHolePocketing != nullptr
                                      : m_pCutLevels != nullptr;
    if (!havePage || m_pTab == nullptr)
        return;

    StopSelectingFeatures();

    m_locating = true;
    m_locateTarget = target;
    if (forDrilling)
        m_pDrilling->ShowLocating(true);
    else if (forHole)
        m_pHolePocketing->ShowLocating(target, true);
    else
        m_pCutLevels->ShowLocating(target, true);
    m_pTab->BeginLocate();
}

void COperationDlg::StopLocating()
{
    if (!m_locating)
        return;

    m_locating = false;
    if (m_pDrilling != nullptr)
        m_pDrilling->ShowLocating(false);
    if (m_pCutLevels != nullptr)
        m_pCutLevels->ShowLocating(m_locateTarget, false);
    if (m_pHolePocketing != nullptr)
        m_pHolePocketing->ShowLocating(m_locateTarget, false);
    if (m_pTab != nullptr)
        m_pTab->EndLocate();
}

void COperationDlg::SetLocatedHeight(bool found, double height)
{
    const LocateTarget target = m_locateTarget;
    StopLocating();

    if (target == LocateTarget::DrillDepth)
    {
        // The drill depth is a distance, so an item with no answer measures as 0.
        const double depth = found ? std::fabs(height) : 0.0;
        if (m_pDrilling != nullptr)
        {
            m_pDrilling->SetDepth(depth);
            m_pDrilling->ShowLocateNote(depth > 0.0 ? L"The depth was measured from the picked item."
                                                    : L"That item measures as 0. It's on the origin's XY plane, "
                                                      L"runs along the drill direction, or is too far away.");
        }
        return;
    }

    if (!found)
    {
        ShowLocateProblem(L"That item runs along the tool axis, or is too far away, so it has no single Z.");
        return;
    }

    if (target == LocateTarget::HoleTop || target == LocateTarget::HoleDepth)
    {
        if (m_pHolePocketing == nullptr)
            return;
        if (target == LocateTarget::HoleTop)
            m_pHolePocketing->SetTopZ(height);
        else
            m_pHolePocketing->SetDepthToHeight(height);
        return;
    }

    if (m_pCutLevels == nullptr)
        return;
    if (target == LocateTarget::CutTop)
        m_pCutLevels->SetTopZ(height);
    else
        m_pCutLevels->SetDepthToHeight(height);
}

void COperationDlg::ShowLocateProblem(const std::wstring& problem)
{
    // The Drilling tab has room for a note. Cut Levels and Hole Pocketing don't, so they get a message.
    if (m_locateTarget == LocateTarget::DrillDepth)
    {
        if (m_pDrilling != nullptr)
            m_pDrilling->ShowLocateNote(problem);
    }
    else
    {
        MessageBoxW(problem.c_str(), OperationTitle(m_step.type), MB_OK | MB_ICONINFORMATION);
    }
}

FeatureFilter COperationDlg::GetFeatureFilter() const
{
    // Each operation type has its own filter. Drill needs points, and Facing faces and edges.
    // Profiling and Pocketing borrow Facing's until their own are decided.
    const bool area = m_step.type == StepType::Facing || m_step.type == StepType::Profiling ||
                      m_step.type == StepType::Pocketing || m_step.type == StepType::Chamfer ||
                      m_step.type == StepType::Engrave;
    if (m_step.type == StepType::PlungeRoughing || m_step.type == StepType::WaterlineRoughing)
        return FeatureFilter::ProjectedArea;
    return area ? FeatureFilter::FacingArea : FeatureFilter::Points;
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

bool COperationDlg::FeedsFromTool(FeedSettings& feeds, std::wstring& problem) const
{
    if (m_pTab == nullptr || m_pToolPage == nullptr)
    {
        problem = L"The CAM tab has closed.";
        return false;
    }

    const ToolLibrary library = m_pTab->GetToolLibrary();
    const Tool* pTool = FindToolById(library, m_pToolPage->ChosenToolId());
    if (pTool == nullptr)
    {
        problem = L"Choose a tool on the Tool tab first.";
        return false;
    }
    const std::string materialId = m_pTab->GetMaterialId();
    if (materialId.empty())
    {
        problem = L"The job has no material. Right-click Material on the CAM tab to choose one.";
        return false;
    }
    const ToolFeeds* pRow = FeedsFor(*pTool, materialId);
    if (pRow == nullptr)
    {
        problem = L"T" + std::to_wstring(pTool->number) + L" " + FromUtf8(pTool->name) +
                  L" has no speeds and feeds for " + m_pTab->GetMaterialName() +
                  L". Add them to the tool on the Machine dialog's Tools tab.";
        return false;
    }
    feeds = pRow->feeds;
    return true;
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
    if (IsBulk() ? ReadBulkSettings() : ReadSettings())
        Finish(true);
}

bool COperationDlg::ReadBulkSettings()
{
    // The fields changed on each tab, from how it was first shown.
    std::vector<COperationPage::ControlState> finals;
    std::vector<std::set<int>> changes;
    for (size_t i = 0; i < m_pages.size(); ++i)
    {
        finals.push_back(m_pages[i]->Snapshot());
        changes.push_back(m_pages[i]->ChangedControls(m_baselines[i], finals.back()));
    }

    // Each operation in turn: its own values, the changed fields over them, then read back.
    std::vector<SequenceStep> results = m_bulk;
    std::wstring problems;
    int firstBad = -1;
    for (SequenceStep& step : results)
    {
        for (size_t i = 0; i < m_pages.size(); ++i)
        {
            COperationPage* pTwin = (i < m_twins.size()) ? m_twins[i].get() : nullptr;
            if (changes[i].empty() || pTwin == nullptr || pTwin->GetSafeHwnd() == nullptr)
                continue;
            pTwin->Load(step);
            pTwin->Overlay(finals[i], changes[i]);
            std::wstring found;
            if (!pTwin->Save(step, found))
            {
                AppendProblem(problems, FromUtf8(step.name) + L": " + found);
                if (firstBad < 0)
                    firstBad = static_cast<int>(i);
            }
        }
    }

    if (!problems.empty())
    {
        if (firstBad >= 0)
        {
            m_tabs.SetCurSel(firstBad);
            ShowPage(firstBad);
        }
        MessageBoxW(problems.c_str(), L"Edit Operations", MB_OK | MB_ICONWARNING);
        return false;
    }

    m_bulk = results;
    return true;
}

void COperationDlg::OnGenerate()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    // The settings go into the job first, and the dialog stays open to change
    // them and generate again.
    if (m_pTab == nullptr || !ReadSettings())
        return;
    StopSelectingFeatures();
    StopLocating();
    m_pTab->OnOperationDialogGenerate(this);
}

bool COperationDlg::ReadSettings()
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
        return false;
    }

    step.name = ToUtf8(static_cast<LPCWSTR>(name));
    m_step = step;
    return true;
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
