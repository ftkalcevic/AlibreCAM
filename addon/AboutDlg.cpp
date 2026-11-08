// AboutDlg.cpp : the CAMForAlibre About box.

#include "stdafx.h"
#include "CAMForAlibre.h"
#include "AboutDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
END_MESSAGE_MAP()

CAboutDlg::CAboutDlg(CWnd* pParent)
    : CDialog(IDD_ABOUTBOX, pParent)
{
}

BOOL CAboutDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    CString addOnVersion;
    addOnVersion.Format(L"Version %s", CAMFORALIBRE_VERSION_STRING);
    SetDlgItemText(IDC_ADDON_VERSION, addOnVersion);

    CString alibreVersion;
    if (theApp.m_strAlibreVersion.IsEmpty())
        alibreVersion = L"Running in Alibre Design";
    else
        alibreVersion.Format(L"Running in Alibre Design %s", (LPCWSTR)theApp.m_strAlibreVersion);
    SetDlgItemText(IDC_ALIBRE_VERSION, alibreVersion);

    return TRUE;    // let the dialog set focus to the OK button
}
