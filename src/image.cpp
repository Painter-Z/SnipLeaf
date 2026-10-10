#include "image.h"
namespace snap
{
Image::Image(int w, int h, unsigned rgb) : width(w), height(h)
{
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || static_cast<uint64_t>(w) * h > 134217728)
        throw std::runtime_error("Image is too large or invalid.");
    pixels.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        pixels[i] = static_cast<uint8_t>(rgb & 255);
        pixels[i + 1] = static_cast<uint8_t>((rgb >> 8) & 255);
        pixels[i + 2] = static_cast<uint8_t>((rgb >> 16) & 255);
        pixels[i + 3] = 255;
    }
}
Image Image::crop(RECT r) const
{
    r.left = std::clamp<LONG>(r.left, 0, width);
    r.right = std::clamp<LONG>(r.right, r.left, width);
    r.top = std::clamp<LONG>(r.top, 0, height);
    r.bottom = std::clamp<LONG>(r.bottom, r.top, height);
    Image out(r.right - r.left, r.bottom - r.top);
    for (int y = 0; y < out.height; ++y)
        std::memcpy(out.pixels.data() + y * out.stride(), pixels.data() + (y + r.top) * stride() + r.left * 4,
                    out.stride());
    return out;
}
Image Image::resized(int w, int h) const
{
    Image out(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float fx = std::clamp((x + .5f) * width / w - .5f, 0.f, static_cast<float>(width - 1));
            float fy = std::clamp((y + .5f) * height / h - .5f, 0.f, static_cast<float>(height - 1));
            int sx = static_cast<int>(fx), sy = static_cast<int>(fy);
            int nx = std::min(width - 1, sx + 1), ny = std::min(height - 1, sy + 1);
            float dx = fx - sx, dy = fy - sy;
            for (int channel = 0; channel < 3; ++channel)
            {
                float a = pixels[sy * stride() + sx * 4 + channel], b = pixels[sy * stride() + nx * 4 + channel];
                float c = pixels[ny * stride() + sx * 4 + channel], d = pixels[ny * stride() + nx * 4 + channel];
                out.pixels[y * out.stride() + x * 4 + channel] =
                    static_cast<uint8_t>(std::round((a + (b - a) * dx) * (1 - dy) + (c + (d - c) * dx) * dy));
            }
        }
    return out;
}
Dib::Dib(int w, int h) : width(w), height(h)
{
    dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *p{};
    bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    bits = static_cast<uint8_t *>(p);
    if (!dc || !bitmap)
    {
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        throw std::runtime_error("Cannot create image buffer.");
    }
    old = SelectObject(dc, bitmap);
}
Dib::Dib(Image const &image) : Dib(image.width, image.height)
{
    std::memcpy(bits, image.pixels.data(), image.pixels.size());
}
Dib::~Dib()
{
    if (dc)
    {
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
}
Image Dib::image() const
{
    Image out(width, height);
    GdiFlush();
    std::memcpy(out.pixels.data(), bits, out.pixels.size());
    for (size_t i = 3; i < out.pixels.size(); i += 4)
        out.pixels[i] = 255;
    return out;
}
Image captureDesktop(RECT &bounds)
{
    bounds = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    bounds.right = bounds.left + w;
    bounds.bottom = bounds.top + h;
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || static_cast<uint64_t>(w) * h > 134217728)
        throw std::runtime_error("Desktop dimensions exceed the capture buffer limit.");
    Dib dib(w, h);
    HDC screen = GetDC(nullptr);
    BOOL ok = BitBlt(dib.dc, 0, 0, w, h, screen, bounds.left, bounds.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (!ok)
        throw std::runtime_error("Desktop capture failed.");
    return dib.image();
}
static Image decodeSource(Graphics &g, IWICBitmapSource *source)
{
    UINT w{}, h{};
    check(source->GetSize(&w, &h));
    Image image(static_cast<int>(w), static_cast<int>(h));
    winrt::com_ptr<IWICFormatConverter> converter;
    check(g.wic->CreateFormatConverter(converter.put()));
    check(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom));
    check(converter->CopyPixels(nullptr, static_cast<UINT>(image.stride()), static_cast<UINT>(image.pixels.size()),
                                image.pixels.data()));
    for (size_t i = 0; i < image.pixels.size(); i += 4)
    {
        unsigned a = image.pixels[i + 3];
        for (int c = 0; c < 3; ++c)
            image.pixels[i + c] = static_cast<uint8_t>(std::min(255u, image.pixels[i + c] + 255 - a));
        image.pixels[i + 3] = 255;
    }
    return image;
}
Image loadImage(Graphics &g, std::wstring const &path)
{
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    check(g.wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                           decoder.put()));
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, frame.put()));
    return decodeSource(g, frame.get());
}
static void encodePng(Graphics &g, Image const &image, IStream *stream)
{
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    check(g.wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    check(encoder->Initialize(stream, WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(frame.put(), nullptr));
    check(frame->Initialize(nullptr));
    check(frame->SetSize(image.width, image.height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format));
    check(frame->WritePixels(image.height, static_cast<UINT>(image.stride()), static_cast<UINT>(image.pixels.size()),
                             const_cast<BYTE *>(image.pixels.data())));
    check(frame->Commit());
    check(encoder->Commit());
}
void savePng(Graphics &g, Image const &image, std::wstring const &path)
{
    winrt::com_ptr<IWICStream> stream;
    check(g.wic->CreateStream(stream.put()));
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    encodePng(g, image, stream.get());
}
struct Clipboard
{
    bool opened{};
    explicit Clipboard(HWND owner)
    {
        for (int i = 0; i < 5 && !opened; ++i)
        {
            opened = OpenClipboard(owner) != FALSE;
            if (!opened)
                Sleep(10);
        }
    }
    ~Clipboard()
    {
        if (opened)
            CloseClipboard();
    }
};
struct GlobalBlock
{
    HGLOBAL handle{};
    explicit GlobalBlock(size_t bytes) : handle(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes)) {}
    ~GlobalBlock()
    {
        if (handle)
            GlobalFree(handle);
    }
    GlobalBlock(GlobalBlock const &) = delete;
    GlobalBlock &operator=(GlobalBlock const &) = delete;
    void release()
    {
        handle = nullptr;
    }
};
bool copyText(HWND owner, std::wstring const &text)
{
    if (text.empty())
        return false;
    GlobalBlock mem((text.size() + 1) * sizeof(wchar_t));
    if (!mem.handle)
        return false;
    auto p = GlobalLock(mem.handle);
    if (!p)
        return false;
    std::memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem.handle);
    Clipboard cb(owner);
    if (!cb.opened || !EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, mem.handle))
        return false;
    mem.release(); // Windows owns the allocation after SetClipboardData succeeds.
    return true;
}
bool copyImage(HWND owner, Graphics &g, Image const &image)
{
    if (image.empty())
        return false;
    size_t size = sizeof(BITMAPV5HEADER) + image.pixels.size();
    GlobalBlock mem(size);
    if (!mem.handle)
        return false;
    auto *head = static_cast<BITMAPV5HEADER *>(GlobalLock(mem.handle));
    if (!head)
        return false;
    *head = {};
    head->bV5Size = sizeof(*head);
    head->bV5Width = image.width;
    head->bV5Height = -image.height;
    head->bV5Planes = 1;
    head->bV5BitCount = 32;
    head->bV5Compression = BI_BITFIELDS;
    head->bV5RedMask = 0x00ff0000;
    head->bV5GreenMask = 0x0000ff00;
    head->bV5BlueMask = 0x000000ff;
    head->bV5AlphaMask = 0xff000000;
    head->bV5CSType = LCS_sRGB;
    std::memcpy(head + 1, image.pixels.data(), image.pixels.size());
    GlobalUnlock(mem.handle);
    winrt::com_ptr<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    encodePng(g, image, stream.get());
    STATSTG info{};
    check(stream->Stat(&info, STATFLAG_NONAME));
    if (!info.cbSize.QuadPart || info.cbSize.QuadPart > std::numeric_limits<ULONG>::max())
        return false;
    auto pngSize = static_cast<ULONG>(info.cbSize.QuadPart);
    GlobalBlock pngCopy(pngSize);
    if (!pngCopy.handle)
        return false;
    check(stream->Seek({}, STREAM_SEEK_SET, nullptr));
    auto bytes = GlobalLock(pngCopy.handle);
    if (!bytes)
        return false;
    ULONG read{};
    auto result = stream->Read(bytes, pngSize, &read);
    GlobalUnlock(pngCopy.handle);
    check(result);
    if (read != pngSize)
        return false;
    Clipboard cb(owner);
    if (!cb.opened || !EmptyClipboard() || !SetClipboardData(CF_DIBV5, mem.handle))
        return false;
    mem.release();
    if (auto format = RegisterClipboardFormat(L"PNG"); format && SetClipboardData(format, pngCopy.handle))
        pngCopy.release();
    return true;
}
Image pasteImage(HWND owner, Graphics &g)
{
    Clipboard cb(owner);
    if (!cb.opened)
        return {};
    if (auto handle = GetClipboardData(RegisterClipboardFormat(L"PNG")))
    {
        auto ptr = static_cast<BYTE *>(GlobalLock(handle));
        auto size = GlobalSize(handle);
        if (ptr && size && size <= UINT32_MAX)
        {
            try
            {
                winrt::com_ptr<IWICStream> stream;
                check(g.wic->CreateStream(stream.put()));
                check(stream->InitializeFromMemory(ptr, static_cast<DWORD>(size)));
                winrt::com_ptr<IWICBitmapDecoder> decoder;
                check(
                    g.wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put()));
                winrt::com_ptr<IWICBitmapFrameDecode> frame;
                check(decoder->GetFrame(0, frame.put()));
                auto image = decodeSource(g, frame.get());
                GlobalUnlock(handle);
                return image;
            }
            catch (...)
            {
                GlobalUnlock(handle);
            }
        }
        else if (ptr)
            GlobalUnlock(handle);
    }
    if (auto bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)))
    {
        winrt::com_ptr<IWICBitmap> wic;
        check(g.wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, wic.put()));
        return decodeSource(g, wic.get());
    }
    return {};
}
template <class Buffer> static void pixelateBuffer(Buffer &image, Box area, int block)
{
    int left = std::clamp(static_cast<int>(std::floor(area.x)), 0, image.width),
        top = std::clamp(static_cast<int>(std::floor(area.y)), 0, image.height);
    int right = std::clamp(static_cast<int>(std::ceil(area.x + area.w)), 0, image.width),
        bottom = std::clamp(static_cast<int>(std::ceil(area.y + area.h)), 0, image.height);
    uint8_t *data;
    if constexpr (std::is_same_v<Buffer, Image>)
        data = image.pixels.data();
    else
    {
        GdiFlush();
        data = image.bits;
    }
    block = std::max(4, block);
    // Anchor tiles to the image, so extending a live mosaic does not make its
    // existing blocks jump. Sample whole blocks, but only write the selected area.
    for (int y = top / block * block; y < bottom; y += block)
        for (int x = left / block * block; x < right; x += block)
        {
            int endX = std::min(x + block, image.width), endY = std::min(y + block, image.height);
            unsigned sum[3]{}, count = static_cast<unsigned>((endX - x) * (endY - y));
            for (int yy = y; yy < endY; ++yy)
                for (int xx = x; xx < endX; ++xx)
                    for (int c = 0; c < 3; ++c)
                        sum[c] += data[(static_cast<size_t>(yy) * image.width + xx) * 4 + c];
            for (int yy = std::max(y, top); yy < std::min(endY, bottom); ++yy)
                for (int xx = std::max(x, left); xx < std::min(endX, right); ++xx)
                {
                    auto p = data + (static_cast<size_t>(yy) * image.width + xx) * 4;
                    for (int c = 0; c < 3; ++c)
                        p[c] = static_cast<uint8_t>(sum[c] / count);
                    p[3] = 255;
                }
        }
}
void pixelate(Image &image, Box b, int block)
{
    pixelateBuffer(image, b, block);
}
void pixelateDib(Dib &dib, Box b, int block)
{
    pixelateBuffer(dib, b, block);
}
Image demoImage(Graphics &g)
{
    Image image(1000, 520, 0xf7f8fc);
    Dib dib(image);
    winrt::com_ptr<ID2D1DCRenderTarget> rt;
    auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    check(g.d2d->CreateDCRenderTarget(&properties, rt.put()));
    RECT r{0, 0, image.width, image.height};
    check(rt->BindDC(dib.dc, &r));
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    check(rt->CreateSolidColorBrush(color(0x21283d), brush.put()));
    rt->BeginDraw();
    auto title = g.font(40, true), body = g.font(26), captionFont = g.font(21);
    auto draw = [&](wchar_t const *text, IDWriteTextFormat *font, Box box, unsigned rgb)
    {
        brush->SetColor(color(rgb));
        rt->DrawTextW(text, static_cast<UINT32>(wcslen(text)), font, dr(box), brush.get());
    };
    draw(L"截图、标注、贴图，随手完成。", title.get(), {52, 42, 900, 65}, 0x21283d);
    draw(L"SnipLeaf  /  轻量截图工具", body.get(), {54, 126, 880, 45}, 0x6b7390);
    draw(L"直接在图片上拖选文字，再按 Ctrl+C。", body.get(), {54, 213, 880, 45}, 0x21283d);
    draw(L"Select words on this image and copy them.", body.get(), {54, 271, 890, 45}, 0x21283d);
    draw(L"中文 English 混合文本：离线识别 12345", body.get(), {54, 329, 890, 45}, 0x21283d);
    brush->SetColor(color(0xe9ecf6));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(52, 420, 948, 480), 12, 12), brush.get());
    draw(L"箭头  ·  直线  ·  文本  ·  马赛克  ·  记号笔", captionFont.get(), {73, 434, 854, 40}, 0x5d6684);
    check(rt->EndDraw());
    return dib.image();
}
} // namespace snap
