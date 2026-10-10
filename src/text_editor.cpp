#include "app.h"
namespace snap
{
bool Canvas::editorBorderAt(Point p) const
{
    if (!edit_)
        return false;
    RECT box{}, format{};
    GetWindowRect(edit_, &box);
    MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT *>(&box), 2);
    SendMessage(edit_, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
    Box frame{box.left - 4 * ui_, box.top - 4 * ui_, box.right - box.left + 8 * ui_, box.bottom - box.top + 8 * ui_};
    Box text{static_cast<float>(box.left + format.left), static_cast<float>(box.top + format.top),
             static_cast<float>(format.right - format.left), static_cast<float>(format.bottom - format.top)};
    return frame.contains(p) && !text.contains(p);
}
void Canvas::beginEditorDrag(Point p)
{
    if (!edit_)
        return;
    textEditorDragging_ = true;
    textDragStart_ = p;
    textDragAnchor_ = textEditorAnchor_;
    SetCapture(hwnd_);
    SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
}
void Canvas::sizeTextEditor()
{
    if (!edit_ || !stroke_ || sizingEditor_)
        return;
    sizingEditor_ = true;
    struct Reset
    {
        bool &value;
        ~Reset()
        {
            value = false;
        }
    } reset{sizingEditor_};
    RECT client{}, box{};
    GetClientRect(hwnd_, &client);
    GetWindowRect(edit_, &box);
    MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT *>(&box), 2);
    int length = GetWindowTextLength(edit_);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowText(edit_, text.data(), length + 1);
    text.resize(length);
    HDC dc = GetDC(edit_);
    if (!dc)
        winrt::throw_last_error();
    auto previous = SelectObject(dc, editFont_);
    TEXTMETRIC metrics{};
    GetTextMetrics(dc, &metrics);
    RECT measured{};
    if (!text.empty())
        DrawText(dc, text.c_str(), length, &measured, DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS);
    SelectObject(dc, previous);
    ReleaseDC(edit_, dc);
    int insetX = std::max(2, static_cast<int>(4 * ui_)), insetY = std::max(2, static_cast<int>(3 * ui_));
    int margin = std::max(1, static_cast<int>(4 * ui_));
    auto picture =
        view_.toView(Box{0, 0, static_cast<float>(doc_->base->width), static_cast<float>(doc_->base->height)});
    int left = std::max(margin, static_cast<int>(std::ceil(picture.x)) - insetX);
    int top = std::max(margin, static_cast<int>(std::ceil(picture.y)) - insetY);
    int right =
        std::min(static_cast<int>(client.right) - margin, static_cast<int>(std::floor(picture.x + picture.w)) + insetX);
    int bottom = std::min(static_cast<int>(client.bottom) - margin,
                          static_cast<int>(std::floor(picture.y + picture.h)) + insetY);
    left = std::clamp(left, margin, std::max(margin, static_cast<int>(client.right) - margin - 1));
    top = std::clamp(top, margin, std::max(margin, static_cast<int>(client.bottom) - margin - 1));
    int maxWidth = std::max(1, right - left), maxHeight = std::max(1, bottom - top);
    int naturalWidth = std::max(std::max(1L, metrics.tmAveCharWidth), measured.right);
    int w = std::min(maxWidth, naturalWidth + insetX * 2 + 2);
    int lineHeight = std::max(1L, metrics.tmHeight);
    int oldHeight = std::max(1L, box.bottom - box.top);
    if (w != box.right - box.left)
        SetWindowPos(edit_, nullptr, 0, 0, w, oldHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT formatting{insetX, insetY, std::max(insetX + 1, w - insetX), std::max(insetY + 1, maxHeight - insetY)};
    SendMessage(edit_, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
    int lines = std::clamp(static_cast<int>(SendMessage(edit_, EM_GETLINECOUNT, 0, 0)), 1, 8192);
    int h = std::min(maxHeight, lines * lineHeight + insetY * 2);
    int x = std::clamp(static_cast<int>(std::round(textEditorAnchor_.x)) - insetX, left, std::max(left, right - w));
    int y = std::clamp(static_cast<int>(std::round(textEditorAnchor_.y)) - insetY, top, std::max(top, bottom - h));
    bool moved = box.left != x || box.top != y || box.right - box.left != w || box.bottom - box.top != h;
    if (moved)
        SetWindowPos(edit_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    formatting.bottom = std::max(insetY + 1, h - insetY);
    SendMessage(edit_, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
    stroke_->from = view_.toImage(Point{static_cast<float>(x + insetX), static_cast<float>(y + insetY)});
    if (moved)
    {
        InvalidateRect(edit_, nullptr, TRUE);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}
HBRUSH Canvas::editorBrush()
{
    // Native EDIT keeps IME, caret and selection behavior. Paint the exact image
    // under its client area instead of a solid paper color, including redactions.
    if (!editorBackdrop_)
        editorBackdrop_ = std::make_unique<EditorBackdrop>();
    auto &cache = *editorBackdrop_;
    RECT box{};
    GetWindowRect(edit_, &box);
    MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT *>(&box), 2);
    auto source = liveBase_ ? liveBase_ : doc_->displayBase;
    if (cache.ready && cache.source == source && EqualRect(&cache.box, &box) && cache.view.zoom == view_.zoom &&
        cache.view.origin.x == view_.origin.x && cache.view.origin.y == view_.origin.y && cache.smooth == smoothZoom_)
        return cache.brush;
    if (!cache.target)
    {
        auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        properties.dpiX = properties.dpiY = 96;
        check(app_.graphics.d2d->CreateDCRenderTarget(&properties, cache.target.put()));
        cache.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    auto bitmap = [&](Image const &image, winrt::com_ptr<ID2D1Bitmap> &result)
    {
        if (!result)
            check(cache.target->CreateBitmap(
                D2D1::SizeU(image.width, image.height), image.pixels.data(), static_cast<UINT32>(image.stride()),
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                result.put()));
    };
    if (cache.source != source)
    {
        cache.bitmap = nullptr;
        cache.effects.clear();
        cache.source = source;
        if (liveBase_)
            for (auto const &effect : tailEffects_)
                cache.effects.push_back({effect.index, effect.box, effect.image, {}});
        else
            for (auto const &layer : doc_->rasterLayers)
                cache.effects.push_back({layer.index, layer.box, layer.image, {}});
    }
    bitmap(*source, cache.bitmap);
    if (desktop_)
        bitmap(*desktop_, cache.desktopBitmap);
    for (auto &effect : cache.effects)
        bitmap(*effect.image, effect.bitmap);
    int w = std::max(1L, box.right - box.left), h = std::max(1L, box.bottom - box.top);
    auto surface = std::make_unique<Dib>(w, h);
    RECT area{0, 0, w, h};
    check(cache.target->BindDC(surface->dc, &area));
    auto target = cache.target.get();
    winrt::com_ptr<ID2D1SolidColorBrush> ink;
    check(target->CreateSolidColorBrush(color(0), ink.put()));
    winrt::com_ptr<ID2D1StrokeStyle> rounded;
    auto properties = D2D1::StrokeStyleProperties();
    properties.startCap = properties.endCap = properties.dashCap = D2D1_CAP_STYLE_ROUND;
    properties.lineJoin = D2D1_LINE_JOIN_ROUND;
    check(app_.graphics.d2d->CreateStrokeStyle(properties, nullptr, 0, rounded.put()));
    View local = view_;
    local.origin.x -= box.left;
    local.origin.y -= box.top;
    auto picture =
        local.toView(Box{0, 0, static_cast<float>(doc_->base->width), static_cast<float>(doc_->base->height)});
    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->Clear(color(pinned_ ? 0xf6f8fb : 0x000000));
    if (desktop_)
    {
        Box desktop{-static_cast<float>(box.left), -static_cast<float>(box.top), static_cast<float>(desktop_->width),
                    static_cast<float>(desktop_->height)};
        target->DrawBitmap(cache.desktopBitmap.get(), dr(desktop));
        ink->SetColor(color(0x101214, .52f));
        target->FillRectangle(dr(desktop), ink.get());
    }
    target->PushAxisAlignedClip(dr(picture), D2D1_ANTIALIAS_MODE_ALIASED);
    auto context = cache.target.try_as<ID2D1DeviceContext>();
    if (smoothZoom_ && !liveBase_ && view_.zoom > 1.001f && context)
        context->DrawBitmap(cache.bitmap.get(), dr(picture), 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    else
        target->DrawBitmap(cache.bitmap.get(), dr(picture), 1.f,
                           liveBase_ || !smoothZoom_ || std::abs(view_.zoom - 1.f) < .001f
                               ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                               : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    target->SetTransform(D2D1::Matrix3x2F(local.zoom, 0, 0, local.zoom, local.origin.x, local.origin.y));
    size_t first = liveBase_ && selected_ >= 0 ? static_cast<size_t>(selected_ + 1) : 0;
    for (size_t i = first; i < doc_->marks.size(); ++i)
    {
        auto const &mark = doc_->marks[i];
        if (mark.kind == Tool::Mosaic)
        {
            for (auto const &effect : cache.effects)
                if (effect.index == i)
                    target->DrawBitmap(effect.bitmap.get(), dr(effect.box), 1.f,
                                       D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }
        else
            drawAnnotation(app_.graphics, target, ink.get(), rounded.get(), mark,
                           {0, 0, static_cast<float>(doc_->base->width), static_cast<float>(doc_->base->height)});
    }
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->PopAxisAlignedClip();
    check(target->EndDraw());
    auto brush = CreatePatternBrush(surface->bitmap);
    if (!brush)
        winrt::throw_last_error();
    if (cache.brush)
        DeleteObject(cache.brush);
    cache.brush = brush;
    cache.surface = std::move(surface);
    cache.box = box;
    cache.view = view_;
    cache.smooth = smoothZoom_;
    cache.ready = true;
    return brush;
}
} // namespace snap
