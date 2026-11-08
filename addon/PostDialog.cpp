// PostDialog.cpp : chooses the post and the output file before posting.

#include "stdafx.h"
#include "CAMForAlibre.h"
#include "PostDialog.h"
#include "Help.h"

#include <algorithm>
#include <map>

#include "FileIO.h"
#include "Log.h"
#include "ModelReader.h"
#include "ScriptedPost.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace fs = std::filesystem;

namespace
{
    std::wstring LowerCase(std::wstring text)
    {
        for (wchar_t& c : text)
            c = static_cast<wchar_t>(towlower(c));
        return text;
    }

    // Each .post file in a folder, by its file name in lower case.
    void AddPosts(const fs::path& folder, std::map<std::wstring, fs::path>& found)
    {
        std::error_code error;
        if (!fs::is_directory(folder, error))
            return;
        for (const fs::directory_entry& entry : fs::directory_iterator(folder, error))
        {
            if (entry.is_regular_file(error) && LowerCase(entry.path().extension().wstring()) == L".post")
                found[LowerCase(entry.path().filename().wstring())] = entry.path();
        }
    }
}

std::vector<PostChoice> FindPosts()
{
    // Installed beside the add-on, then the user's, which replace any of the same name.
    std::map<std::wstring, fs::path> found;
    wchar_t module[MAX_PATH] = {};
    if (GetModuleFileNameW(AfxGetInstanceHandle(), module, MAX_PATH) > 0)
        AddPosts(fs::path(module).parent_path() / L"posts", found);
    wchar_t appData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        AddPosts(fs::path(appData) / L"CAMForAlibre" / L"posts", found);

    std::vector<PostChoice> posts;
    for (const auto& file : found)
    {
        PostChoice choice;
        choice.script = file.second;
        choice.extension = L"ngc";
        std::string text;
        const camcore::PostScriptRead read = camcore::ReadWholeFile(file.second, text)
                                                 ? camcore::ReadPostScript(text)
                                                 : camcore::PostScriptRead();
        if (read.ok)
        {
            choice.name = FromUtf8(read.script.name);
            choice.extension = FromUtf8(read.script.extension);
        }
        else
        {
            choice.name = file.second.filename().wstring() + L" (has a mistake)";
            Log(L"[post] %s: %s", file.second.c_str(), FromUtf8(read.problem).c_str());
        }
        posts.push_back(choice);
    }
    std::sort(posts.begin(), posts.end(),
              [](const PostChoice& a, const PostChoice& b) { return LowerCase(a.name) < LowerCase(b.name); });

    PostChoice builtIn;
    builtIn.name = L"APT CL data (built in)";
    builtIn.extension = L"cls";
    posts.push_back(builtIn);
    return posts;
}

BEGIN_MESSAGE_MAP(CPostDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CPostDlg::OnHelpButton)
    ON_BN_CLICKED(IDC_POST_BROWSE, &CPostDlg::OnBrowse)
    ON_CBN_SELCHANGE(IDC_POST_PROCESSOR, &CPostDlg::OnPostChanged)
END_MESSAGE_MAP()

void CPostDlg::OnHelpButton()
{
    ShowHelp(L"post");
}

CPostDlg::CPostDlg(const std::wstring& scope, const fs::path& outputPath, const std::vector<PostChoice>& posts,
                   size_t chosen, CWnd* pParent)
    : CDialog(IDD_POST, pParent)
    , m_scope(scope)
    , m_outputPath(outputPath)
    , m_posts(posts)
    , m_chosen(chosen < posts.size() ? chosen : 0)
{
}

BOOL CPostDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    SetDlgItemText(IDC_POST_SCOPE, m_scope.c_str());

    // The scripted posts, then the built-in APT CL data.
    m_post.SubclassDlgItem(IDC_POST_PROCESSOR, this);
    for (const PostChoice& post : m_posts)
        m_post.AddString(post.name.c_str());
    m_post.SetCurSel(static_cast<int>(m_chosen));

    SetDlgItemText(IDC_POST_FILE, m_outputPath.wstring().c_str());
    OnPostChanged();
    return TRUE;
}

void CPostDlg::OnPostChanged()
{
    // The file takes the chosen post's extension.
    const int selected = m_post.GetCurSel();
    if (selected < 0 || static_cast<size_t>(selected) >= m_posts.size())
        return;
    m_chosen = static_cast<size_t>(selected);

    CString text;
    GetDlgItemText(IDC_POST_FILE, text);
    fs::path path(static_cast<LPCWSTR>(text));
    if (!path.empty())
    {
        path.replace_extension(L"." + m_posts[m_chosen].extension);
        SetDlgItemText(IDC_POST_FILE, path.wstring().c_str());
    }
}

void CPostDlg::OnBrowse()
{
    CString current;
    GetDlgItemText(IDC_POST_FILE, current);

    const std::wstring extension = m_posts[m_chosen].extension;
    const std::wstring filter = m_posts[m_chosen].name + L" (*." + extension + L")|*." + extension +
                                L"|All files (*.*)|*.*||";
    CFileDialog dialog(FALSE, extension.c_str(), current, OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST,
                       filter.c_str(), this);
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
