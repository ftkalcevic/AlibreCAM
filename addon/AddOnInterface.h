// AddOnInterface.h : the IAlibreAddOn implementation for CAMForAlibre.
//
// Menu tree exposed to Alibre:
//
//   CAMForAlibre            (root, id 100)  -> shown as a dedicated ribbon tab
//     CAM Tab           (leaf, id 102)  -> two-way toggle: Design Explorer tab
//     About             (leaf, id 101)  -> stateless command: shows About box

#pragma once

class CCamTabCommand;

// Menu ids (any non-zero values unique within this add-on).
const long MENU_ID_ROOT    = 100;
const long MENU_ID_ABOUT   = 101;
const long MENU_ID_CAM_TAB = 102;

// Oldest Alibre build this add-on is written against (27.0.1.27039).
const long MINIMUM_ALIBRE_BUILD = 27039;

class CAddOnInterface : public IAlibreAddOn
{
public:
    CAddOnInterface();
    virtual ~CAddOnInterface();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppObj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IDispatch - Alibre only uses the vtable, so these are stubs.
    STDMETHOD(GetTypeInfoCount)(UINT* pctinfo) override;
    STDMETHOD(GetTypeInfo)(UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo) override;
    STDMETHOD(GetIDsOfNames)(REFIID riid, LPOLESTR* rgszNames, UINT cNames,
                             LCID lcid, DISPID* rgDispId) override;
    STDMETHOD(Invoke)(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags,
                      DISPPARAMS* pDispParams, VARIANT* pVarResult,
                      EXCEPINFO* pExcepInfo, UINT* puArgErr) override;

    // IAlibreAddOn
    STDMETHOD(get_RootMenuItem)(long* pRootMenuID) override;
    STDMETHOD(HasSubMenus)(long menuID, VARIANT_BOOL* pHasSubMenus) override;
    STDMETHOD(SubMenuItems)(long menuID, SAFEARRAY** pSubMenuIDs) override;
    STDMETHOD(MenuItemText)(long menuID, BSTR* pMenuDisplayText) override;
    STDMETHOD(MenuItemState)(long menuID, BSTR sessionIdentifier,
                             enum ADDONMenuStates* pType) override;
    STDMETHOD(MenuItemToolTip)(long menuID, BSTR* pToolTip) override;
    STDMETHOD(PopupMenu)(long menuID, VARIANT_BOOL* pIsPopup) override;
    STDMETHOD(HasPersistentDataToSave)(BSTR sessionIdentifier,
                                       VARIANT_BOOL* pHasDataToSave) override;
    STDMETHOD(InvokeCommand)(long menuID, BSTR sessionIdentifier,
                             struct IAlibreAddOnCommand** pCommand) override;
    STDMETHOD(LoadData)(struct IStream* pCustomData, BSTR sessionIdentifier) override;
    STDMETHOD(SaveData)(struct IStream* pCustomData, BSTR sessionIdentifier) override;
    STDMETHOD(setIsAddOnLicensed)(VARIANT_BOOL isLicensed) override;

    // Spec version 2 additions (need specificationVersion="2" in the .adc)
    STDMETHOD(MenuIcon)(long menuID, BSTR* pMenuIconPath) override;
    STDMETHOD(UseDedicatedRibbonTab)(VARIANT_BOOL* pFlag) override;

    // CAM tabs, one per assembly workspace
    void RemoveTabForSession(const std::wstring& sessionId);
    void CloseAllTabs();

private:
    void ShowAboutBox();
    static CString GetModuleFolder();
    static bool IsAssemblySession(BSTR sessionIdentifier);
    static bool IsAlibreVersionSupported();

    long m_nRefCount;
    std::map<std::wstring, CCamTabCommand*> m_tabs;     // each entry holds a reference
};
