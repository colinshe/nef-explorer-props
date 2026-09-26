/* Explorer property handler for Nikon NEF/NRW.
   Fills camera, lens, exposure, and GPS fields that PhotoMetadataHandler leaves blank.
   Read-only. Thumbnails stay on the Windows photo thumbnail provider. */

#define UNICODE
#define _UNICODE
#define INITGUID

#include "nefprop.h"

#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>
#include <propsys.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

constexpr uint16_t kExifIfd = 34665;
constexpr uint16_t kGpsIfd = 34853;
constexpr int kMaxDepth = 6;
constexpr uint32_t kMaxEntries = 512;
constexpr uint32_t kMaxValue = 65536;

struct Meta {
    std::wstring make;
    std::wstring model;
    std::wstring lensMake;
    std::wstring lensModel;
    bool hasDate = false;
    SYSTEMTIME date{};
    bool hasOffset = false;
    int offsetMin = 0;
    bool hasExposure = false;
    double exposure = 0;
    bool hasFNumber = false;
    double fNumber = 0;
    bool hasIso = false;
    uint32_t iso = 0;
    bool hasFocal = false;
    double focal = 0;
    bool hasFocal35 = false;
    uint32_t focal35 = 0;
    bool hasOrientation = false;
    uint16_t orientation = 1;
    bool hasBias = false;
    double bias = 0;
    bool hasGpsLat = false;
    double lat[3]{};
    uint32_t latNum[3]{};
    uint32_t latDen[3]{1, 1, 1};
    char latRef = 'N';
    bool hasGpsLon = false;
    double lon[3]{};
    uint32_t lonNum[3]{};
    uint32_t lonDen[3]{1, 1, 1};
    char lonRef = 'E';
    bool hasAlt = false;
    double alt = 0;
    uint32_t altNum = 0;
    uint32_t altDen = 1;
    bool altBelow = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitDepth = 0;
};

struct Entry {
    PROPERTYKEY key{};
    PROPVARIANT value{};
};

long g_locks = 0;

void LockModule() { InterlockedIncrement(&g_locks); }
void UnlockModule() { InterlockedDecrement(&g_locks); }

bool IsEmpty(const PROPVARIANT& v) {
    if (v.vt == VT_EMPTY || v.vt == VT_NULL) return true;
    if (v.vt == VT_LPWSTR) return v.pwszVal == nullptr || v.pwszVal[0] == L'\0';
    return false;
}

bool SameKey(const PROPERTYKEY& a, const PROPERTYKEY& b) {
    return a.pid == b.pid && IsEqualGUID(a.fmtid, b.fmtid);
}

uint16_t ReadU16(const uint8_t* p, bool be) {
    return be ? static_cast<uint16_t>((p[0] << 8) | p[1])
              : static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t ReadU32(const uint8_t* p, bool be) {
    return be ? (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                    (static_cast<uint32_t>(p[2]) << 8) | p[3]
              : static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

int32_t ReadI32(const uint8_t* p, bool be) { return static_cast<int32_t>(ReadU32(p, be)); }

uint32_t ComponentSize(uint16_t type) {
    switch (type) {
        case 1:
        case 2:
        case 6:
        case 7:
            return 1;
        case 3:
        case 8:
            return 2;
        case 4:
        case 9:
        case 11:
            return 4;
        case 5:
        case 10:
        case 12:
            return 8;
        default:
            return 0;
    }
}

bool ReadAt(IStream* stream, uint64_t offset, void* buf, uint32_t size) {
    if (size == 0) return true;
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (FAILED(stream->Seek(pos, STREAM_SEEK_SET, nullptr))) return false;
    ULONG got = 0;
    if (FAILED(stream->Read(buf, size, &got))) return false;
    return got == size;
}

bool ReadValue(IStream* stream, uint64_t entryOff, bool be, uint16_t type, uint32_t count,
               std::vector<uint8_t>& out) {
    const uint32_t comp = ComponentSize(type);
    if (comp == 0 || count == 0) return false;
    if (count > kMaxValue / comp) return false;
    const uint32_t total = comp * count;
    uint8_t head[4]{};
    if (!ReadAt(stream, entryOff + 8, head, 4)) return false;
    out.resize(total);
    if (total <= 4) {
        memcpy(out.data(), head, total);
        return true;
    }
    const uint32_t off = ReadU32(head, be);
    return ReadAt(stream, off, out.data(), total);
}

std::wstring AsciiToWide(const uint8_t* p, uint32_t n) {
    while (n > 0 && (p[n - 1] == 0 || p[n - 1] == ' ')) --n;
    uint32_t start = 0;
    while (start < n && p[start] == ' ') ++start;
    if (start >= n) return L"";
    const int len = static_cast<int>(n - start);
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    reinterpret_cast<const char*>(p + start), len, nullptr, 0);
    UINT cp = CP_UTF8;
    if (need <= 0) {
        cp = CP_ACP;
        need = MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(p + start), len, nullptr, 0);
    }
    if (need <= 0) return L"";
    std::wstring s(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(cp, 0, reinterpret_cast<const char*>(p + start), len, s.data(), need);
    while (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

bool ParseDate(const std::wstring& text, SYSTEMTIME& st) {
    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (swscanf(text.c_str(), L"%4u:%2u:%2u %2u:%2u:%2u", &y, &mo, &d, &h, &mi, &s) != 6) {
        return false;
    }
    if (y < 1980 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
        return false;
    }
    st = SYSTEMTIME{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(mo);
    st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    st.wSecond = static_cast<WORD>(s);
    return true;
}

bool ParseOffset(const std::wstring& text, int& minutes) {
    int sign = 1;
    unsigned h = 0, m = 0;
    if (text.size() < 6) return false;
    if (text[0] == L'+') sign = 1;
    else if (text[0] == L'-') sign = -1;
    else return false;
    if (swscanf(text.c_str() + 1, L"%2u:%2u", &h, &m) != 2) return false;
    if (h > 14 || m > 59) return false;
    minutes = sign * static_cast<int>(h * 60 + m);
    return true;
}

double UnsignedRational(const uint8_t* p, bool be) {
    const uint32_t num = ReadU32(p, be);
    const uint32_t den = ReadU32(p + 4, be);
    if (den == 0) return 0;
    return static_cast<double>(num) / static_cast<double>(den);
}

double SignedRational(const uint8_t* p, bool be) {
    const int32_t num = ReadI32(p, be);
    const int32_t den = ReadI32(p + 4, be);
    if (den == 0) return 0;
    return static_cast<double>(num) / static_cast<double>(den);
}

void NoteSize(Meta& meta, uint32_t w, uint32_t h) {
    if (w < 2 || h < 2) return;
    const uint64_t area = static_cast<uint64_t>(w) * h;
    const uint64_t prev = static_cast<uint64_t>(meta.width) * meta.height;
    if (area > prev) {
        meta.width = w;
        meta.height = h;
    }
}

void Walk(IStream* stream, bool be, uint32_t offset, int depth, int kind, Meta& meta) {
    if (depth > kMaxDepth || offset < 8) return;
    uint8_t countBuf[2]{};
    if (!ReadAt(stream, offset, countBuf, 2)) return;
    uint32_t count = ReadU16(countBuf, be);
    if (count == 0 || count > kMaxEntries) return;
    const uint32_t bytes = count * 12;
    std::vector<uint8_t> entries(bytes);
    if (!ReadAt(stream, offset + 2, entries.data(), bytes)) return;

    uint32_t curW = 0;
    uint32_t curH = 0;
    const auto considerSize = [&]() {
        if (curW >= 2 && curH >= 2) NoteSize(meta, curW, curH);
    };

    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t entry = static_cast<uint64_t>(offset) + 2 + static_cast<uint64_t>(i) * 12;
        const uint8_t* e = entries.data() + i * 12;
        const uint16_t tag = ReadU16(e, be);
        const uint16_t type = ReadU16(e + 2, be);
        const uint32_t n = ReadU32(e + 4, be);
        if (n == 0) continue;

        if ((tag == kExifIfd || tag == kGpsIfd) && type == 4 && n >= 1) {
            uint8_t inlineVal[4]{};
            memcpy(inlineVal, e + 8, 4);
            const uint32_t next = ReadU32(inlineVal, be);
            Walk(stream, be, next, depth + 1, tag == kGpsIfd ? 2 : 1, meta);
            continue;
        }

        std::vector<uint8_t> val;
        const auto load = [&]() -> bool { return ReadValue(stream, entry, be, type, n, val); };

        if (kind == 0) {
            if (tag == 271 && load()) meta.make = AsciiToWide(val.data(), n);
            else if (tag == 272 && load()) meta.model = AsciiToWide(val.data(), n);
            else if (tag == 274 && type == 3 && load() && val.size() >= 2) {
                const uint16_t o = ReadU16(val.data(), be);
                if (o >= 1 && o <= 8) {
                    meta.hasOrientation = true;
                    meta.orientation = o;
                }
            } else if (tag == 256 && load() && !val.empty()) {
                if (type == 3 && val.size() >= 2) curW = ReadU16(val.data(), be);
                else if (type == 4 && val.size() >= 4) curW = ReadU32(val.data(), be);
                considerSize();
            } else if (tag == 257 && load() && !val.empty()) {
                if (type == 3 && val.size() >= 2) curH = ReadU16(val.data(), be);
                else if (type == 4 && val.size() >= 4) curH = ReadU32(val.data(), be);
                considerSize();
            } else if (tag == 258 && type == 3 && load() && val.size() >= 2) {
                meta.bitDepth = ReadU16(val.data(), be);
            }
        } else if (kind == 1) {
            if ((tag == 36867 || tag == 36868) && !meta.hasDate && load()) {
                const std::wstring text = AsciiToWide(val.data(), n);
                meta.hasDate = ParseDate(text, meta.date);
            } else if ((tag == 36880 || tag == 36881) && load()) {
                int minutes = 0;
                if (ParseOffset(AsciiToWide(val.data(), n), minutes) && (tag == 36881 || !meta.hasOffset)) {
                    meta.hasOffset = true;
                    meta.offsetMin = minutes;
                }
            } else if (tag == 33434 && type == 5 && load() && val.size() >= 8) {
                meta.exposure = UnsignedRational(val.data(), be);
                meta.hasExposure = meta.exposure > 0;
            } else if (tag == 33437 && type == 5 && load() && val.size() >= 8) {
                meta.fNumber = UnsignedRational(val.data(), be);
                meta.hasFNumber = meta.fNumber > 0;
            } else if (tag == 34855 && load() && val.size() >= 2) {
                if (type == 3) meta.iso = ReadU16(val.data(), be);
                else if (type == 4 && val.size() >= 4) meta.iso = ReadU32(val.data(), be);
                meta.hasIso = meta.iso > 0;
            } else if (tag == 37386 && type == 5 && load() && val.size() >= 8) {
                meta.focal = UnsignedRational(val.data(), be);
                meta.hasFocal = meta.focal > 0;
            } else if (tag == 41989 && type == 3 && load() && val.size() >= 2) {
                meta.focal35 = ReadU16(val.data(), be);
                meta.hasFocal35 = meta.focal35 > 0;
            } else if (tag == 42036 && load()) {
                meta.lensModel = AsciiToWide(val.data(), n);
            } else if (tag == 42035 && load()) {
                meta.lensMake = AsciiToWide(val.data(), n);
            } else if (tag == 37380 && (type == 10 || type == 5) && load() && val.size() >= 8) {
                meta.bias = type == 10 ? SignedRational(val.data(), be) : UnsignedRational(val.data(), be);
                meta.hasBias = true;
            } else if ((tag == 40962 || tag == 40963) && load()) {
                uint32_t v = 0;
                if (type == 3 && val.size() >= 2) v = ReadU16(val.data(), be);
                else if ((type == 4 || type == 9) && val.size() >= 4) v = ReadU32(val.data(), be);
                if (tag == 40962) curW = v;
                else curH = v;
                considerSize();
            }
        } else if (kind == 2) {
            if (tag == 2 && type == 5 && n >= 3 && load() && val.size() >= 24) {
                for (int part = 0; part < 3; ++part) {
                    meta.latNum[part] = ReadU32(val.data() + part * 8, be);
                    meta.latDen[part] = ReadU32(val.data() + part * 8 + 4, be);
                    meta.lat[part] = meta.latDen[part] ? static_cast<double>(meta.latNum[part]) / meta.latDen[part] : 0;
                }
                meta.hasGpsLat = true;
            } else if (tag == 1 && type == 2 && load() && !val.empty()) {
                meta.latRef = static_cast<char>(val[0]);
            } else if (tag == 4 && type == 5 && n >= 3 && load() && val.size() >= 24) {
                for (int part = 0; part < 3; ++part) {
                    meta.lonNum[part] = ReadU32(val.data() + part * 8, be);
                    meta.lonDen[part] = ReadU32(val.data() + part * 8 + 4, be);
                    meta.lon[part] = meta.lonDen[part] ? static_cast<double>(meta.lonNum[part]) / meta.lonDen[part] : 0;
                }
                meta.hasGpsLon = true;
            } else if (tag == 3 && type == 2 && load() && !val.empty()) {
                meta.lonRef = static_cast<char>(val[0]);
            } else if (tag == 6 && type == 5 && load() && val.size() >= 8) {
                meta.altNum = ReadU32(val.data(), be);
                meta.altDen = ReadU32(val.data() + 4, be);
                meta.alt = meta.altDen ? static_cast<double>(meta.altNum) / meta.altDen : 0;
                meta.hasAlt = meta.altDen != 0;
            } else if (tag == 5 && type == 1 && load() && !val.empty()) {
                meta.altBelow = val[0] == 1;
            }
        }
    }
}

bool ParseNef(IStream* stream, Meta& meta) {
    uint8_t hdr[8]{};
    if (!ReadAt(stream, 0, hdr, 8)) return false;
    bool be = false;
    if (hdr[0] == 'I' && hdr[1] == 'I') be = false;
    else if (hdr[0] == 'M' && hdr[1] == 'M') be = true;
    else return false;
    if (ReadU16(hdr + 2, be) != 42) return false;
    const uint32_t ifd0 = ReadU32(hdr + 4, be);
    Walk(stream, be, ifd0, 0, 0, meta);
    return true;
}

FILETIME DateToFileTime(const Meta& meta) {
    FILETIME ft{};
    if (!SystemTimeToFileTime(&meta.date, &ft)) return FILETIME{};
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (meta.hasOffset) {
        const int64_t delta = static_cast<int64_t>(meta.offsetMin) * 60LL * 10000000LL;
        u.QuadPart = static_cast<uint64_t>(static_cast<int64_t>(u.QuadPart) - delta);
    } else {
        FILETIME local = ft;
        if (!LocalFileTimeToFileTime(&local, &ft)) return local;
        return ft;
    }
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}

HRESULT AddNamed(std::vector<Entry>& items, const wchar_t* name, PROPVARIANT* pv) {
    if (pv == nullptr || IsEmpty(*pv)) {
        if (pv) PropVariantClear(pv);
        return S_OK;
    }
    PROPERTYKEY key{};
    if (FAILED(PSGetPropertyKeyFromName(name, &key))) {
        PropVariantClear(pv);
        return S_OK;
    }
    IPropertyDescription* desc = nullptr;
    if (SUCCEEDED(PSGetPropertyDescription(key, IID_IPropertyDescription, reinterpret_cast<void**>(&desc)))) {
        PROPVARIANT saved{};
        PropVariantInit(&saved);
        PropVariantCopy(&saved, pv);
        if (FAILED(desc->CoerceToCanonicalValue(pv)) || IsEmpty(*pv)) {
            PropVariantClear(pv);
            *pv = saved;
            PropVariantInit(&saved);
        }
        PropVariantClear(&saved);
        desc->Release();
        if (IsEmpty(*pv)) {
            PropVariantClear(pv);
            return S_OK;
        }
    }
    for (Entry& existing : items) {
        if (SameKey(existing.key, key)) {
            if (!IsEmpty(existing.value)) {
                PropVariantClear(pv);
                return S_OK;
            }
            PropVariantClear(&existing.value);
            existing.value = *pv;
            PropVariantInit(pv);
            return S_OK;
        }
    }
    Entry e;
    e.key = key;
    e.value = *pv;
    PropVariantInit(pv);
    items.push_back(e);
    return S_OK;
}

void PutString(std::vector<Entry>& items, const wchar_t* name, const std::wstring& text) {
    if (text.empty()) return;
    PROPVARIANT pv{};
    if (SUCCEEDED(InitPropVariantFromString(text.c_str(), &pv))) AddNamed(items, name, &pv);
}

void PutDouble(std::vector<Entry>& items, const wchar_t* name, double value) {
    PROPVARIANT pv{};
    if (SUCCEEDED(InitPropVariantFromDouble(value, &pv))) AddNamed(items, name, &pv);
}

void PutUInt(std::vector<Entry>& items, const wchar_t* name, uint32_t value) {
    PROPVARIANT pv{};
    if (SUCCEEDED(InitPropVariantFromUInt32(value, &pv))) AddNamed(items, name, &pv);
}

void PutFileTime(std::vector<Entry>& items, const wchar_t* name, const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return;
    PROPVARIANT pv{};
    if (SUCCEEDED(InitPropVariantFromFileTime(&ft, &pv))) AddNamed(items, name, &pv);
}

std::wstring FormatTakenWithSeconds(const FILETIME& utc) {
    FILETIME localFt{};
    SYSTEMTIME local{};
    if (!FileTimeToLocalFileTime(&utc, &localFt) || !FileTimeToSystemTime(&localFt, &local)) return L"";
    wchar_t date[80]{};
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 80, nullptr)) {
        return L"";
    }
    wchar_t timeFmt[80]{};
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, timeFmt, 80)) {
        wcscpy(timeFmt, L"h:mm:ss tt");
    }
    if (wcsstr(timeFmt, L"ss") == nullptr) {
        wchar_t* marker = wcsstr(timeFmt, L"tt");
        wchar_t rebuilt[96]{};
        if (marker != nullptr) {
            const size_t head = static_cast<size_t>(marker - timeFmt);
            wcsncpy(rebuilt, timeFmt, head);
            rebuilt[head] = L'\0';
            while (head > 0 && rebuilt[wcslen(rebuilt) - 1] == L' ') rebuilt[wcslen(rebuilt) - 1] = L'\0';
            wcscat(rebuilt, L":ss ");
            wcscat(rebuilt, marker);
            wcscpy(timeFmt, rebuilt);
        } else {
            wcscat(timeFmt, L":ss");
        }
    }
    wchar_t time[80]{};
    if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, timeFmt, time, 80)) return date;
    std::wstring text = date;
    text += L" ";
    text += time;
    return text;
}

void PutUIntVector(std::vector<Entry>& items, const wchar_t* name, const uint32_t* vals, uint32_t count) {
    PROPVARIANT pv{};
    PropVariantInit(&pv);
    pv.vt = VT_VECTOR | VT_UI4;
    pv.caul.cElems = count;
    pv.caul.pElems = static_cast<ULONG*>(CoTaskMemAlloc(sizeof(ULONG) * count));
    if (pv.caul.pElems == nullptr) return;
    for (uint32_t i = 0; i < count; ++i) pv.caul.pElems[i] = vals[i];
    AddNamed(items, name, &pv);
}

void PutByte(std::vector<Entry>& items, const wchar_t* name, uint8_t value) {
    PROPVARIANT pv{};
    PropVariantInit(&pv);
    pv.vt = VT_UI1;
    pv.bVal = value;
    AddNamed(items, name, &pv);
}

wchar_t RefLetter(char ref, wchar_t fallback) {
    wchar_t ch = static_cast<wchar_t>(ref);
    if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    if (ch < 32) return fallback;
    return ch;
}

// Same layout Windows uses for JPEG: degrees/1, minutes/1, seconds*10000/10000.
void CanonicalDms(const double in[3], uint32_t num[3], uint32_t den[3], double shown[3]) {
    double deg = in[0] < 0 ? -in[0] : in[0];
    double minutes = in[1] < 0 ? -in[1] : in[1];
    double seconds = in[2] < 0 ? -in[2] : in[2];
    minutes += (deg - static_cast<uint32_t>(deg)) * 60.0;
    deg = static_cast<uint32_t>(deg);
    seconds += (minutes - static_cast<uint32_t>(minutes)) * 60.0;
    minutes = static_cast<uint32_t>(minutes);
    uint32_t secNum = static_cast<uint32_t>(seconds * 10000.0 + 0.5);
    uint32_t minNum = static_cast<uint32_t>(minutes);
    uint32_t degNum = static_cast<uint32_t>(deg);
    if (secNum >= 600000) {
        secNum -= 600000;
        ++minNum;
    }
    if (minNum >= 60) {
        minNum -= 60;
        ++degNum;
    }
    num[0] = degNum;
    num[1] = minNum;
    num[2] = secNum;
    den[0] = 1;
    den[1] = 1;
    den[2] = 10000;
    shown[0] = degNum;
    shown[1] = minNum;
    shown[2] = secNum / 10000.0;
}

void PutDms(std::vector<Entry>& items, const wchar_t* name, const double dms[3]) {
    PROPVARIANT pv{};
    PropVariantInit(&pv);
    pv.vt = VT_VECTOR | VT_R8;
    pv.cadbl.cElems = 3;
    pv.cadbl.pElems = static_cast<DOUBLE*>(CoTaskMemAlloc(sizeof(DOUBLE) * 3));
    if (!pv.cadbl.pElems) return;
    pv.cadbl.pElems[0] = dms[0];
    pv.cadbl.pElems[1] = dms[1];
    pv.cadbl.pElems[2] = dms[2];
    AddNamed(items, name, &pv);
}

void OverlayMeta(std::vector<Entry>& items, const Meta& meta) {
    PutString(items, L"System.Photo.CameraManufacturer", meta.make);
    PutString(items, L"System.Photo.CameraModel", meta.model);
    PutString(items, L"System.Photo.LensManufacturer", meta.lensMake);
    PutString(items, L"System.Photo.LensModel", meta.lensModel);
    if (meta.hasDate) {
        const FILETIME taken = DateToFileTime(meta);
        PutFileTime(items, L"System.Photo.DateTaken", taken);
        // The built-in Date taken field uses the short time format, which stops at the minute.
        PutString(items, L"PhotoGeoTagger.DateTaken", FormatTakenWithSeconds(taken));
    }
    if (meta.hasExposure) PutDouble(items, L"System.Photo.ExposureTime", meta.exposure);
    if (meta.hasFNumber) PutDouble(items, L"System.Photo.FNumber", meta.fNumber);
    if (meta.hasIso) PutUInt(items, L"System.Photo.ISOSpeed", meta.iso);
    if (meta.hasFocal) PutDouble(items, L"System.Photo.FocalLength", meta.focal);
    if (meta.hasFocal35) PutUInt(items, L"System.Photo.FocalLengthInFilm", meta.focal35);
    if (meta.hasOrientation) PutUInt(items, L"System.Photo.Orientation", meta.orientation);
    if (meta.hasBias) PutDouble(items, L"System.Photo.ExposureBias", meta.bias);
    if (meta.hasGpsLat) {
        const wchar_t ref[] = {RefLetter(meta.latRef, L'N'), L'\0'};
        uint32_t num[3]{};
        uint32_t den[3]{};
        double shown[3]{};
        CanonicalDms(meta.lat, num, den, shown);
        PutDms(items, L"System.GPS.Latitude", shown);
        PutUIntVector(items, L"System.GPS.LatitudeNumerator", num, 3);
        PutUIntVector(items, L"System.GPS.LatitudeDenominator", den, 3);
        PutString(items, L"System.GPS.LatitudeRef", ref);
    }
    if (meta.hasGpsLon) {
        const wchar_t ref[] = {RefLetter(meta.lonRef, L'E'), L'\0'};
        uint32_t num[3]{};
        uint32_t den[3]{};
        double shown[3]{};
        CanonicalDms(meta.lon, num, den, shown);
        PutDms(items, L"System.GPS.Longitude", shown);
        PutUIntVector(items, L"System.GPS.LongitudeNumerator", num, 3);
        PutUIntVector(items, L"System.GPS.LongitudeDenominator", den, 3);
        PutString(items, L"System.GPS.LongitudeRef", ref);
    }
    if (meta.hasAlt) {
        PutDouble(items, L"System.GPS.Altitude", meta.altBelow ? -meta.alt : meta.alt);
        PutUInt(items, L"System.GPS.AltitudeNumerator", meta.altNum);
        PutUInt(items, L"System.GPS.AltitudeDenominator", meta.altDen);
        PutByte(items, L"System.GPS.AltitudeRef", meta.altBelow ? 1 : 0);
    }
    if (meta.width && meta.height) {
        PutUInt(items, L"System.Image.HorizontalSize", meta.width);
        PutUInt(items, L"System.Image.VerticalSize", meta.height);
        wchar_t dims[64]{};
        swprintf(dims, 64, L"%u x %u", meta.width, meta.height);
        PutString(items, L"System.Image.Dimensions", dims);
    }
    if (meta.bitDepth) PutUInt(items, L"System.Image.BitDepth", meta.bitDepth);
}

void CopyInbox(IStream* stream, DWORD mode, std::vector<Entry>& items) {
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    IPropertyStore* inbox = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_PhotoMetadataHandler, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IPropertyStore, reinterpret_cast<void**>(&inbox));
    if (FAILED(hr) || inbox == nullptr) return;
    IInitializeWithStream* init = nullptr;
    hr = inbox->QueryInterface(IID_IInitializeWithStream, reinterpret_cast<void**>(&init));
    if (SUCCEEDED(hr) && init != nullptr) {
        hr = init->Initialize(stream, mode);
        init->Release();
    }
    if (SUCCEEDED(hr)) {
        DWORD count = 0;
        if (SUCCEEDED(inbox->GetCount(&count))) {
            for (DWORD i = 0; i < count; ++i) {
                Entry e;
                PropVariantInit(&e.value);
                if (FAILED(inbox->GetAt(i, &e.key))) continue;
                if (FAILED(inbox->GetValue(e.key, &e.value)) || IsEmpty(e.value)) {
                    PropVariantClear(&e.value);
                    continue;
                }
                items.push_back(e);
            }
        }
    }
    inbox->Release();
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
}

class Store final : public IPropertyStore,
                    public IInitializeWithStream,
                    public IInitializeWithFile,
                    public IPropertyStoreCapabilities {
public:
    Store() : ref_(1) { LockModule(); }
    ~Store() {
        Clear();
        UnlockModule();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPropertyStore) {
            *ppv = static_cast<IPropertyStore*>(this);
        } else if (riid == IID_IInitializeWithStream) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else if (riid == IID_IInitializeWithFile) {
            *ppv = static_cast<IInitializeWithFile*>(this);
        } else if (riid == IID_IPropertyStoreCapabilities) {
            *ppv = static_cast<IPropertyStoreCapabilities*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetCount(DWORD* cProps) override {
        if (cProps == nullptr) return E_POINTER;
        *cProps = static_cast<DWORD>(items_.size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAt(DWORD iProp, PROPERTYKEY* pkey) override {
        if (pkey == nullptr) return E_POINTER;
        if (iProp >= items_.size()) return E_INVALIDARG;
        *pkey = items_[iProp].key;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        if (pv == nullptr) return E_POINTER;
        PropVariantInit(pv);
        for (const Entry& e : items_) {
            if (SameKey(e.key, key)) return PropVariantCopy(pv, &e.value);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override {
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Commit() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE Initialize(IStream* stream, DWORD grfMode) override {
        if (stream == nullptr) return E_INVALIDARG;
        if (ready_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        try {
            Clear();
            CopyInbox(stream, grfMode, items_);
            IStream* mine = nullptr;
            if (FAILED(stream->Clone(&mine)) || mine == nullptr) {
                stream->AddRef();
                mine = stream;
            }
            Meta meta;
            ParseNef(mine, meta);
            mine->Release();
            OverlayMeta(items_, meta);
            ready_ = true;
            return S_OK;
        } catch (...) {
            Clear();
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR path, DWORD grfMode) override {
        if (path == nullptr || path[0] == L'\0') return E_INVALIDARG;
        IStream* stream = nullptr;
        HRESULT hr = SHCreateStreamOnFileEx(path, STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream);
        if (FAILED(hr)) return hr;
        hr = Initialize(stream, grfMode);
        stream->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE IsPropertyWritable(REFPROPERTYKEY) override { return S_FALSE; }

private:
    void Clear() {
        for (Entry& e : items_) PropVariantClear(&e.value);
        items_.clear();
        ready_ = false;
    }

    long ref_;
    bool ready_ = false;
    std::vector<Entry> items_;
};

class Factory final : public IClassFactory {
public:
    Factory() : ref_(1) { LockModule(); }
    ~Factory() { UnlockModule(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        *ppv = nullptr;
        if (outer != nullptr) return CLASS_E_NOAGGREGATION;
        Store* store = new (std::nothrow) Store();
        if (store == nullptr) return E_OUTOFMEMORY;
        const HRESULT hr = store->QueryInterface(riid, ppv);
        store->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) LockModule();
        else UnlockModule();
        return S_OK;
    }

private:
    long ref_;
};

HRESULT WriteRegString(HKEY root, const wchar_t* sub, const wchar_t* valueName, const wchar_t* data) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(data),
                        static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(rc);
}

HRESULT WriteRegDword(HKEY root, const wchar_t* sub, const wchar_t* valueName, DWORD data) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = RegSetValueExW(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(rc);
}

void RememberHandler(const wchar_t* ext) {
    HKEY key = nullptr;
    wchar_t path[96]{};
    swprintf(path, 96, L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\%s", ext);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;
    wchar_t current[64]{};
    DWORD cb = sizeof(current);
    DWORD type = 0;
    wchar_t ours[64]{};
    StringFromGUID2(CLSID_NefPropertyHandler, ours, 64);
    if (RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(current), &cb) == ERROR_SUCCESS &&
        type == REG_SZ && _wcsicmp(current, ours) != 0) {
        const wchar_t* backup = L"Software\\PhotoGeoTagger\\NefPropertyHandler";
        HKEY dest = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, backup, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &dest, nullptr) ==
            ERROR_SUCCESS) {
            RegSetValueExW(dest, ext, 0, REG_SZ, reinterpret_cast<const BYTE*>(current),
                           static_cast<DWORD>((wcslen(current) + 1) * sizeof(wchar_t)));
            RegCloseKey(dest);
        }
    }
    RegCloseKey(key);
}

}  // namespace

extern "C" {

STDAPI DllCanUnloadNow(void) { return g_locks == 0 ? S_OK : S_FALSE; }

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_NefPropertyHandler) return CLASS_E_CLASSNOTAVAILABLE;
    Factory* factory = new (std::nothrow) Factory();
    if (factory == nullptr) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllRegisterServer(void) {
    wchar_t module[MAX_PATH]{};
    if (GetModuleFileNameW(reinterpret_cast<HMODULE>(GetModuleHandleW(L"NefPropHandler.dll")), module, MAX_PATH) == 0) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&DllRegisterServer), &self);
        GetModuleFileNameW(self, module, MAX_PATH);
    }
    wchar_t clsid[64]{};
    StringFromGUID2(CLSID_NefPropertyHandler, clsid, 64);

    wchar_t clsidKey[96]{};
    swprintf(clsidKey, 96, L"Software\\Classes\\CLSID\\%s", clsid);
    wchar_t inproc[128]{};
    swprintf(inproc, 128, L"Software\\Classes\\CLSID\\%s\\InProcServer32", clsid);

    HRESULT hr = WriteRegString(HKEY_CURRENT_USER, clsidKey, nullptr, L"NEF Photo Property Handler");
    if (FAILED(hr)) return hr;
    hr = WriteRegDword(HKEY_CURRENT_USER, clsidKey, L"ManualSafeSave", 1);
    if (FAILED(hr)) return hr;
    hr = WriteRegDword(HKEY_CURRENT_USER, clsidKey, L"DisableProcessIsolation", 1);
    if (FAILED(hr)) return hr;
    hr = WriteRegString(HKEY_CURRENT_USER, inproc, nullptr, module);
    if (FAILED(hr)) return hr;
    hr = WriteRegString(HKEY_CURRENT_USER, inproc, L"ThreadingModel", L"Both");
    if (FAILED(hr)) return hr;

    const wchar_t* exts[] = {L".nef", L".nrw"};
    for (const wchar_t* ext : exts) {
        RememberHandler(ext);
        wchar_t handler[160]{};
        swprintf(handler, 160,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\%s", ext);
        hr = WriteRegString(HKEY_LOCAL_MACHINE, handler, nullptr, clsid);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

STDAPI DllUnregisterServer(void) {
    wchar_t clsid[64]{};
    StringFromGUID2(CLSID_NefPropertyHandler, clsid, 64);
    wchar_t clsidKey[96]{};
    swprintf(clsidKey, 96, L"Software\\Classes\\CLSID\\%s", clsid);
    RegDeleteTreeW(HKEY_CURRENT_USER, clsidKey);

    const wchar_t* exts[] = {L".nef", L".nrw"};
    const wchar_t* fallback = L"{a38b883c-1682-497e-97b0-0a3a9e801682}";
    for (const wchar_t* ext : exts) {
        wchar_t saved[64]{};
        DWORD cb = sizeof(saved);
        DWORD type = 0;
        HKEY backup = nullptr;
        const wchar_t* restore = fallback;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PhotoGeoTagger\\NefPropertyHandler", 0, KEY_QUERY_VALUE,
                          &backup) == ERROR_SUCCESS) {
            if (RegQueryValueExW(backup, ext, nullptr, &type, reinterpret_cast<BYTE*>(saved), &cb) == ERROR_SUCCESS &&
                type == REG_SZ && saved[0] == L'{') {
                restore = saved;
            }
            RegCloseKey(backup);
        }
        wchar_t handler[160]{};
        swprintf(handler, 160,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\%s", ext);
        WriteRegString(HKEY_LOCAL_MACHINE, handler, nullptr, restore);
    }
    return S_OK;
}

}  // extern "C"
