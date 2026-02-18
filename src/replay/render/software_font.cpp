#include "replay/render/software_font.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace replay::render
{
namespace
{
#if defined(_WIN32)
constexpr int kEmbeddedItcBoltResourceId = 101;
constexpr int kEmbeddedFontBasePixelHeight = 9;
constexpr std::uint8_t kEmbeddedFontInkThreshold = 128;
constexpr int kUnicodeTextLaneHeightPx = 12;

struct CustomFontRendererState
{
    bool fontInstallAttempted = false;
    bool fontInstalled = false;
    HANDLE fontResource = nullptr;
    DWORD installedFontCount = 0;

    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    void* bits = nullptr;
    int dibWidth = 0;
    int dibHeight = 0;

    HFONT font = nullptr;
    int fontPixelHeight = 0;
};

struct Utf8FontRendererState
{
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    void* bits = nullptr;
    int dibWidth = 0;
    int dibHeight = 0;

    HFONT font = nullptr;
    int fontPixelHeight = 0;
    int fontProfile = 0;
};

CustomFontRendererState g_customFont = {};
Utf8FontRendererState g_utf8Font = {};
bool g_loggedEmbeddedRendererUse = false;
bool g_loggedFallbackRendererUse = false;
bool g_loggedUtf8RendererUse = false;

std::string NormalizeFaceName(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (char c : value)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        {
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return normalized;
}

bool IsItcBoltFace(const std::string& faceName)
{
    const std::string normalized = NormalizeFaceName(faceName);
    return normalized.find("itcbolt") != std::string::npos;
}

bool ContainsNonAscii(const std::string& text)
{
    for (unsigned char c : text)
    {
        if ((c & 0x80u) != 0)
        {
            return true;
        }
    }
    return false;
}

bool ContainsCjkCodepoint(const std::wstring& text)
{
    for (wchar_t ch : text)
    {
        const unsigned int cp = static_cast<unsigned int>(ch);
        // Hiragana, Katakana, Half-width Katakana, CJK Unified Ideographs.
        if ((cp >= 0x3040u && cp <= 0x30FFu)
            || (cp >= 0x31F0u && cp <= 0x31FFu)
            || (cp >= 0xFF66u && cp <= 0xFF9Fu)
            || (cp >= 0x4E00u && cp <= 0x9FFFu))
        {
            return true;
        }
    }
    return false;
}

bool ContainsCyrillicCodepoint(const std::wstring& text)
{
    for (wchar_t ch : text)
    {
        const unsigned int cp = static_cast<unsigned int>(ch);
        if ((cp >= 0x0400u && cp <= 0x04FFu) || (cp >= 0x0500u && cp <= 0x052Fu))
        {
            return true;
        }
    }
    return false;
}

bool ContainsAsciiAlnum(const std::wstring& text)
{
    for (wchar_t ch : text)
    {
        if ((ch >= L'0' && ch <= L'9')
            || (ch >= L'A' && ch <= L'Z')
            || (ch >= L'a' && ch <= L'z'))
        {
            return true;
        }
    }
    return false;
}

HMODULE ResolveOwnModule()
{
    HMODULE module = nullptr;
    const BOOL ok = GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ResolveOwnModule),
        &module);
    return ok ? module : nullptr;
}

bool EnsurePrivateFontInstalled()
{
    if (g_customFont.fontInstallAttempted)
    {
        return g_customFont.fontInstalled;
    }
    g_customFont.fontInstallAttempted = true;

    HMODULE module = ResolveOwnModule();
    if (module == nullptr)
    {
        mod::Log("ReplayFont::EnsurePrivateFontInstalled: ResolveOwnModule failed");
        return false;
    }

    HRSRC res = FindResourceA(module, MAKEINTRESOURCEA(kEmbeddedItcBoltResourceId), RT_RCDATA);
    if (res == nullptr)
    {
        mod::Log(
            "ReplayFont::EnsurePrivateFontInstalled: FindResource failed id=%d err=%lu",
            kEmbeddedItcBoltResourceId,
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    const DWORD size = SizeofResource(module, res);
    if (size == 0)
    {
        mod::Log("ReplayFont::EnsurePrivateFontInstalled: SizeofResource returned 0");
        return false;
    }

    HGLOBAL loaded = LoadResource(module, res);
    if (loaded == nullptr)
    {
        mod::Log(
            "ReplayFont::EnsurePrivateFontInstalled: LoadResource failed err=%lu",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }
    void* data = LockResource(loaded);
    if (data == nullptr)
    {
        mod::Log("ReplayFont::EnsurePrivateFontInstalled: LockResource returned null");
        return false;
    }

    DWORD installedFonts = 0;
    g_customFont.fontResource = AddFontMemResourceEx(data, size, nullptr, &installedFonts);
    g_customFont.installedFontCount = installedFonts;
    g_customFont.fontInstalled = (g_customFont.fontResource != nullptr) && (installedFonts > 0);
    mod::Log(
        "ReplayFont::EnsurePrivateFontInstalled: size=%lu addResult=%p installedFonts=%lu installed=%d",
        static_cast<unsigned long>(size),
        g_customFont.fontResource,
        static_cast<unsigned long>(installedFonts),
        g_customFont.fontInstalled ? 1 : 0);
    return g_customFont.fontInstalled;
}

HFONT CreateItcBoltFont(int pixelHeight)
{
    if (pixelHeight <= 0)
    {
        return nullptr;
    }

    const char* faceCandidates[] = {
        "ITC Bolt Bold Regular",
        "ITC Bolt Bold",
        "ITC Bolt",
    };

    for (const char* face : faceCandidates)
    {
        HFONT font = CreateFontA(
            -pixelHeight,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_TT_ONLY_PRECIS,
            CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            face);
        if (font == nullptr)
        {
            mod::Log(
                "ReplayFont::CreateItcBoltFont: CreateFont failed request='%s' height=%d err=%lu",
                face,
                pixelHeight,
                static_cast<unsigned long>(GetLastError()));
            continue;
        }

        char actualFace[LF_FACESIZE] = {};
        bool accepted = false;
        if (g_customFont.dc != nullptr)
        {
            HGDIOBJ old = SelectObject(g_customFont.dc, font);
            const int got = GetTextFaceA(g_customFont.dc, static_cast<int>(sizeof(actualFace)), actualFace);
            if (old != nullptr && old != HGDI_ERROR)
            {
                SelectObject(g_customFont.dc, old);
            }
            if (got > 0)
            {
                accepted = IsItcBoltFace(actualFace);
            }
        }

        if (accepted)
        {
            mod::Log(
                "ReplayFont::CreateItcBoltFont: selected request='%s' actual='%s' height=%d",
                face,
                actualFace,
                pixelHeight);
            return font;
        }

        mod::Log(
            "ReplayFont::CreateItcBoltFont: rejected request='%s' actual='%s' height=%d",
            face,
            (actualFace[0] != '\0') ? actualFace : "(unknown)",
            pixelHeight);
        DeleteObject(font);
    }
    mod::Log("ReplayFont::CreateItcBoltFont: no ITC Bolt face selected");
    return nullptr;
}

bool EnsureGdiDc()
{
    if (g_customFont.dc != nullptr)
    {
        return true;
    }
    g_customFont.dc = CreateCompatibleDC(nullptr);
    return g_customFont.dc != nullptr;
}

bool EnsureScratchDib(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }
    if (!EnsureGdiDc())
    {
        return false;
    }

    if (g_customFont.dib != nullptr && g_customFont.dibWidth == width && g_customFont.dibHeight == height)
    {
        return true;
    }

    if (g_customFont.dib != nullptr)
    {
        if (g_customFont.oldBitmap != nullptr)
        {
            SelectObject(g_customFont.dc, g_customFont.oldBitmap);
            g_customFont.oldBitmap = nullptr;
        }
        DeleteObject(g_customFont.dib);
        g_customFont.dib = nullptr;
        g_customFont.bits = nullptr;
        g_customFont.dibWidth = 0;
        g_customFont.dibHeight = 0;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(g_customFont.dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr)
    {
        return false;
    }

    HGDIOBJ prev = SelectObject(g_customFont.dc, dib);
    if (prev == nullptr || prev == HGDI_ERROR)
    {
        DeleteObject(dib);
        return false;
    }

    g_customFont.dib = dib;
    g_customFont.oldBitmap = prev;
    g_customFont.bits = bits;
    g_customFont.dibWidth = width;
    g_customFont.dibHeight = height;
    return true;
}

bool EnsureCustomFont(int pixelHeight)
{
    if (!EnsurePrivateFontInstalled())
    {
        mod::LogThrottled(
            "ReplayFont::EnsureCustomFont.privateFontMissing",
            5000,
            "ReplayFont::EnsureCustomFont: private font not installed");
        return false;
    }
    if (!EnsureGdiDc())
    {
        mod::LogThrottled(
            "ReplayFont::EnsureCustomFont.ensureGdiDcFailed",
            5000,
            "ReplayFont::EnsureCustomFont: EnsureGdiDc failed");
        return false;
    }

    if (g_customFont.font != nullptr && g_customFont.fontPixelHeight == pixelHeight)
    {
        return true;
    }

    if (g_customFont.font != nullptr)
    {
        DeleteObject(g_customFont.font);
        g_customFont.font = nullptr;
        g_customFont.fontPixelHeight = 0;
    }

    g_customFont.font = CreateItcBoltFont(pixelHeight);
    g_customFont.fontPixelHeight = (g_customFont.font != nullptr) ? pixelHeight : 0;
    if (g_customFont.font == nullptr)
    {
        mod::LogThrottled(
            "ReplayFont::EnsureCustomFont.createItcBoltFontFailed",
            5000,
            "ReplayFont::EnsureCustomFont: CreateItcBoltFont failed height=%d",
            pixelHeight);
    }
    return g_customFont.font != nullptr;
}

int MeasureTextGdi(const std::string& text, int pixelHeight)
{
    if (text.empty() || !EnsureCustomFont(pixelHeight))
    {
        return 0;
    }

    HFONT old = static_cast<HFONT>(SelectObject(g_customFont.dc, g_customFont.font));
    SIZE size = {};
    const BOOL ok = GetTextExtentPoint32A(g_customFont.dc, text.c_str(), static_cast<int>(text.size()), &size);
    if (old != nullptr && old != HGDI_ERROR)
    {
        SelectObject(g_customFont.dc, old);
    }
    return ok ? size.cx : 0;
}

std::string FitCenteredGdi(const std::string& text, int maxWidth, int pixelHeight)
{
    if (maxWidth <= 0)
    {
        return {};
    }
    if (MeasureTextGdi(text, pixelHeight) <= maxWidth)
    {
        return text;
    }

    const std::string ellipsis = "...";
    const int ellipsisW = MeasureTextGdi(ellipsis, pixelHeight);
    if (ellipsisW <= 0 || ellipsisW > maxWidth)
    {
        return {};
    }

    std::string clipped = text;
    while (!clipped.empty())
    {
        const std::string candidate = clipped + ellipsis;
        if (MeasureTextGdi(candidate, pixelHeight) <= maxWidth)
        {
            return candidate;
        }
        clipped.pop_back();
    }
    return ellipsis;
}

std::wstring MultiByteToWideBestEffort(UINT codePage, DWORD flags, const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int required = MultiByteToWideChar(
        codePage,
        flags,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (required <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        codePage,
        flags,
        text.c_str(),
        static_cast<int>(text.size()),
        wide.data(),
        required);
    if (written <= 0)
    {
        return {};
    }
    return wide;
}

std::wstring Utf8ToWideBestEffort(const std::string& text)
{
    std::wstring wide = MultiByteToWideBestEffort(CP_UTF8, MB_ERR_INVALID_CHARS, text);
    if (!wide.empty())
    {
        return wide;
    }

    wide = MultiByteToWideBestEffort(CP_UTF8, 0, text);
    if (!wide.empty())
    {
        return wide;
    }

    return MultiByteToWideBestEffort(CP_ACP, 0, text);
}

bool EnsureUtf8GdiDc()
{
    if (g_utf8Font.dc != nullptr)
    {
        return true;
    }
    g_utf8Font.dc = CreateCompatibleDC(nullptr);
    return g_utf8Font.dc != nullptr;
}

bool EnsureUtf8ScratchDib(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }
    if (!EnsureUtf8GdiDc())
    {
        return false;
    }

    if (g_utf8Font.dib != nullptr && g_utf8Font.dibWidth == width && g_utf8Font.dibHeight == height)
    {
        return true;
    }

    if (g_utf8Font.dib != nullptr)
    {
        if (g_utf8Font.oldBitmap != nullptr)
        {
            SelectObject(g_utf8Font.dc, g_utf8Font.oldBitmap);
            g_utf8Font.oldBitmap = nullptr;
        }
        DeleteObject(g_utf8Font.dib);
        g_utf8Font.dib = nullptr;
        g_utf8Font.bits = nullptr;
        g_utf8Font.dibWidth = 0;
        g_utf8Font.dibHeight = 0;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(g_utf8Font.dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr)
    {
        return false;
    }

    HGDIOBJ prev = SelectObject(g_utf8Font.dc, dib);
    if (prev == nullptr || prev == HGDI_ERROR)
    {
        DeleteObject(dib);
        return false;
    }

    g_utf8Font.dib = dib;
    g_utf8Font.oldBitmap = prev;
    g_utf8Font.bits = bits;
    g_utf8Font.dibWidth = width;
    g_utf8Font.dibHeight = height;
    return true;
}

HFONT CreateUnicodeUiFont(int pixelHeight, int profile)
{
    if (pixelHeight <= 0)
    {
        return nullptr;
    }

    static const wchar_t* kCjkPureFaces[] = {
        L"MS PGothic",
        L"MS Gothic",
        L"MS UI Gothic",
        L"Meiryo UI",
        L"Yu Gothic UI",
        L"Arial Unicode MS",
    };
    static const wchar_t* kCjkMixedFaces[] = {
        L"MS PGothic",
        L"MS UI Gothic",
        L"Meiryo UI",
        L"Yu Gothic UI",
        L"Segoe UI",
        L"Arial Unicode MS",
    };
    static const wchar_t* kCyrillicFaces[] = {
        L"Segoe UI",
        L"Tahoma",
        L"Arial",
        L"Arial Unicode MS",
    };
    static const wchar_t* kGeneralFaces[] = {
        L"Segoe UI",
        L"Arial",
        L"Tahoma",
        L"MS UI Gothic",
        L"Arial Unicode MS",
    };

    const wchar_t* const* faces = kGeneralFaces;
    std::size_t faceCount = sizeof(kGeneralFaces) / sizeof(kGeneralFaces[0]);
    if (profile == 1)
    {
        faces = kCjkPureFaces;
        faceCount = sizeof(kCjkPureFaces) / sizeof(kCjkPureFaces[0]);
    }
    else if (profile == 2)
    {
        faces = kCjkMixedFaces;
        faceCount = sizeof(kCjkMixedFaces) / sizeof(kCjkMixedFaces[0]);
    }
    else if (profile == 3)
    {
        faces = kCyrillicFaces;
        faceCount = sizeof(kCyrillicFaces) / sizeof(kCyrillicFaces[0]);
    }

    for (std::size_t i = 0; i < faceCount; ++i)
    {
        const wchar_t* face = faces[i];
        const int fontWeight = FW_NORMAL;
        const DWORD quality = NONANTIALIASED_QUALITY;
        HFONT font = CreateFontW(
            -pixelHeight,
            0,
            0,
            0,
            fontWeight,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            quality,
            DEFAULT_PITCH | FF_DONTCARE,
            face);
        if (font == nullptr)
        {
            continue;
        }

        wchar_t actualFace[LF_FACESIZE] = {};
        HGDIOBJ old = SelectObject(g_utf8Font.dc, font);
        const int got = GetTextFaceW(g_utf8Font.dc, static_cast<int>(LF_FACESIZE), actualFace);
        if (old != nullptr && old != HGDI_ERROR)
        {
            SelectObject(g_utf8Font.dc, old);
        }
        char logOnceKey[96] = {};
        snprintf(
            logOnceKey,
            sizeof(logOnceKey),
            "ReplayFont::CreateUnicodeUiFont.profile%d.height%d",
            profile,
            pixelHeight);
        mod::LogOnce(
            logOnceKey,
            "ReplayFont::CreateUnicodeUiFont: request='%ls' actual='%ls' height=%d got=%d",
            face,
            (got > 0) ? actualFace : L"(unknown)",
            pixelHeight,
            got);
        return font;
    }

    return nullptr;
}

bool EnsureUtf8Font(int pixelHeight, int profile)
{
    if (!EnsureUtf8GdiDc())
    {
        return false;
    }
    const int requestedProfile = profile;
    if (g_utf8Font.font != nullptr
        && g_utf8Font.fontPixelHeight == pixelHeight
        && g_utf8Font.fontProfile == requestedProfile)
    {
        return true;
    }

    if (g_utf8Font.font != nullptr)
    {
        DeleteObject(g_utf8Font.font);
        g_utf8Font.font = nullptr;
        g_utf8Font.fontPixelHeight = 0;
    }

    g_utf8Font.font = CreateUnicodeUiFont(pixelHeight, requestedProfile);
    g_utf8Font.fontPixelHeight = (g_utf8Font.font != nullptr) ? pixelHeight : 0;
    g_utf8Font.fontProfile = (g_utf8Font.font != nullptr) ? requestedProfile : 0;
    return g_utf8Font.font != nullptr;
}

int MeasureTextWideGdi(const std::wstring& text, int pixelHeight, int profile)
{
    if (text.empty() || !EnsureUtf8Font(pixelHeight, profile))
    {
        return 0;
    }

    HFONT old = static_cast<HFONT>(SelectObject(g_utf8Font.dc, g_utf8Font.font));
    SIZE size = {};
    const BOOL ok = GetTextExtentPoint32W(g_utf8Font.dc, text.c_str(), static_cast<int>(text.size()), &size);
    if (old != nullptr && old != HGDI_ERROR)
    {
        SelectObject(g_utf8Font.dc, old);
    }
    return ok ? size.cx : 0;
}

std::wstring FitCenteredWideGdi(const std::wstring& text, int maxWidth, int pixelHeight, int profile)
{
    if (maxWidth <= 0)
    {
        return {};
    }
    if (MeasureTextWideGdi(text, pixelHeight, profile) <= maxWidth)
    {
        return text;
    }

    const std::wstring ellipsis = L"...";
    const int ellipsisW = MeasureTextWideGdi(ellipsis, pixelHeight, profile);
    if (ellipsisW <= 0 || ellipsisW > maxWidth)
    {
        return {};
    }

    std::wstring clipped = text;
    while (!clipped.empty())
    {
        const std::wstring candidate = clipped + ellipsis;
        if (MeasureTextWideGdi(candidate, pixelHeight, profile) <= maxWidth)
        {
            return candidate;
        }
        clipped.pop_back();
    }
    return ellipsis;
}

bool DrawTextCenteredUtf8Gdi(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleY,
    std::uint8_t color)
{
    if (text.empty() || surface.pixels == nullptr || rightX < leftX || scaleY <= 0)
    {
        return false;
    }

    const std::wstring wideText = Utf8ToWideBestEffort(text);
    if (wideText.empty())
    {
        return false;
    }

    const bool hasCjk = ContainsCjkCodepoint(wideText);
    const bool hasCyrillic = ContainsCyrillicCodepoint(wideText);
    const bool hasAscii = ContainsAsciiAlnum(wideText);
    const int profile = hasCjk ? ((hasAscii || hasCyrillic) ? 2 : 1) : (hasCyrillic ? 3 : 0);
    const int pixelHeight = (profile == 1)
        ? std::max(10, 9 * scaleY + 1)
        : ((profile == 2) ? std::max(10, 9 * scaleY) : ((profile == 3) ? std::max(9, 8 * scaleY) : std::max(8, 7 * scaleY)));
    if (!EnsureUtf8Font(pixelHeight, profile))
    {
        return false;
    }

    const int maxWidth = (rightX - leftX) + 1;
    const std::wstring fitted = FitCenteredWideGdi(wideText, maxWidth, pixelHeight, profile);
    if (fitted.empty())
    {
        return false;
    }

    const int textWidth = MeasureTextWideGdi(fitted, pixelHeight, profile);
    if (textWidth <= 0)
    {
        return false;
    }

    const int drawW = std::max(8, textWidth + 2);
    const int drawHUnclamped = std::max(pixelHeight + ((profile == 1) ? 3 : 2), 10);
    const int drawH = std::min(kUnicodeTextLaneHeightPx, drawHUnclamped);
    if (!EnsureUtf8ScratchDib(drawW, drawH))
    {
        return false;
    }

    std::memset(g_utf8Font.bits, 0, static_cast<std::size_t>(drawW) * static_cast<std::size_t>(drawH) * 4u);

    HFONT old = static_cast<HFONT>(SelectObject(g_utf8Font.dc, g_utf8Font.font));
    SetBkMode(g_utf8Font.dc, TRANSPARENT);
    SetTextColor(g_utf8Font.dc, RGB(255, 255, 255));
    SetTextAlign(g_utf8Font.dc, TA_LEFT | TA_TOP);
    (void)TextOutW(g_utf8Font.dc, 0, 0, fitted.c_str(), static_cast<int>(fitted.size()));
    if (old != nullptr && old != HGDI_ERROR)
    {
        SelectObject(g_utf8Font.dc, old);
    }

    const int startX = leftX + ((maxWidth - textWidth) / 2);
    const int drawBaseY = y - 1;
    const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(g_utf8Font.bits);
    const std::uint8_t inkThreshold = static_cast<std::uint8_t>(32);
    for (int py = 0; py < drawH; ++py)
    {
        const int dstY = drawBaseY + py;
        if (dstY < 0 || dstY >= surface.height)
        {
            continue;
        }

        for (int px = 0; px < drawW; ++px)
        {
            const int dstX = startX + px;
            if (dstX < 0 || dstX >= surface.width)
            {
                continue;
            }

            const std::size_t srcOff = (static_cast<std::size_t>(py) * static_cast<std::size_t>(drawW) + static_cast<std::size_t>(px)) * 4u;
            const std::uint8_t b = src[srcOff + 0];
            const std::uint8_t g = src[srcOff + 1];
            const std::uint8_t r = src[srcOff + 2];
            const std::uint8_t intensity = static_cast<std::uint8_t>(std::max({b, g, r}));
            if (intensity < inkThreshold)
            {
                continue;
            }

            surface.pixels[dstY * surface.pitch + dstX] = color;
        }
    }

    if (!g_loggedUtf8RendererUse)
    {
        g_loggedUtf8RendererUse = true;
        mod::Log(
            "ReplayFont::DrawTextCenteredUtf8Gdi: enabled for non-ASCII text height=%d profile=%d cjk=%d ascii=%d color=%u",
            pixelHeight,
            profile,
            hasCjk ? 1 : 0,
            hasAscii ? 1 : 0,
            static_cast<unsigned>(color));
    }
    return true;
}

bool DrawTextCenteredEmbeddedFont(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleY,
    std::uint8_t color)
{
    if (text.empty() || surface.pixels == nullptr || rightX < leftX || scaleY <= 0)
    {
        return false;
    }

    const int pixelHeight = std::max(8, kEmbeddedFontBasePixelHeight * scaleY);
    if (!EnsureCustomFont(pixelHeight))
    {
        return false;
    }

    const int maxWidth = (rightX - leftX) + 1;
    const std::string fitted = FitCenteredGdi(text, maxWidth, pixelHeight);
    if (fitted.empty())
    {
        return false;
    }

    const int textWidth = MeasureTextGdi(fitted, pixelHeight);
    if (textWidth <= 0)
    {
        return false;
    }

    const int drawW = std::max(8, textWidth + 4);
    const int drawH = std::max(pixelHeight + 6, 10);
    if (!EnsureScratchDib(drawW, drawH))
    {
        return false;
    }

    std::memset(g_customFont.bits, 0, static_cast<std::size_t>(drawW) * static_cast<std::size_t>(drawH) * 4u);

    HFONT old = static_cast<HFONT>(SelectObject(g_customFont.dc, g_customFont.font));
    SetBkMode(g_customFont.dc, OPAQUE);
    SetBkColor(g_customFont.dc, RGB(0, 0, 0));
    SetTextColor(g_customFont.dc, RGB(255, 255, 255));
    SetTextAlign(g_customFont.dc, TA_LEFT | TA_TOP);
    (void)TextOutA(g_customFont.dc, 2, 1, fitted.c_str(), static_cast<int>(fitted.size()));
    if (old != nullptr && old != HGDI_ERROR)
    {
        SelectObject(g_customFont.dc, old);
    }

    const int startX = leftX + ((maxWidth - textWidth) / 2);
    const std::uint8_t* src = reinterpret_cast<const std::uint8_t*>(g_customFont.bits);
    const int baselineLift = std::max(1, scaleY * 3);
    for (int py = 0; py < drawH; ++py)
    {
        const int dstY = y + py - baselineLift;
        if (dstY < 0 || dstY >= surface.height)
        {
            continue;
        }

        for (int px = 0; px < drawW; ++px)
        {
            const int dstX = startX + px;
            if (dstX < 0 || dstX >= surface.width)
            {
                continue;
            }

            const std::size_t srcOff = (static_cast<std::size_t>(py) * static_cast<std::size_t>(drawW) + static_cast<std::size_t>(px)) * 4u;
            const std::uint8_t b = src[srcOff + 0];
            const std::uint8_t g = src[srcOff + 1];
            const std::uint8_t r = src[srcOff + 2];
            const std::uint8_t intensity = static_cast<std::uint8_t>(std::max({b, g, r}));
            if (intensity < kEmbeddedFontInkThreshold)
            {
                continue;
            }

            surface.pixels[dstY * surface.pitch + dstX] = color;
        }
    }
    if (!g_loggedEmbeddedRendererUse)
    {
        g_loggedEmbeddedRendererUse = true;
        mod::Log(
            "ReplayFont::DrawTextCenteredEmbeddedFont: using embedded renderer height=%d color=%u",
            pixelHeight,
            static_cast<unsigned int>(color));
    }
    return true;
}
#endif

const std::uint8_t* GetGlyph5x7(char c)
{
    switch (c)
    {
    case 'A': { static const std::uint8_t g[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}; return g; }
    case 'B': { static const std::uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}; return g; }
    case 'C': { static const std::uint8_t g[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}; return g; }
    case 'D': { static const std::uint8_t g[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}; return g; }
    case 'E': { static const std::uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}; return g; }
    case 'F': { static const std::uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}; return g; }
    case 'G': { static const std::uint8_t g[7] = {0x0E, 0x11, 0x10, 0x10, 0x13, 0x11, 0x0E}; return g; }
    case 'H': { static const std::uint8_t g[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}; return g; }
    case 'I': { static const std::uint8_t g[7] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case 'J': { static const std::uint8_t g[7] = {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}; return g; }
    case 'K': { static const std::uint8_t g[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}; return g; }
    case 'L': { static const std::uint8_t g[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}; return g; }
    case 'M': { static const std::uint8_t g[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}; return g; }
    case 'N': { static const std::uint8_t g[7] = {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}; return g; }
    case 'O': { static const std::uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}; return g; }
    case 'P': { static const std::uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}; return g; }
    case 'Q': { static const std::uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}; return g; }
    case 'R': { static const std::uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}; return g; }
    case 'S': { static const std::uint8_t g[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}; return g; }
    case 'T': { static const std::uint8_t g[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}; return g; }
    case 'U': { static const std::uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}; return g; }
    case 'V': { static const std::uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}; return g; }
    case 'W': { static const std::uint8_t g[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}; return g; }
    case 'X': { static const std::uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}; return g; }
    case 'Y': { static const std::uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}; return g; }
    case 'Z': { static const std::uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}; return g; }
    case 'a': { static const std::uint8_t g[7] = {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}; return g; }
    case 'b': { static const std::uint8_t g[7] = {0x10, 0x10, 0x1C, 0x12, 0x11, 0x11, 0x1E}; return g; }
    case 'c': { static const std::uint8_t g[7] = {0x00, 0x00, 0x0E, 0x10, 0x10, 0x10, 0x0E}; return g; }
    case 'd': { static const std::uint8_t g[7] = {0x01, 0x01, 0x07, 0x09, 0x11, 0x11, 0x0F}; return g; }
    case 'e': { static const std::uint8_t g[7] = {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}; return g; }
    case 'f': { static const std::uint8_t g[7] = {0x06, 0x08, 0x08, 0x1E, 0x08, 0x08, 0x08}; return g; }
    case 'g': { static const std::uint8_t g[7] = {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}; return g; }
    case 'h': { static const std::uint8_t g[7] = {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}; return g; }
    case 'i': { static const std::uint8_t g[7] = {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case 'j': { static const std::uint8_t g[7] = {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}; return g; }
    case 'k': { static const std::uint8_t g[7] = {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}; return g; }
    case 'l': { static const std::uint8_t g[7] = {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case 'm': { static const std::uint8_t g[7] = {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}; return g; }
    case 'n': { static const std::uint8_t g[7] = {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}; return g; }
    case 'o': { static const std::uint8_t g[7] = {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}; return g; }
    case 'p': { static const std::uint8_t g[7] = {0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10}; return g; }
    case 'q': { static const std::uint8_t g[7] = {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01}; return g; }
    case 'r': { static const std::uint8_t g[7] = {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}; return g; }
    case 's': { static const std::uint8_t g[7] = {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}; return g; }
    case 't': { static const std::uint8_t g[7] = {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06}; return g; }
    case 'u': { static const std::uint8_t g[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}; return g; }
    case 'v': { static const std::uint8_t g[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}; return g; }
    case 'w': { static const std::uint8_t g[7] = {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}; return g; }
    case 'x': { static const std::uint8_t g[7] = {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}; return g; }
    case 'y': { static const std::uint8_t g[7] = {0x00, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x0E}; return g; }
    case 'z': { static const std::uint8_t g[7] = {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}; return g; }
    case '0': { static const std::uint8_t g[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}; return g; }
    case '1': { static const std::uint8_t g[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case '2': { static const std::uint8_t g[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}; return g; }
    case '3': { static const std::uint8_t g[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}; return g; }
    case '4': { static const std::uint8_t g[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}; return g; }
    case '5': { static const std::uint8_t g[7] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}; return g; }
    case '6': { static const std::uint8_t g[7] = {0x07, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}; return g; }
    case '7': { static const std::uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}; return g; }
    case '8': { static const std::uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}; return g; }
    case '9': { static const std::uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C}; return g; }
    case ':': { static const std::uint8_t g[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}; return g; }
    case '.': { static const std::uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}; return g; }
    case '-': { static const std::uint8_t g[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}; return g; }
    case '/': { static const std::uint8_t g[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}; return g; }
    case '_': { static const std::uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}; return g; }
    case '(': { static const std::uint8_t g[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}; return g; }
    case ')': { static const std::uint8_t g[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}; return g; }
    case '+': { static const std::uint8_t g[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}; return g; }
    case '#': { static const std::uint8_t g[7] = {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}; return g; }
    case '[': { static const std::uint8_t g[7] = {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}; return g; }
    case ']': { static const std::uint8_t g[7] = {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}; return g; }
    case ' ': { static const std::uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; return g; }
    default:
        return nullptr;
    }
}

int MeasureText5x7(const std::string& text, int scaleX)
{
    if (text.empty())
    {
        return 0;
    }
    const int glyphW = 5 * scaleX;
    const int gap = scaleX;
    return static_cast<int>(text.size()) * (glyphW + gap) - gap;
}

void PutSurfacePixel(const IndexedSurfaceView& surface, int x, int y, std::uint8_t color)
{
    if (surface.pixels == nullptr || x < 0 || y < 0 || x >= surface.width || y >= surface.height)
    {
        return;
    }
    surface.pixels[y * surface.pitch + x] = color;
}

void DrawGlyph5x7(
    const IndexedSurfaceView& surface,
    int x,
    int y,
    char c,
    int scaleX,
    int scaleY,
    std::uint8_t color)
{
    const std::uint8_t* glyph = GetGlyph5x7(c);
    if (glyph == nullptr)
    {
        return;
    }
    for (int row = 0; row < 7; ++row)
    {
        const std::uint8_t bits = glyph[row];
        for (int col = 0; col < 5; ++col)
        {
            if ((bits & (1u << (4 - col))) == 0)
            {
                continue;
            }
            const int px = x + col * scaleX;
            const int py = y + row * scaleY;
            for (int sy = 0; sy < scaleY; ++sy)
            {
                for (int sx = 0; sx < scaleX; ++sx)
                {
                    PutSurfacePixel(surface, px + sx, py + sy, color);
                }
            }
        }
    }
}

std::string FitCentered(const std::string& text, int maxWidth, int scaleX)
{
    if (maxWidth <= 0)
    {
        return {};
    }
    if (MeasureText5x7(text, scaleX) <= maxWidth)
    {
        return text;
    }

    const std::string ellipsis = "...";
    if (MeasureText5x7(ellipsis, scaleX) > maxWidth)
    {
        return {};
    }

    std::string clipped = text;
    while (!clipped.empty() && MeasureText5x7(clipped + ellipsis, scaleX) > maxWidth)
    {
        clipped.pop_back();
    }
    if (clipped.empty())
    {
        return ellipsis;
    }
    return clipped + ellipsis;
}
}

const std::uint8_t* Glyph5x7(char c)
{
    return GetGlyph5x7(c);
}

void DrawTextCentered5x7(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    std::uint8_t color)
{
    if (scaleX <= 0 || scaleY <= 0 || rightX < leftX)
    {
        return;
    }

#if defined(_WIN32)
    constexpr bool kEnableEmbeddedItcFont = false;
    if (ContainsNonAscii(text) && DrawTextCenteredUtf8Gdi(surface, text, leftX, rightX, y, scaleY, color))
    {
        return;
    }
    if (kEnableEmbeddedItcFont && DrawTextCenteredEmbeddedFont(surface, text, leftX, rightX, y, scaleY, color))
    {
        return;
    }
    if (!g_loggedFallbackRendererUse)
    {
        g_loggedFallbackRendererUse = true;
        mod::Log("ReplayFont::DrawTextCentered5x7: using 5x7 fallback renderer");
    }
#endif

    const int maxWidth = (rightX - leftX) + 1;
    const std::string fitted = FitCentered(text, maxWidth, scaleX);
    if (fitted.empty())
    {
        return;
    }

    int x = leftX + ((maxWidth - MeasureText5x7(fitted, scaleX)) / 2);
    const int glyphW = 5 * scaleX;
    const int gap = scaleX;
    for (char c : fitted)
    {
        DrawGlyph5x7(surface, x, y, c, scaleX, scaleY, color);
        x += glyphW + gap;
    }
}
}
