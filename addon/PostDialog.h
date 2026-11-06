// PostDialog.h : chooses the post and the output file before posting.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "resource.h"

// A post the dialog offers: a post script, or the built-in APT CL data.
struct PostChoice
{
    std::wstring name;              // shown in the list
    std::filesystem::path script;   // empty for the built-in post
    std::wstring extension;         // the output file's, without the dot
};

// The posts there are: the installed posts folder beside the add-on, with the
// user's %APPDATA%\AlibreCAM\posts\ adding more and replacing any of the same
// file name, sorted by name, then the built-in APT CL data last. A script that
// can't be read is listed with its file name, so posting with it says why.
std::vector<PostChoice> FindPosts();

class CPostDlg : public CDialog
{
public:
    // scope says what's being posted, such as "All operations" or a folder's name.
    CPostDlg(const std::wstring& scope, const std::filesystem::path& outputPath, const std::vector<PostChoice>& posts,
             size_t chosen, CWnd* pParent);

    const std::filesystem::path& GetOutputPath() const { return m_outputPath; }
    size_t GetChosen() const { return m_chosen; }

    enum { IDD = IDD_POST };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnBrowse();
    afx_msg void OnPostChanged();
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    std::wstring m_scope;
    std::filesystem::path m_outputPath;
    std::filesystem::path m_confirmedPath;      // already confirmed for overwriting in Browse
    std::vector<PostChoice> m_posts;
    size_t m_chosen;
    CComboBox m_post;
};
