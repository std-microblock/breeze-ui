#include "breeze_ui/hbitmap_utils.h"
#include "Windows.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
// Shell menus hand us bitmaps owned by the shell or by third-party shell
// extensions. They are frequently not 32bpp (1/4/8/16/24bpp: extension icons,
// monochrome check marks, ...), so the pixel buffer must be sized for the
// *target* 32bpp format - sizing it from the source format reported by the
// query call overflows the heap as soon as the conversion is requested.
constexpr long kMaxIconDimension = 8192;

long bitmap_dimension(LONG value) {
    return value < 0 ? -static_cast<long>(value) : static_cast<long>(value);
}
}  // namespace

ui::NVGImage ui::LoadBitmapImage(nanovg_context ctx, void *hbitmap) {
    HBITMAP hBitmap = (HBITMAP)hbitmap;

    if (!hBitmap || GetObjectType(hBitmap) != OBJ_BITMAP) {
        printf("LoadBitmapImage: not a bitmap handle\n");
        return NVGImage(-1, 0, 0, ctx);
    }

    BITMAP bm = {};

    auto dc = CreateCompatibleDC(NULL);
    if (!dc)
        return NVGImage(-1, 0, 0, ctx);

    if (!GetObject(hBitmap, sizeof(bm), &bm)) {
        printf("LoadBitmapImage: GetObject failed\n");
        DeleteDC(dc);
        return NVGImage(-1, 0, 0, ctx);
    }

    const long width = bitmap_dimension(bm.bmWidth);
    const long height = bitmap_dimension(bm.bmHeight);
    if (width <= 0 || height <= 0 || width > kMaxIconDimension ||
        height > kMaxIconDimension) {
        printf("LoadBitmapImage: unsupported size %ldx%ld\n", bm.bmWidth,
               bm.bmHeight);
        DeleteDC(dc);
        return NVGImage(-1, 0, 0, ctx);
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);

    if (!GetDIBits(dc, hBitmap, 0, 0, NULL, &bi, DIB_RGB_COLORS)) {
        printf("Failed to get DIB bits\n");
        DeleteDC(dc);
        return NVGImage(-1, 0, 0, ctx);
    }

    // Ask GDI for a 32bpp bottom-up copy and size the buffer for *that* format.
    const size_t stride = (static_cast<size_t>(width) * 32 + 31) / 32 * 4;
    const size_t pixel_bytes = stride * static_cast<size_t>(height);

    bi.bmiHeader.biWidth = static_cast<LONG>(width);
    bi.bmiHeader.biHeight = static_cast<LONG>(height);  // positive: bottom-up
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biSizeImage = static_cast<DWORD>(pixel_bytes);

    std::vector<unsigned char> bits(pixel_bytes);

    if (!GetDIBits(dc, hBitmap, 0, static_cast<UINT>(height), bits.data(), &bi,
                   DIB_RGB_COLORS)) {
        printf("Failed to get DIB bits\n");
        DeleteDC(dc);
        return NVGImage(-1, 0, 0, ctx);
    }

    DeleteDC(dc);

    // The source only has a meaningful alpha channel when it already is 32bpp;
    // for converted bitmaps GDI leaves the fourth byte zeroed, which would render
    // the icon fully transparent.
    const bool source_has_alpha = bm.bmBitsPixel >= 32;

    std::vector<uint8_t> rgba(static_cast<size_t>(width) *
                              static_cast<size_t>(height) * 4);

    for (long i = 0; i < width * height; i++) {
        rgba[i * 4 + 0] = bits[i * 4 + 2];
        rgba[i * 4 + 1] = bits[i * 4 + 1];
        rgba[i * 4 + 2] = bits[i * 4 + 0];
        rgba[i * 4 + 3] = source_has_alpha ? bits[i * 4 + 3] : 0xFF;
    }

    // hbitmap is in reverse order (bottom to top)
    for (long y = 0; y < height / 2; y++) {
        for (long x = 0; x < width; x++) {
            for (int i = 0; i < 4; i++) {
                std::swap(rgba[(static_cast<size_t>(y) * width + x) * 4 + i],
                          rgba[(static_cast<size_t>(height - y - 1) * width + x) * 4 + i]);
            }
        }
    }

    auto id = ctx.createImageRGBA(static_cast<int>(width),
                                  static_cast<int>(height), 0, rgba.data());
    if (id == -1) {
        printf("Failed to create image\n");
        return NVGImage(-1, 0, 0, ctx);
    }
    return NVGImage(id, static_cast<int>(width), static_cast<int>(height), ctx);
}
