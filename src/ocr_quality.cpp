#include "ocr.h"
#include <winrt/Windows.Media.Ocr.h>
namespace snap
{
std::string json(std::wstring const &);
void writeReport(std::wstring const &, std::string const &);
namespace
{
struct TruthLine
{
    Box box;
    std::wstring text;
};
std::wstring compact(std::wstring const &text)
{
    std::wstring result;
    for (auto c : text)
        if (!iswspace(c))
            result += c;
    return result;
}
size_t distance(std::wstring const &a, std::wstring const &b)
{
    std::vector<size_t> row(b.size() + 1);
    std::iota(row.begin(), row.end(), 0);
    for (size_t i = 0; i < a.size(); ++i)
    {
        auto diagonal = row[0];
        row[0] = i + 1;
        for (size_t j = 0; j < b.size(); ++j)
        {
            auto above = row[j + 1];
            row[j + 1] = std::min({above + 1, row[j] + 1, diagonal + (a[i] != b[j])});
            diagonal = above;
        }
    }
    return row.back();
}
} // namespace
int ocrQualityTest(Graphics &g, std::wstring const &report)
{
    struct Case
    {
        wchar_t const *name;
        int width, height, font, rows, columns;
        unsigned background, foreground;
        bool mixed;
    };
    const Case cases[] = {
        {L"normal", 1080, 520, 25, 10, 1, 0xffffff, 0x202b38, true},
        {L"dense-chinese", 1460, 980, 14, 36, 1, 0xffffff, 0x253040, false},
        {L"dense-mixed", 1460, 980, 14, 36, 1, 0xffffff, 0x253040, true},
        {L"tiny-chinese", 1300, 500, 11, 22, 1, 0xffffff, 0x253040, false},
        {L"dark-dense", 1460, 980, 14, 36, 1, 0x20242a, 0xdce1e8, true},
        {L"low-contrast", 1200, 760, 13, 30, 1, 0xf8f9fa, 0x8a939f, true},
        {L"wide-desktop", 6200, 900, 14, 34, 3, 0xffffff, 0x253040, true},
        {L"long-document", 1400, 5400, 14, 180, 1, 0xffffff, 0x253040, true},
    };
    const std::array<std::wstring, 4> chinese = {L"截图完成后可以直接拖选文字，复制到其他应用中继续编辑。",
                                                 L"密集文本需要保留原始像素，分块识别时注意边界和重复内容。",
                                                 L"中文、英文、数字和标点都需要准确识别，贴图移动后仍可选字。",
                                                 L"工具保持轻量，离线处理图片，改善小字号和深色背景的阅读体验。"};
    const std::array<std::wstring, 3> english = {
        L"Select words on this image and copy them. Keep every original pixel.",
        L"Windows OCR supports offline text recognition. Version 2026, total 128.",
        L"Dense paragraphs need clear characters, correct order and stable selection."};
    auto folder = std::filesystem::path(report).parent_path();
    std::filesystem::create_directories(folder);
    std::string results = "[";
    size_t totalCharacters = 0, totalErrors = 0, missingLines = 0, totalLines = 0;
    bool hitCorrect = true, valid = !packageIdentity().empty();
    for (auto const &c : cases)
    {
        Dib dib(Image(c.width, c.height, c.background));
        auto font = CreateFontW(-c.font, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        if (!font)
            throw std::runtime_error("Cannot create OCR corpus font.");
        auto old = SelectObject(dib.dc, font);
        SetBkMode(dib.dc, TRANSPARENT);
        SetTextColor(dib.dc, RGB((c.foreground >> 16) & 255, (c.foreground >> 8) & 255, c.foreground & 255));
        std::vector<TruthLine> truth;
        for (int column = 0; column < c.columns; ++column)
            for (int row = 0; row < c.rows; ++row)
            {
                auto text = c.mixed && row % 3 == 1 ? english[row % english.size()] : chinese[row % chinese.size()];
                int x = 24 + column * (c.width / c.columns), y = 20 + row * ((c.height - 40) / c.rows);
                SIZE size{};
                GetTextExtentPoint32W(dib.dc, text.c_str(), static_cast<int>(text.size()), &size);
                TextOutW(dib.dc, x, y, text.c_str(), static_cast<int>(text.size()));
                truth.push_back({{static_cast<float>(x), static_cast<float>(y), static_cast<float>(size.cx),
                                  static_cast<float>(size.cy)},
                                 text});
            }
        SelectObject(dib.dc, old);
        DeleteObject(font);
        auto image = std::make_shared<Image>(dib.image());
        uint64_t hash = 14695981039346656037ULL;
        for (auto byte : image->pixels)
            hash = (hash ^ byte) * 1099511628211ULL;
        savePng(g, *image, (folder / (std::wstring(L"ocr-quality-") + c.name + L".local.png")).wstring());
        auto snapshot = recognize({1, 1, image, {}, L""});
        valid = valid && !snapshot->tokens.empty();
        std::vector<std::vector<std::pair<float, std::wstring>>> found(truth.size());
        size_t extra = 0;
        for (auto const &token : snapshot->tokens)
        {
            auto quad = snapshot->quad(token);
            Point center{};
            for (auto p : quad)
            {
                center.x += p.x / 4;
                center.y += p.y / 4;
            }
            hitCorrect = hitCorrect && snapshot->hit(center) >= 0;
            int closest = -1;
            float best = std::numeric_limits<float>::max();
            for (size_t i = 0; i < truth.size(); ++i)
            {
                auto box = truth[i].box;
                if (center.x < box.x - 4 || center.x > box.x + box.w + 4 || center.y < box.y - 4 ||
                    center.y > box.y + box.h + 4)
                    continue;
                float score = std::abs(center.y - box.y - box.h / 2);
                if (score < best)
                {
                    best = score;
                    closest = static_cast<int>(i);
                }
            }
            if (closest >= 0)
                found[closest].push_back({center.x, token.text});
            else
                extra += compact(token.text).size();
        }
        size_t characters = 0, errors = extra, missing = 0;
        std::string lines = "[";
        for (size_t i = 0; i < truth.size(); ++i)
        {
            std::sort(found[i].begin(), found[i].end());
            std::wstring recognized;
            for (auto const &[x, text] : found[i])
                recognized += text;
            auto expected = compact(truth[i].text), actual = compact(recognized);
            characters += expected.size();
            errors += distance(expected, actual);
            missing += actual.empty();
            if (i)
                lines += ",";
            lines += "{\"expected\":" + json(truth[i].text) + ",\"actual\":" + json(recognized) + "}";
        }
        lines += "]";
        if (totalLines)
            results += ",";
        results += "{\"name\":" + json(c.name) + ",\"width\":" + std::to_string(c.width) +
                   ",\"height\":" + std::to_string(c.height) + ",\"fontPixels\":" + std::to_string(c.font) +
                   ",\"pixelHashFNV1a64\":\"" + std::to_string(hash) +
                   "\",\"characters\":" + std::to_string(characters) + ",\"errors\":" + std::to_string(errors) +
                   ",\"cer\":" + std::to_string(static_cast<double>(errors) / characters) +
                   ",\"missingLines\":" + std::to_string(missing) + ",\"lineCount\":" + std::to_string(truth.size()) +
                   ",\"elapsedMs\":" + std::to_string(snapshot->elapsedMs) + ",\"status\":" + json(snapshot->status) +
                   ",\"passes\":" + std::to_string(snapshot->passCount) +
                   ",\"tiles\":" + std::to_string(snapshot->tileCount) +
                   ",\"enhanced\":" + (snapshot->enhanced ? "true" : "false") +
                   ",\"sourceTextHeight\":" + std::to_string(snapshot->sourceTextHeight) + ",\"lines\":" + lines + "}";
        totalCharacters += characters;
        totalErrors += errors;
        totalLines += truth.size();
        missingLines += missing;
    }
    results += "]";
    writeReport(
        report,
        "{\"suite\":\"ocr-quality\",\"valid\":" + std::string(valid ? "true" : "false") +
            ",\"corpus\":\"generated-gdi-v1\",\"maxImageDimension\":" +
            std::to_string(winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension()) +
            ",\"characters\":" + std::to_string(totalCharacters) + ",\"errors\":" + std::to_string(totalErrors) +
            ",\"cer\":" + std::to_string(static_cast<double>(totalErrors) / totalCharacters) +
            ",\"missingLines\":" + std::to_string(missingLines) + ",\"lineCount\":" + std::to_string(totalLines) +
            ",\"tokenCentersSelectable\":" + (hitCorrect ? "true" : "false") + ",\"cases\":" + results + "}");
    return valid && hitCorrect ? 0 : 1;
}
} // namespace snap
