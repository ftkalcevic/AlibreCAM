// PostDialog.cpp : chooses the post and the output file before posting.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "PostDialog.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace fs = std::filesystem;

BEGIN_MESSAGE_MAP(CPostDlg, CDialog)
    ON_BN_CLICKED(IDC_POST_BROWSE, &CPostDlg::OnBrowse)
END_MESSAGE_MAP()

CPostDlg::CPostDlg(const std::wstring& scope, const fs::path& outputPath, CWnd* pParent)
    : CDialog(IDD_POST, pParent)
    , m_scope(scope)
    , m_outputPath(outputPath)
{
}

BOOL CPostDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    SetDlgItemText(IDC_POST_SCOPE, m_scope.c_str());

    // Real posts are added with machines. Until then the built-in post writes APT CL data.
    m_post.SubclassDlgItem(IDC_POST_PROCESSOR, this);
    m_post.AddString(L"APT CL data (built in)");
    m_post.SetCurSel(0);

    SetDlgItemText(IDC_POST_FILE, m_outputPath.wstring().c_str());
    return TRUE;
}

void CPostDlg::OnBrowse()
{
    CString current;
    GetDlgItemText(IDC_POST_FILE, current);

    CFileDialog dialog(FALSE, L"cls", current, OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST,
                       L"APT CL data (*.cls)|*.cls|All files (*.*)|*.*||", this);
    if (dialog.DoModal() != IDOK)
        return;

    const CString chosen = dialog.GetPathName();
    SetDlgItemText(IDC_POST_FILE, chosen);
    m_confirmedPath = fs::path(static_cast<LPCWSTR>(chosen));
}

void CPostDlg::OnOK()
{
    CString text;
    GetDlgItemText(IDC_POST_FILE, text);
    text.Trim();
    if (text.IsEmpty())
    {
        MessageBoxW(L"Choose a file to post to.", L"Post", MB_OK | MB_ICONWARNING);
        return;
    }

    const fs::path path(static_cast<LPCWSTR>(text));
    std::error_code ignored;
    if (!path.has_parent_path() || !fs::is_directory(path.parent_path(), ignored))
    {
        MessageBoxW(L"The folder for that file doesn't exist.", L"Post", MB_OK | MB_ICONWARNING);
        return;
    }

    // Browse has already asked about replacing the file it chose.
    if (path != m_confirmedPath && fs::exists(path, ignored))
    {
        const std::wstring question = path.filename().wstring() + L" already exists.\n\nReplace it?";
        if (MessageBoxW(question.c_str(), L"Post", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }

    m_outputPath = path;
    CDialog::OnOK();
}
