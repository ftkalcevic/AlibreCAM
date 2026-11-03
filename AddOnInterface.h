// AddOnInterface.h : the IAlibreAddOn implementation for AlibreCAM.
//
// Menu tree exposed to Alibre:
//
//   AlibreCAM            (root, id 100)  -> shown as a dedicated ribbon tab
//     About             (leaf, id 101)  -> stateless command: shows About box
//
// Commands are "fire-and-forget": InvokeCommand does the work and returns a
// NULL IAlibreAddOnCommand, so no command objects or lifetimes to manage.

#pragma once

// Menu ids (any non-zero values unique within this add-on).
const long MENU_ID_ROOT  = 100;
const long MENU_ID_ABOUT = 101;

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

private:
    void ShowAboutBox();
    static CString GetModuleFolder();

    long m_nRefCount;
};
