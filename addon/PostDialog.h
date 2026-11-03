// PostDialog.h : chooses the post and the output file before posting.

#pragma once

#include "resource.h"

class CPostDlg : public CDialog
{
public:
    // scope says what's being posted, such as "All operations" or a folder's name.
    CPostDlg(const std::wstring& scope, const std::filesystem::path& outputPath, CWnd* pParent);

    const std::filesystem::path& GetOutputPath() const { return m_outputPath; }

    enum { IDD = IDD_POST };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnBrowse();

    DECLARE_MESSAGE_MAP()

private:
    std::wstring m_scope;
    std::filesystem::path m_outputPath;
    std::filesystem::path m_confirmedPath;      // already confirmed for overwriting in Browse
    CComboBox m_post;
};
