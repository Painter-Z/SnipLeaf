#include "canvas.h"
#include "app.h"
namespace snap
{
namespace
{
constexpr int Move = 1, Arrow = 2, Line = 3, Text = 4, Mosaic = 5, Marker = 6, Select = 7, Rectangle = 8, Ellipse = 9;
constexpr int Undo = 10, Redo = 11, Color = 12, Width = 13, Save = 14, Pin = 15, Copy = 16, Close = 17, AllText = 18,
              Retry = 19, Tools = 20, SmoothZoom = 21, PixelZoom = 22, Translate = 23, CopyFile = 24, ActualSize = 25,
              DoubleSize = 26;
constexpr unsigned Bg = 0xf6f8fb, Fg = 0x344054, Muted = 0x667085, Accent = 0x168dd9;
constexpr float ToolbarWidth = 636.f, ToolbarHeight = 67.f;
Point mouse(LPARAM lp)
{
    return {static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp))};
}
Tool toolFrom(int n)
{
    return static_cast<Tool>(n - 1);
}
} // namespace
Canvas::Canvas(App &a, uint64_t id, std::unique_ptr<Document> doc, bool tools, std::optional<PinPlacement> placement)
    : app_(a), id_(id), doc_(std::move(doc)), pinned_(true), showTools_(tools), initialPlacement_(placement)
{
    fontPt_ = a.config.textSizeTenths / 10.f;
}
Canvas::Canvas(App &a, uint64_t id, Image desktop, RECT bounds)
    : app_(a), id_(id), desktop_(std::make_shared<Image>(std::move(desktop))), desktopBounds_(bounds), showTools_(true)
{
    fontPt_ = a.config.textSizeTenths / 10.f;
}
Canvas::~Canvas()
{
    dropPreview();
    closeFontPicker(false);
    if (hwnd_ && IsWindow(hwnd_))
        DestroyWindow(hwnd_);
    if (editFont_)
        DeleteObject(editFont_);
}
bool Canvas::create(bool activate)
{
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEX wc{sizeof(wc)};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = app_.instance();
        wc.lpszClassName = L"SnapLite.Canvas";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(app_.instance(), MAKEINTRESOURCE(1));
        wc.style = CS_DBLCLKS;
        registered = RegisterClassEx(&wc) != 0;
    }
    if (!registered)
        return false;
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfo(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    int x{}, y{}, w{}, h{};
    if (pinned_)
    {
        float maxW = static_cast<float>(mi.rcWork.right - mi.rcWork.left) * .75f,
              maxH = static_cast<float>(mi.rcWork.bottom - mi.rcWork.top) * .75f;
        view_.zoom = std::min({1.f, maxW / doc_->base->width, (maxH - 80.f) / doc_->base->height});
        ui_ = GetDpiForSystem() / 96.f;
        w = static_cast<int>(std::ceil(doc_->base->width * view_.zoom)) + 2;
        h = static_cast<int>(std::ceil(doc_->base->height * view_.zoom)) + 2;
        if (showTools_)
        {
            w = std::max(w, static_cast<int>((ToolbarWidth + 8) * ui_));
            h += static_cast<int>((ToolbarHeight + 4) * ui_);
        }
        x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
        y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
        if (initialPlacement_ && !showTools_)
        {
            // A captured image is already positioned in physical screen pixels.
            // Keep its content in place; the one-pixel frame sits outside it.
            view_.zoom = initialPlacement_->zoom;
            w = static_cast<int>(std::ceil(doc_->base->width * view_.zoom)) + 2;
            h = static_cast<int>(std::ceil(doc_->base->height * view_.zoom)) + 2;
            x = initialPlacement_->imageOrigin.x - 1;
            y = initialPlacement_->imageOrigin.y - 1;
        }
    }
    else
    {
        x = desktopBounds_.left;
        y = desktopBounds_.top;
        w = desktop_->width;
        h = desktop_->height;
    }
    hwnd_ = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SnapLite.Canvas", L"SnipLeaf",
                           WS_POPUP | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, app_.instance(), this);
    if (!hwnd_)
        return false;
    ui_ = GetDpiForWindow(hwnd_) / 96.f;
    if (!pinned_ && !app_.testing_)
        SetCursor(crosshair_.get(GetDpiForWindow(hwnd_)));
    if (pinned_)
    {
        if (initialPlacement_ && !showTools_)
            SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        else
            fitPin(true);
    }
    layout();
    initialPlacement_.reset();
    DWORD corner = 1;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    // We draw a one-pixel frame ourselves. Older Windows versions ignore this attribute.
    DWORD border = 0xfffffffe; // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    if (!app_.testing_)
        ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate && !app_.testing_)
    {
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
    }
    if (doc_ && app_.config.autoOcr && !ocr_)
        requestOcr();
    return true;
}
void Canvas::show(bool visible)
{
    if (!visible)
        dropPreview();
    ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}
void Canvas::close()
{
    if (closing_)
        return;
    closing_ = true;
    app_.translation_->sourceClosed(id_);
    closeFontPicker(false);
    app_.ocr().cancel(id_);
    KillTimer(hwnd_, 1);
    finishText(false);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    PostMessage(app_.root(), WM_CANVAS_CLOSED, static_cast<WPARAM>(id_), 0);
}
LRESULT CALLBACK Canvas::windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Canvas *self = reinterpret_cast<Canvas *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        self = static_cast<Canvas *>(reinterpret_cast<CREATESTRUCT *>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProc(hwnd, msg, wp, lp);
    try
    {
        return self->message(msg, wp, lp);
    }
    catch (...)
    {
        self->status_ = L"操作失败：" + errorText();
        if (msg == WM_PAINT)
        {
            ValidateRect(hwnd, nullptr);
            self->app_.balloon(L"画面显示失败", self->status_, NIIF_ERROR);
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        else
            InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
}
Box Canvas::availableArea() const
{
    RECT client{}, window{};
    GetClientRect(hwnd_, &client);
    GetWindowRect(hwnd_, &window);
    RECT target = pinned_ ? window
                          : RECT{desktopBounds_.left + crop_.left, desktopBounds_.top + crop_.top,
                                 desktopBounds_.left + crop_.right, desktopBounds_.top + crop_.bottom};
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfo(MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &mi);
    float left = static_cast<float>(std::max<LONG>(0, mi.rcWork.left - window.left));
    float top = static_cast<float>(std::max<LONG>(0, mi.rcWork.top - window.top));
    float right = static_cast<float>(std::min(client.right, mi.rcWork.right - window.left));
    float bottom = static_cast<float>(std::min(client.bottom, mi.rcWork.bottom - window.top));
    if (right - left < 40 || bottom - top < 80)
        return {0, 0, static_cast<float>(client.right), static_cast<float>(client.bottom)};
    return {left, top, right - left, bottom - top};
}
void Canvas::fitPin(bool reposition)
{
    if (!pinned_ || !doc_)
        return;
    RECT window{};
    GetWindowRect(hwnd_, &window);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfo(MonitorFromRect(&window, MONITOR_DEFAULTTONEAREST), &mi);
    int maxW = mi.rcWork.right - mi.rcWork.left, maxH = mi.rcWork.bottom - mi.rcWork.top;
    int w = static_cast<int>(std::ceil(doc_->base->width * view_.zoom)) + 2,
        h = static_cast<int>(std::ceil(doc_->base->height * view_.zoom)) + 2;
    if (showTools_)
    {
        w = std::max(w, static_cast<int>((ToolbarWidth + 8) * ui_));
        int columns = std::clamp(static_cast<int>((maxW - 10 * ui_) / (34 * ui_)), 1, 17);
        h += static_cast<int>((42.f * ((17 + columns - 1) / columns) + 29.f) * ui_);
    }
    w = std::clamp(w, 8, std::max(8, maxW));
    h = std::clamp(h, 8, std::max(8, maxH));
    int x = reposition ? std::clamp(window.left, mi.rcWork.left, mi.rcWork.right - w) : window.left;
    int y = reposition ? std::clamp(window.top, mi.rcWork.top, mi.rcWork.bottom - h) : window.top;
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}
PinPlacement Canvas::imagePlacement() const
{
    POINT origin{static_cast<LONG>(std::lround(view_.origin.x)), static_cast<LONG>(std::lround(view_.origin.y))};
    if (!ClientToScreen(hwnd_, &origin))
        throw std::runtime_error("Cannot determine image screen position.");
    return {origin, view_.zoom};
}
void Canvas::layout()
{
    if (!hwnd_)
        return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    float w = static_cast<float>(client.right);
    if (pinned_ && doc_)
        view_.origin = {std::max(1.f, std::round((w - doc_->base->width * view_.zoom) / 2.f)), 1.f};
    else if (doc_)
        view_.origin = {static_cast<float>(crop_.left), static_cast<float>(crop_.top)};
    if (showTools_ && doc_)
    {
        struct Def
        {
            int id;
            wchar_t const *hint;
        };
        constexpr Def defs[] = {{Move, L"移动 / 选择标注 (V) · 空白默认移动截图"},
                                {Rectangle, L"矩形 (R) · Shift 绘制正方形"},
                                {Ellipse, L"椭圆 (E) · Shift 绘制圆形"},
                                {Arrow, L"箭头 (A)"},
                                {Line, L"直线 (L)"},
                                {Text, L"文字 (T) · Enter 完成 · 双击文字修改"},
                                {Mosaic, L"马赛克 (M) · 拖动遮挡"},
                                {Marker, L"直线记号笔 (P) · Shift 吸附方向"},
                                {Select, L"选字 (O) · 拖选后 Ctrl+C"},
                                {Translate, L"英译中 · 翻译所选文字 / 全文（联网）"},
                                {Undo, L"撤销 (Ctrl+Z)"},
                                {Redo, L"重做 (Ctrl+Y)"},
                                {Color, L"标注颜色"},
                                {Width, L"线条粗细 / 文字字号 (pt) / 马赛克方格 (px)"},
                                {Save, L"保存 PNG (Ctrl+S)"},
                                {Pin, L"贴到屏幕 (F3 / Ctrl+T)"},
                                {Copy, L"复制 (Enter / Ctrl+C)"},
                                {Close, L"关闭 (Esc)"}};
        auto area = availableArea();
        float pad = std::min(4.f * ui_, std::min(area.w, area.h) / 10.f);
        float usable = std::max(1.f, area.w - pad * 2);
        int count = static_cast<int>(std::size(defs));
        int columns = std::clamp(static_cast<int>((usable - 10 * ui_) / (34 * ui_)), 1, count);
        int rows = (count + columns - 1) / columns;
        float cell = std::min(34.f * ui_, (usable - 10 * ui_) / columns);
        cell = std::max(1.f, std::min(cell, (area.h - pad * 2 - 25 * ui_ - 10 * ui_) / rows));
        float barW = std::min(usable, cell * columns + 10 * ui_), barH = cell * rows + 10 * ui_;
        float statusH = std::min(25 * ui_, std::max(0.f, area.h - barH - pad * 2));
        float x = pinned_ ? area.x + (area.w - barW) / 2 : static_cast<float>(crop_.right) - barW;
        x = std::clamp(x, area.x + pad, std::max(area.x + pad, area.x + area.w - barW - pad));
        float y = pinned_ ? area.y + area.h - barH - statusH - pad : crop_.bottom + 8 * ui_;
        if (!pinned_ && y + barH + statusH > area.y + area.h - pad)
        {
            y = crop_.top - barH - statusH - 8 * ui_;
            if (y < area.y + pad)
                y = crop_.bottom - barH - statusH - 8 * ui_; // Fullscreen selection: move inside.
        }
        y = std::clamp(y, area.y + pad, std::max(area.y + pad, area.y + area.h - barH - statusH - pad));
        toolbar_ = {x, y, barW, barH};
        statusBox_ = {x, y + barH, barW, statusH};
        buttons_.clear();
        for (size_t i = 0; i < std::size(defs); ++i)
            buttons_.push_back({defs[i].id,
                                {x + 5 * ui_ + static_cast<int>(i % columns) * cell,
                                 y + 5 * ui_ + static_cast<int>(i / columns) * cell, cell, cell},
                                defs[i].hint});
    }
    else
    {
        buttons_.clear();
        toolbar_ = statusBox_ = {};
    }
    if (rt_)
        if (auto target = rt_.try_as<ID2D1HwndRenderTarget>())
        {
            auto size = target->GetPixelSize();
            if (size.width != static_cast<UINT32>(client.right) || size.height != static_cast<UINT32>(client.bottom))
                check(target->Resize(D2D1::SizeU(client.right, client.bottom)));
        }
}
void Canvas::refreshBitmap()
{
    dropPreview();
    bitmap_ = nullptr;
    displayRevision_ = 0;
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void Canvas::discardTargetBitmaps()
{
    previewBitmap_ = nullptr;
    pinFrame_ = nullptr;
    bitmap_ = nullptr;
    backgroundBitmap_ = nullptr;
    mosaicBitmap_ = nullptr;
    for (auto &effect : tailEffects_)
        effect.bitmap = nullptr;
    for (auto &effect : displayEffects_)
        effect.bitmap = nullptr;
}
void Canvas::dropPreview()
{
    if (hwnd_)
        KillTimer(hwnd_, 3);
    if (previewRequest_ && app_.previewScaler_)
        app_.previewScaler_->cancel(id_);
    previewRequest_.reset();
    previewImage_.reset();
    previewBitmap_ = nullptr;
    ++previewEpoch_;
}
void Canvas::syncPreview()
{
    bool eligible = !closing_ && pinned_ && doc_ && smoothZoom_ && view_.zoom < .999f && !edit_ && !liveBase_ &&
                    !stroke_ && !cropping_;
    int width = eligible ? std::max(1, static_cast<int>(std::ceil(doc_->base->width * view_.zoom))) : 0;
    int height = eligible ? std::max(1, static_cast<int>(std::ceil(doc_->base->height * view_.zoom))) : 0;
    eligible = eligible && validPreviewSize(*doc_->rendered, width, height);
    if (!eligible)
    {
        if (previewRequest_)
            dropPreview();
        return;
    }
    if (previewRequest_ && previewRequest_->source == doc_->rendered && previewRequest_->revision == doc_->revision &&
        previewRequest_->width == width && previewRequest_->height == height)
        return;
    dropPreview();
    previewRequest_ = PreviewRequest{id_, previewEpoch_, doc_->revision, doc_->rendered, width, height, {}};
    SetTimer(hwnd_, 3, 120, nullptr);
}
void Canvas::receivePreview(PreviewResult result)
{
    syncPreview();
    if (!previewRequest_ || result.request.epoch != previewRequest_->epoch ||
        result.request.source != previewRequest_->source || result.request.revision != previewRequest_->revision ||
        result.request.width != previewRequest_->width || result.request.height != previewRequest_->height)
        return;
    previewImage_ = std::move(result.image);
    previewBitmap_ = nullptr;
    requestFrame();
}
void Canvas::requestFrame(bool interactive)
{
    InvalidateRect(hwnd_, nullptr, FALSE);
    // WM_PAINT has lower queue priority than pointer messages. Keep live strokes
    // visible even under a continuous stream of mouse input, at most 120 updates/s.
    if (interactive && !app_.testing_ && Clock::now() - lastPaint_ >= std::chrono::milliseconds(8))
        UpdateWindow(hwnd_);
}
void Canvas::invalidateOcr()
{
    if (app_.translation_)
        app_.translation_->sourceChanged(id_);
    anchor_ = caret_ = -1;
    ocr_ = nullptr;
    ++ocrEpoch_;
    KillTimer(hwnd_, 1);
    app_.ocr().cancel(id_);
}
void Canvas::clearLive()
{
    liveBase_.reset();
    mosaicImage_.reset();
    mosaicBitmap_ = nullptr;
    tailEffects_.clear();
    bitmap_ = nullptr;
}
void Canvas::prepareLive(int index, Tool kind)
{
    dropPreview();
    clearLive();
    if (index < 0)
        liveBase_ = doc_->rendered;
    else
        liveBase_ = index == 0 ? doc_->base
                               : std::make_shared<Image>(renderAnnotations(app_.graphics, *doc_->base,
                                                                           std::span(doc_->marks).first(index)));
    if (kind == Tool::Mosaic)
    {
        auto image = std::make_shared<Image>(*liveBase_);
        int pixels = index < 0 ? mosaicPixels_ : mosaicCellPixels(doc_->marks[index]);
        pixelate(*image, {0, 0, static_cast<float>(image->width), static_cast<float>(image->height)}, pixels);
        mosaicImage_ = std::move(image);
    }
    if (index >= 0)
        for (size_t i = index + 1; i < doc_->marks.size(); ++i)
        {
            auto const &m = doc_->marks[i];
            if (m.kind != Tool::Mosaic)
                continue;
            std::vector<Annotation> below(doc_->marks.begin(), doc_->marks.begin() + i);
            below.erase(below.begin() + index);
            Image image = renderAnnotations(app_.graphics, *doc_->base, below);
            auto b = between(m.from, m.to);
            RECT r{static_cast<LONG>(std::clamp(std::floor(b.x), 0.f, static_cast<float>(image.width))),
                   static_cast<LONG>(std::clamp(std::floor(b.y), 0.f, static_cast<float>(image.height))),
                   static_cast<LONG>(std::clamp(std::ceil(b.x + b.w), 0.f, static_cast<float>(image.width))),
                   static_cast<LONG>(std::clamp(std::ceil(b.y + b.h), 0.f, static_cast<float>(image.height)))};
            if (r.right <= r.left || r.bottom <= r.top)
                continue;
            pixelate(image, b, mosaicCellPixels(m));
            tailEffects_.push_back({i,
                                    {static_cast<float>(r.left), static_cast<float>(r.top),
                                     static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top)},
                                    std::make_shared<Image>(image.crop(r)),
                                    {}});
        }
}
int Canvas::objectAt(Point image) const
{
    if (!doc_)
        return -1;
    for (int i = static_cast<int>(doc_->marks.size()) - 1; i >= 0; --i)
        if (hitAnnotation(doc_->marks[i], image, 5 * ui_ / view_.zoom))
            return i;
    return -1;
}
bool Canvas::annotationsEditable() const
{
    return !pinned_ || showTools_;
}
std::vector<Point> Canvas::objectHandles(Annotation const &m) const
{
    if (segmentAnnotation(m))
        return {m.from, m.to};
    auto b = annotationBounds(m);
    return {{b.x, b.y},
            {b.x + b.w / 2, b.y},
            {b.x + b.w, b.y},
            {b.x + b.w, b.y + b.h / 2},
            {b.x + b.w, b.y + b.h},
            {b.x + b.w / 2, b.y + b.h},
            {b.x, b.y + b.h},
            {b.x, b.y + b.h / 2}};
}
int Canvas::handleAt(Point p) const
{
    if (!annotationsEditable() || !doc_ || selected_ < 0 || selected_ >= static_cast<int>(doc_->marks.size()))
        return -1;
    auto handles = objectHandles(doc_->marks[selected_]);
    for (size_t i = 0; i < handles.size(); ++i)
    {
        auto v = view_.toView(handles[i]);
        if (std::hypot(v.x - p.x, v.y - p.y) <= 6 * ui_)
            return static_cast<int>(i);
    }
    return -1;
}
void Canvas::beginObject(int index, int handle, Point p)
{
    if (!annotationsEditable() || !doc_ || index < 0 || index >= static_cast<int>(doc_->marks.size()))
        return;
    selected_ = index;
    objectHandle_ = handle;
    objectOriginal_ = doc_->marks[index];
    objectStart_ = annotationBounds(objectOriginal_);
    objectDragging_ = true;
    objectMoved_ = false;
    start_ = last_ = p;
    stroke_ = objectOriginal_;
    invalidateOcr();
    prepareLive(index, objectOriginal_.kind);
    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void Canvas::paint()
{
    struct PaintScope
    {
        HWND window;
        PAINTSTRUCT paint{};
        explicit PaintScope(HWND hwnd) : window(hwnd)
        {
            BeginPaint(window, &paint);
        }
        ~PaintScope()
        {
            EndPaint(window, &paint);
        }
    } paintScope(hwnd_);
    if (!rt_)
    {
        RECT r{};
        GetClientRect(hwnd_, &r);
        auto props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = 96;
        winrt::com_ptr<ID2D1HwndRenderTarget> hwndTarget;
        HRESULT hr = app_.graphics.d2d->CreateHwndRenderTarget(
            props,
            D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(r.right, r.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY),
            hwndTarget.put());
        check(hr);
        rt_ = hwndTarget;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    auto makeBitmap = [&](Image const &image, winrt::com_ptr<ID2D1Bitmap> &bmp)
    {
        if (!bmp)
            check(rt_->CreateBitmap(
                D2D1::SizeU(image.width, image.height), image.pixels.data(), static_cast<UINT32>(image.stride()),
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                bmp.put()));
    };
    if (desktop_)
        makeBitmap(*desktop_, backgroundBitmap_);
    syncPreview();
    downsampleUsed_ = false;
    cubicZoomUsed_ = false;
    if (previewImage_)
        makeBitmap(*previewImage_, previewBitmap_);
    bool cropPreview = doc_ && !pinned_ && (cropping_ || (moving_ && cropDirty_));
    if (doc_ && !cropPreview)
    {
        makeBitmap(*(liveBase_ ? liveBase_ : doc_->displayBase), bitmap_);
        if (displayRevision_ != doc_->revision)
        {
            displayEffects_.clear();
            for (auto const &layer : doc_->rasterLayers)
                displayEffects_.push_back({layer.index, layer.box, layer.image, {}});
            displayRevision_ = doc_->revision;
        }
    }
    if (mosaicImage_)
        makeBitmap(*mosaicImage_, mosaicBitmap_);
    for (auto &effect : tailEffects_)
        makeBitmap(*effect.image, effect.bitmap);
    for (auto &effect : displayEffects_)
        makeBitmap(*effect.image, effect.bitmap);
    if (!rounded_)
    {
        auto props = D2D1::StrokeStyleProperties();
        props.startCap = props.endCap = props.dashCap = D2D1_CAP_STYLE_ROUND;
        props.lineJoin = D2D1_LINE_JOIN_ROUND;
        check(app_.graphics.d2d->CreateStrokeStyle(props, nullptr, 0, rounded_.put()));
    }
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    check(rt_->CreateSolidColorBrush(color(Fg), brush.put()));
    rt_->BeginDraw();
    rt_->Clear(color(pinned_ ? Bg : 0x000000));
    auto fill = [&](Box b, unsigned rgb, float alpha = 1.f)
    {
        brush->SetColor(color(rgb, alpha));
        rt_->FillRectangle(dr(b), brush.get());
    };
    auto text = [&](std::wstring const &s, Box b, float size, unsigned rgb, bool bold = false)
    {
        auto f = app_.graphics.font(size, bold);
        brush->SetColor(color(rgb));
        rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f.get(), dr(b), brush.get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    if (desktop_)
    {
        rt_->DrawBitmap(backgroundBitmap_.get(),
                        D2D1::RectF(0, 0, static_cast<float>(desktop_->width), static_cast<float>(desktop_->height)));
        fill({0, 0, static_cast<float>(desktop_->width), static_cast<float>(desktop_->height)}, 0x101214, .52f);
    }
    if (doc_)
    {
        float imageW =
            cropPreview ? static_cast<float>(crop_.right - crop_.left) : static_cast<float>(doc_->base->width);
        float imageH =
            cropPreview ? static_cast<float>(crop_.bottom - crop_.top) : static_cast<float>(doc_->base->height);
        Box picture = view_.toView(Box{0, 0, imageW, imageH});
        rt_->PushAxisAlignedClip(dr(picture), D2D1_ANTIALIAS_MODE_ALIASED);
        if (cropPreview)
            rt_->DrawBitmap(backgroundBitmap_.get(), dr(picture), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                            D2D1::RectF(static_cast<float>(crop_.left), static_cast<float>(crop_.top),
                                        static_cast<float>(crop_.right), static_cast<float>(crop_.bottom)));
        else if (previewBitmap_)
        {
            rt_->DrawBitmap(previewBitmap_.get(), dr(picture), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            downsampleUsed_ = true;
        }
        else
        {
            auto context = rt_.try_as<ID2D1DeviceContext>();
            cubicZoomUsed_ = smoothZoom_ && !liveBase_ && view_.zoom > 1.001f && context;
            if (cubicZoomUsed_)
                context->DrawBitmap(bitmap_.get(), dr(picture), 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            else
                rt_->DrawBitmap(bitmap_.get(), dr(picture), 1.f,
                                liveBase_ || !smoothZoom_ || std::abs(view_.zoom - 1.f) < .001f
                                    ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                    : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt_->SetTransform(D2D1::Matrix3x2F(view_.zoom, 0, 0, view_.zoom, view_.origin.x, view_.origin.y));
            if (!liveBase_)
                for (size_t i = 0; i < doc_->marks.size(); ++i)
                {
                    auto const &mark = doc_->marks[i];
                    if (mark.kind == Tool::Mosaic)
                    {
                        for (auto const &effect : displayEffects_)
                            if (effect.index == i)
                                rt_->DrawBitmap(effect.bitmap.get(), dr(effect.box), 1.f,
                                                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                    }
                    else
                        drawAnnotation(app_.graphics, rt_.get(), brush.get(), rounded_.get(), mark,
                                       {0, 0, imageW, imageH});
                }
            if (stroke_ && !edit_)
            {
                if (stroke_->kind == Tool::Mosaic && mosaicBitmap_)
                {
                    auto b = between(stroke_->from, stroke_->to);
                    float x = std::clamp(std::floor(b.x), 0.f, imageW), y = std::clamp(std::floor(b.y), 0.f, imageH);
                    Box region{x, y, std::clamp(std::ceil(b.x + b.w), x, imageW) - x,
                               std::clamp(std::ceil(b.y + b.h), y, imageH) - y};
                    rt_->DrawBitmap(mosaicBitmap_.get(), dr(region), 1.f,
                                    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, dr(region));
                }
                else
                    drawAnnotation(app_.graphics, rt_.get(), brush.get(), rounded_.get(), *stroke_,
                                   {0, 0, imageW, imageH});
            }
            if (liveBase_ && selected_ >= 0)
                for (size_t i = selected_ + 1; i < doc_->marks.size(); ++i)
                {
                    auto const &m = doc_->marks[i];
                    if (m.kind == Tool::Mosaic)
                    {
                        for (auto const &effect : tailEffects_)
                            if (effect.index == i)
                                rt_->DrawBitmap(effect.bitmap.get(), dr(effect.box), 1.f,
                                                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                    }
                    else
                        drawAnnotation(app_.graphics, rt_.get(), brush.get(), rounded_.get(), m,
                                       {0, 0, imageW, imageH});
                }
            rt_->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        if (ocr_ && ocr_->revision == doc_->revision && anchor_ >= 0 && caret_ >= 0 && !cropping_ && !stroke_)
        {
            int first = std::min(anchor_, caret_), last = std::max(anchor_, caret_);
            for (int i = first; i <= last && i < static_cast<int>(ocr_->tokens.size()); ++i)
            {
                auto quad = ocr_->quad(ocr_->tokens[i]);
                winrt::com_ptr<ID2D1PathGeometry> geometry;
                check(app_.graphics.d2d->CreatePathGeometry(geometry.put()));
                winrt::com_ptr<ID2D1GeometrySink> sink;
                check(geometry->Open(sink.put()));
                sink->BeginFigure(dp(view_.toView(quad.front())), D2D1_FIGURE_BEGIN_FILLED);
                for (size_t k = 1; k < quad.size(); ++k)
                    sink->AddLine(dp(view_.toView(quad[k])));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                check(sink->Close());
                brush->SetColor(color(0x298fff, .3f));
                rt_->FillGeometry(geometry.get(), brush.get());
            }
        }
        rt_->PopAxisAlignedClip();
        if (annotationsEditable() && selected_ >= 0 && selected_ < static_cast<int>(doc_->marks.size()) && !edit_ &&
            !cropPreview)
        {
            auto const &m = objectDragging_ && stroke_ ? *stroke_ : doc_->marks[selected_];
            brush->SetColor(color(Accent));
            auto bounds = view_.toView(annotationBounds(m));
            rt_->DrawRectangle(dr(bounds), brush.get(), ui_);
            for (auto point : objectHandles(m))
            {
                point = view_.toView(point);
                Box handle{point.x - 3 * ui_, point.y - 3 * ui_, 6 * ui_, 6 * ui_};
                fill(handle, 0xffffff);
                brush->SetColor(color(Accent));
                rt_->DrawRectangle(dr(handle), brush.get(), ui_);
            }
        }
        if (pinned_)
        {
            if (!pinFrame_)
            {
                const D2D1_GRADIENT_STOP stops[] = {
                    {0.f, color(0x6bc6d8)}, {.5f, color(0x7d9ce2)}, {1.f, color(0xb3a1d5)}};
                winrt::com_ptr<ID2D1GradientStopCollection> collection;
                check(rt_->CreateGradientStopCollection(stops, 3, collection.put()));
                check(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), collection.get(),
                                                     pinFrame_.put()));
            }
            pinFrame_->SetStartPoint({picture.x, picture.y});
            pinFrame_->SetEndPoint({picture.x + picture.w, picture.y + picture.h});
            auto antialias = rt_->GetAntialiasMode();
            rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            rt_->DrawRectangle(dr({picture.x - .5f, picture.y - .5f, picture.w + 1, picture.h + 1}), pinFrame_.get(),
                               1.f);
            rt_->SetAntialiasMode(antialias);
        }
        else
        {
            brush->SetColor(color(Accent));
            rt_->DrawRectangle(dr(picture), brush.get(), 1.f);
        }
        if (!pinned_ && !stroke_)
        {
            for (float x : {picture.x, picture.x + picture.w / 2, picture.x + picture.w})
                for (float y : {picture.y, picture.y + picture.h / 2, picture.y + picture.h})
                    if (x != picture.x + picture.w / 2 || y != picture.y + picture.h / 2)
                        fill({x - 2.5f, y - 2.5f, 5, 5}, 0xffffff);
            std::wstring size =
                std::to_wstring(static_cast<int>(imageW)) + L" × " + std::to_wstring(static_cast<int>(imageH));
            float sy = picture.y > 30 * ui_ ? picture.y - 28 * ui_ : picture.y + 6 * ui_;
            fill({picture.x, sy, 118 * ui_, 24 * ui_}, Bg, .96f);
            text(size, {picture.x + 7 * ui_, sy + 2 * ui_, 110 * ui_, 22 * ui_}, 12 * ui_, Fg);
        }
        if (showTools_)
        {
            drawToolbar(brush.get());
            fill(statusBox_, 0xeef2f6, .98f);
            std::wstring s =
                hoverButton_ >= 0 && hoverButton_ < static_cast<int>(buttons_.size())
                    ? buttons_[hoverButton_].hint
                    : (!status_.empty()
                           ? status_
                           : (selected_ >= 0 ? L"拖动标注或控制点调整 · Delete 删除 · 双击文字修改"
                                             : (ocr_ ? ocr_->status : L"空白拖动移动截图 · 文/O 选字 · V 选择标注")));
            if (hoverButton_ >= 0 && hoverButton_ < static_cast<int>(buttons_.size()) &&
                buttons_[hoverButton_].command == Width && mosaicSizeContext())
                s = L"马赛克方格 " + std::to_wstring(currentMosaicPixels()) + L" px · 点击切换 6 / 8 / 12 / 16 px";
            if (pinned_ && hoverButton_ < 0)
                s += L"  ·  " + std::to_wstring(static_cast<int>(view_.zoom * 100)) + L"%";
            text(s, {statusBox_.x + 9 * ui_, statusBox_.y + 3 * ui_, statusBox_.w - 18 * ui_, statusBox_.h - 3 * ui_},
                 11 * ui_, Muted);
        }
    }
    else if (dragging_)
    {
        Box b = between(start_, last_);
        if (b.w >= 1 && b.h >= 1)
        {
            rt_->DrawBitmap(backgroundBitmap_.get(), dr(b), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                            dr(b));
            brush->SetColor(color(Accent));
            rt_->DrawRectangle(dr(b), brush.get(), 1.f);
            auto s = std::to_wstring(static_cast<int>(b.w)) + L" × " + std::to_wstring(static_cast<int>(b.h));
            fill({b.x, std::max(0.f, b.y - 28 * ui_), 125 * ui_, 24 * ui_}, Bg);
            text(s, {b.x + 6 * ui_, std::max(0.f, b.y - 26 * ui_), 120 * ui_, 22 * ui_}, 12 * ui_, Fg);
        }
    }
    else
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        float x = cursor.x - desktopBounds_.left + 18.f, y = cursor.y - desktopBounds_.top + 22.f;
        x = std::clamp(x, 8.f, std::max(8.f, desktop_->width - 284 * ui_));
        y = std::clamp(y, 8.f, std::max(8.f, desktop_->height - 58 * ui_));
        fill({x, y, 276 * ui_, 50 * ui_}, Bg, .98f);
        text(L"拖动鼠标选择截图区域", {x + 12 * ui_, y + 6 * ui_, 250 * ui_, 25 * ui_}, 14 * ui_, Fg, true);
        text(L"Esc 取消  ·  右键取消", {x + 12 * ui_, y + 29 * ui_, 250 * ui_, 18 * ui_}, 11 * ui_, Muted);
    }
    if (edit_)
    {
        RECT box{};
        GetWindowRect(edit_, &box);
        MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT *>(&box), 2);
        Box card{box.left - 2 * ui_, box.top - 2 * ui_, box.right - box.left + 4 * ui_, box.bottom - box.top + 4 * ui_};
        auto shape = D2D1::RoundedRect(dr(card), 4 * ui_, 4 * ui_);
        brush->SetColor(color(Accent));
        rt_->DrawRoundedRectangle(shape, brush.get(), ui_);
    }
    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        rt_ = nullptr;
        discardTargetBitmaps();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else
        check(hr);
    lastPaint_ = Clock::now();
    ++paintCount_;
    // Native EDIT must present after the Direct2D parent, including its first frame.
    if (edit_)
        RedrawWindow(edit_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
}
void Canvas::drawToolbar(ID2D1SolidColorBrush *brush)
{
    brush->SetColor(color(Bg));
    rt_->FillRoundedRectangle(D2D1::RoundedRect(dr(toolbar_), 5 * ui_, 5 * ui_), brush);
    for (size_t i = 0; i < buttons_.size(); ++i)
    {
        auto const &b = buttons_[i];
        bool active = b.command >= Move && b.command <= Ellipse && tool_ == toolFrom(b.command);
        if (active || hoverButton_ == static_cast<int>(i))
        {
            brush->SetColor(color(active ? 0xd7edff : 0xe4e9f0));
            rt_->FillRoundedRectangle(D2D1::RoundedRect(dr(b.box), 3 * ui_, 3 * ui_), brush);
        }
        bool disabled =
            (b.command == Undo && doc_->undoEdits.empty()) || (b.command == Redo && doc_->redoEdits.empty());
        brush->SetColor(color(disabled ? 0xb7bec9 : (active ? Accent : Fg)));
        icon(b.command, b.box, brush);
    }
}
void Canvas::icon(int command, Box b, ID2D1SolidColorBrush *brush)
{
    float s = std::min(b.w, b.h) / 32.f, x = b.x + b.w / 2.f, y = b.y + b.h / 2.f;
    auto line = [&](float a, float c, float d, float e, float width = 1.5f)
    { rt_->DrawLine({x + a * s, y + c * s}, {x + d * s, y + e * s}, brush, width * s); };
    auto rect = [&](float a, float c, float w, float h)
    { rt_->DrawRectangle(D2D1::RectF(x + a * s, y + c * s, x + (a + w) * s, y + (c + h) * s), brush, 1.4f * s); };
    auto txt = [&](wchar_t const *t, float size)
    {
        auto f = app_.graphics.font(size * s, true);
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rt_->DrawTextW(t, static_cast<UINT32>(wcslen(t)), f.get(), dr(b), brush);
    };
    switch (command)
    {
    case Move:
        line(-8, 0, 8, 0);
        line(0, -8, 0, 8);
        line(-8, 0, -5, -3);
        line(-8, 0, -5, 3);
        line(8, 0, 5, -3);
        line(8, 0, 5, 3);
        line(0, -8, -3, -5);
        line(0, -8, 3, -5);
        break;
    case Arrow:
        line(-8, 7, 8, -7, 1.8f);
        line(8, -7, 1, -7, 1.8f);
        line(8, -7, 8, 0, 1.8f);
        break;
    case Line:
        line(-8, 7, 8, -7, 1.8f);
        break;
    case Rectangle:
        rect(-9, -6, 18, 12);
        break;
    case Ellipse:
        rt_->DrawEllipse(D2D1::Ellipse({x, y}, 9 * s, 6 * s), brush, 1.5f * s);
        break;
    case Text:
        txt(L"T", 22);
        break;
    case Mosaic:
        for (int yy = -6; yy <= 6; yy += 6)
            for (int xx = -6; xx <= 6; xx += 6)
            {
                brush->SetColor(color((xx + yy) % 12 ? 0xa7adb5 : Fg));
                rt_->FillRectangle(D2D1::RectF(x + (xx - 2) * s, y + (yy - 2) * s, x + (xx + 2) * s, y + (yy + 2) * s),
                                   brush);
            }
        break;
    case Marker:
        line(-5, 5, 6, -6, 5);
        line(-8, 8, -3, 8, 2);
        line(4, -8, 9, -3);
        break;
    case Select:
        txt(L"文", 18);
        line(-9, 10, 9, 10);
        break;
    case Translate:
        txt(L"译", 18);
        break;
    case Undo:
        line(8, 6, 8, -3);
        line(8, -3, -7, -3);
        line(-7, -3, -2, -8);
        line(-7, -3, -2, 2);
        break;
    case Redo:
        line(-8, 6, -8, -3);
        line(-8, -3, 7, -3);
        line(7, -3, 2, -8);
        line(7, -3, 2, 2);
        break;
    case Color:
        brush->SetColor(color(ink_));
        rt_->FillEllipse(D2D1::Ellipse({x, y}, 7 * s, 7 * s), brush);
        brush->SetColor(color(Fg));
        rt_->DrawEllipse(D2D1::Ellipse({x, y}, 8 * s, 8 * s), brush, s);
        break;
    case Width:
        if (textSizeContext())
        {
            wchar_t size[32]{};
            swprintf_s(size, L"%g", static_cast<double>(currentFontPoints()));
            txt(size, currentFontPoints() >= 100 ? 11.f : 14.f);
        }
        else if (mosaicSizeContext())
        {
            auto size = std::to_wstring(currentMosaicPixels());
            txt(size.c_str(), 14.f);
        }
        else
            rt_->FillEllipse(D2D1::Ellipse({x, y}, (width_ + 1) * s, (width_ + 1) * s), brush);
        break;
    case Save:
        rect(-8, -8, 16, 16);
        rect(-4, -8, 8, 5);
        rect(-4, 2, 8, 6);
        break;
    case Pin:
        line(-5, -8, 5, -8);
        line(-3, -8, -3, 0);
        line(3, -8, 3, 0);
        line(-3, 0, -7, 4);
        line(3, 0, 7, 4);
        line(-7, 4, 7, 4);
        line(0, 4, 0, 10);
        break;
    case Copy:
        rect(-7, -8, 12, 14);
        rect(-3, -4, 12, 14);
        break;
    case Close:
        line(-6, -6, 6, 6, 1.8f);
        line(-6, 6, 6, -6, 1.8f);
        break;
    }
}
Point Canvas::clampImage(Point p) const
{
    return {std::clamp(p.x, 0.f, static_cast<float>(doc_->base->width)),
            std::clamp(p.y, 0.f, static_cast<float>(doc_->base->height))};
}
int Canvas::edgeAt(Point p) const
{
    if (pinned_ || !doc_)
        return 0;
    float margin = 6 * ui_;
    int e = 0;
    if (p.x < crop_.left - margin || p.x > crop_.right + margin || p.y < crop_.top - margin ||
        p.y > crop_.bottom + margin)
        return 0;
    if (std::abs(p.x - crop_.left) <= margin)
        e |= 1;
    if (std::abs(p.x - crop_.right) <= margin)
        e |= 2;
    if (std::abs(p.y - crop_.top) <= margin)
        e |= 4;
    if (std::abs(p.y - crop_.bottom) <= margin)
        e |= 8;
    return e;
}
int Canvas::textAt(Point p) const
{
    if (!doc_ || tool_ != Tool::Select || !ocr_ || ocr_->revision != doc_->revision || edit_ || stroke_ || cropping_)
        return -1;
    if (showTools_ && (toolbar_.contains(p) || statusBox_.contains(p)))
        return -1;
    auto picture =
        view_.toView(Box{0, 0, static_cast<float>(doc_->base->width), static_cast<float>(doc_->base->height)});
    if (!picture.contains(p))
        return -1;
    auto image = view_.toImage(p);
    for (auto const &mark : doc_->marks)
        if (mark.kind == Tool::Mosaic && between(mark.from, mark.to).contains(image))
            return -1;
    return ocr_->hit(image, false, 8 * ui_ / view_.zoom);
}
void Canvas::pointerDown(Point p, WPARAM keys)
{
    status_.clear();
    if (showTools_ && doc_)
    {
        for (auto const &b : buttons_)
            if (b.box.contains(p))
            {
                command(b.command);
                return;
            }
        if (statusBox_.contains(p))
        {
            anchor_ = caret_ = -1;
            requestFrame(true);
            return;
        }
    }
    if (edit_ && editorBorderAt(p))
    {
        beginEditorDrag(p);
        return;
    }
    SetFocus(hwnd_);
    finishText(true);
    if (!doc_)
    {
        start_ = last_ = p;
        dragging_ = true;
        SetCursor(crosshair_.get(GetDpiForWindow(hwnd_)));
        SetCapture(hwnd_);
        return;
    }
    Box picture =
        view_.toView(Box{0, 0, static_cast<float>(doc_->base->width), static_cast<float>(doc_->base->height)});
    bool space = (GetKeyState(VK_SPACE) & 0x8000) != 0;
    int handle = space ? -1 : handleAt(p);
    if (handle >= 0)
    {
        beginObject(selected_, handle, p);
        return;
    }
    int text = space || ((keys & MK_CONTROL) && pinned_) ? -1 : textAt(p);
    if (text >= 0)
    {
        start_ = last_ = p;
        selected_ = -1;
        anchor_ = caret_ = text;
        selecting_ = true;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (anchor_ >= 0 || caret_ >= 0)
    {
        anchor_ = caret_ = -1;
        requestFrame(true);
    }
    resizeEdge_ = space ? 0 : edgeAt(p);
    if (resizeEdge_)
    {
        cropping_ = true;
        start_ = last_ = p;
        startCrop_ = crop_;
        selected_ = -1;
        invalidateOcr();
        SetCapture(hwnd_);
        return;
    }
    if (!picture.contains(p))
    {
        anchor_ = caret_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    Point image = clampImage(view_.toImage(p));
    start_ = last_ = p;
    if ((keys & MK_CONTROL) && pinned_)
    {
        panning_ = true;
        panOrigin_ = view_.origin;
        SetCapture(hwnd_);
        return;
    }
    int object = space || !annotationsEditable() ? -1 : objectAt(image);
    if (!space && annotationsEditable() && object < 0 && selected_ >= 0 &&
        annotationBounds(doc_->marks[selected_]).contains(image))
        object = selected_;
    if (object >= 0 && (tool_ == Tool::Move || tool_ == Tool::Select || doc_->marks[object].kind == tool_))
    {
        beginObject(object, -1, p);
        return;
    }
    if (space || !annotationsEditable() || tool_ == Tool::Move || tool_ == Tool::Select)
    {
        moving_ = true;
        GetWindowRect(hwnd_, &startWindow_);
        startCrop_ = crop_;
        cropDirty_ = false;
        selected_ = -1;
        // Moving the window changes no image pixels. Keep ready and in-flight
        // OCR valid; only changing the screenshot crop invalidates its content.
        anchor_ = caret_ = -1;
        SetCapture(hwnd_);
        return;
    }
    selected_ = -1;
    invalidateOcr();
    if (tool_ == Tool::Text)
    {
        beginText(image);
        return;
    }
    stroke_ = Annotation{tool_, image, image, {}, L"", ink_, width_};
    stroke_->mosaicPixels = mosaicPixels_;
    prepareLive(-1, tool_);
    if (tool_ == Tool::Marker)
        stroke_->path = {image, image};
    dragging_ = true;
    SetCapture(hwnd_);
}
void Canvas::pointerMove(Point p, WPARAM)
{
    last_ = p;
    if (textEditorDragging_ && edit_)
    {
        textEditorAnchor_ = {textDragAnchor_.x + p.x - textDragStart_.x, textDragAnchor_.y + p.y - textDragStart_.y};
        sizeTextEditor();
        requestFrame(true);
        return;
    }
    if (!hovering_)
    {
        hovering_ = true;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tracking);
    }
    int hover = -1;
    for (size_t i = 0; i < buttons_.size(); ++i)
        if (buttons_[i].box.contains(p))
            hover = static_cast<int>(i);
    if (hover != hoverButton_)
    {
        hoverButton_ = hover;
        requestFrame(dragging_ || objectDragging_ || cropping_ || moving_ || selecting_ || panning_);
    }
    if (!doc_)
    {
        // WM_SETCURSOR is not sent normally while the mouse is captured.
        // Keep the same high-contrast cursor throughout region selection.
        SetCursor(crosshair_.get(GetDpiForWindow(hwnd_)));
        requestFrame(dragging_ || objectDragging_ || cropping_ || moving_ || selecting_ || panning_);
        return;
    }
    if (cropping_)
    {
        int dx = static_cast<int>(p.x - start_.x), dy = static_cast<int>(p.y - start_.y);
        crop_ = startCrop_;
        if (resizeEdge_ & 1)
            crop_.left = std::clamp<LONG>(startCrop_.left + dx, 0, crop_.right - 3);
        if (resizeEdge_ & 2)
            crop_.right = std::clamp<LONG>(startCrop_.right + dx, crop_.left + 3, desktop_->width);
        if (resizeEdge_ & 4)
            crop_.top = std::clamp<LONG>(startCrop_.top + dy, 0, crop_.bottom - 3);
        if (resizeEdge_ & 8)
            crop_.bottom = std::clamp<LONG>(startCrop_.bottom + dy, crop_.top + 3, desktop_->height);
        view_.origin = {static_cast<float>(crop_.left), static_cast<float>(crop_.top)};
        layout();
        requestFrame(dragging_ || objectDragging_ || cropping_ || moving_ || selecting_ || panning_);
        return;
    }
    if (moving_)
    {
        if (pinned_)
        {
            POINT global{};
            GetCursorPos(&global);
            SetWindowPos(hwnd_, nullptr, global.x - static_cast<int>(start_.x), global.y - static_cast<int>(start_.y),
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        else
        {
            LONG dx = static_cast<LONG>(p.x - start_.x), dy = static_cast<LONG>(p.y - start_.y),
                 w = startCrop_.right - startCrop_.left, h = startCrop_.bottom - startCrop_.top;
            crop_.left = std::clamp<LONG>(startCrop_.left + dx, 0, desktop_->width - w);
            crop_.top = std::clamp<LONG>(startCrop_.top + dy, 0, desktop_->height - h);
            crop_.right = crop_.left + w;
            crop_.bottom = crop_.top + h;
            if (!cropDirty_ && !EqualRect(&crop_, &startCrop_))
            {
                cropDirty_ = true;
                invalidateOcr();
            }
            view_.origin = {static_cast<float>(crop_.left), static_cast<float>(crop_.top)};
            layout();
            requestFrame(dragging_ || objectDragging_ || cropping_ || moving_ || selecting_ || panning_);
        }
        return;
    }
    if (panning_)
    {
        view_.origin = {panOrigin_.x + p.x - start_.x, panOrigin_.y + p.y - start_.y};
        requestFrame(dragging_ || objectDragging_ || cropping_ || moving_ || selecting_ || panning_);
        return;
    }
    if (selecting_ && ocr_)
    {
        caret_ = ocr_->hit(clampImage(view_.toImage(p)), true);
        requestFrame(dragging_ || objectDragging_ || cropping_ || moving_ || selecting_ || panning_);
        return;
    }
    if (objectDragging_ && stroke_)
    {
        Point image = clampImage(view_.toImage(p)), origin = view_.toImage(start_);
        float dx = image.x - origin.x, dy = image.y - origin.y;
        if (std::hypot(p.x - start_.x, p.y - start_.y) < 2 * ui_ && !objectMoved_)
            return;
        objectMoved_ = true;
        if (segmentAnnotation(objectOriginal_) && objectHandle_ >= 0)
        {
            *stroke_ = objectOriginal_;
            (objectHandle_ == 0 ? stroke_->from : stroke_->to) = image;
            if (stroke_->kind == Tool::Marker)
                stroke_->path = {stroke_->from, stroke_->to};
        }
        else
        {
            auto b = objectStart_;
            if (objectHandle_ < 0)
            {
                float minX = -b.x, maxX = doc_->base->width - b.x - b.w;
                float minY = -b.y, maxY = doc_->base->height - b.y - b.h;
                b.x += std::clamp(dx, std::min(minX, maxX), std::max(minX, maxX));
                b.y += std::clamp(dy, std::min(minY, maxY), std::max(minY, maxY));
            }
            else
            {
                float right = b.x + b.w, bottom = b.y + b.h, minimum = 4.f / view_.zoom;
                if (objectHandle_ == 0 || objectHandle_ == 6 || objectHandle_ == 7)
                    b.x = std::min(b.x + dx, right - minimum);
                if (objectHandle_ == 2 || objectHandle_ == 3 || objectHandle_ == 4)
                    right = std::max(right + dx, b.x + minimum);
                if (objectHandle_ == 0 || objectHandle_ == 1 || objectHandle_ == 2)
                    b.y = std::min(b.y + dy, bottom - minimum);
                if (objectHandle_ == 4 || objectHandle_ == 5 || objectHandle_ == 6)
                    bottom = std::max(bottom + dy, b.y + minimum);
                b.w = right - b.x;
                b.h = bottom - b.y;
            }
            *stroke_ = transformAnnotation(objectOriginal_, objectStart_, b);
            if (stroke_->kind == Tool::Text && objectHandle_ >= 0)
                measureText(app_.graphics, *stroke_, std::max(16.f, b.w));
        }
        requestFrame(dragging_ || objectDragging_ || cropping_ || moving_ || selecting_ || panning_);
        return;
    }
    if (dragging_ && stroke_)
    {
        Point image = clampImage(view_.toImage(p));
        stroke_->to = image;
        if ((GetKeyState(VK_SHIFT) & 0x8000) &&
            (stroke_->kind == Tool::Arrow || stroke_->kind == Tool::Line || stroke_->kind == Tool::Marker))
        {
            float dx = image.x - stroke_->from.x, dy = image.y - stroke_->from.y, d = std::hypot(dx, dy);
            float a = std::round(std::atan2(dy, dx) / (3.14159265f / 4.f)) * (3.14159265f / 4.f);
            float ux = std::cos(a), uy = std::sin(a);
            if (std::abs(ux) > .001f)
                d = std::min(d, (ux > 0 ? doc_->base->width - stroke_->from.x : -stroke_->from.x) / ux);
            if (std::abs(uy) > .001f)
                d = std::min(d, (uy > 0 ? doc_->base->height - stroke_->from.y : -stroke_->from.y) / uy);
            stroke_->to = {stroke_->from.x + d * std::cos(a), stroke_->from.y + d * std::sin(a)};
        }
        if ((GetKeyState(VK_SHIFT) & 0x8000) && (stroke_->kind == Tool::Rectangle || stroke_->kind == Tool::Ellipse))
        {
            float dx = image.x - stroke_->from.x, dy = image.y - stroke_->from.y;
            float side = std::max(std::abs(dx), std::abs(dy));
            side = std::min({side, dx >= 0 ? doc_->base->width - stroke_->from.x : stroke_->from.x,
                             dy >= 0 ? doc_->base->height - stroke_->from.y : stroke_->from.y});
            stroke_->to = {stroke_->from.x + std::copysign(side, dx), stroke_->from.y + std::copysign(side, dy)};
        }
        if (stroke_->kind == Tool::Marker)
            stroke_->path = {stroke_->from, stroke_->to};
        requestFrame(dragging_ || objectDragging_ || cropping_ || moving_ || selecting_ || panning_);
    }
}
void Canvas::pointerUp(Point p)
{
    if (objectDragging_ || (dragging_ && stroke_))
        pointerMove(p, 0);
    releasing_ = true;
    if (GetCapture() == hwnd_)
        ReleaseCapture();
    releasing_ = false;
    if (textEditorDragging_)
    {
        textEditorDragging_ = false;
        if (edit_)
            SetFocus(edit_);
        requestFrame(true);
        return;
    }
    if (!doc_ && dragging_)
    {
        auto b = between(start_, p);
        dragging_ = false;
        if (b.w < 3 || b.h < 3)
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        crop_ = {static_cast<LONG>(b.x), static_cast<LONG>(b.y), static_cast<LONG>(b.x + b.w),
                 static_cast<LONG>(b.y + b.h)};
        crop_.left = std::clamp<LONG>(crop_.left, 0, desktop_->width - 3);
        crop_.top = std::clamp<LONG>(crop_.top, 0, desktop_->height - 3);
        crop_.right = std::clamp<LONG>(crop_.right, crop_.left + 3, desktop_->width);
        crop_.bottom = std::clamp<LONG>(crop_.bottom, crop_.top + 3, desktop_->height);
        doc_ = std::make_unique<Document>(desktop_->crop(crop_));
        layout();
        refreshBitmap();
        if (app_.config.autoOcr)
            requestOcr();
        return;
    }
    if (cropping_ || (!pinned_ && moving_))
    {
        cropping_ = moving_ = false;
        if (!EqualRect(&crop_, &startCrop_))
            commitCrop();
        else
            requestFrame(true);
        return;
    }
    moving_ = panning_ = selecting_ = false;
    if (objectDragging_)
    {
        objectDragging_ = false;
        if (objectMoved_ && stroke_)
            doc_->replace(selected_, std::move(*stroke_));
        stroke_.reset();
        clearLive();
        if (objectMoved_)
            changed();
        else if (app_.config.autoOcr)
            SetTimer(hwnd_, 1, 300, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (dragging_ && stroke_)
    {
        dragging_ = false;
        auto mark = std::move(*stroke_);
        stroke_.reset();
        bool areaTool = mark.kind == Tool::Rectangle || mark.kind == Tool::Ellipse || mark.kind == Tool::Mosaic;
        auto bounds = between(mark.from, mark.to);
        if (areaTool ? bounds.w >= 1 && bounds.h >= 1
                     : std::hypot(mark.to.x - mark.from.x, mark.to.y - mark.from.y) >= 1)
        {
            doc_->add(std::move(mark));
            selected_ = static_cast<int>(doc_->marks.size()) - 1;
        }
        changed();
    }
}
void Canvas::commitCrop()
{
    if (!doc_)
        return;
    float dx = static_cast<float>(startCrop_.left - crop_.left), dy = static_cast<float>(startCrop_.top - crop_.top);
    doc_->translate(dx, dy);
    doc_->base = std::make_shared<Image>(desktop_->crop(crop_));
    ++doc_->revision;
    changed();
    layout();
}
void Canvas::changed()
{
    invalidateOcr();
    clearLive();
    status_.clear();
    doc_->flatten(app_.graphics);
    refreshBitmap();
    app_.ocr().cancel(id_);
    if (app_.config.autoOcr)
        SetTimer(hwnd_, 1, 300, nullptr);
}
void Canvas::requestOcr(bool enhance)
{
    if (!doc_ || stroke_ || edit_ || objectDragging_ || cropping_ || (!pinned_ && moving_))
        return;
    KillTimer(hwnd_, 1);
    anchor_ = caret_ = -1;
    ocr_ = nullptr;
    status_ = enhance ? L"正在增强识别…" : L"正在离线识别…";
    app_.ocr().submit(
        {id_, doc_->revision, doc_->rendered, doc_->redactions(), app_.config.language, ++ocrEpoch_, {}, enhance});
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void Canvas::receive(std::shared_ptr<OcrSnapshot> snapshot)
{
    if (!doc_ || snapshot->revision != doc_->revision || snapshot->epoch != ocrEpoch_ || stroke_ || edit_ ||
        cropping_ || (!pinned_ && moving_ && cropDirty_))
        return;
    ocr_ = std::move(snapshot);
    app_.translation_->ocrReady(id_, doc_->revision, *ocr_);
    status_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
bool Canvas::mosaicSizeContext() const
{
    if (doc_ && selected_ >= 0 && selected_ < static_cast<int>(doc_->marks.size()))
        return doc_->marks[selected_].kind == Tool::Mosaic;
    return tool_ == Tool::Mosaic;
}
int Canvas::currentMosaicPixels() const
{
    if (doc_ && selected_ >= 0 && selected_ < static_cast<int>(doc_->marks.size()) &&
        doc_->marks[selected_].kind == Tool::Mosaic)
        return mosaicCellPixels(doc_->marks[selected_]);
    return mosaicPixels_;
}
unsigned Canvas::textEditorInk() const
{
    // Editing an existing annotation must use its own ink, even if the default
    // color for the next annotation has since changed.
    return stroke_ && stroke_->kind == Tool::Text ? stroke_->rgb : ink_;
}
void Canvas::beginText(Point image)
{
    if (!annotationsEditable() || !doc_)
        return;
    textIndex_ = selected_ = -1;
    invalidateOcr();
    if (pinned_)
    {
        RECT r{};
        GetClientRect(hwnd_, &r);
        if (r.right < 220 * ui_ || r.bottom < 100 * ui_)
        {
            SetWindowPos(hwnd_, nullptr, 0, 0, std::max(r.right, static_cast<LONG>(220 * ui_)),
                         std::max(r.bottom, static_cast<LONG>(100 * ui_)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
        }
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    int clientW = client.right, clientH = client.bottom;
    Point p = view_.toView(image);
    float size = fontPt_ * (96.f / 72.f) * view_.zoom;
    int w = std::min(static_cast<int>(24 * ui_), std::max(1, clientW - static_cast<int>(8 * ui_)));
    int h = std::min(static_cast<int>(size * 1.5f + 12 * ui_), std::max(1, clientH - static_cast<int>(8 * ui_)));
    int x = std::clamp(static_cast<int>(p.x), 0, std::max(0, clientW - w));
    int y = std::clamp(static_cast<int>(p.y), 0, std::max(0, clientH - h));
    edit_ = CreateWindowEx(0, L"EDIT", L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL, x, y, w,
                           h, hwnd_, reinterpret_cast<HMENU>(101), app_.instance(), nullptr);
    if (!edit_)
        winrt::throw_last_error();
    editorBackdrop_.reset();
    stroke_ = Annotation{Tool::Text, image, image, {}, L"", ink_, width_, fontPt_};
    textEditorAnchor_ = p;
    updateEditorFont();
    SendMessage(edit_, EM_LIMITTEXT, 8192, 0);
    sizeTextEditor();
    SetWindowLongPtr(edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    oldEditProc_ =
        reinterpret_cast<WNDPROC>(SetWindowLongPtr(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(editProc)));
    if (!app_.testing_)
        SetForegroundWindow(hwnd_);
    SetFocus(edit_);
    status_ = L"输入文字 · 拖动细边框移动 · Enter 完成 · Shift+Enter 换行 · Esc 取消";
    InvalidateRect(hwnd_, nullptr, FALSE);
    RedrawWindow(edit_, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
}
void Canvas::editText(int index)
{
    if (!annotationsEditable() || !doc_ || index < 0 || index >= static_cast<int>(doc_->marks.size()) ||
        doc_->marks[index].kind != Tool::Text)
        return;
    Annotation original = doc_->marks[index];
    float previousWidth = width_;
    float previousFont = fontPt_;
    unsigned previousInk = ink_;
    width_ = original.width;
    fontPt_ = textFontPoints(original);
    ink_ = original.rgb;
    beginText(original.from);
    width_ = previousWidth;
    fontPt_ = previousFont;
    ink_ = previousInk;
    textIndex_ = selected_ = index;
    stroke_ = original;
    updateEditorFont();
    prepareLive(index, Tool::Text);
    editorBackdrop_.reset();
    SetWindowText(edit_, original.text.c_str());
    SendMessage(edit_, EM_SETSEL, 0, -1);
    SetFocus(edit_);
}
LRESULT CALLBACK Canvas::editProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto *self = reinterpret_cast<Canvas *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
    if (msg == WM_LBUTTONDOWN || msg == WM_SETCURSOR)
    {
        POINT p{};
        if (msg == WM_LBUTTONDOWN)
            p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        else
        {
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
        }
        MapWindowPoints(hwnd, self->hwnd_, &p, 1);
        Point point{static_cast<float>(p.x), static_cast<float>(p.y)};
        if (self->editorBorderAt(point))
        {
            if (msg == WM_LBUTTONDOWN)
                self->beginEditorDrag(point);
            else
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return msg == WM_SETCURSOR ? TRUE : 0;
        }
    }
    if (msg == WM_SETTEXT)
    {
        // Multiline EDIT does not emit EN_CHANGE for WM_SETTEXT. This path is
        // also used when an existing annotation is opened for editing.
        auto result = CallWindowProc(self->oldEditProc_, hwnd, msg, wp, lp);
        self->sizeTextEditor();
        return result;
    }
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE) && !(GetKeyState(VK_SHIFT) & 0x8000) &&
        !GetProp(hwnd, L"SnapLite.Composing") && !GetProp(hwnd, L"SnapLite.ImeKey"))
    {
        PostMessage(self->hwnd_, WM_FINISH_TEXT, wp == VK_RETURN, 0);
        return 0;
    }
    if (msg == WM_IME_STARTCOMPOSITION)
        SetProp(hwnd, L"SnapLite.Composing", reinterpret_cast<HANDLE>(1));
    if (msg == WM_IME_ENDCOMPOSITION)
        RemoveProp(hwnd, L"SnapLite.Composing");
    if (msg == WM_KEYUP && (wp == VK_RETURN || wp == VK_ESCAPE))
        RemoveProp(hwnd, L"SnapLite.ImeKey");
    return CallWindowProc(self->oldEditProc_, hwnd, msg, wp, lp);
}
void Canvas::finishText(bool accept)
{
    if (!edit_)
        return;
    if (textEditorDragging_)
    {
        textEditorDragging_ = false;
        releasing_ = true;
        if (GetCapture() == hwnd_)
            ReleaseCapture();
        releasing_ = false;
    }
    closeFontPicker(false);
    HWND edit = edit_;
    edit_ = nullptr;
    int length = GetWindowTextLength(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowText(edit, text.data(), length + 1);
    text.resize(length);
    DestroyWindow(edit);
    editorBackdrop_.reset();
    bool modified = false;
    if (stroke_)
    {
        // Native EDIT coordinates are integer pixels. Opening an existing label
        // at fractional zoom must not turn that rounding into a document edit.
        bool repositioned = false;
        if (textIndex_ >= 0)
        {
            auto original = doc_->marks[textIndex_].from;
            auto anchor = view_.toView(original);
            repositioned = std::hypot(textEditorAnchor_.x - anchor.x, textEditorAnchor_.y - anchor.y) > .01f &&
                           std::hypot(stroke_->from.x - original.x, stroke_->from.y - original.y) > .01f;
        }
        if (accept && !text.empty() &&
            (textIndex_ < 0 || text != doc_->marks[textIndex_].text ||
             std::abs(textFontPoints(*stroke_) - textFontPoints(doc_->marks[textIndex_])) > .001f || repositioned))
        {
            stroke_->text = std::move(text);
            measureText(app_.graphics, *stroke_, std::max(16.f, doc_->base->width - stroke_->from.x));
            if (textIndex_ >= 0)
                doc_->replace(textIndex_, std::move(*stroke_));
            else
            {
                doc_->add(std::move(*stroke_));
                selected_ = static_cast<int>(doc_->marks.size()) - 1;
            }
            modified = true;
        }
        else if (accept && text.empty() && textIndex_ >= 0)
        {
            doc_->remove(textIndex_);
            selected_ = -1;
            modified = true;
        }
        stroke_.reset();
    }
    textIndex_ = -1;
    clearLive();
    if (doc_ && !closing_)
    {
        if (modified)
            changed();
        else if (app_.config.autoOcr)
            SetTimer(hwnd_, 1, 300, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (!closing_)
        SetFocus(hwnd_);
    if (!accept)
        status_.clear();
}
void Canvas::copy(bool imageOnly)
{
    finishGesture();
    if (!doc_)
        return;
    bool text = !imageOnly && ocr_ && ocr_->revision == doc_->revision && anchor_ >= 0 && caret_ >= 0;
    bool ok = text ? app_.copyText_(hwnd_, ocr_->selection(anchor_, caret_))
                   : app_.copyImage_(hwnd_, app_.graphics, *doc_->rendered);
    if (ok)
    {
        if (text)
            app_.copiedImage_.reset();
        else
            app_.copiedImage_ = App::CopiedImagePlacement{imagePlacement(), doc_->base->width, doc_->base->height,
                                                          app_.clipboardSequence_()};
    }
    status_ = ok ? (text ? L"文字已复制" : L"图片已复制") : L"剪贴板暂时不可用，请重试";
    if (ok && !text && !pinned_)
        close();
    else
        InvalidateRect(hwnd_, nullptr, FALSE);
}
void Canvas::copyFile()
{
    finishGesture();
    if (!doc_)
        return;
    auto path = app_.copyImageFile_(hwnd_, app_.graphics, *doc_->rendered);
    if (path.empty())
        status_ = L"PNG 文件复制失败，请重试或使用“保存 PNG”。";
    else
    {
        app_.copiedImage_ = App::CopiedImagePlacement{imagePlacement(), doc_->base->width, doc_->base->height,
                                                      app_.clipboardSequence_()};
        status_ = L"PNG 文件已复制 · 可粘贴为附件 · 原尺寸";
        if (!pinned_)
            close();
    }
    requestFrame();
}
void Canvas::beginTranslation()
{
    if (!doc_)
        return;
    if (ocr_ && ocr_->revision == doc_->revision)
        app_.translation_->open(id_, doc_->revision, translationRows(*ocr_, anchor_, caret_));
    else
    {
        requestOcr();
        app_.translation_->open(id_, doc_->revision, {}, true);
    }
}
void Canvas::save()
{
    finishGesture();
    if (!doc_)
        return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[128]{};
    swprintf_s(name, L"SnipLeaf_%04u%02u%02u_%02u%02u%02u.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond);
    std::vector<wchar_t> path(32768);
    wcscpy_s(path.data(), path.size(), name);
    OPENFILENAME ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0\0";
    ofn.lpstrFile = path.data();
    ofn.nMaxFile = static_cast<DWORD>(path.size());
    ofn.lpstrDefExt = L"png";
    ofn.lpstrTitle = L"保存截图";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileName(&ofn))
    {
        savePng(app_.graphics, *doc_->rendered, path.data());
        status_ = L"图片已保存";
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
void Canvas::command(int id)
{
    // A hidden toolbar makes the annotation layers read-only. OCR, moving the
    // whole pin and viewing commands remain available without entering edit mode.
    bool editing =
        (id >= Move && id <= Ellipse && id != Select) || id == Undo || id == Redo || id == Color || id == Width;
    if (!annotationsEditable() && editing)
        return;
    if (id == Width && doc_ && textSizeContext())
    {
        showFontPicker();
        return;
    }
    if (id == Undo && (stroke_ || cropping_ || moving_))
    {
        if (edit_)
            finishText(false);
        else
            cancelGesture();
        return;
    }
    if (id == Close)
        cancelGesture();
    else if (!edit_)
        finishGesture();
    finishText(true);
    if (!doc_ && id != Close)
        return;
    if (id >= Move && id <= Ellipse)
    {
        tool_ = toolFrom(id);
        selected_ = -1;
        anchor_ = caret_ = -1;
        if (tool_ == Tool::Select && !ocr_)
            requestOcr();
        status_.clear();
        if (tool_ == Tool::Mosaic)
            status_ = L"马赛克 · " + std::to_wstring(mosaicPixels_) + L" px 方格 · 点击数字调整疏密";
    }
    else
        switch (id)
        {
        case Undo:
            selected_ = -1;
            if (doc_->undo())
                changed();
            break;
        case Redo:
            selected_ = -1;
            if (doc_->redo())
                changed();
            break;
        case Color:
        {
            static COLORREF custom[16]{};
            CHOOSECOLOR cc{sizeof(cc)};
            cc.hwndOwner = hwnd_;
            cc.rgbResult = RGB((ink_ >> 16) & 255, (ink_ >> 8) & 255, ink_ & 255);
            cc.lpCustColors = custom;
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (ChooseColor(&cc))
            {
                ink_ = (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
                if (selected_ >= 0)
                {
                    auto mark = doc_->marks[selected_];
                    mark.rgb = ink_;
                    doc_->replace(selected_, std::move(mark));
                    changed();
                }
            }
            break;
        }
        case Width:
            if (mosaicSizeContext())
            {
                int pixels = currentMosaicPixels();
                mosaicPixels_ = pixels < 8 ? 8 : pixels < 12 ? 12 : pixels < 16 ? 16 : 6;
                status_ = L"马赛克方格 " + std::to_wstring(mosaicPixels_) + L" px · 点击调整疏密";
                if (selected_ >= 0)
                {
                    auto mark = doc_->marks[selected_];
                    mark.mosaicPixels = mosaicPixels_;
                    doc_->replace(selected_, std::move(mark));
                    changed();
                }
                break;
            }
            width_ = width_ < 3 ? 3.f : width_ < 5 ? 5.f : width_ < 8 ? 8.f : 1.5f;
            status_ = L"线宽 " + std::to_wstring(static_cast<int>(width_)) + L" px";
            if (selected_ >= 0)
            {
                auto mark = doc_->marks[selected_];
                mark.width = width_;
                if (mark.kind == Tool::Text)
                    measureText(app_.graphics, mark, std::max(16.f, doc_->base->width - mark.from.x));
                doc_->replace(selected_, std::move(mark));
                changed();
            }
            break;
        case SmoothZoom:
        case PixelZoom:
            smoothZoom_ = id == SmoothZoom;
            status_ = smoothZoom_ ? L"清晰缩放" : L"像素显示";
            break;
        case ActualSize:
        case DoubleSize:
            zoom((id == ActualSize ? 1.f : 2.f) / view_.zoom, {1, 1});
            break;
        case Save:
            save();
            break;
        case Pin:
            app_.pin(*doc_, ocr_, imagePlacement());
            if (!pinned_)
                close();
            break;
        case Copy:
            copy();
            break;
        case CopyFile:
            copyFile();
            break;
        case Close:
            close();
            return;
        case AllText:
            if (ocr_ && !ocr_->tokens.empty())
            {
                anchor_ = 0;
                caret_ = static_cast<int>(ocr_->tokens.size()) - 1;
                copy();
            }
            else
                requestOcr();
            break;
        case Retry:
            requestOcr(true);
            break;
        case Translate:
            beginTranslation();
            break;
        case Tools:
        {
            showTools_ = !showTools_;
            hoverButton_ = -1;
            if (!showTools_)
            {
                closeFontPicker(false);
                selected_ = objectHandle_ = -1;
                tool_ = Tool::Select;
            }
            fitPin(true);
            layout();
            break;
        }
        }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void Canvas::finishGesture()
{
    finishText(true);
    if (dragging_ || objectDragging_ || cropping_ || moving_ || panning_ || selecting_)
        pointerUp(last_);
}
void Canvas::key(UINT key, bool ctrl, bool shift)
{
    if (edit_)
        return;
    if (key == VK_ESCAPE)
    {
        if (stroke_ || cropping_ || moving_ || panning_ || selecting_)
        {
            cancelGesture();
        }
        else if (selected_ >= 0)
        {
            selected_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else if (anchor_ >= 0)
        {
            anchor_ = caret_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else
            close();
        return;
    }
    if (ctrl)
    {
        if (key == 'C')
            copy(shift);
        else if (key == 'S')
            save();
        else if (key == 'F' && shift)
            copyFile();
        else if (key == 'Z')
            command(shift ? Redo : Undo);
        else if (key == 'Y')
            command(Redo);
        else if (key == 'T')
            command(Pin);
        else if (key == 'W')
            close();
        else if (key == 'A' && ocr_ && !ocr_->tokens.empty())
        {
            anchor_ = 0;
            caret_ = static_cast<int>(ocr_->tokens.size()) - 1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else if (key == '0' && pinned_)
            zoom(1.f / view_.zoom, {2, 2});
        return;
    }
    if (key == VK_RETURN)
    {
        copy();
        return;
    }
    if (key == VK_F3)
    {
        if (pinned_)
            app_.paste();
        else
            command(Pin);
        return;
    }
    if (key == VK_TAB && pinned_)
    {
        command(Tools);
        return;
    }
    if (!doc_)
        return;
    if (!annotationsEditable())
    {
        if (key == 'O')
            command(Select);
        return;
    }
    if ((key == VK_DELETE || key == VK_BACK) && selected_ >= 0)
    {
        if (objectDragging_)
            cancelGesture();
        doc_->remove(selected_);
        selected_ = -1;
        changed();
        return;
    }
    if (selected_ >= 0 && key >= VK_LEFT && key <= VK_DOWN)
    {
        auto b = annotationBounds(doc_->marks[selected_]), next = b;
        float step = shift ? 10.f : 1.f;
        next.x += key == VK_LEFT ? -step : key == VK_RIGHT ? step : 0.f;
        next.y += key == VK_UP ? -step : key == VK_DOWN ? step : 0.f;
        doc_->replace(selected_, transformAnnotation(doc_->marks[selected_], b, next));
        changed();
        return;
    }
    switch (key)
    {
    case 'V':
        command(Move);
        break;
    case 'A':
        command(Arrow);
        break;
    case 'L':
        command(Line);
        break;
    case 'R':
        command(Rectangle);
        break;
    case 'E':
        command(Ellipse);
        break;
    case 'T':
        command(Text);
        break;
    case 'M':
        command(Mosaic);
        break;
    case 'P':
        command(Marker);
        break;
    case 'O':
        command(Select);
        break;
    }
}
void Canvas::zoom(float factor, Point around)
{
    if (!pinned_ || !doc_ || edit_)
        return;
    Point image = view_.toImage(around);
    float next = std::clamp(view_.zoom * factor, .1f, 8.f);
    if (std::abs(next - view_.zoom) < .001f)
        return;
    RECT r{};
    GetWindowRect(hwnd_, &r);
    view_.zoom = next;
    fitPin(false);
    RECT client{};
    GetClientRect(hwnd_, &client);
    int w = client.right, h = client.bottom;
    float origin = std::max(1.f, std::round((w - doc_->base->width * next) / 2.f));
    int x = static_cast<int>(r.left + around.x - image.x * next - origin),
        y = static_cast<int>(r.top + around.y - image.y * next - 1);
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    fitPin(true);
    layout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void Canvas::cancelGesture()
{
    if (textEditorDragging_)
    {
        textEditorDragging_ = false;
        textEditorAnchor_ = textDragAnchor_;
        releasing_ = true;
        if (GetCapture() == hwnd_)
            ReleaseCapture();
        releasing_ = false;
        sizeTextEditor();
        if (edit_)
            SetFocus(edit_);
        requestFrame(true);
        return;
    }
    bool edited = stroke_.has_value() || cropping_ || (!pinned_ && moving_ && cropDirty_);
    if (cropping_ || (!pinned_ && moving_))
        crop_ = startCrop_;
    dragging_ = selecting_ = panning_ = cropping_ = moving_ = objectDragging_ = false;
    stroke_.reset();
    clearLive();
    releasing_ = true;
    if (GetCapture() == hwnd_)
        ReleaseCapture();
    releasing_ = false;
    if (doc_)
    {
        if (edited)
        {
            layout();
            if (app_.config.autoOcr)
                SetTimer(hwnd_, 1, 300, nullptr);
        }
        refreshBitmap();
    }
    else
        InvalidateRect(hwnd_, nullptr, FALSE);
}
void Canvas::contextMenu(POINT screen)
{
    if (!doc_)
    {
        close();
        return;
    }
    finishText(true);
    HMENU menu = CreatePopupMenu();
    AppendMenu(menu, MF_STRING, Copy, L"复制图片 / 所选文字\tCtrl+C");
    AppendMenu(menu, MF_STRING, CopyFile, L"复制 PNG 文件（原尺寸）\tCtrl+Shift+F");
    AppendMenu(menu, MF_STRING, AllText, L"复制全部文字");
    AppendMenu(menu, MF_STRING, Translate, L"英译中：所选文字 / 全文（联网）");
    AppendMenu(menu, MF_STRING, Retry, L"增强识别文字");
    AppendMenu(menu, MF_STRING, Save, L"保存 PNG\tCtrl+S");
    AppendMenu(menu, MF_STRING, Pin, L"新增贴图\tF3");
    AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
    if (pinned_)
    {
        AppendMenu(menu, MF_STRING | (showTools_ ? MF_CHECKED : 0), Tools, L"显示标注工具栏\tTab");
        HMENU zoomMenu = CreatePopupMenu();
        AppendMenu(zoomMenu, MF_STRING | (smoothZoom_ ? MF_CHECKED : 0), SmoothZoom, L"清晰缩放");
        AppendMenu(zoomMenu, MF_STRING | (!smoothZoom_ ? MF_CHECKED : 0), PixelZoom, L"像素显示");
        AppendMenu(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(zoomMenu), L"缩放显示");
        AppendMenu(menu, MF_STRING, ActualSize, L"原始大小（100%）\tCtrl+0");
        AppendMenu(menu, MF_STRING, DoubleSize, L"放大阅读（200%）");
    }
    if (textSizeContext())
        AppendMenu(menu, MF_STRING, Width, L"文字字号 (pt)…");
    AppendMenu(menu, MF_STRING, Close, L"关闭\tEsc");
    SetForegroundWindow(hwnd_);
    int result = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (result)
        command(result);
}
bool Canvas::preTranslate(MSG const &msg)
{
    if (textEditorDragging_ && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
    {
        cancelGesture();
        return true;
    }
    if (fontPopup_ && (msg.hwnd == fontPopup_ || IsChild(fontPopup_, msg.hwnd)))
    {
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE) &&
            !SendMessage(fontCombo_, CB_GETDROPPEDSTATE, 0, 0))
        {
            if (msg.wParam == VK_RETURN)
                applyFontPicker();
            else
                closeFontPicker();
            return true;
        }
        MSG copy = msg;
        return IsDialogMessage(fontPopup_, &copy) != FALSE;
    }
    if (edit_ && msg.hwnd == edit_ && msg.message == WM_KEYDOWN)
    {
        if ((msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE) && GetProp(edit_, L"SnapLite.Composing"))
        {
            // Translation may finish composition before the EDIT subclass sees
            // the same key. Preserve its IME ownership until key-up.
            SetProp(edit_, L"SnapLite.ImeKey", reinterpret_cast<HANDLE>(1));
            return false;
        }
        if (GetKeyState(VK_CONTROL) < 0 && msg.wParam == 'A')
        {
            SendMessage(edit_, EM_SETSEL, 0, -1);
            return true;
        }
        if ((msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE) && GetKeyState(VK_SHIFT) >= 0 &&
            !GetProp(edit_, L"SnapLite.Composing") && !GetProp(edit_, L"SnapLite.ImeKey"))
        {
            finishText(msg.wParam == VK_RETURN);
            SetFocus(hwnd_);
            return true;
        }
    }
    return false;
}
LRESULT Canvas::message(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOVE:
        if (doc_)
        {
            auto origin = view_.origin;
            layout();
            view_.origin = origin;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETFOCUS:
        if (edit_)
            SetFocus(edit_);
        return 0;
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lp) == edit_)
        {
            auto dc = reinterpret_cast<HDC>(wp);
            unsigned ink = textEditorInk();
            SetTextColor(dc, RGB((ink >> 16) & 255, (ink >> 8) & 255, ink & 255));
            SetBkMode(dc, TRANSPARENT);
            auto brush = editorBrush();
            POINT origin{};
            GetViewportOrgEx(dc, &origin);
            SetBrushOrgEx(dc, origin.x, origin.y, nullptr);
            return reinterpret_cast<LRESULT>(brush);
        }
        break;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == edit_ && (HIWORD(wp) == EN_UPDATE || HIWORD(wp) == EN_CHANGE))
        {
            sizeTextEditor();
            InvalidateRect(edit_, nullptr, TRUE);
        }
        return 0;
    case WM_DPICHANGED:
    {
        closeFontPicker(false);
        ui_ = HIWORD(wp) / 96.f;
        if (pinned_ && doc_ && !initialPlacement_)
        {
            auto r = reinterpret_cast<RECT *>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            fitPin(true);
        }
        if (edit_)
            finishText(true);
        layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        pointerDown(mouse(lp), wp);
        return 0;
    case WM_MOUSEMOVE:
        pointerMove(mouse(lp), wp);
        return 0;
    case WM_LBUTTONUP:
        pointerUp(mouse(lp));
        return 0;
    case WM_LBUTTONDBLCLK:
    {
        auto p = mouse(lp);
        if (showTools_ && (toolbar_.contains(p) || statusBox_.contains(p)))
            return 0;
        int object = doc_ ? objectAt(view_.toImage(p)) : -1;
        if (object >= 0 && annotationsEditable())
        {
            cancelGesture();
            if (doc_->marks[object].kind == Tool::Text)
                editText(object);
            return 0;
        }
        if (textAt(p) >= 0)
        {
            pointerDown(p, wp);
            return 0;
        }
        // Locked annotations are part of the displayed image. Double-clicking
        // them must neither reopen a text editor nor accidentally close the pin.
        if (object >= 0)
            return 0;
        if (pinned_)
            close();
        else if (doc_)
            copy(true);
        return 0;
    }
    case WM_RBUTTONUP:
    {
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd_, &p);
        contextMenu(p);
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        zoom(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.1f : 1.f / 1.1f, {static_cast<float>(pt.x), static_cast<float>(pt.y)});
        return 0;
    }
    case WM_MOUSELEAVE:
        hovering_ = false;
        hoverButton_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        key(static_cast<UINT>(wp), (GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 0;
    case WM_FINISH_TEXT:
        finishText(wp != 0);
        SetFocus(hwnd_);
        return 0;
    case WM_TIMER:
        if (wp == 1)
            requestOcr();
        else if (wp == 3)
        {
            KillTimer(hwnd_, 3);
            syncPreview();
            if (previewRequest_)
                app_.previewScaler_->submit(*previewRequest_);
        }
        return 0;
    case WM_SETCURSOR:
    {
        if (reinterpret_cast<HWND>(wp) != hwnd_ || LOWORD(lp) != HTCLIENT)
            break;
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(hwnd_, &p);
        Point point{static_cast<float>(p.x), static_cast<float>(p.y)};
        LPCWSTR cursor = IDC_ARROW;
        if (!doc_)
            cursor = IDC_CROSS;
        else if (edit_ && editorBorderAt(point))
            cursor = IDC_SIZEALL;
        else if (!(GetKeyState(VK_SPACE) & 0x8000) && handleAt(point) >= 0)
        {
            bool segment = segmentAnnotation(doc_->marks[selected_]);
            int handle = handleAt(point);
            cursor = segment                        ? IDC_CROSS
                     : (handle == 1 || handle == 5) ? IDC_SIZENS
                     : (handle == 3 || handle == 7) ? IDC_SIZEWE
                     : (handle == 0 || handle == 4) ? IDC_SIZENWSE
                                                    : IDC_SIZENESW;
        }
        else if (!(GetKeyState(VK_SPACE) & 0x8000) && !(pinned_ && (GetKeyState(VK_CONTROL) & 0x8000)) &&
                 textAt(point) >= 0)
            cursor = IDC_IBEAM;
        else if (!(GetKeyState(VK_SPACE) & 0x8000) && edgeAt(point))
        {
            int e = edgeAt(point);
            cursor = (e == 1 || e == 2)    ? IDC_SIZEWE
                     : (e == 4 || e == 8)  ? IDC_SIZENS
                     : (e == 5 || e == 10) ? IDC_SIZENWSE
                                           : IDC_SIZENESW;
        }
        else if (tool_ == Tool::Text && objectAt(view_.toImage(point)) >= 0)
            cursor = IDC_SIZEALL;
        else if ((tool_ == Tool::Move || tool_ == Tool::Select) && doc_)
            cursor = IDC_SIZEALL;
        else if (tool_ != Tool::Move && tool_ != Tool::Select)
            cursor = IDC_CROSS;
        SetCursor(cursor == IDC_CROSS ? crosshair_.get(GetDpiForWindow(hwnd_)) : LoadCursor(nullptr, cursor));
        return TRUE;
    }
    case WM_CAPTURECHANGED:
        if (!releasing_)
            cancelGesture();
        return 0;
    case WM_CLOSE:
        close();
        return 0;
    case WM_DESTROY:
        dropPreview();
        rt_ = nullptr;
        pinFrame_ = nullptr;
        bitmap_ = nullptr;
        backgroundBitmap_ = nullptr;
        mosaicBitmap_ = nullptr;
        for (auto &effect : tailEffects_)
            effect.bitmap = nullptr;
        return 0;
    }
    return DefWindowProc(hwnd_, msg, wp, lp);
}
void Canvas::renderPreview(std::wstring const &path)
{
    if (doc_)
        savePng(app_.graphics, *doc_->rendered, path);
}
void Canvas::renderUiPreview(std::wstring const &path)
{
    RECT r{};
    GetClientRect(hwnd_, &r);
    Image image(r.right, r.bottom, Bg);
    Dib dib(image);
    winrt::com_ptr<ID2D1DCRenderTarget> target;
    auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    check(app_.graphics.d2d->CreateDCRenderTarget(&properties, target.put()));
    check(target->BindDC(dib.dc, &r));
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    auto previous = std::move(rt_);
    discardTargetBitmaps();
    rt_ = target;
    try
    {
        paint();
        if (edit_)
        {
            RECT child{};
            GetWindowRect(edit_, &child);
            MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT *>(&child), 2);
            GdiFlush();
            int saved = SaveDC(dib.dc);
            SetViewportOrgEx(dib.dc, child.left, child.top, nullptr);
            SendMessage(edit_, WM_PRINT, reinterpret_cast<WPARAM>(dib.dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
            RestoreDC(dib.dc, saved);
        }
        savePng(app_.graphics, dib.image(), path);
    }
    catch (...)
    {
        rt_ = std::move(previous);
        discardTargetBitmaps();
        throw;
    }
    rt_ = std::move(previous);
    discardTargetBitmaps();
}
} // namespace snap
