// Loaded into explorer.exe by WallpaperSystem through XAML diagnostics (the same
// route TranslucentTB uses). Hides the taskbar's XAML background so it is clear.
// Event driven: XAML calls back only when taskbar elements are created, so this
// costs nothing between those moments.
#include <windows.h>
#undef GetCurrentTime  // winbase.h macro clashes with a XAML method name.
#include <ocidl.h>
#include <xamlom.h>
#include <windows.ui.xaml.h>
#pragma warning(disable : 4324)  // Padding inside WRL's own module storage.
#include <wrl.h>

#include <string_view>
#include <unordered_set>

#include "clear_taskbar.h"

using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

class ClearTaskbarTap
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IObjectWithSite,
                          ChainInterfaces<IVisualTreeServiceCallback2, IVisualTreeServiceCallback>>
{
public:
    IFACEMETHODIMP SetSite(IUnknown* site) override
    {
        m_diagnostics.Reset();
        if (!site)
            return S_OK;
        const HRESULT hr = site->QueryInterface(IID_PPV_ARGS(&m_diagnostics));
        if (FAILED(hr))
            return hr;

        // Advising from this thread hangs XAML (TranslucentTB and Windhawk hit the same).
        AddRef();
        const HANDLE thread = CreateThread(nullptr, 0, &Advise, this, 0, nullptr);
        if (!thread)
        {
            Release();
            return HRESULT_FROM_WIN32(GetLastError());
        }
        CloseHandle(thread);
        return S_OK;
    }

    IFACEMETHODIMP GetSite(REFIID iid, void** site) override
    {
        return m_diagnostics ? m_diagnostics.CopyTo(iid, site) : E_FAIL;
    }

    // Visual tree: Taskbar.TaskbarBackground > Grid > Rectangle#BackgroundFill / #BackgroundStroke.
    IFACEMETHODIMP OnVisualTreeChange(ParentChildRelation relation, VisualElement element,
                                      VisualMutationType mutation) override
    {
        if (mutation == Remove)
        {
            m_backgrounds.erase(element.Handle);
            m_grids.erase(element.Handle);
            return S_OK;
        }

        const std::wstring_view type = element.Type ? element.Type : L"";
        const std::wstring_view name = element.Name ? element.Name : L"";
        if (type == L"Taskbar.TaskbarBackground")
            m_backgrounds.insert(element.Handle);
        else if (type == L"Windows.UI.Xaml.Controls.Grid" && m_backgrounds.count(relation.Parent))
            m_grids.insert(element.Handle);
        else if (type == L"Windows.UI.Xaml.Shapes.Rectangle" && m_grids.count(relation.Parent) &&
                 (name == L"BackgroundFill" || name == L"BackgroundStroke"))
            Hide(element.Handle);
        return S_OK;
    }

    IFACEMETHODIMP OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) override
    {
        return S_OK;
    }

private:
    static DWORD WINAPI Advise(void* parameter)
    {
        ClearTaskbarTap* const self = static_cast<ClearTaskbarTap*>(parameter);
        ComPtr<IVisualTreeService> tree;
        if (self->m_diagnostics && SUCCEEDED(self->m_diagnostics.As(&tree)))
            tree->AdviseVisualTreeChange(self);
        self->Release();
        return 0;
    }

    // Opacity, not Fill = null, so the empty taskbar still takes clicks.
    void Hide(InstanceHandle handle)
    {
        ComPtr<IInspectable> object;
        ComPtr<ABI::Windows::UI::Xaml::IUIElement> element;
        if (m_diagnostics && SUCCEEDED(m_diagnostics->GetIInspectableFromHandle(handle, &object)) &&
            SUCCEEDED(object.As(&element)))
            element->put_Opacity(0.0);
    }

    ComPtr<IXamlDiagnostics> m_diagnostics;
    std::unordered_set<InstanceHandle> m_backgrounds;
    std::unordered_set<InstanceHandle> m_grids;
};

CoCreatableClass(ClearTaskbarTap);

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object)
{
    return Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule().GetClassObject(clsid, iid,
                                                                                     object);
}

// Stays loaded until Explorer exits; WallpaperSystem restarts Explorer to remove it.
STDAPI DllCanUnloadNow()
{
    return S_FALSE;
}
