#include "breeze_ui/font.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <windows.h>

#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "nanovg.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t color_cache_soft_limit = 256;

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::wstring to_lower(std::wstring text) {
    std::ranges::transform(text, text.begin(),
                           [](wchar_t ch) { return std::towlower(ch); });
    return text;
}

std::wstring_view trim(std::wstring_view text) {
    while (!text.empty() && std::iswspace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::iswspace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

struct mapped_file {
    unsigned char *data = nullptr;
    size_t size = 0;
};

// Font files stay mapped for the process lifetime so that every NanoVG
// context shares the same read-only pages instead of a private copy.
const mapped_file *map_font_file(const std::wstring &path) {
    static std::mutex mutex;
    static auto *files = new std::unordered_map<std::wstring, mapped_file>();

    std::lock_guard lock(mutex);
    const auto key = to_lower(path);
    if (auto it = files->find(key); it != files->end()) {
        return &it->second;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > INT32_MAX) {
        CloseHandle(file);
        return nullptr;
    }
    HANDLE mapping =
        CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) {
        return nullptr;
    }
    auto *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    if (!view) {
        return nullptr;
    }
    auto [it, _] = files->emplace(
        key, mapped_file{.data = static_cast<unsigned char *>(view),
                         .size = static_cast<size_t>(size.QuadPart)});
    return &it->second;
}

struct dwrite_context {
    ComPtr<IDWriteFactory4> factory;
    ComPtr<IDWriteFactory8> factory8;
    ComPtr<IDWriteFontCollection> collection;
    ComPtr<IDWriteFontFallback> fallback;
    ComPtr<ID2D1Factory1> d2d;
    std::wstring locale;
};

const dwrite_context &dwrite() {
    static const auto *context = [] {
        auto *ctx = new dwrite_context();
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory4),
                reinterpret_cast<IUnknown **>(ctx->factory.GetAddressOf())))) {
            return ctx;
        }
        ctx->factory.As(&ctx->factory8);
        ctx->factory->GetSystemFontCollection(&ctx->collection, FALSE);
        ctx->factory->GetSystemFontFallback(&ctx->fallback);
        D2D1_FACTORY_OPTIONS options{};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
                          __uuidof(ID2D1Factory1), &options,
                          reinterpret_cast<void **>(ctx->d2d.GetAddressOf()));
        wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0) {
            ctx->locale = locale;
        } else {
            ctx->locale = L"en-US";
        }
        return ctx;
    }();
    return *context;
}

std::wstring family_name_of(IDWriteFontFamily *family) {
    ComPtr<IDWriteLocalizedStrings> names;
    if (!family || FAILED(family->GetFamilyNames(&names))) {
        return {};
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    names->FindLocaleName(L"en-us", &index, &exists);
    if (!exists) {
        index = 0;
    }
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(index, &length))) {
        return {};
    }
    std::wstring result(length + 1, L'\0');
    names->GetString(index, result.data(), length + 1);
    result.resize(length);
    return result;
}

struct font_face_ref {
    std::wstring path;
    UINT32 index = 0;
    std::wstring family;
    ComPtr<IDWriteFontFace> face;

    [[nodiscard]] std::wstring key() const {
        return to_lower(path) + L"|" + std::to_wstring(index);
    }
};

std::optional<std::wstring> local_file_path(IDWriteFontFace *face) {
    UINT32 count = 0;
    if (FAILED(face->GetFiles(&count, nullptr)) || count != 1) {
        return std::nullopt;
    }
    ComPtr<IDWriteFontFile> file;
    if (FAILED(face->GetFiles(&count, &file))) {
        return std::nullopt;
    }
    const void *key = nullptr;
    UINT32 key_size = 0;
    ComPtr<IDWriteFontFileLoader> loader;
    ComPtr<IDWriteLocalFontFileLoader> local_loader;
    if (FAILED(file->GetReferenceKey(&key, &key_size)) ||
        FAILED(file->GetLoader(&loader)) || FAILED(loader.As(&local_loader))) {
        return std::nullopt;
    }
    UINT32 length = 0;
    if (FAILED(local_loader->GetFilePathLengthFromKey(key, key_size, &length))) {
        return std::nullopt;
    }
    std::wstring path(length + 1, L'\0');
    if (FAILED(local_loader->GetFilePathFromKey(key, key_size, path.data(),
                                                 length + 1))) {
        return std::nullopt;
    }
    path.resize(length);
    return path;
}

std::optional<font_face_ref> face_from_font(IDWriteFont *font) {
    ComPtr<IDWriteFontFace> face;
    if (!font || FAILED(font->CreateFontFace(&face))) {
        return std::nullopt;
    }
    auto path = local_file_path(face.Get());
    if (!path) {
        return std::nullopt;
    }
    ComPtr<IDWriteFontFamily> family;
    font->GetFontFamily(&family);
    return font_face_ref{.path = std::move(*path),
                         .index = face->GetIndex(),
                         .family = family_name_of(family.Get()),
                         .face = std::move(face)};
}

ComPtr<IDWriteFontFamily> find_system_family(const std::wstring &name) {
    const auto &dw = dwrite();
    if (!dw.collection || name.empty()) {
        return nullptr;
    }
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(dw.collection->FindFamilyName(name.c_str(), &index, &exists)) ||
        !exists) {
        return nullptr;
    }
    ComPtr<IDWriteFontFamily> family;
    dw.collection->GetFontFamily(index, &family);
    return family;
}

std::optional<font_face_ref> face_from_family(const std::wstring &name,
                                              int weight) {
    auto family = find_system_family(name);
    if (!family) {
        return std::nullopt;
    }
    ComPtr<IDWriteFont> font;
    if (FAILED(family->GetFirstMatchingFont(
            static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STRETCH_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, &font))) {
        return std::nullopt;
    }
    return face_from_font(font.Get());
}

std::optional<font_face_ref> face_from_file(const std::wstring &path,
                                            UINT32 index) {
    const auto &dw = dwrite();
    ComPtr<IDWriteFontFile> file;
    if (!dw.factory ||
        FAILED(dw.factory->CreateFontFileReference(path.c_str(), nullptr,
                                                   &file))) {
        return std::nullopt;
    }
    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type{};
    DWRITE_FONT_FACE_TYPE face_type{};
    UINT32 faces = 0;
    if (FAILED(file->Analyze(&supported, &file_type, &face_type, &faces)) ||
        !supported || index >= faces) {
        return std::nullopt;
    }
    ComPtr<IDWriteFontFace> face;
    if (FAILED(dw.factory->CreateFontFace(face_type, 1, file.GetAddressOf(),
                                          index, DWRITE_FONT_SIMULATIONS_NONE,
                                          &face))) {
        return std::nullopt;
    }
    std::wstring family;
    ComPtr<IDWriteFontFace3> face3;
    ComPtr<IDWriteLocalizedStrings> names;
    if (SUCCEEDED(face.As(&face3)) && SUCCEEDED(face3->GetFamilyNames(&names))) {
        UINT32 name_index = 0;
        BOOL exists = FALSE;
        names->FindLocaleName(L"en-us", &name_index, &exists);
        UINT32 length = 0;
        if (SUCCEEDED(names->GetStringLength(exists ? name_index : 0, &length))) {
            family.resize(length + 1);
            names->GetString(exists ? name_index : 0, family.data(), length + 1);
            family.resize(length);
        }
    }
    return font_face_ref{
        .path = path, .index = index, .family = family, .face = face};
}

struct font_source {
    std::wstring family;
    std::wstring path;
    UINT32 index = 0;
};

bool looks_like_path(std::wstring_view text) {
    if (text.find_first_of(L"\\/") != std::wstring_view::npos) {
        return true;
    }
    const auto lower = to_lower(std::wstring(text));
    for (auto ext : {L".ttf", L".otf", L".ttc", L".otc"}) {
        if (lower.ends_with(ext)) {
            return true;
        }
    }
    return false;
}

std::wstring windows_font_directory() {
    wchar_t buffer[MAX_PATH] = {};
    const auto written = GetWindowsDirectoryW(buffer, MAX_PATH);
    return (written > 0 ? std::wstring(buffer, written) : L"C:\\Windows") +
           L"\\Fonts";
}

// A path that points at an installed font is promoted to its family, so
// that legacy "segoeui.ttf"-style settings still get every weight.
std::optional<font_source> parse_source(std::string_view text) {
    auto value = std::wstring(trim(widen(text)));
    if (value.empty()) {
        return std::nullopt;
    }
    if (!looks_like_path(value)) {
        return font_source{.family = std::move(value)};
    }

    UINT32 index = 0;
    if (auto hash = value.rfind(L'#'); hash != std::wstring::npos) {
        try {
            index = static_cast<UINT32>(std::stoul(value.substr(hash + 1)));
            value.resize(hash);
        } catch (...) {
        }
    }
    if (GetFileAttributesW(value.c_str()) == INVALID_FILE_ATTRIBUTES &&
        value.find_first_of(L"\\/") == std::wstring::npos) {
        value = windows_font_directory() + L"\\" + value;
    }

    auto face = face_from_file(value, index);
    if (!face) {
        return std::nullopt;
    }
    if (auto installed = face_from_family(face->family, 400);
        installed && to_lower(installed->path) == to_lower(face->path) &&
        installed->index == face->index) {
        return font_source{.family = face->family};
    }
    return font_source{.path = std::move(value), .index = index};
}

std::optional<font_face_ref> resolve_source(const font_source &source,
                                            int weight) {
    if (!source.family.empty()) {
        return face_from_family(source.family, weight);
    }
    return face_from_file(source.path, source.index);
}

class text_source final : public IDWriteTextAnalysisSource {
public:
    text_source(std::wstring text, const std::wstring &locale)
        : text_(std::move(text)), locale_(locale) {}

    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void **object) noexcept override {
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IDWriteTextAnalysisSource)) {
            *object = static_cast<IDWriteTextAnalysisSource *>(this);
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(
        UINT32 position, WCHAR const **text,
        UINT32 *length) noexcept override {
        if (position >= text_.size()) {
            *text = nullptr;
            *length = 0;
        } else {
            *text = text_.data() + position;
            *length = static_cast<UINT32>(text_.size()) - position;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(
        UINT32 position, WCHAR const **text,
        UINT32 *length) noexcept override {
        position = std::min<UINT32>(position, static_cast<UINT32>(text_.size()));
        *text = position ? text_.data() : nullptr;
        *length = position;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE
    GetParagraphReadingDirection() noexcept override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(
        UINT32 position, UINT32 *length,
        WCHAR const **locale) noexcept override {
        *length = static_cast<UINT32>(text_.size()) - position;
        *locale = locale_.c_str();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(
        UINT32 position, UINT32 *length,
        IDWriteNumberSubstitution **substitution) noexcept override {
        *length = static_cast<UINT32>(text_.size()) - position;
        *substitution = nullptr;
        return S_OK;
    }

private:
    std::wstring text_;
    const std::wstring &locale_;
};

std::wstring utf16_of(uint32_t codepoint) {
    if (codepoint < 0x10000) {
        return std::wstring(1, static_cast<wchar_t>(codepoint));
    }
    codepoint -= 0x10000;
    return {static_cast<wchar_t>(0xD800 + (codepoint >> 10)),
            static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF))};
}

struct color_bitmap {
    int width = 0;
    int height = 0;
    int x0 = 0;
    int y0 = 0;
    bool uses_foreground = false;
    std::vector<unsigned char> rgba;
};

class gdi_canvas {
public:
    gdi_canvas(int width, int height) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc_ = CreateCompatibleDC(nullptr);
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits_, nullptr,
                                   0);
        if (dc_ && bitmap_) {
            previous_ = SelectObject(dc_, bitmap_);
        }
    }
    ~gdi_canvas() {
        if (previous_) {
            SelectObject(dc_, previous_);
        }
        if (bitmap_) {
            DeleteObject(bitmap_);
        }
        if (dc_) {
            DeleteDC(dc_);
        }
    }
    gdi_canvas(const gdi_canvas &) = delete;
    gdi_canvas &operator=(const gdi_canvas &) = delete;

    [[nodiscard]] bool valid() const { return dc_ && bitmap_ && bits_; }
    [[nodiscard]] HDC dc() const { return dc_; }
    [[nodiscard]] const uint8_t *pixels() const {
        return static_cast<const uint8_t *>(bits_);
    }

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void *bits_ = nullptr;
};

D2D1_COLOR_F to_d2d(NVGcolor color) {
    return D2D1::ColorF(color.r, color.g, color.b, 1.0f);
}

void draw_color_run(ID2D1DeviceContext4 *dc, ID2D1DeviceContext7 *dc7,
                    ID2D1SolidColorBrush *foreground, ID2D1SolidColorBrush *layer,
                    const DWRITE_COLOR_GLYPH_RUN1 &run, bool &uses_foreground) {
    const auto origin = D2D1::Point2F(run.baselineOriginX, run.baselineOriginY);
    switch (run.glyphImageFormat) {
    case DWRITE_GLYPH_IMAGE_FORMATS_PNG:
    case DWRITE_GLYPH_IMAGE_FORMATS_JPEG:
    case DWRITE_GLYPH_IMAGE_FORMATS_TIFF:
    case DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8:
        dc->DrawColorBitmapGlyphRun(run.glyphImageFormat, origin, &run.glyphRun,
                                    run.measuringMode);
        break;
    case DWRITE_GLYPH_IMAGE_FORMATS_SVG:
        uses_foreground = true;
        dc->DrawSvgGlyphRun(origin, &run.glyphRun, foreground, nullptr, 0,
                            run.measuringMode);
        break;
    case DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE:
        if (dc7) {
            uses_foreground = true;
            dc7->DrawPaintGlyphRun(origin, &run.glyphRun, foreground, 0,
                                   run.measuringMode);
        }
        break;
    default:
        if (run.paletteIndex == 0xFFFF) {
            uses_foreground = true;
            dc->DrawGlyphRun(origin, &run.glyphRun, run.glyphRunDescription,
                             foreground, run.measuringMode);
        } else {
            layer->SetColor(run.runColor);
            dc->DrawGlyphRun(origin, &run.glyphRun, run.glyphRunDescription,
                             layer, run.measuringMode);
        }
        break;
    }
}

// Returns nullopt when the glyph has no color representation.
std::optional<color_bitmap> rasterize_color_glyph(IDWriteFontFace *face,
                                                  UINT16 glyph, float size,
                                                  NVGcolor foreground) {
    const auto &dw = dwrite();
    if (!dw.factory || !dw.d2d || size < 1.0f) {
        return std::nullopt;
    }

    DWRITE_FONT_METRICS metrics{};
    face->GetMetrics(&metrics);
    DWRITE_GLYPH_METRICS glyph_metrics{};
    if (FAILED(face->GetDesignGlyphMetrics(&glyph, 1, &glyph_metrics, FALSE))) {
        return std::nullopt;
    }
    const float scale = size / metrics.designUnitsPerEm;
    const int pad = static_cast<int>(std::ceil(size * 0.5f)) + 2;
    const int ascent = static_cast<int>(std::ceil(metrics.ascent * scale));
    const int descent = static_cast<int>(std::ceil(metrics.descent * scale));
    const int width =
        static_cast<int>(std::ceil(glyph_metrics.advanceWidth * scale)) + pad * 2;
    const int height = ascent + descent + pad * 2;
    const auto origin = D2D1::Point2F(static_cast<float>(pad),
                                      static_cast<float>(pad + ascent));

    const FLOAT advance = 0;
    DWRITE_GLYPH_RUN run{.fontFace = face,
                         .fontEmSize = size,
                         .glyphCount = 1,
                         .glyphIndices = &glyph,
                         .glyphAdvances = &advance};

    auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1DeviceContext4> dc;
    if (FAILED(dw.d2d->CreateDCRenderTarget(&properties, &target)) ||
        FAILED(target.As(&dc))) {
        return std::nullopt;
    }
    ComPtr<ID2D1DeviceContext7> dc7;
    target.As(&dc7);

    auto formats = DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE |
                   DWRITE_GLYPH_IMAGE_FORMATS_CFF |
                   DWRITE_GLYPH_IMAGE_FORMATS_COLR |
                   DWRITE_GLYPH_IMAGE_FORMATS_SVG |
                   DWRITE_GLYPH_IMAGE_FORMATS_PNG |
                   DWRITE_GLYPH_IMAGE_FORMATS_JPEG |
                   DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
                   DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8;
    ComPtr<IDWriteColorGlyphRunEnumerator1> runs;
    HRESULT hr;
    if (dw.factory8 && dc7) {
        hr = dw.factory8->TranslateColorGlyphRun(
            origin, &run, nullptr,
            formats | DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,
            dc7->GetPaintFeatureLevel(), DWRITE_MEASURING_MODE_NATURAL, nullptr,
            0, &runs);
    } else {
        hr = dw.factory->TranslateColorGlyphRun(
            origin, &run, nullptr, formats, DWRITE_MEASURING_MODE_NATURAL,
            nullptr, 0, &runs);
    }
    if (FAILED(hr)) {
        return std::nullopt;
    }

    gdi_canvas canvas(width, height);
    RECT rect{0, 0, width, height};
    if (!canvas.valid() || FAILED(target->BindDC(canvas.dc(), &rect))) {
        return std::nullopt;
    }
    ComPtr<ID2D1SolidColorBrush> foreground_brush;
    ComPtr<ID2D1SolidColorBrush> layer_brush;
    target->CreateSolidColorBrush(to_d2d(foreground), &foreground_brush);
    target->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &layer_brush);
    if (!foreground_brush || !layer_brush) {
        return std::nullopt;
    }

    color_bitmap result;
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0, 0, 0));
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    BOOL has_run = FALSE;
    while (SUCCEEDED(runs->MoveNext(&has_run)) && has_run) {
        DWRITE_COLOR_GLYPH_RUN1 const *color_run = nullptr;
        if (FAILED(runs->GetCurrentRun(&color_run)) || !color_run) {
            break;
        }
        draw_color_run(dc.Get(), dc7.Get(), foreground_brush.Get(),
                       layer_brush.Get(), *color_run, result.uses_foreground);
    }
    if (FAILED(target->EndDraw())) {
        return std::nullopt;
    }
    GdiFlush();

    const auto *pixels = canvas.pixels();
    int left = width, top = height, right = -1, bottom = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (pixels[(y * width + x) * 4 + 3] != 0) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < left || bottom < top) {
        return std::nullopt;
    }

    result.width = right - left + 1;
    result.height = bottom - top + 1;
    result.x0 = left - pad;
    result.y0 = top - (pad + ascent);
    result.rgba.resize(static_cast<size_t>(result.width) * result.height * 4);
    for (int y = 0; y < result.height; ++y) {
        const auto *src = pixels + ((top + y) * width + left) * 4;
        auto *dst = result.rgba.data() + static_cast<size_t>(y) * result.width * 4;
        for (int x = 0; x < result.width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    return result;
}

uint32_t pack_color(NVGcolor color) {
    auto channel = [](float value) {
        return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f +
                                     0.5f);
    };
    return channel(color.r) << 16 | channel(color.g) << 8 | channel(color.b);
}

class font_manager {
public:
    explicit font_manager(NVGcontext *nvg) : nvg_(nvg) {
        update_locale();
        nvgSetFontFallbackCallback(nvg_, &font_manager::on_missing_glyph, this);
        nvgSetColorGlyphCallback(nvg_, &font_manager::on_color_glyph, this);
    }

    ~font_manager() {
        nvgSetFontFallbackCallback(nvg_, nullptr, nullptr);
        nvgSetColorGlyphCallback(nvg_, nullptr, nullptr);
        for (auto &[_, entry] : color_glyphs_) {
            delete_images(entry);
        }
    }

    font_manager(const font_manager &) = delete;
    font_manager &operator=(const font_manager &) = delete;

    void configure(ui::font_settings settings) {
        std::lock_guard lock(mutex_);
        settings_ = std::move(settings);
        update_locale();
        families_.clear();
        bases_.clear();
    }

    int resolve(std::string_view family, int weight) {
        weight = std::clamp(weight, 1, 999);
        std::lock_guard lock(mutex_);
        auto family_it = families_.find(family);
        if (family_it == families_.end()) {
            family_it = families_.emplace(std::string(family),
                                          std::unordered_map<int, int>{})
                            .first;
        }
        auto &weights = family_it->second;
        if (auto it = weights.find(weight); it != weights.end()) {
            return it->second;
        }
        const auto id = create_family_font(family_it->first, weight);
        weights.emplace(weight, id);
        return id;
    }

    void begin_frame() {
        std::lock_guard lock(mutex_);
        ++frame_;
        if (color_glyphs_.size() <= color_cache_soft_limit) {
            return;
        }
        std::erase_if(color_glyphs_, [&](auto &item) {
            if (item.second.last_used + 1 >= frame_) {
                return false;
            }
            delete_images(item.second);
            return true;
        });
    }

private:
    struct base_font {
        std::wstring family;
        int weight = 400;
        std::unordered_set<uint32_t> misses;
    };

    struct color_glyph_key {
        int font;
        int glyph;
        int size;
        auto operator<=>(const color_glyph_key &) const = default;
    };

    struct color_image {
        int image = 0;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };

    struct color_glyph_entry {
        bool is_color = true;
        bool uses_foreground = false;
        uint64_t last_used = 0;
        std::unordered_map<uint32_t, color_image> images;
    };

    std::vector<font_source> sources_for(std::string_view family) const {
        std::vector<std::string_view> names;
        auto append = [&](const std::vector<std::string> &list) {
            names.insert(names.end(), list.begin(), list.end());
        };
        if (family == "main") {
            append(settings_.main);
        } else if (family == "monospace") {
            append(settings_.monospace);
            append(settings_.main);
        } else if (family != "fallback") {
            names.push_back(family);
            append(settings_.main);
        }
        append(settings_.fallback);
        names.push_back("Segoe UI");

        std::vector<font_source> sources;
        std::unordered_set<std::wstring> seen;
        for (auto name : names) {
            auto source = parse_source(name);
            if (!source) {
                continue;
            }
            auto key = source->family.empty()
                           ? to_lower(source->path) + L"#" +
                                 std::to_wstring(source->index)
                           : to_lower(source->family);
            if (seen.insert(std::move(key)).second) {
                sources.push_back(std::move(*source));
            }
        }
        return sources;
    }

    int create_family_font(const std::string &family, int weight) {
        std::optional<font_face_ref> primary;
        std::vector<int> fallbacks;
        for (const auto &source : sources_for(family)) {
            auto face = resolve_source(source, weight);
            if (!face) {
                continue;
            }
            if (!primary) {
                primary = std::move(face);
                continue;
            }
            if (face->key() == primary->key()) {
                continue;
            }
            if (auto id = load_shared_face(*face); id >= 0) {
                fallbacks.push_back(id);
            }
        }
        if (!primary) {
            return -1;
        }

        const auto name = "#" + std::to_string(++font_serial_);
        const auto id = add_font(name, *primary);
        if (id < 0) {
            return -1;
        }
        for (auto fallback : fallbacks) {
            nvgAddFallbackFontId(nvg_, id, fallback);
        }
        bases_[id] = base_font{.family = primary->family, .weight = weight};
        return id;
    }

    int add_font(const std::string &name, const font_face_ref &face) {
        const auto *file = map_font_file(face.path);
        if (!file) {
            return -1;
        }
        const auto id = nvgCreateFontMemAtIndex(
            nvg_, name.c_str(), file->data, static_cast<int>(file->size), 0,
            static_cast<int>(face.index));
        if (id < 0) {
            return -1;
        }
        ComPtr<IDWriteFontFace2> face2;
        if (settings_.color_glyphs && SUCCEEDED(face.face.As(&face2)) &&
            face2->IsColorFont()) {
            nvgSetFontColor(nvg_, id, 1);
        }
        dwrite_faces_[id] = face.face;
        return id;
    }

    int load_shared_face(const font_face_ref &face) {
        const auto key = face.key();
        if (auto it = shared_faces_.find(key); it != shared_faces_.end()) {
            return it->second;
        }
        const auto id = add_font("#" + std::to_string(++font_serial_), face);
        shared_faces_.emplace(key, id);
        return id;
    }

    int find_fallback(int font, uint32_t codepoint) {
        const auto &dw = dwrite();
        if (!settings_.system_fallback || !dw.fallback) {
            return -1;
        }
        auto &base = bases_[font];
        if (base.misses.contains(codepoint)) {
            return -1;
        }

        text_source text(utf16_of(codepoint), locale_);
        const auto length = static_cast<UINT32>(utf16_of(codepoint).size());
        UINT32 mapped_length = 0;
        ComPtr<IDWriteFont> mapped;
        FLOAT scale = 1.0f;
        const auto hr = dw.fallback->MapCharacters(
            &text, 0, length, dw.collection.Get(),
            base.family.empty() ? nullptr : base.family.c_str(),
            static_cast<DWRITE_FONT_WEIGHT>(base.weight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            &mapped_length, &mapped, &scale);
        BOOL has_character = FALSE;
        if (FAILED(hr) || !mapped ||
            FAILED(mapped->HasCharacter(codepoint, &has_character)) ||
            !has_character) {
            base.misses.insert(codepoint);
            return -1;
        }
        auto face = face_from_font(mapped.Get());
        const auto id = face ? load_shared_face(*face) : -1;
        if (id < 0) {
            base.misses.insert(codepoint);
        }
        return id;
    }

    bool find_color_glyph(int font, int glyph, float size, NVGcolor color,
                          NVGcolorGlyph *out) {
        const auto face_it = dwrite_faces_.find(font);
        if (face_it == dwrite_faces_.end() || glyph <= 0 || glyph > 0xFFFF) {
            return false;
        }
        const color_glyph_key key{font, glyph,
                                  static_cast<int>(std::lround(size * 10))};
        auto &entry = color_glyphs_[key];
        entry.last_used = frame_;
        if (!entry.is_color) {
            return false;
        }

        const auto color_key = entry.uses_foreground ? pack_color(color) : 0;
        auto image_it = entry.images.find(color_key);
        if (image_it == entry.images.end()) {
            auto bitmap = rasterize_color_glyph(
                face_it->second.Get(), static_cast<UINT16>(glyph), size, color);
            if (!bitmap) {
                entry.is_color = false;
                return false;
            }
            entry.uses_foreground = bitmap->uses_foreground;
            const auto image = nvgCreateImageRGBA(
                nvg_, bitmap->width, bitmap->height, NVG_IMAGE_PREMULTIPLIED,
                bitmap->rgba.data());
            if (image == 0) {
                entry.is_color = false;
                return false;
            }
            image_it =
                entry.images
                    .emplace(entry.uses_foreground ? pack_color(color) : 0,
                             color_image{
                                 .image = image,
                                 .x0 = static_cast<float>(bitmap->x0),
                                 .y0 = static_cast<float>(bitmap->y0),
                                 .x1 = static_cast<float>(bitmap->x0 +
                                                          bitmap->width),
                                 .y1 = static_cast<float>(bitmap->y0 +
                                                          bitmap->height),
                             })
                    .first;
        }
        const auto &image = image_it->second;
        *out = NVGcolorGlyph{.image = image.image,
                             .x0 = image.x0,
                             .y0 = image.y0,
                             .x1 = image.x1,
                             .y1 = image.y1};
        return true;
    }

    void delete_images(color_glyph_entry &entry) {
        for (auto &[_, image] : entry.images) {
            nvgDeleteImage(nvg_, image.image);
        }
        entry.images.clear();
    }

    static int on_missing_glyph(void *self, int font, unsigned int codepoint) {
        auto *manager = static_cast<font_manager *>(self);
        std::lock_guard lock(manager->mutex_);
        return manager->find_fallback(font, codepoint);
    }

    static int on_color_glyph(void *self, int font, int glyph, float size,
                              NVGcolor color, NVGcolorGlyph *out) {
        auto *manager = static_cast<font_manager *>(self);
        std::lock_guard lock(manager->mutex_);
        return manager->find_color_glyph(font, glyph, size, color, out) ? 1 : 0;
    }

    void update_locale() {
        locale_ = settings_.locale.empty() ? dwrite().locale
                                           : widen(settings_.locale);
    }

    NVGcontext *nvg_;
    std::mutex mutex_;
    ui::font_settings settings_;
    std::wstring locale_;
    uint64_t frame_ = 0;
    int font_serial_ = 0;
    std::map<std::string, std::unordered_map<int, int>, std::less<>> families_;
    std::unordered_map<int, base_font> bases_;
    std::unordered_map<std::wstring, int> shared_faces_;
    std::unordered_map<int, ComPtr<IDWriteFontFace>> dwrite_faces_;
    std::map<color_glyph_key, color_glyph_entry> color_glyphs_;
};

std::mutex g_managers_mutex;
std::unordered_map<NVGcontext *, std::unique_ptr<font_manager>> g_managers;

font_manager *manager_for(NVGcontext *nvg) {
    if (!nvg) {
        return nullptr;
    }
    std::lock_guard lock(g_managers_mutex);
    auto &manager = g_managers[nvg];
    if (!manager) {
        manager = std::make_unique<font_manager>(nvg);
    }
    return manager.get();
}

} // namespace

namespace ui {

void configure_fonts(NVGcontext *nvg, font_settings settings) {
    if (auto *manager = manager_for(nvg)) {
        manager->configure(std::move(settings));
    }
}

void release_fonts(NVGcontext *nvg) {
    std::unique_ptr<font_manager> manager;
    {
        std::lock_guard lock(g_managers_mutex);
        if (auto it = g_managers.find(nvg); it != g_managers.end()) {
            manager = std::move(it->second);
            g_managers.erase(it);
        }
    }
}

int resolve_font(NVGcontext *nvg, std::string_view family, int weight) {
    auto *manager = manager_for(nvg);
    return manager ? manager->resolve(family.empty() ? "main" : family, weight)
                   : -1;
}

void begin_font_frame(NVGcontext *nvg) {
    if (auto *manager = manager_for(nvg)) {
        manager->begin_frame();
    }
}

bool font_source_available(std::string_view source) {
    auto parsed = parse_source(source);
    return parsed && resolve_source(*parsed, 400).has_value();
}

} // namespace ui
