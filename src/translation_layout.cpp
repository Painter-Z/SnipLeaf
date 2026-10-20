#include "translation.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
namespace snap
{
namespace
{
std::wstring trimmed(std::wstring const &text)
{
    auto start = text.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return {};
    return text.substr(start, text.find_last_not_of(L" \t\r\n") - start + 1);
}
bool listStart(std::wstring const &text)
{
    if (text.empty()) return false;
    if (text.front() == L'•' || text.front() == L'·' || text.front() == L'‣') return true;
    if (text.size() > 1 && (text.front() == L'-' || text.front() == L'*' || text.front() == L'+') && iswspace(text[1])) return true;
    size_t at = text.front() == L'(' || text.front() == L'[' ? 1 : 0;
    auto start = at;
    while (at < text.size() && iswdigit(text[at])) ++at;
    if (at == start && at < text.size() && ((text[at] >= L'A' && text[at] <= L'Z') || (text[at] >= L'a' && text[at] <= L'z'))) ++at;
    return at > start && at + 1 < text.size() && (text[at] == L'.' || text[at] == L')' || text[at] == L']') && iswspace(text[at + 1]);
}
} // namespace
std::vector<TranslationRow> translationRows(OcrSnapshot const &snapshot, int first, int last)
{
    if (snapshot.tokens.empty()) return {};
    if (first < 0 || last < 0) { first = 0; last = static_cast<int>(snapshot.tokens.size()) - 1; }
    if (first > last) std::swap(first, last);
    first = std::clamp(first, 0, static_cast<int>(snapshot.tokens.size()) - 1);
    last = std::clamp(last, first, static_cast<int>(snapshot.tokens.size()) - 1);
    std::vector<TranslationRow> rows;
    Box previous{};
    for (int at = first; at <= last;)
    {
        int end = at;
        while (end < last && snapshot.tokens[end + 1].line == snapshot.tokens[at].line) ++end;
        auto text = trimmed(snapshot.selection(at, end));
        if (!text.empty())
        {
            Box bounds = snapshot.tokens[at].box;
            for (int i = at + 1; i <= end; ++i)
            {
                auto box = snapshot.tokens[i].box;
                float right = std::max(bounds.x + bounds.w, box.x + box.w), bottom = std::max(bounds.y + bounds.h, box.y + box.h);
                bounds.x = std::min(bounds.x, box.x); bounds.y = std::min(bounds.y, box.y);
                bounds.w = right - bounds.x; bounds.h = bottom - bounds.y;
            }
            bool paragraph = !rows.empty() && (listStart(text) || bounds.y < previous.y ||
                bounds.y - previous.y > std::max(1.f, std::max(previous.h, bounds.h)) * 1.8f);
            rows.push_back({std::move(text), {}, snapshot.quad(bounds), paragraph, false});
            previous = bounds;
        }
        at = end + 1;
    }
    return rows;
}
std::vector<TranslationRow> translationRows(std::wstring const &text)
{
    std::vector<TranslationRow> rows;
    bool paragraph{};
    for (size_t at = 0; at < text.size();)
    {
        auto end = text.find_first_of(L"\r\n", at);
        if (end == std::wstring::npos) end = text.size();
        auto line = trimmed(text.substr(at, end - at));
        if (line.empty()) paragraph = !rows.empty();
        else
        {
            bool startsParagraph = !rows.empty() && (paragraph || listStart(line));
            rows.push_back({std::move(line), {}, {}, startsParagraph, false}); paragraph = false;
        }
        at = end;
        if (at < text.size() && text[at] == L'\r') ++at;
        if (at < text.size() && text[at] == L'\n') ++at;
    }
    return rows;
}
std::vector<TranslationRow> translationParagraphs(std::vector<TranslationRow> const &rows)
{
    std::vector<TranslationRow> paragraphs;
    for (auto const &row : rows)
    {
        auto text = trimmed(row.source);
        if (text.empty()) continue;
        if (paragraphs.empty() || row.paragraphBefore || listStart(text))
            paragraphs.push_back({std::move(text), {}, {}, !paragraphs.empty(), false});
        else
        {
            auto &paragraph = paragraphs.back().source;
            // Undo a word hyphenated only by screenshot wrapping, preserving
            // the original OCR text shown in the separate source editor.
            if (paragraph.back() == L'-' && text.front() >= L'a' && text.front() <= L'z') paragraph.pop_back();
            else paragraph += L' ';
            paragraph += text;
        }
    }
    return paragraphs;
}
std::wstring translationText(std::vector<TranslationRow> const &rows, bool translated)
{
    std::wstring text;
    for (auto const &row : rows)
    {
        auto const &part = translated ? row.translated : row.source;
        if (part.empty() || (translated && !row.complete)) continue;
        if (!text.empty()) text += row.paragraphBefore ? L"\r\n\r\n" : L"\r\n";
        text += part;
    }
    return text;
}
std::vector<std::wstring> alignedTranslationResponse(unsigned status, std::string const &body,
    std::vector<std::wstring> const &sources, std::wstring &error)
{
    error.clear();
    auto result = parseTranslationResponse(status, body);
    if (!result.ok()) { error = result.error; return {}; }
    try
    {
        auto object = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
        auto items = object.GetNamedArray(L"trans_result");
        if (items.Size() != sources.size()) throw std::runtime_error("Ambiguous translation rows.");
        std::vector<std::wstring> translated(sources.size());
        for (auto const &value : items)
        {
            auto item = value.GetObject();
            auto source = trimmed(std::wstring(item.GetNamedString(L"src")));
            size_t index = sources.size();
            for (size_t i = 0; i < sources.size(); ++i)
                if (translated[i].empty() && trimmed(sources[i]) == source) { index = i; break; }
            if (index == sources.size()) throw std::runtime_error("Translation source mismatch.");
            auto target = trimmed(std::wstring(item.GetNamedString(L"dst")));
            if (target.empty()) throw std::runtime_error("Missing translation row.");
            translated[index] = std::move(target);
        }
        return translated;
    }
    catch (...) { error = L"翻译服务返回的段落不完整，请缩小选区后重试。"; return {}; }
}
} // namespace snap
