#include "document.h"
namespace snap
{
bool isCjk(wchar_t c)
{
    return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0x3000 && c <= 0x303f) ||
           (c >= 0xff00 && c <= 0xffef);
}
int OcrSnapshot::hit(Point p, bool nearest, float tolerance) const
{
    int best = -1;
    float distance = std::numeric_limits<float>::max();
    for (size_t i = 0; i < tokens.size(); ++i)
    {
        auto b = tokens[i].box;
        auto local = rotate(p, {width / 2.f, height / 2.f}, -tokens[i].angle.value_or(angle));
        if (b.contains(local))
            return static_cast<int>(i);
        if (nearest || tolerance > 0)
        {
            float dx = std::max({b.x - local.x, 0.f, local.x - b.x - b.w}),
                  dy = std::max({b.y - local.y, 0.f, local.y - b.y - b.h});
            // Make glyph edges and short word gaps selectable, without treating
            // distant whitespace or the next column as text. Canvas supplies a
            // screen-space tolerance converted into image coordinates.
            float slopX = std::max(tolerance, std::min(b.h * .6f, tolerance * 1.5f));
            float slopY = std::max(tolerance * .75f, std::min(b.h * .35f, tolerance));
            if (!nearest && (dx > slopX || dy > slopY))
                continue;
            float d = dx * dx + dy * dy * 4.f;
            if (d < distance)
            {
                distance = d;
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}
std::wstring OcrSnapshot::selection(int first, int last) const
{
    if (first < 0 || last < 0 || tokens.empty())
        return {};
    first = std::clamp(first, 0, static_cast<int>(tokens.size()) - 1);
    last = std::clamp(last, 0, static_cast<int>(tokens.size()) - 1);
    if (first > last)
        std::swap(first, last);
    std::wstring result;
    for (int i = first; i <= last; ++i)
    {
        auto const &token = tokens[i];
        if (i > first)
        {
            auto const &prev = tokens[i - 1];
            if (prev.line != token.line)
                result += L"\r\n";
            else if (!prev.text.empty() && !token.text.empty())
            {
                wchar_t a = prev.text.back(), b = token.text.front();
                bool latinA = a < 128 && (iswalnum(a) || a == L')' || a == L'%' || a == L'.' || a == L',' ||
                                          a == L':' || a == L';' || a == L'!' || a == L'/');
                bool latinB = b < 128 && (iswalnum(b) || b == L'(' || b == L'/');
                float gap = token.box.x - prev.box.x - prev.box.w;
                float spaceThreshold = std::max(2.f, std::min(prev.box.h, token.box.h) * .18f);
                bool mixed = ((a >= 0x3400 && a <= 0x9fff) && latinB) || (latinA && (b >= 0x3400 && b <= 0x9fff));
                if ((latinA && latinB || mixed) && gap > spaceThreshold)
                    result += L' ';
            }
        }
        result += token.text;
    }
    return result;
}
std::vector<Point> OcrSnapshot::quad(Box b) const
{
    Point c{width / 2.f, height / 2.f};
    return {rotate({b.x, b.y}, c, angle), rotate({b.x + b.w, b.y}, c, angle), rotate({b.x + b.w, b.y + b.h}, c, angle),
            rotate({b.x, b.y + b.h}, c, angle)};
}
std::vector<Point> OcrSnapshot::quad(Token const &token) const
{
    Point c{width / 2.f, height / 2.f};
    auto b = token.box;
    float rotation = token.angle.value_or(angle);
    return {rotate({b.x, b.y}, c, rotation), rotate({b.x + b.w, b.y}, c, rotation),
            rotate({b.x + b.w, b.y + b.h}, c, rotation), rotate({b.x, b.y + b.h}, c, rotation)};
}
void Document::add(Annotation m)
{
    undoEdits.push_back({marks.size(), std::nullopt, m});
    marks.push_back(std::move(m));
    redoEdits.clear();
    ++revision;
}
void Document::replace(size_t index, Annotation m)
{
    if (index >= marks.size())
        return;
    undoEdits.push_back({index, marks[index], m});
    marks[index] = std::move(m);
    redoEdits.clear();
    ++revision;
}
void Document::remove(size_t index)
{
    if (index >= marks.size())
        return;
    undoEdits.push_back({index, marks[index], std::nullopt});
    marks.erase(marks.begin() + index);
    redoEdits.clear();
    ++revision;
}
static void applyEdit(std::vector<Annotation> &marks, Document::Edit const &edit, bool forward)
{
    auto const &before = forward ? edit.before : edit.after;
    auto const &after = forward ? edit.after : edit.before;
    if (before && after)
        marks[edit.index] = *after;
    else if (after)
        marks.insert(marks.begin() + edit.index, *after);
    else
        marks.erase(marks.begin() + edit.index);
}
bool Document::undo()
{
    if (undoEdits.empty())
        return false;
    applyEdit(marks, undoEdits.back(), false);
    redoEdits.push_back(std::move(undoEdits.back()));
    undoEdits.pop_back();
    ++revision;
    return true;
}
bool Document::redo()
{
    if (redoEdits.empty())
        return false;
    applyEdit(marks, redoEdits.back(), true);
    undoEdits.push_back(std::move(redoEdits.back()));
    redoEdits.pop_back();
    ++revision;
    return true;
}
void Document::translate(float dx, float dy)
{
    auto shift = [=](Annotation &m)
    {
        m.from.x += dx;
        m.from.y += dy;
        m.to.x += dx;
        m.to.y += dy;
        for (auto &p : m.path)
        {
            p.x += dx;
            p.y += dy;
        }
    };
    for (auto &m : marks)
        shift(m);
    for (auto *history : {&undoEdits, &redoEdits})
        for (auto &e : *history)
        {
            if (e.before)
                shift(*e.before);
            if (e.after)
                shift(*e.after);
        }
}
Box annotationBounds(Annotation const &m)
{
    Box b = between(m.from, m.to);
    if (m.kind == Tool::Marker)
        for (auto p : m.path)
        {
            float x = std::min(b.x, p.x), y = std::min(b.y, p.y);
            b = {x, y, std::max(b.x + b.w, p.x) - x, std::max(b.y + b.h, p.y) - y};
        }
    if (m.kind == Tool::Text && (m.to.x <= m.from.x || m.to.y <= m.from.y))
        b = {m.from.x, m.from.y, textFontPixels(m) * std::max(1.f, m.text.size() * .65f), textFontPixels(m) * 1.5f};
    return b;
}
bool segmentAnnotation(Annotation const &m)
{
    return m.kind == Tool::Arrow || m.kind == Tool::Line || (m.kind == Tool::Marker && m.path.size() <= 2);
}
static float distanceToSegment(Point p, Point a, Point b)
{
    float dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
    float t = length > .001f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0.f, 1.f) : 0.f;
    return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}
bool hitAnnotation(Annotation const &m, Point p, float tolerance)
{
    auto b = annotationBounds(m);
    float t = tolerance + m.width / 2.f;
    if (m.kind == Tool::Line || m.kind == Tool::Arrow)
    {
        if (distanceToSegment(p, m.from, m.to) <= t)
            return true;
        if (m.kind == Tool::Arrow)
        {
            float a = std::atan2(m.to.y - m.from.y, m.to.x - m.from.x), length = std::max(12.f, m.width * 4.f);
            for (float sign : {-1.f, 1.f})
                if (distanceToSegment(p, m.to,
                                      {m.to.x - length * std::cos(a + sign * .48f),
                                       m.to.y - length * std::sin(a + sign * .48f)}) <= t)
                    return true;
        }
        return false;
    }
    if (m.kind == Tool::Marker)
    {
        Point last = m.from;
        for (auto next : m.path)
        {
            if (distanceToSegment(p, last, next) <= tolerance + std::max(12.f, m.width * 6.f) / 2)
                return true;
            last = next;
        }
        return false;
    }
    if (m.kind == Tool::Text || m.kind == Tool::Mosaic)
        return Box{b.x - tolerance, b.y - tolerance, b.w + tolerance * 2, b.h + tolerance * 2}.contains(p);
    if (m.kind == Tool::Ellipse)
    {
        float rx = std::max(.5f, b.w / 2), ry = std::max(.5f, b.h / 2);
        return std::abs(std::hypot((p.x - b.x - rx) / rx, (p.y - b.y - ry) / ry) - 1.f) * std::min(rx, ry) <= t;
    }
    return Box{b.x - t, b.y - t, b.w + t * 2, b.h + t * 2}.contains(p) &&
           !Box{b.x + t, b.y + t, std::max(0.f, b.w - t * 2), std::max(0.f, b.h - t * 2)}.contains(p);
}
Annotation transformAnnotation(Annotation const &m, Box from, Box to)
{
    Annotation result = m;
    auto transform = [=](Point p)
    {
        return Point{to.x + (p.x - from.x) * (from.w > .001f ? to.w / from.w : 1.f),
                     to.y + (p.y - from.y) * (from.h > .001f ? to.h / from.h : 1.f)};
    };
    result.from = transform(m.from);
    result.to = transform(m.to);
    for (auto &p : result.path)
        p = transform(p);
    if (m.kind == Tool::Text && from.h > .001f)
    {
        if (m.fontPt > 0)
            result.fontPt = std::clamp(m.fontPt * to.h / from.h, 6.f, 288.f);
        else
            result.width = std::clamp(m.width * to.h / from.h, 1.f, 64.f);
    }
    return result;
}
float textFontPoints(Annotation const &m)
{
    return m.fontPt > 0 ? m.fontPt : std::max(16.f, m.width * 8.f) * .75f;
}
float textFontPixels(Annotation const &m)
{
    return textFontPoints(m) * (96.f / 72.f);
}
void measureText(Graphics &g, Annotation &m, float maxWidth)
{
    auto font = g.font(textFontPixels(m));
    winrt::com_ptr<IDWriteTextLayout> layout;
    check(g.text->CreateTextLayout(m.text.c_str(), static_cast<UINT32>(m.text.size()), font.get(),
                                   std::max(16.f, maxWidth), 65536.f, layout.put()));
    DWRITE_TEXT_METRICS metrics{};
    check(layout->GetMetrics(&metrics));
    m.to = {m.from.x + std::max(8.f, metrics.widthIncludingTrailingWhitespace + 2.f),
            m.from.y + std::max(16.f, metrics.height + 2.f)};
}
void drawAnnotation(Graphics &g, ID2D1RenderTarget *rt, ID2D1SolidColorBrush *brush, ID2D1StrokeStyle *rounded,
                    Annotation const &m, Box clip)
{
    brush->SetColor(color(m.rgb, m.kind == Tool::Marker ? .32f : 1.f));
    if (m.kind == Tool::Arrow || m.kind == Tool::Line)
    {
        rt->DrawLine(dp(m.from), dp(m.to), brush, m.width, rounded);
        if (m.kind == Tool::Arrow)
        {
            float a = std::atan2(m.to.y - m.from.y, m.to.x - m.from.x), length = std::max(12.f, m.width * 4.f);
            for (float sign : {-1.f, 1.f})
                rt->DrawLine(
                    dp(m.to),
                    dp({m.to.x - length * std::cos(a + sign * .48f), m.to.y - length * std::sin(a + sign * .48f)}),
                    brush, m.width, rounded);
        }
    }
    else if (m.kind == Tool::Rectangle)
        rt->DrawRectangle(dr(between(m.from, m.to)), brush, m.width);
    else if (m.kind == Tool::Ellipse)
    {
        auto b = between(m.from, m.to);
        rt->DrawEllipse(D2D1::Ellipse({b.x + b.w / 2, b.y + b.h / 2}, b.w / 2, b.h / 2), brush, m.width);
    }
    else if (m.kind == Tool::Marker)
    {
        winrt::com_ptr<ID2D1PathGeometry> path;
        check(g.d2d->CreatePathGeometry(path.put()));
        winrt::com_ptr<ID2D1GeometrySink> sink;
        check(path->Open(sink.put()));
        sink->BeginFigure(dp(m.from), D2D1_FIGURE_BEGIN_HOLLOW);
        for (auto p : m.path)
            sink->AddLine(dp(p));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        check(sink->Close());
        rt->DrawGeometry(path.get(), brush, std::max(12.f, m.width * 6.f), rounded);
    }
    else if (m.kind == Tool::Text)
    {
        auto font = g.font(textFontPixels(m));
        auto b = annotationBounds(m);
        b.w = std::min(b.w, std::max(0.f, clip.x + clip.w - b.x));
        rt->DrawTextW(m.text.c_str(), static_cast<UINT32>(m.text.size()), font.get(), dr(b), brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}
std::vector<Box> Document::redactions() const
{
    std::vector<Box> r;
    for (auto const &m : marks)
        if (m.kind == Tool::Mosaic)
            r.push_back(between(m.from, m.to));
    return r;
}
int mosaicCellPixels(Annotation const &mark)
{
    return std::clamp(mark.mosaicPixels, 6, 16);
}
Image renderAnnotations(Graphics &g, Image const &base, std::span<const Annotation> marks,
                        std::vector<RasterLayer> *rasterLayers)
{
    if (rasterLayers)
        rasterLayers->clear();
    Dib dib(base);
    winrt::com_ptr<ID2D1DCRenderTarget> rt;
    auto properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    check(g.d2d->CreateDCRenderTarget(&properties, rt.put()));
    RECT r{0, 0, base.width, base.height};
    check(rt->BindDC(dib.dc, &r));
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
    check(rt->CreateSolidColorBrush(color(0xffffff), brush.put()));
    winrt::com_ptr<ID2D1StrokeStyle> rounded;
    auto props = D2D1::StrokeStyleProperties();
    props.startCap = props.endCap = props.dashCap = D2D1_CAP_STYLE_ROUND;
    props.lineJoin = D2D1_LINE_JOIN_ROUND;
    check(g.d2d->CreateStrokeStyle(props, nullptr, 0, rounded.put()));
    for (auto const &m : marks)
    {
        if (m.kind == Tool::Mosaic)
        {
            pixelateDib(dib, between(m.from, m.to), mosaicCellPixels(m));
            if (rasterLayers)
            {
                auto b = between(m.from, m.to);
                RECT region{static_cast<LONG>(std::clamp(std::floor(b.x), 0.f, static_cast<float>(base.width))),
                            static_cast<LONG>(std::clamp(std::floor(b.y), 0.f, static_cast<float>(base.height))),
                            static_cast<LONG>(std::clamp(std::ceil(b.x + b.w), 0.f, static_cast<float>(base.width))),
                            static_cast<LONG>(std::clamp(std::ceil(b.y + b.h), 0.f, static_cast<float>(base.height)))};
                if (region.right > region.left && region.bottom > region.top)
                {
                    Image tile(region.right - region.left, region.bottom - region.top);
                    GdiFlush();
                    for (int y = 0; y < tile.height; ++y)
                        std::memcpy(tile.pixels.data() + y * tile.stride(),
                                    dib.bits + (y + region.top) * base.stride() + region.left * 4, tile.stride());
                    rasterLayers->push_back({static_cast<size_t>(&m - marks.data()),
                                             {static_cast<float>(region.left), static_cast<float>(region.top),
                                              static_cast<float>(tile.width), static_cast<float>(tile.height)},
                                             std::make_shared<Image>(std::move(tile))});
                }
            }
            continue;
        }
        rt->BeginDraw();
        drawAnnotation(g, rt.get(), brush.get(), rounded.get(), m,
                       {0, 0, static_cast<float>(base.width), static_cast<float>(base.height)});
        check(rt->EndDraw());
    }
    return dib.image();
}
void Document::flatten(Graphics &g, Annotation const *preview)
{
    ++flattenCount;
    rasterLayers.clear();
    if (marks.empty() && !preview)
    {
        rendered = base;
        displayBase = base;
        return;
    }
    if (preview)
    {
        auto list = marks;
        list.push_back(*preview);
        rendered = std::make_shared<Image>(renderAnnotations(g, *base, list, &rasterLayers));
    }
    else
        rendered = std::make_shared<Image>(renderAnnotations(g, *base, marks, &rasterLayers));
    displayBase = base;
    if (!rasterLayers.empty())
    {
        // Filtering must not sample the original hidden pixels outside an opaque mask edge.
        auto safe = std::make_shared<Image>(*base);
        for (auto const &layer : rasterLayers)
            for (int y = 0; y < layer.image->height; ++y)
                std::memcpy(safe->pixels.data() + (y + static_cast<int>(layer.box.y)) * safe->stride() +
                                static_cast<int>(layer.box.x) * 4,
                            layer.image->pixels.data() + y * layer.image->stride(), layer.image->stride());
        displayBase = std::move(safe);
    }
}
} // namespace snap
