#include "ocr.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
namespace snap
{
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage::Streams;
using winrt::Windows::Globalization::Language;
struct __declspec(uuid("905a0fef-bc53-11df-8c49-001e4fc686da")) BufferAccess : IUnknown
{
    virtual HRESULT __stdcall Buffer(BYTE **value) = 0;
};
namespace
{
bool han(wchar_t c)
{
    return c >= 0x3400 && c <= 0x9fff;
}
bool latinFragment(std::wstring const &text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(),
                                        [](wchar_t c) { return c < 128 && (iswalnum(c) || c == L'_' || c == L'\''); });
}
Box unite(Box a, Box b)
{
    float x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
}
std::vector<Token> lineTokens(OcrLine const &line, float scale, float angle, int index)
{
    std::vector<Token> tokens;
    for (auto const &word : line.Words())
    {
        auto r = word.BoundingRect();
        Token token{
            std::wstring(word.Text()), {r.X / scale, r.Y / scale, r.Width / scale, r.Height / scale}, index, angle};
        if (token.text.empty())
            continue;
        if (!tokens.empty())
        {
            auto &prev = tokens.back();
            float gap = token.box.x - prev.box.x - prev.box.w;
            float threshold = std::max(2.f, std::min(prev.box.h, token.box.h) * .18f);
            if (latinFragment(prev.text) && latinFragment(token.text) && gap >= -1.f && gap <= threshold)
            {
                prev.text += token.text;
                prev.box = unite(prev.box, token.box);
                continue;
            }
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}
Box physicalBounds(OcrSnapshot const &snapshot, std::span<const Token> tokens)
{
    Box result{};
    bool first = true;
    for (auto const &token : tokens)
    {
        auto quad = snapshot.quad(token);
        for (auto p : quad)
        {
            Box point{p.x, p.y, .01f, .01f};
            result = first ? point : unite(result, point);
            first = false;
        }
    }
    return result;
}
struct RecognizedLine
{
    std::vector<Token> tokens;
    Box bounds;
};
bool cancelled(OcrRequest const &request)
{
    return request.cancelled && request.cancelled->load();
}
int luminance(uint8_t const *p)
{
    return (p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8;
}
constexpr int Padding = 12, Context = 96;
Image enlargeForOcr(Image &source, int scale)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                           factory.put_void()));
    winrt::com_ptr<IWICBitmap> bitmap;
    check(factory->CreateBitmapFromMemory(source.width, source.height, GUID_WICPixelFormat32bppBGRA,
                                          static_cast<UINT>(source.stride()), static_cast<UINT>(source.pixels.size()),
                                          source.pixels.data(), bitmap.put()));
    winrt::com_ptr<IWICBitmapScaler> scaler;
    check(factory->CreateBitmapScaler(scaler.put()));
    Image result(source.width * scale, source.height * scale);
    check(scaler->Initialize(bitmap.get(), result.width, result.height, WICBitmapInterpolationModeHighQualityCubic));
    check(scaler->CopyPixels(nullptr, static_cast<UINT>(result.stride()), static_cast<UINT>(result.pixels.size()),
                             result.pixels.data()));
    return result;
}
struct Prepared
{
    Image image;
    RECT crop{}, owner{};
    int scale{1};
};
Prepared prepare(Image const &source, RECT crop, RECT owner, int scale, bool cubic = true)
{
    Prepared result;
    result.owner = owner;
    result.scale = scale;
    auto background = source.pixels.data() + crop.top * source.stride() + crop.left * 4;
    auto blank = [&](int x, int y)
    {
        auto p = source.pixels.data() + y * source.stride() + x * 4;
        return std::abs(p[0] - background[0]) <= 8 && std::abs(p[1] - background[1]) <= 8 &&
               std::abs(p[2] - background[2]) <= 8;
    };
    auto blankRow = [&](int y)
    {
        for (int x = crop.left; x < crop.right; ++x)
            if (!blank(x, y))
                return false;
        return true;
    };
    auto blankColumn = [&](int x)
    {
        for (int y = crop.top; y < crop.bottom; ++y)
            if (!blank(x, y))
                return false;
        return true;
    };
    // Trim only uniform exterior margins. Never discard an interior gap or a low-confidence line.
    while (crop.top < crop.bottom && blankRow(crop.top))
        ++crop.top;
    while (crop.top < crop.bottom && blankRow(crop.bottom - 1))
        --crop.bottom;
    while (crop.left < crop.right && blankColumn(crop.left))
        ++crop.left;
    while (crop.left < crop.right && blankColumn(crop.right - 1))
        --crop.right;
    result.crop = crop;
    if (crop.top == crop.bottom || crop.left == crop.right)
        return result;
    auto content = source.crop(crop);
    size_t dark = 0;
    int low = 255, high = 0;
    for (size_t i = 0; i < content.pixels.size(); i += 4)
    {
        int gray = luminance(content.pixels.data() + i);
        dark += gray < 128;
        low = std::min(low, gray);
        high = std::max(high, gray);
    }
    bool invert = dark * 4 > content.pixels.size() / 4 * 3;
    bool stretch = high - low >= 32 && high - low < 176;
    Image padded(content.width + Padding * 2, content.height + Padding * 2);
    for (int y = 0; y < content.height; ++y)
        for (int x = 0; x < content.width; ++x)
        {
            auto src = content.pixels.data() + y * content.stride() + x * 4;
            auto dst = padded.pixels.data() + (y + Padding) * padded.stride() + (x + Padding) * 4;
            for (int channel = 0; channel < 3; ++channel)
            {
                int value = stretch ? (luminance(src) - low) * 255 / (high - low) : src[channel];
                dst[channel] = static_cast<uint8_t>(invert ? 255 - value : value);
            }
        }
    if (scale == 1)
        result.image = std::move(padded);
    else if (stretch || !cubic)
        result.image = padded.resized(padded.width * scale, padded.height * scale);
    else
        result.image = enlargeForOcr(padded, scale);
    return result;
}
std::vector<RecognizedLine> readPass(OcrEngine const &engine, Prepared const &input, OcrRequest const &request,
                                     OcrSnapshot &snapshot)
{
    std::vector<RecognizedLine> lines;
    if (input.image.empty() || cancelled(request))
        return lines;
    Buffer buffer(static_cast<uint32_t>(input.image.pixels.size()));
    buffer.Length(static_cast<uint32_t>(input.image.pixels.size()));
    BYTE *bytes{};
    check(buffer.as<BufferAccess>()->Buffer(&bytes));
    std::memcpy(bytes, input.image.pixels.data(), input.image.pixels.size());
    SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, input.image.width, input.image.height, BitmapAlphaMode::Ignore);
    bitmap.CopyFromBuffer(buffer);
    auto result = engine.RecognizeAsync(bitmap).get();
    ++snapshot.passCount;
    float angle = result.TextAngle() ? static_cast<float>(result.TextAngle().Value()) : 0.f;
    Point localCenter{input.image.width / (2.f * input.scale), input.image.height / (2.f * input.scale)};
    Point globalCenter{snapshot.width / 2.f, snapshot.height / 2.f};
    for (auto const &line : result.Lines())
    {
        auto tokens = lineTokens(line, static_cast<float>(input.scale), angle, 0);
        std::erase_if(tokens,
                      [&](auto &token)
                      {
                          auto physical = rotate({token.box.x + token.box.w / 2, token.box.y + token.box.h / 2},
                                                 localCenter, angle);
                          physical.x += input.crop.left - Padding;
                          physical.y += input.crop.top - Padding;
                          auto center = rotate(physical, globalCenter, -angle);
                          token.box.x = center.x - token.box.w / 2;
                          token.box.y = center.y - token.box.h / 2;
                          return physical.x < input.owner.left || physical.x >= input.owner.right ||
                                 physical.y < input.owner.top || physical.y >= input.owner.bottom;
                      });
        if (!tokens.empty())
        {
            auto bounds = physicalBounds(snapshot, tokens);
            lines.push_back({std::move(tokens), bounds});
        }
    }
    return lines;
}
float overlap(Box a, Box b)
{
    float width = std::max(0.f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
    float height = std::max(0.f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    return width * height / std::max(1.f, std::min(a.w * a.h, b.w * b.h));
}
bool hasHan(RecognizedLine const &line)
{
    return std::any_of(line.tokens.begin(), line.tokens.end(),
                       [](auto const &token) { return std::any_of(token.text.begin(), token.text.end(), han); });
}
std::vector<RecognizedLine> mergeLanguages(std::vector<RecognizedLine> primary, std::vector<RecognizedLine> english)
{
    std::vector<bool> used(english.size());
    for (auto &line : primary)
    {
        if (hasHan(line))
            continue;
        int best = -1;
        float score = 0.f;
        for (size_t i = 0; i < english.size(); ++i)
        {
            float match = overlap(line.bounds, english[i].bounds);
            float distance =
                std::abs(line.bounds.y + line.bounds.h / 2 - english[i].bounds.y - english[i].bounds.h / 2);
            if (!used[i] && match > .35f && distance < std::max(line.bounds.h, english[i].bounds.h) * .6f &&
                match > score)
            {
                best = static_cast<int>(i);
                score = match;
            }
        }
        if (best >= 0)
        {
            line = std::move(english[best]);
            used[best] = true;
        }
    }
    // A secondary recognizer must be allowed to recover entire missing lines.
    // Reject candidates overlapping Chinese content, where English OCR can hallucinate Latin letters.
    for (size_t i = 0; i < english.size(); ++i)
    {
        if (used[i])
            continue;
        auto &candidate = english[i];
        int letters = 0;
        for (auto const &token : candidate.tokens)
            for (auto c : token.text)
                letters += c < 128 && iswalnum(c);
        if (letters < 2 || hasHan(candidate))
            continue;
        bool covered = std::any_of(primary.begin(), primary.end(),
                                   [&](auto const &line) { return overlap(line.bounds, candidate.bounds) > .2f; });
        if (!covered)
            primary.push_back(std::move(candidate));
    }
    return primary;
}
void assemble(OcrSnapshot &snapshot, std::vector<RecognizedLine> lines, OcrRequest const &request)
{
    std::sort(lines.begin(), lines.end(), [](auto const &a, auto const &b) { return a.bounds.y < b.bounds.y; });
    std::vector<std::vector<RecognizedLine>> rows;
    for (auto &line : lines)
    {
        auto row = std::find_if(rows.rbegin(), rows.rend(),
                                [&](auto const &candidate)
                                {
                                    auto a = candidate.front().bounds, b = line.bounds;
                                    return std::abs(a.y + a.h / 2 - b.y - b.h / 2) < std::min(a.h, b.h) * .5f;
                                });
        if (row == rows.rend())
            rows.push_back({std::move(line)});
        else
            row->push_back(std::move(line));
    }
    int lineIndex = -1;
    for (auto &row : rows)
    {
        const auto rowStart = snapshot.tokens.size();
        std::sort(row.begin(), row.end(), [](auto const &a, auto const &b) { return a.bounds.x < b.bounds.x; });
        Box previous{};
        bool first = true;
        for (auto &line : row)
        {
            if (first || line.bounds.x - previous.x - previous.w > std::max(24.f, line.bounds.h * 2.f))
                ++lineIndex;
            first = false;
            previous = line.bounds;
            std::sort(line.tokens.begin(), line.tokens.end(),
                      [](auto const &a, auto const &b) { return a.box.x < b.box.x; });
            for (auto &token : line.tokens)
            {
                auto bounds = physicalBounds(snapshot, std::span(&token, 1));
                if (std::any_of(request.redactions.begin(), request.redactions.end(),
                                [&](auto box) { return box.intersects(bounds); }))
                    continue;
                bool duplicate =
                    std::any_of(snapshot.tokens.begin() + rowStart, snapshot.tokens.end(),
                                [&](auto const &prev)
                                {
                                    return prev.text == token.text &&
                                           overlap(bounds, physicalBounds(snapshot, std::span(&prev, 1))) > .65f;
                                });
                if (!duplicate)
                {
                    token.line = lineIndex;
                    snapshot.tokens.push_back(std::move(token));
                }
            }
        }
    }
}
} // namespace
std::vector<std::wstring> ocrLanguages()
{
    std::vector<std::wstring> result;
    for (auto const &lang : OcrEngine::AvailableRecognizerLanguages())
        result.emplace_back(lang.LanguageTag());
    return result;
}
std::shared_ptr<OcrSnapshot> recognize(OcrRequest const &request, bool requireIdentity)
{
    auto out = std::make_shared<OcrSnapshot>();
    out->revision = request.revision;
    out->epoch = request.epoch;
    if (!request.image || request.image->empty())
    {
        out->status = L"没有可识别的图片。";
        return out;
    }
    out->width = request.image->width;
    out->height = request.image->height;
    auto start = Clock::now();
    if (cancelled(request))
    {
        out->status = L"已取消识别。";
        return out;
    }
    try
    {
        if (requireIdentity && packageIdentity().empty())
        {
            out->status = L"OCR 身份尚未就绪，请在托盘设置中选择“修复 OCR 身份…”，完成后重启软件。";
            return out;
        }
        OcrEngine engine{nullptr};
        if (!request.language.empty() && OcrEngine::IsLanguageSupported(Language(request.language)))
            engine = OcrEngine::TryCreateFromLanguage(Language(request.language));
        if (!engine && OcrEngine::IsLanguageSupported(Language(L"zh-Hans-CN")))
            engine = OcrEngine::TryCreateFromLanguage(Language(L"zh-Hans-CN"));
        if (!engine)
            engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine)
        {
            out->status = L"没有可用的离线识别语言，请在 Windows 语言设置中添加中文或英文 OCR。";
            return out;
        }
        out->language = engine.RecognizerLanguage().LanguageTag();
        OcrEngine english{nullptr};
        if (request.language.empty() && out->language.starts_with(L"zh") &&
            OcrEngine::IsLanguageSupported(Language(L"en-US")))
        {
            try
            {
                english = OcrEngine::TryCreateFromLanguage(Language(L"en-US"));
                if (english)
                    out->language += L" + en-US";
            }
            catch (...)
            { /* The primary recognizer remains available. */
            }
        }
        const int limit = static_cast<int>(OcrEngine::MaxImageDimension());
        RECT full{0, 0, out->width, out->height};
        std::vector<RecognizedLine> primary;
        std::optional<Prepared> initial;
        bool fits = std::max(out->width, out->height) <= limit - Padding * 2 &&
                    static_cast<uint64_t>(out->width) * out->height <= 16000000;
        if (fits && !cancelled(request))
        {
            initial = prepare(*request.image, full, full, 1);
            primary = readPass(engine, *initial, request, *out);
        }
        std::vector<float> heights;
        for (auto const &line : primary)
            for (auto const &token : line.tokens)
                if (token.box.h >= 4)
                    heights.push_back(token.box.h);
        float median = 0;
        if (!heights.empty())
        {
            auto middle = heights.begin() + heights.size() / 2;
            std::nth_element(heights.begin(), middle, heights.end());
            median = *middle;
        }
        bool refine = request.forceEnhance || !fits || (!heights.empty() && median < 18.f);
        out->sourceTextHeight = median;
        // Empty non-uniform pictures can contain tiny text missed by the first pass.
        if (fits && primary.empty() && initial && !initial->image.empty())
            refine = true;
        std::vector<RecognizedLine> lines;
        auto readRegion = [&](Prepared const &input)
        {
            auto native = readPass(engine, input, request, *out);
            if (english && !cancelled(request))
            {
                try
                {
                    auto secondary = readPass(english, input, request, *out);
                    native = mergeLanguages(std::move(native), std::move(secondary));
                }
                catch (...)
                { /* A failed secondary provider must not remove the primary result. */
                }
            }
            return native;
        };
        if (refine && !cancelled(request))
        {
            out->enhanced = true;
            constexpr int scale = 2;
            int edge = std::min(2048, limit / scale - 2 * (Padding + Context));
            if (edge < 1)
                throw std::runtime_error("OCR image limit is too small.");
            for (int y = 0; y < out->height && !cancelled(request); y += edge)
                for (int x = 0; x < out->width && !cancelled(request); x += edge)
                {
                    RECT owner{x, y, std::min(out->width, x + edge), std::min(out->height, y + edge)};
                    RECT crop{std::max(0, x - Context), std::max(0, y - Context),
                              std::min(out->width, x + edge + Context), std::min(out->height, y + edge + Context)};
                    auto input = prepare(*request.image, crop, owner, scale, median == 0 || median >= 12.f);
                    if (!input.image.empty())
                    {
                        ++out->tileCount;
                        auto region = readRegion(input);
                        lines.insert(lines.end(), std::make_move_iterator(region.begin()),
                                     std::make_move_iterator(region.end()));
                    }
                }
            // Preserve a native line only when the refined pass found no corresponding content.
            for (auto &line : primary)
            {
                bool covered = std::any_of(lines.begin(), lines.end(), [&](auto const &candidate)
                                           { return overlap(line.bounds, candidate.bounds) > .2f; });
                if (!covered)
                    lines.push_back(std::move(line));
            }
        }
        else if (initial && !cancelled(request))
        {
            lines = std::move(primary);
            if (english)
            {
                try
                {
                    auto secondary = readPass(english, *initial, request, *out);
                    lines = mergeLanguages(std::move(lines), std::move(secondary));
                }
                catch (...)
                { /* Keep the primary result. */
                }
            }
        }
        if (!cancelled(request))
            assemble(*out, std::move(lines), request);
        out->elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        out->status = out->tokens.empty() ? L"未找到可选文字，可更换识别语言后重试。"
                                          : L"拖选文字，Ctrl+C 复制 · " + out->language;
    }
    catch (...)
    {
        out->status = L"识别失败：" + errorText();
    }
    return out;
}
OcrService::OcrService(HWND notify) : notify_(notify), thread_([this] { run(); }) {}
OcrService::~OcrService()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        if (activeCancel_)
            activeCancel_->store(true);
        pending_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}
void OcrService::submit(OcrRequest request)
{
    if (!request.cancelled)
        request.cancelled = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard lock(mutex_);
        if (activeWindow_ == request.window && activeCancel_)
            activeCancel_->store(true);
        std::erase_if(pending_, [&](auto const &pending) { return pending.window == request.window; });
        pending_.push_back(std::move(request));
    }
    cv_.notify_one();
}
void OcrService::cancel(uint64_t window)
{
    std::lock_guard lock(mutex_);
    if (activeWindow_ == window && activeCancel_)
        activeCancel_->store(true);
    std::erase_if(pending_, [&](auto const &request) { return request.window == window; });
}
std::vector<OcrCompletion> OcrService::take()
{
    std::lock_guard lock(mutex_);
    std::vector<OcrCompletion> result;
    while (!completed_.empty())
    {
        result.push_back(std::move(completed_.front()));
        completed_.pop_front();
    }
    return result;
}
void OcrService::run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    for (;;)
    {
        OcrRequest request;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
            if (stopping_)
                break;
            request = std::move(pending_.front());
            pending_.pop_front();
            activeWindow_ = request.window;
            activeCancel_ = request.cancelled;
        }
        auto result = recognize(request);
        {
            std::lock_guard lock(mutex_);
            if (stopping_)
                break;
            activeWindow_ = 0;
            activeCancel_.reset();
            if (cancelled(request))
                continue;
            completed_.push_back({request.window, std::move(result)});
        }
        PostMessage(notify_, WM_OCR_READY, 0, 0);
    }
    winrt::uninit_apartment();
}
} // namespace snap
