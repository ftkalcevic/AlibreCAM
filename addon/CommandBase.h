// CommandBase.h : default IAlibreAddOnCommand behaviour.
//
// Holds the COM plumbing and the command site, and leaves every user event
// unhandled so normal Alibre selection and view controls keep working.

#pragma once

class CCommandBase : public IAlibreAddOnCommand
{
public:
    explicit CCommandBase(const wchar_t* name);
    virtual ~CCommandBase();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppObj) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IDispatch - Alibre only uses the vtable.
    STDMETHOD(GetTypeInfoCount)(UINT* pctinfo) override;
    STDMETHOD(GetTypeInfo)(UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo) override;
    STDMETHOD(GetIDsOfNames)(REFIID riid, LPOLESTR* rgszNames, UINT cNames,
                             LCID lcid, DISPID* rgDispId) override;
    STDMETHOD(Invoke)(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags,
                      DISPPARAMS* pDispParams, VARIANT* pVarResult,
                      EXCEPINFO* pExcepInfo, UINT* puArgErr) override;

    // IAlibreAddOnCommand
    STDMETHOD(putref_CommandSite)(struct IADAddOnCommandSite* pSite) override;
    STDMETHOD(get_CommandSite)(struct IADAddOnCommandSite** ppSite) override;
    STDMETHOD(IsTwoWayToggle)(VARIANT_BOOL* pValue) override;
    STDMETHOD(AddTab)(VARIANT_BOOL* pValue) override;
    STDMETHOD(OnShowUI)(__int64 hWnd) override;
    STDMETHOD(OnRender)(long hDC, long clipRectX, long clipRectY,
                        long clipRectWidth, long clipRectHeight) override;
    STDMETHOD(OnClick)(long screenX, long screenY, enum ADDONMouseButtons buttons,
                       VARIANT_BOOL* pHandled) override;
    STDMETHOD(OnDoubleClick)(long screenX, long screenY, VARIANT_BOOL* pHandled) override;
    STDMETHOD(OnMouseDown)(long screenX, long screenY, enum ADDONMouseButtons buttons,
                           VARIANT_BOOL* pHandled) override;
    STDMETHOD(OnMouseMove)(long screenX, long screenY, enum ADDONMouseButtons buttons,
                           VARIANT_BOOL* pHandled) override;
    STDMETHOD(OnMouseUp)(long screenX, long screenY, enum ADDONMouseButtons buttons,
                         VARIANT_BOOL* pHandled) override;
    STDMETHOD(OnSelectionChange)() override;
    STDMETHOD(OnTerminate)() override;
    STDMETHOD(OnComplete)() override;
    STDMETHOD(OnKeyDown)(long keycode, VARIANT_BOOL* pHandled) override;
    STDMETHOD(OnKeyUp)(long keycode, VARIANT_BOOL* pHandled) override;
    STDMETHOD(OnEscape)(VARIANT_BOOL* pHandled) override;
    STDMETHOD(OnMouseWheel)(double delta, VARIANT_BOOL* pHandled) override;
    STDMETHOD(get_TabName)(BSTR* pName) override;
    STDMETHOD(get_Extents)(SAFEARRAY** ppExtents) override;
    STDMETHOD(On3DRender)() override;

protected:
    const wchar_t* m_name;
    IADAddOnCommandSitePtr m_site;

private:
    long m_refCount;
};
