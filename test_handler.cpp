#define UNICODE
#define _UNICODE

#include "nefprop.h"

#include <objbase.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

static void PrintStore(IPropertyStore* store) {
    DWORD count = 0;
    store->GetCount(&count);
    std::wprintf(L"count %lu\n", count);
    const wchar_t* want[] = {
        L"System.Photo.CameraManufacturer", L"System.Photo.CameraModel",
        L"System.Photo.DateTaken",          L"PhotoGeoTagger.DateTaken",
        L"System.Photo.ExposureTime",
        L"System.Photo.FNumber",            L"System.Photo.ISOSpeed",
        L"System.Photo.FocalLength",        L"System.Photo.FocalLengthInFilm",
        L"System.Photo.LensModel",          L"System.Photo.Orientation",
        L"System.Image.HorizontalSize",     L"System.Image.VerticalSize",
        L"System.Image.Dimensions",         L"System.GPS.Latitude",             L"System.GPS.LatitudeRef",
        L"System.GPS.LatitudeNumerator",    L"System.GPS.Longitude",
        L"System.GPS.LongitudeRef",         L"System.GPS.Altitude",
        L"System.GPS.AltitudeRef",          L"System.GPS.AltitudeNumerator",
    };
    for (const wchar_t* name : want) {
        PROPERTYKEY key{};
        if (FAILED(PSGetPropertyKeyFromName(name, &key))) {
            std::wprintf(L"  %ls = MISSING KEY\n", name);
            continue;
        }
        PROPVARIANT pv{};
        PropVariantInit(&pv);
        store->GetValue(key, &pv);
        wchar_t* text = nullptr;
        if (FAILED(PropVariantToStringAlloc(pv, &text)) || text == nullptr) {
            std::wprintf(L"  %ls = (empty vt=%u)\n", name, pv.vt);
        } else {
            std::wprintf(L"  %ls = %ls\n", name, text);
            CoTaskMemFree(text);
        }
        PropVariantClear(&pv);
    }
}

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 3) {
        std::fwprintf(stderr, L"usage: test_handler.exe NefPropHandler.dll file.nef\n");
        return 2;
    }
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) return 1;

    if (argc >= 3 && wcscmp(argv[1], L"lookup") == 0) {
        IPropertyStore* store = nullptr;
        HRESULT hr = SHGetPropertyStoreFromParsingName(argv[2], nullptr, GPS_DEFAULT, IID_IPropertyStore,
                                                       reinterpret_cast<void**>(&store));
        if (FAILED(hr)) {
            std::fwprintf(stderr, L"lookup 0x%08lx\n", static_cast<unsigned long>(hr));
            LocalFree(argv);
            return 1;
        }
        PrintStore(store);
        store->Release();
        LocalFree(argv);
        CoUninitialize();
        return 0;
    }

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) {
        std::fwprintf(stderr, L"LoadLibrary failed %lu\n", GetLastError());
        return 1;
    }
    using GetClass = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    auto get = reinterpret_cast<GetClass>(GetProcAddress(dll, "DllGetClassObject"));
    if (!get) {
        std::fwprintf(stderr, L"DllGetClassObject missing\n");
        return 1;
    }
    IClassFactory* factory = nullptr;
    HRESULT hr = get(CLSID_NefPropertyHandler, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"class factory 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    IInitializeWithFile* init = nullptr;
    hr = factory->CreateInstance(nullptr, IID_IInitializeWithFile, reinterpret_cast<void**>(&init));
    factory->Release();
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"create 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    hr = init->Initialize(argv[2], STGM_READ);
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"init 0x%08lx\n", static_cast<unsigned long>(hr));
        init->Release();
        return 1;
    }
    IPropertyStore* store = nullptr;
    hr = init->QueryInterface(IID_IPropertyStore, reinterpret_cast<void**>(&store));
    init->Release();
    if (FAILED(hr)) return 1;
    PrintStore(store);
    store->Release();
    FreeLibrary(dll);
    LocalFree(argv);
    CoUninitialize();
    return 0;
}
