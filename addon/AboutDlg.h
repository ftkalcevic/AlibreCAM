// AboutDlg.h : the CAMForAlibre About box.

#pragma once

#include "resource.h"

class CAboutDlg : public CDialog
{
public:
    explicit CAboutDlg(CWnd* pParent = nullptr);

    enum { IDD = IDD_ABOUTBOX };

protected:
    BOOL OnInitDialog() override;

    DECLARE_MESSAGE_MAP()
};
