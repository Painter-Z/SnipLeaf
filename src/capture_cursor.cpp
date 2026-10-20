#include "capture_cursor.h"
namespace snap
{
CaptureCursor::~CaptureCursor()
{
    if (cursor_)
    {
        if (GetCursor() == cursor_)
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        DestroyCursor(cursor_);
    }
}
HCURSOR CaptureCursor::get(UINT dpi)
{
    dpi = std::clamp(dpi, 96u, 384u);
    if (cursor_ && dpi == dpi_)
        return cursor_;
    int arm = MulDiv(12, dpi, 96), outline = std::max(1, MulDiv(1, dpi, 96));
    int core = std::max(1, MulDiv(1, dpi, 96)), half = core / 2;
    int radius = arm + outline, size = radius * 2 + 1;
    Dib colorBitmap(size, size);
    std::fill_n(colorBitmap.bits, size * size * 4, uint8_t{});
    int stride = ((size + 15) / 16) * 2;
    std::vector<uint8_t> mask(static_cast<size_t>(stride) * size, 0xff);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
        {
            int dx = std::abs(x - radius), dy = std::abs(y - radius);
            bool dark = (dx <= half && dy <= arm) || (dy <= half && dx <= arm);
            bool white = (dx <= half + outline && dy <= arm + outline) ||
                         (dy <= half + outline && dx <= arm + outline);
            if (!white)
                continue;
            auto p = colorBitmap.bits + (y * size + x) * 4;
            p[0] = p[1] = p[2] = dark ? 22 : 255;
            p[3] = 255;
            mask[y * stride + x / 8] &= static_cast<uint8_t>(~(0x80 >> (x % 8)));
        }
    HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, mask.data());
    if (!maskBitmap)
        return cursor_ ? cursor_ : LoadCursor(nullptr, IDC_CROSS);
    ICONINFO info{FALSE, static_cast<DWORD>(radius), static_cast<DWORD>(radius), maskBitmap, colorBitmap.bitmap};
    HCURSOR next = static_cast<HCURSOR>(CreateIconIndirect(&info));
    DeleteObject(maskBitmap);
    if (next)
    {
        if (cursor_)
        {
            if (GetCursor() == cursor_)
                SetCursor(next);
            DestroyCursor(cursor_);
        }
        cursor_ = next;
        dpi_ = dpi;
    }
    return cursor_ ? cursor_ : LoadCursor(nullptr, IDC_CROSS);
}
} // namespace snap
