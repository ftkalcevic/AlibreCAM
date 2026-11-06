// StockDialog.h : the dialogs that make or change the job's stock.
//
// Box Stock and Imported Stock are modal. Part Box Stock is modeless, so parts
// can be clicked in the view while it's open; the tab passes it each click.

#pragma once

#include "resource.h"
#include "Stock.h"

class CCamTabCommand;

// A box from entered dimensions and position, in the assembly's axes.
class CBoxStockDlg : public CDialog
{
public:
    // haveParts says whether partsLower and partsUpper hold the parts' box, for Fit to Parts.
    CBoxStockDlg(const camcore::Stock& stock, bool haveParts, const camcore::Vec3& partsLower,
                 const camcore::Vec3& partsUpper, CWnd* pParent);

    const camcore::Stock& GetStock() const { return m_stock; }

    enum { IDD = IDD_STOCK_BOX };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;

    afx_msg void OnFit();
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    void Show();

    camcore::Stock m_stock;
    bool m_haveParts;
    camcore::Vec3 m_partsLower;
    camcore::Vec3 m_partsUpper;
};

// Another Alibre part, placed by its position and rotation.
class CImportStockDlg : public CDialog
{
public:
    CImportStockDlg(const camcore::Stock& stock, CWnd* pParent);

    const camcore::Stock& GetStock() const { return m_stock; }

    enum { IDD = IDD_STOCK_IMPORT };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;

    afx_msg void OnBrowse();
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    camcore::Stock m_stock;
};

// A box around some or all of the parts, plus six offsets. Modeless.
class CPartBoxStockDlg : public CDialog
{
public:
    CPartBoxStockDlg(CCamTabCommand* pTab, const camcore::Stock& stock);

    bool CreateModeless(CWnd* pParent);
    void DetachFromTab() { m_pTab = nullptr; }

    // The stock as edited so far. Its offsets are the last valid ones typed.
    const camcore::Stock& GetStock() const { return m_stock; }

    // Select Parts: each part clicked in the view is added, or removed if it's listed.
    bool IsSelectingParts() const { return m_selecting; }
    void StopSelectingParts();
    void ToggleParts(const std::vector<camcore::StockPart>& parts);

    // The resulting box's size, or why there isn't one.
    void ShowResult(const std::wstring& text);

    enum { IDD = IDD_STOCK_PART_BOX };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;
    void PostNcDestroy() override;

    afx_msg void OnSelectParts();
    afx_msg void OnAllParts();
    afx_msg void OnOffsetChanged();
    afx_msg void OnClose();
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    void ShowParts();
    bool ReadOffsets(std::wstring& problem);
    void Finish(bool apply);

    CCamTabCommand* m_pTab;
    camcore::Stock m_stock;
    bool m_selecting;
    bool m_filling;
};
