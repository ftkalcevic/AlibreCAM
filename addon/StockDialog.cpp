// StockDialog.cpp : the dialogs that make or change the job's stock.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "StockDialog.h"
#include "Help.h"
#include "CamTabCommand.h"
#include "ModelReader.h"
#include "Log.h"

#include <cwchar>

using camcore::Stock;
using camcore::StockPart;
using camcore::Vec3;

namespace
{
    // Reads a number from a box. False if it's blank or not a number.
    bool ReadNumber(CWnd* pDialog, int id, double& value)
    {
        CString text;
        pDialog->GetDlgItemText(id, text);
        text.Trim();
        if (text.IsEmpty())
            return false;
        wchar_t* pEnd = nullptr;
        const double number = std::wcstod(text, &pEnd);
        if (pEnd == nullptr || *pEnd != L'\0' || !std::isfinite(number))
            return false;
        value = number;
        return true;
    }

    void SetNumber(CWnd* pDialog, int id, double value)
    {
        wchar_t text[64];
        swprintf_s(text, L"%.4f", value);
        // Trim trailing zeros, and the point if nothing follows it.
        wchar_t* pEnd = text + wcslen(text) - 1;
        while (pEnd > text && *pEnd == L'0')
            *pEnd-- = L'\0';
        if (pEnd > text && *pEnd == L'.')
            *pEnd = L'\0';
        if (wcscmp(text, L"-0") == 0)
            wcscpy_s(text, L"0");
        pDialog->SetDlgItemText(id, text);
    }

    bool ReadVec3(CWnd* pDialog, int idX, int idY, int idZ, Vec3& value)
    {
        Vec3 read;
        if (!ReadNumber(pDialog, idX, read.x) || !ReadNumber(pDialog, idY, read.y) ||
            !ReadNumber(pDialog, idZ, read.z))
            return false;
        value = read;
        return true;
    }

    void SetVec3(CWnd* pDialog, int idX, int idY, int idZ, const Vec3& value)
    {
        SetNumber(pDialog, idX, value.x);
        SetNumber(pDialog, idY, value.y);
        SetNumber(pDialog, idZ, value.z);
    }

    std::wstring JoinProblems(const std::vector<std::string>& problems)
    {
        std::wstring text;
        for (const std::string& problem : problems)
            text += (text.empty() ? L"" : L"\n") + FromUtf8(problem);
        return text;
    }

    CPoint g_lastPartBoxPosition(-1, -1);
}

/////////////////////////////////////////////////////////////////////////////
// CBoxStockDlg

BEGIN_MESSAGE_MAP(CBoxStockDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CBoxStockDlg::OnHelpButton)
    ON_BN_CLICKED(IDC_STOCK_FIT, &CBoxStockDlg::OnFit)
END_MESSAGE_MAP()

void CBoxStockDlg::OnHelpButton()
{
    ShowHelp(L"stock-box");
}

CBoxStockDlg::CBoxStockDlg(const Stock& stock, bool haveParts, const Vec3& partsLower, const Vec3& partsUpper,
                           CWnd* pParent)
    : CDialog(IDD_STOCK_BOX, pParent)
    , m_stock(stock)
    , m_haveParts(haveParts)
    , m_partsLower(partsLower)
    , m_partsUpper(partsUpper)
{
}

BOOL CBoxStockDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    // A new box starts round the parts.
    if (m_stock.kind != camcore::StockKind::Box && m_haveParts)
    {
        m_stock.boxCorner = m_partsLower;
        m_stock.boxSize = m_partsUpper - m_partsLower;
    }
    m_stock.kind = camcore::StockKind::Box;

    GetDlgItem(IDC_STOCK_FIT)->EnableWindow(m_haveParts);
    Show();
    return TRUE;
}

void CBoxStockDlg::Show()
{
    SetVec3(this, IDC_STOCK_CORNER_X, IDC_STOCK_CORNER_Y, IDC_STOCK_CORNER_Z, m_stock.boxCorner);
    SetVec3(this, IDC_STOCK_SIZE_X, IDC_STOCK_SIZE_Y, IDC_STOCK_SIZE_Z, m_stock.boxSize);
}

void CBoxStockDlg::OnFit()
{
    if (!m_haveParts)
        return;
    m_stock.boxCorner = m_partsLower;
    m_stock.boxSize = m_partsUpper - m_partsLower;
    Show();
}

void CBoxStockDlg::OnOK()
{
    Stock stock = m_stock;
    if (!ReadVec3(this, IDC_STOCK_CORNER_X, IDC_STOCK_CORNER_Y, IDC_STOCK_CORNER_Z, stock.boxCorner) ||
        !ReadVec3(this, IDC_STOCK_SIZE_X, IDC_STOCK_SIZE_Y, IDC_STOCK_SIZE_Z, stock.boxSize))
    {
        AfxMessageBox(L"Each corner and size box needs a number.", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::vector<std::string> problems = camcore::ValidateStock(stock);
    if (!problems.empty())
    {
        AfxMessageBox(JoinProblems(problems).c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    m_stock = stock;
    CDialog::OnOK();
}

/////////////////////////////////////////////////////////////////////////////
// CImportStockDlg

BEGIN_MESSAGE_MAP(CImportStockDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CImportStockDlg::OnHelpButton)
    ON_BN_CLICKED(IDC_STOCK_BROWSE, &CImportStockDlg::OnBrowse)
END_MESSAGE_MAP()

void CImportStockDlg::OnHelpButton()
{
    ShowHelp(L"stock-import");
}

CImportStockDlg::CImportStockDlg(const Stock& stock, CWnd* pParent)
    : CDialog(IDD_STOCK_IMPORT, pParent)
    , m_stock(stock)
{
}

BOOL CImportStockDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    if (m_stock.kind != camcore::StockKind::Imported)
    {
        const Stock defaults;
        m_stock.partPath.clear();
        m_stock.position = defaults.position;
        m_stock.rotation = defaults.rotation;
    }
    m_stock.kind = camcore::StockKind::Imported;

    SetDlgItemText(IDC_STOCK_FILE, FromUtf8(m_stock.partPath).c_str());
    SetVec3(this, IDC_STOCK_POS_X, IDC_STOCK_POS_Y, IDC_STOCK_POS_Z, m_stock.position);
    SetVec3(this, IDC_STOCK_ROT_X, IDC_STOCK_ROT_Y, IDC_STOCK_ROT_Z, m_stock.rotation);
    return TRUE;
}

void CImportStockDlg::OnBrowse()
{
    CString current;
    GetDlgItemText(IDC_STOCK_FILE, current);
    CFileDialog dialog(TRUE, L"AD_PRT", current, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                       L"Alibre parts (*.AD_PRT)|*.AD_PRT|All files (*.*)|*.*||", this);
    if (dialog.DoModal() == IDOK)
        SetDlgItemText(IDC_STOCK_FILE, dialog.GetPathName());
}

void CImportStockDlg::OnOK()
{
    Stock stock = m_stock;
    CString path;
    GetDlgItemText(IDC_STOCK_FILE, path);
    path.Trim();
    stock.partPath = ToUtf8(static_cast<LPCWSTR>(path));

    if (!ReadVec3(this, IDC_STOCK_POS_X, IDC_STOCK_POS_Y, IDC_STOCK_POS_Z, stock.position) ||
        !ReadVec3(this, IDC_STOCK_ROT_X, IDC_STOCK_ROT_Y, IDC_STOCK_ROT_Z, stock.rotation))
    {
        AfxMessageBox(L"Each position and rotation box needs a number.", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::vector<std::string> problems = camcore::ValidateStock(stock);
    if (!problems.empty())
    {
        AfxMessageBox(JoinProblems(problems).c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::path(static_cast<LPCWSTR>(path)), error))
    {
        AfxMessageBox(L"The part file can't be found.", MB_OK | MB_ICONWARNING);
        return;
    }
    m_stock = stock;
    CDialog::OnOK();
}

/////////////////////////////////////////////////////////////////////////////
// CPartBoxStockDlg

BEGIN_MESSAGE_MAP(CPartBoxStockDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CPartBoxStockDlg::OnHelpButton)
    ON_BN_CLICKED(IDC_STOCK_SELECT_PARTS, &CPartBoxStockDlg::OnSelectParts)
    ON_BN_CLICKED(IDC_STOCK_ALL_PARTS, &CPartBoxStockDlg::OnAllParts)
    ON_EN_CHANGE(IDC_STOCK_OFFSET_PX, &CPartBoxStockDlg::OnOffsetChanged)
    ON_EN_CHANGE(IDC_STOCK_OFFSET_MX, &CPartBoxStockDlg::OnOffsetChanged)
    ON_EN_CHANGE(IDC_STOCK_OFFSET_PY, &CPartBoxStockDlg::OnOffsetChanged)
    ON_EN_CHANGE(IDC_STOCK_OFFSET_MY, &CPartBoxStockDlg::OnOffsetChanged)
    ON_EN_CHANGE(IDC_STOCK_OFFSET_PZ, &CPartBoxStockDlg::OnOffsetChanged)
    ON_EN_CHANGE(IDC_STOCK_OFFSET_MZ, &CPartBoxStockDlg::OnOffsetChanged)
    ON_WM_CLOSE()
END_MESSAGE_MAP()

void CPartBoxStockDlg::OnHelpButton()
{
    ShowHelp(L"stock-part-box");
}

CPartBoxStockDlg::CPartBoxStockDlg(CCamTabCommand* pTab, const Stock& stock)
    : CDialog(IDD_STOCK_PART_BOX, nullptr)
    , m_pTab(pTab)
    , m_stock(stock)
    , m_selecting(false)
    , m_filling(false)
{
    if (m_stock.kind != camcore::StockKind::PartBox)
    {
        const Stock defaults;
        m_stock = defaults;
    }
    m_stock.kind = camcore::StockKind::PartBox;
}

bool CPartBoxStockDlg::CreateModeless(CWnd* pParent)
{
    return Create(IDD_STOCK_PART_BOX, pParent) != FALSE;
}

BOOL CPartBoxStockDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    m_filling = true;
    SetNumber(this, IDC_STOCK_OFFSET_PX, m_stock.offsetPlusX);
    SetNumber(this, IDC_STOCK_OFFSET_MX, m_stock.offsetMinusX);
    SetNumber(this, IDC_STOCK_OFFSET_PY, m_stock.offsetPlusY);
    SetNumber(this, IDC_STOCK_OFFSET_MY, m_stock.offsetMinusY);
    SetNumber(this, IDC_STOCK_OFFSET_PZ, m_stock.offsetPlusZ);
    SetNumber(this, IDC_STOCK_OFFSET_MZ, m_stock.offsetMinusZ);
    m_filling = false;

    ShowParts();
    SetDlgItemText(IDC_STOCK_PROMPT, L"Click Select Parts to choose parts in the view.");

    if (g_lastPartBoxPosition.x >= 0)
        SetWindowPos(nullptr, g_lastPartBoxPosition.x, g_lastPartBoxPosition.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    return TRUE;
}

void CPartBoxStockDlg::PostNcDestroy()
{
    delete this;
}

void CPartBoxStockDlg::ShowParts()
{
    CListBox* pList = static_cast<CListBox*>(GetDlgItem(IDC_STOCK_PARTS));
    pList->ResetContent();
    if (m_stock.parts.empty())
        pList->AddString(L"All parts");
    for (const StockPart& part : m_stock.parts)
        pList->AddString(FromUtf8(part.name).c_str());
    GetDlgItem(IDC_STOCK_ALL_PARTS)->EnableWindow(!m_stock.parts.empty());
}

void CPartBoxStockDlg::OnSelectParts()
{
    if (m_selecting)
    {
        StopSelectingParts();
        if (m_pTab != nullptr)
            m_pTab->EndPartSelect();
        return;
    }

    m_selecting = true;
    SetDlgItemText(IDC_STOCK_SELECT_PARTS, L"Done");
    SetDlgItemText(IDC_STOCK_PROMPT, L"Click parts in the view. A listed part is taken off the list. "
                                     L"Click Done when finished.");
    if (m_pTab != nullptr)
        m_pTab->BeginPartSelect();
}

void CPartBoxStockDlg::StopSelectingParts()
{
    m_selecting = false;
    SetDlgItemText(IDC_STOCK_SELECT_PARTS, L"Select Parts");
    SetDlgItemText(IDC_STOCK_PROMPT, L"Click Select Parts to choose parts in the view.");
}

void CPartBoxStockDlg::ToggleParts(const std::vector<StockPart>& parts)
{
    for (const StockPart& part : parts)
    {
        const auto listed = std::find_if(m_stock.parts.begin(), m_stock.parts.end(), [&](const StockPart& other)
                                         { return other.occurrenceKey.hex == part.occurrenceKey.hex; });
        if (listed != m_stock.parts.end())
            m_stock.parts.erase(listed);
        else
            m_stock.parts.push_back(part);
    }
    ShowParts();
    if (m_pTab != nullptr)
        m_pTab->OnStockPreviewChanged();
}

void CPartBoxStockDlg::OnAllParts()
{
    m_stock.parts.clear();
    ShowParts();
    if (m_pTab != nullptr)
        m_pTab->OnStockPreviewChanged();
}

bool CPartBoxStockDlg::ReadOffsets(std::wstring& problem)
{
    Stock stock = m_stock;
    if (!ReadNumber(this, IDC_STOCK_OFFSET_PX, stock.offsetPlusX) ||
        !ReadNumber(this, IDC_STOCK_OFFSET_MX, stock.offsetMinusX) ||
        !ReadNumber(this, IDC_STOCK_OFFSET_PY, stock.offsetPlusY) ||
        !ReadNumber(this, IDC_STOCK_OFFSET_MY, stock.offsetMinusY) ||
        !ReadNumber(this, IDC_STOCK_OFFSET_PZ, stock.offsetPlusZ) ||
        !ReadNumber(this, IDC_STOCK_OFFSET_MZ, stock.offsetMinusZ))
    {
        problem = L"Each offset box needs a number.";
        return false;
    }
    const std::vector<std::string> problems = camcore::ValidateStock(stock);
    if (!problems.empty())
    {
        problem = JoinProblems(problems);
        return false;
    }
    m_stock = stock;
    return true;
}

void CPartBoxStockDlg::OnOffsetChanged()
{
    if (m_filling)
        return;
    std::wstring problem;
    if (ReadOffsets(problem))
    {
        if (m_pTab != nullptr)
            m_pTab->OnStockPreviewChanged();
    }
    else
    {
        ShowResult(problem);
    }
}

void CPartBoxStockDlg::ShowResult(const std::wstring& text)
{
    SetDlgItemText(IDC_STOCK_RESULT, text.c_str());
}

void CPartBoxStockDlg::OnOK()
{
    std::wstring problem;
    if (!ReadOffsets(problem))
    {
        AfxMessageBox(problem.c_str(), MB_OK | MB_ICONWARNING);
        return;
    }
    Finish(true);
}

void CPartBoxStockDlg::OnCancel()
{
    Finish(false);
}

void CPartBoxStockDlg::OnClose()
{
    Finish(false);
}

void CPartBoxStockDlg::Finish(bool apply)
{
    CRect rect;
    GetWindowRect(&rect);
    g_lastPartBoxPosition = rect.TopLeft();

    if (m_pTab != nullptr)
    {
        CCamTabCommand* pTab = m_pTab;
        m_pTab = nullptr;
        pTab->OnPartBoxDialogDone(this, apply);
    }
    DestroyWindow();
}
