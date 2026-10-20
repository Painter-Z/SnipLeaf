#include "app.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
namespace snap
{
std::string json(std::wstring const &);
void writeReport(std::wstring const &, std::string const &);
void require(bool, char const *);
namespace
{
TranslationCredentials exampleCredentials()
{
    return {L"2015063000000001", L"12345678", true};
}
std::wstring requestText(std::string const &form)
{
    auto start = form.find("q=");
    if (start == std::string::npos) return L"sample";
    start += 2; auto end = form.find('&', start);
    auto encoded = form.substr(start, end == std::string::npos ? end : end - start);
    std::string decoded;
    for (size_t i = 0; i < encoded.size(); ++i)
        if (encoded[i] == '%' && i + 2 < encoded.size()) { decoded += static_cast<char>(std::stoi(encoded.substr(i + 1, 2), nullptr, 16)); i += 2; }
        else decoded += encoded[i] == '+' ? ' ' : encoded[i];
    return std::wstring(winrt::to_hstring(decoded));
}
TranslationHttpResponse translatedResponse(std::string const &form = {})
{
    using namespace winrt::Windows::Data::Json;
    JsonObject object; JsonArray items;
    for (auto const &row : translationRows(requestText(form)))
    {
        JsonObject item;
        item.Insert(L"src", JsonValue::CreateStringValue(row.source));
        item.Insert(L"dst", JsonValue::CreateStringValue(L"测试译文"));
        items.Append(item);
    }
    object.Insert(L"from", JsonValue::CreateStringValue(L"en")); object.Insert(L"to", JsonValue::CreateStringValue(L"zh"));
    object.Insert(L"trans_result", items);
    return {200, winrt::to_string(object.Stringify())};
}
std::wstring windowText(HWND window)
{
    int n = GetWindowTextLength(window);
    std::wstring result(static_cast<size_t>(n) + 1, L'\0');
    GetWindowText(window, result.data(), n + 1);
    result.resize(n);
    return result;
}
OcrSnapshot sampleSnapshot()
{
    OcrSnapshot snapshot;
    snapshot.revision = 1;
    snapshot.width = 480; snapshot.height = 240;
    snapshot.tokens = {{L"This", {20, 20, 30, 14}, 0}, {L"is", {60, 20, 12, 14}, 0},
                       {L"a", {82, 20, 8, 14}, 0}, {L"simple", {20, 40, 40, 14}, 1},
                       {L"test.", {70, 40, 30, 14}, 1}};
    return snapshot;
}
} // namespace
int App::translationTests(std::wstring const &report)
{
    int passed{}, failed{};
    std::string cases = "[";
    auto folder = std::filesystem::path(report).parent_path();
    std::filesystem::create_directories(folder);
    auto file = [&](wchar_t const *name) { return folder / name; };
    HWND previousForeground = GetForegroundWindow();
    auto previousCopy = copyText_;
    std::wstring copied;
    copyText_ = [&](HWND, std::wstring const &text) { copied = text; return true; };
    auto pump = [&]
    {
        MSG message{};
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
            if (message.message != WM_QUIT && !preTranslate(message))
            { TranslateMessage(&message); DispatchMessage(&message); }
    };
    auto until = [&](auto condition, int timeout = 3000)
    {
        auto end = Clock::now() + std::chrono::milliseconds(timeout);
        while (!condition() && Clock::now() < end) { pump(); Sleep(5); }
        pump();
        require(condition(), "Translation operation did not finish within its test deadline.");
    };
    auto test = [&](wchar_t const *name, auto body)
    {
        if (passed + failed) cases += ",";
        cases += "{\"name\":" + json(name);
        try { body(); ++passed; cases += ",\"passed\":true}"; }
        catch (...) { ++failed; cases += ",\"passed\":false,\"error\":" + json(errorText()) + "}"; }
    };
    test(L"缺少凭据时默认关闭联网翻译", [&]
    { TranslationCredentials empty; require(!empty.valid() && !empty.enabled, "Translation enabled without credentials."); });
    test(L"凭据校验拒绝错误 APPID、空格和控制字符", [&]
    {
        auto credentials = exampleCredentials(); require(credentials.valid(), "Example rejected.");
        credentials.appId = L"invalid"; require(!credentials.valid(), "Invalid APPID accepted.");
        credentials = exampleCredentials(); credentials.secret += L"\r\n";
        require(!credentials.valid(), "Header/control injection accepted.");
    });
    test(L"用户凭据以 DPAPI 加密保存并可完整读取", [&]
    {
        TranslationCredentialStore store(file(L"credential-test.local.dat"));
        store.clear(); require(!store.load().valid(), "Missing store returned credentials.");
        auto credentials = exampleCredentials(); store.save(credentials);
        auto loaded = store.load();
        require(loaded.enabled && loaded.appId == credentials.appId && loaded.secret == credentials.secret, "Credential round trip failed.");
        std::ifstream in(file(L"credential-test.local.dat"), std::ios::binary);
        std::string raw(std::istreambuf_iterator<char>(in), {});
        in.close();
        require(raw.find(winrt::to_string(credentials.secret)) == std::string::npos &&
                raw.find(winrt::to_string(credentials.appId)) == std::string::npos, "Credentials persisted as plaintext.");
        credentials.enabled = false; store.save(credentials); require(!store.load().enabled, "Consent was lost.");
    });
    test(L"损坏的加密凭据不能解密或静默启用翻译", [&]
    {
        auto path = file(L"damaged-credential.local.dat");
        std::ofstream out(path, std::ios::binary); out << "not-encrypted"; out.close();
        bool threw{}; try { TranslationCredentialStore(path).load(); } catch (...) { threw = true; }
        require(threw, "Damaged credentials accepted.");
    });
    test(L"清除凭据只删除专用文件", [&]
    {
        auto path = file(L"credential-test.local.dat");
        TranslationCredentialStore(path).clear();
        require(!std::filesystem::exists(path) && std::filesystem::exists(file(L"damaged-credential.local.dat")), "Clear touched unrelated files.");
    });
    test(L"相邻英文行保留截图换行而不合并", [&]
    { require(translationText(sampleSnapshot()) == L"This is a\r\nsimple test.", "Screenshot line breaks were lost."); });
    test(L"拖选翻译仅提取选中部分并支持反向选择", [&]
    { auto snapshot = sampleSnapshot(); require(translationText(snapshot, 1, 2) == L"is a" && translationText(snapshot, 2, 1) == L"is a", "Selection leaked unselected text."); });
    test(L"大段间距与列表保留段落结构", [&]
    {
        auto snapshot = sampleSnapshot(); snapshot.tokens[3].box.y = snapshot.tokens[4].box.y = 80;
        require(translationText(snapshot).find(L"\r\n\r\n") != std::wstring::npos, "Paragraph gap lost.");
        snapshot.tokens[3].box.y = 40; snapshot.tokens[4].box.y = 40; snapshot.tokens[3].text = L"•";
        require(translationText(snapshot).find(L"\r\n•") != std::wstring::npos, "List line break lost.");
    });
    test(L"英文断行连字符与原行位置一起保留", [&]
    {
        auto snapshot = sampleSnapshot(); snapshot.tokens = {{L"trans-", {20, 20, 40, 14}, 0}, {L"lation", {20, 40, 30, 14}, 1}};
        require(translationText(snapshot) == L"trans-\r\nlation", "Physical lines were merged.");
    });
    test(L"空识别结果不会生成翻译文字", [&]
    { require(translationText(OcrSnapshot{}).empty(), "Empty OCR produced text."); });
    test(L"OCR 行保留范围与旋转坐标以定位原图", [&]
    {
        auto snapshot = sampleSnapshot(); snapshot.angle = 12;
        auto rows = translationRows(snapshot, 1, 4);
        require(rows.size() == 2 && rows[0].source == L"is a" && rows[0].quad.size() == 4, "OCR row selection was flattened.");
        auto expected = snapshot.quad(Box{60, 20, 30, 14});
        require(std::abs(rows[0].quad[0].x - expected[0].x) < .01f && std::abs(rows[0].quad[0].y - expected[0].y) < .01f, "Original screenshot coordinates lost.");
    });
    test(L"文本 CRLF、LF、空段和列表生成固定对应行", [&]
    {
        auto rows = translationRows(L"First.\r\nSecond.\n\n1. Item\r\n2. Item");
        require(rows.size() == 4 && rows[2].paragraphBefore && rows[3].paragraphBefore, "Edited line structure lost.");
        for (auto &row : rows) { row.translated = L"中文"; row.complete = true; }
        require(translationText(rows, true) == L"中文\r\n中文\r\n\r\n中文\r\n\r\n中文", "Copied translation lost row/paragraph breaks.");
    });
    test(L"多行译文按服务回显原文匹配，支持顺序变化与重复行", [&]
    {
        std::wstring error;
        auto translated = alignedTranslationResponse(200,
            R"({"to":"zh","trans_result":[{"src":"two","dst":"second"},{"src":"one","dst":"first"}]})", {L"one", L"two"}, error);
        require(error.empty() && translated.size() == 2 && translated[0] == L"first" && translated[1] == L"second", "Reordered response mismatched rows.");
        translated = alignedTranslationResponse(200,
            R"({"to":"zh","trans_result":[{"src":"same","dst":"first"},{"src":"same","dst":"second"}]})", {L"same", L"same"}, error);
        require(error.empty() && translated[1] == L"second", "Repeated source consumed twice.");
    });
    test(L"缺行、错误原文与合并响应不能静默错配中文", [&]
    {
        for (auto body : {R"({"to":"zh","trans_result":[{"src":"one two","dst":"merged"}]})",
             R"({"to":"zh","trans_result":[{"src":"one","dst":"first"},{"src":"wrong","dst":"second"}]})",
             R"({"to":"zh","trans_result":[{"dst":"first"},{"dst":"second"}]})"})
        {
            std::wstring error; auto result = alignedTranslationResponse(200, body, {L"one", L"two"}, error);
            require(result.empty() && !error.empty(), "Ambiguous response was displayed with wrong alignment.");
        }
    });
    test(L"同段英文合并后翻译而原文框保留 OCR 换行", [&]
    {
        auto rows = translationRows(sampleSnapshot()); auto paragraphs = translationParagraphs(rows);
        require(paragraphs.size() == 1 && paragraphs[0].source == L"This is a simple test." &&
                translationText(rows) == L"This is a\r\nsimple test.", "Reading paragraphs or original OCR line breaks lost.");
    });
    test(L"多段中文用空行分隔，段内不按截图行强制换行", [&]
    {
        auto paragraphs = translationParagraphs(translationRows(L"First line\ncontinued.\n\nSecond paragraph."));
        require(paragraphs.size() == 2 && paragraphs[1].paragraphBefore, "Paragraph boundaries were lost.");
        paragraphs[0].translated = L"第一段中文连续阅读。"; paragraphs[1].translated = L"第二段中文。";
        for (auto &row : paragraphs) row.complete = true;
        require(translationText(paragraphs, true) == L"第一段中文连续阅读。\r\n\r\n第二段中文。", "Chinese paragraph breaks missing.");
    });
    test(L"列表编号、项目符号与续行保留正确的分段", [&]
    {
        auto paragraphs = translationParagraphs(translationRows(L"Intro.\n10. First item\ncontinued here.\n11. Second item\n- Bullet\nA) Letter item\n(12) Bracket item"));
        require(paragraphs.size() == 6 && paragraphs[1].source == L"10. First item continued here." &&
                paragraphs[2].source == L"11. Second item", "List items merged or continuation split.");
    });
    test(L"英文自动折行的连字符在段内恢复，空段不生成空请求", [&]
    {
        auto paragraphs = translationParagraphs(translationRows(L"\r\ntrans-\nlation\n\n\n\nNext."));
        require(paragraphs.size() == 2 && paragraphs[0].source == L"translation" && paragraphs[1].source == L"Next.",
                "Hyphenation or empty paragraphs were mishandled.");
    });
    test(L"长文本按句子和词边界分段且不超请求上限", [&]
    {
        std::wstring text; for (int i = 0; i < 100; ++i) text += L"A sentence for translation. ";
        auto chunks = translationChunks(text);
        require(chunks.size() >= 3 && std::all_of(chunks.begin(), chunks.end(), [](auto const &c) { return !c.empty() && c.size() <= 900; }), "Chunk limits violated.");
    });
    test(L"分段不会拆开 UTF16 代理对", [&]
    {
        std::wstring text(899, L'a'); text += L"\U0001f600"; text += std::wstring(100, L'b');
        std::wstring combined; for (auto const &chunk : translationChunks(text)) { combined += chunk; winrt::to_string(chunk); }
        require(combined == text, "Unicode surrogate split or lost.");
    });
    test(L"空白和过长输入不会调用接口", [&]
    {
        require(translationChunks(L" \r\n \t").empty(), "Whitespace generated requests.");
        bool rejected{}; try { translationChunks(std::wstring(TranslationTextLimit + 1, L'x')); } catch (...) { rejected = true; }
        require(rejected, "Oversized input accepted.");
    });
    test(L"百度签名使用原始 UTF8 文本并与官方示例一致", [&]
    {
        auto form = translationForm(exampleCredentials(), L"apple", "1435660288");
        require(form.find("sign=f89f9594663708c1605f3d736d01d2d4") != std::string::npos, "Baidu signature differs from official vector.");
    });
    test(L"请求正确编码中文、换行与特殊符号且不包含密钥", [&]
    {
        auto credentials = exampleCredentials(); auto form = translationForm(credentials, L"a&b=中文\r\n+%", "example");
        require(form.find("a%26b%3D%E4%B8%AD%E6%96%87%0D%0A%2B%25") != std::string::npos &&
                form.find(winrt::to_string(credentials.secret)) == std::string::npos, "Encoding or credential handling failed.");
    });
    test(L"译文 JSON 支持转义、多段和中文", [&]
    {
        auto response = translatedResponse(); auto result = parseTranslationResponse(response.status, response.body);
        require(result.ok() && result.text == L"测试译文", "Escaped Chinese response failed.");
        result = parseTranslationResponse(200, R"({"to":"zh","trans_result":[{"dst":"one"},{"dst":"two"}]})");
        require(result.ok() && result.text == L"one\r\ntwo", "Multi-paragraph response failed.");
    });
    test(L"认证、额度、限流和超时映射为可理解错误", [&]
    {
        for (auto code : {"54001", "54004", "54003", "52001", "52003", "58002", "58000", "59004"})
        {
            auto result = parseTranslationResponse(200, std::string("{\"error_code\":\"") + code + "\",\"error_msg\":\"private-sentinel\"}");
            require(!result.ok() && !result.error.empty() && result.error.find(L"private-sentinel") == std::wstring::npos, "Provider error leaked raw content.");
        }
        for (unsigned status : {401, 403, 429, 500, 302}) require(!parseTranslationResponse(status, "private-sentinel").ok(), "HTTP failure accepted.");
    });
    test(L"异常响应、错误语言、空译文与过大响应安全失败", [&]
    {
        for (auto body : {"", "{broken", "null", "[]", "{}", "{\"to\":\"en\",\"trans_result\":[]}",
             "{\"to\":\"zh\",\"trans_result\":[{\"dst\":\"\"}]}", "{\"to\":\"zh\",\"trans_result\":null}"})
            require(!parseTranslationResponse(200, body).ok(), "Malformed response accepted.");
        require(!parseTranslationResponse(200, std::string(524289, 'x')).ok(), "Oversized response accepted.");
    });
    test(L"异常数值和非标量错误码不能触发溢出或泄露响应", [&]
    {
        for (auto body : {"{\"error_code\":-1}", "{\"error_code\":1e300}", "{\"error_code\":1.5}",
                          "{\"error_code\":{\"private\":\"sentinel\"}}", "{\"error_code\":\"private-sentinel\"}"})
            require(!parseTranslationResponse(200, body).ok(), "Invalid error code accepted.");
    });
    test(L"后台翻译支持取消且不会回传已经取消的结果", [&]
    {
        std::atomic_bool entered{}, stopped{};
        TranslationService service(nullptr, [&](auto const &body, TranslationCancel const &cancelled)
        { entered = true; while (!cancelled->load()) Sleep(5); stopped = true; return translatedResponse(body); });
        service.submit(1, L"sample", exampleCredentials()); until([&] { return entered.load(); });
        service.cancel(); until([&] { return stopped.load(); });
        require(service.take().empty(), "Cancelled result was delivered.");
    });
    test(L"新请求替换旧请求并丢弃过期响应", [&]
    {
        std::atomic_uint calls{};
        TranslationService service(nullptr, [&](auto const &body, TranslationCancel const &cancelled)
        { unsigned n = ++calls; if (n == 1) while (!cancelled->load()) Sleep(5); return translatedResponse(body); });
        service.submit(1, L"first", exampleCredentials()); until([&] { return calls.load() == 1; });
        service.submit(2, L"second", exampleCredentials());
        std::vector<TranslationCompletion> results;
        until([&] { for (auto &r : service.take()) results.push_back(std::move(r));
                    return std::any_of(results.begin(), results.end(), [](auto const &r) { return r.finished; }); });
        require(std::all_of(results.begin(), results.end(), [](auto const &r) { return r.generation == 2; }), "Old request response escaped cancellation.");
    });
    test(L"限流按全局请求间隔执行并上报真实分段完成数", [&]
    {
        std::vector<Clock::time_point> starts;
        std::mutex timeMutex;
        TranslationService service(nullptr, [&](auto const &body, TranslationCancel const &)
        { std::lock_guard lock(timeMutex); starts.push_back(Clock::now()); return translatedResponse(body); });
        service.submit(3, std::wstring(1000, L'a'), exampleCredentials());
        std::vector<TranslationCompletion> results;
        until([&] { for (auto &r : service.take()) results.push_back(std::move(r));
                    return std::any_of(results.begin(), results.end(), [](auto const &r) { return r.finished; }); });
        require(starts.size() == 2 && starts[1] - starts[0] >= std::chrono::milliseconds(1000), "Provider QPS limit violated.");
        require(results.back().completed == 2 && results.back().total == 2 && results.back().result.ok(), "Incorrect completion progress.");
    });
    test(L"多行合并为一次请求但每行独立映射返回", [&]
    {
        std::atomic_uint calls{};
        TranslationService service(nullptr, [&](auto const &body, TranslationCancel const &)
        { ++calls; require(requestText(body) == L"First.\nSecond.\nThird.", "Batch did not retain query separators."); return translatedResponse(body); });
        service.submit(7, L"First.\nSecond.\n\nThird.", exampleCredentials());
        TranslationCompletion final;
        until([&] { for (auto &r : service.take()) if (r.finished) final = std::move(r); return final.finished; });
        require(calls == 1 && final.rows.size() == 3 && final.rows[2].paragraphBefore && final.rows[0].complete &&
                final.result.text == L"测试译文\r\n测试译文\r\n\r\n测试译文", "Batched row mapping or copy formatting failed.");
    });
    test(L"接口错误不自动重试或显示未完成译文为成功", [&]
    {
        std::atomic_uint calls{};
        TranslationService service(nullptr, [&](auto const &, TranslationCancel const &)
        { ++calls; return TranslationHttpResponse{200, "{\"error_code\":\"54004\"}"}; });
        service.submit(4, L"sample", exampleCredentials());
        std::vector<TranslationCompletion> results;
        until([&] { for (auto &r : service.take()) results.push_back(std::move(r)); return !results.empty() && results.back().finished; });
        require(calls == 1 && !results.back().result.ok(), "Unexpected retry or success.");
    });
    test(L"网络超时明确提示且不暴露内部异常内容", [&]
    {
        TranslationService service(nullptr, [](auto const &, TranslationCancel const &) -> TranslationHttpResponse
        { throw std::runtime_error("network timeout private-sentinel"); });
        service.submit(5, L"sample", exampleCredentials());
        TranslationCompletion final;
        until([&] { for (auto &r : service.take()) if (r.finished) final = std::move(r); return final.finished; });
        require(!final.result.ok() && final.result.error.find(L"超时") != std::wstring::npos &&
                final.result.error.find(L"private-sentinel") == std::wstring::npos, "Timeout error was unclear or leaked data.");
    });
    // All following UI tests use private example credentials and an injected
    // transport. They never use the user's account, network or clipboard.
    auto &panel = *translation_;
    panel.close(); panel.service_.reset();
    panel.store_ = TranslationCredentialStore(file(L"ui-credential.local.dat"));
    panel.credentials_ = exampleCredentials();
    std::atomic_uint calls{};
    DWORD workerThread{};
    panel.service_ = std::make_unique<TranslationService>(root_, [&](auto const &body, TranslationCancel const &)
    { workerThread = GetCurrentThreadId(); ++calls; return translatedResponse(body); });
    auto canvas = std::make_unique<Canvas>(*this, nextId_++, std::make_unique<Document>(Image(480, 240, 0xffffff)), false);
    require(canvas->create(false), "Cannot create translation test pin.");
    auto source = canvas.get(); windows_.push_back(std::move(canvas));
    source->ocr_ = std::make_shared<OcrSnapshot>(sampleSnapshot());
    test(L"贴图隐藏工具栏时仍能翻译所选文字且标注保持锁定", [&]
    {
        source->anchor_ = 1; source->caret_ = 2; source->command(23);
        until([&] { return !panel.busy_; });
        require(panel.window_ && !source->showTools_ && !source->annotationsEditable() &&
                windowText(GetDlgItem(panel.window_, 401)) == L"is a", "Pin translation changed editing mode or selection.");
        require(workerThread != GetCurrentThreadId(), "Network transport ran on UI thread.");
    });
    test(L"复制译文不会复制原文或覆盖测试机真实剪贴板", [&]
    {
        copied.clear(); SendMessage(panel.window_, WM_COMMAND, 404, 0);
        require(copied == L"测试译文", "Copy result used wrong text.");
    });
    test(L"编辑原文立即清除旧译文并支持重新翻译", [&]
    {
        auto edit = GetDlgItem(panel.window_, 401);
        SendMessage(edit, EM_SETSEL, 0, -1);
        SendMessage(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"Edited source."));
        require(panel.result_.empty() && !IsWindowEnabled(GetDlgItem(panel.window_, 404)), "Old result remained copyable.");
        SendMessage(panel.window_, WM_COMMAND, 403, 0); until([&] { return !panel.busy_; });
        require(panel.result_ == L"测试译文", "Retranslation failed.");
    });
    test(L"翻译面板缩放后全部控件保持在窗口内", [&]
    {
        SetWindowPos(panel.window_, nullptr, 0, 0, 470, 510, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); panel.layout();
        RECT client{}; GetClientRect(panel.window_, &client);
        for (int id : {401, 402, 403, 404, 405, 406, 407, 408})
        {
            RECT r{}; GetWindowRect(GetDlgItem(panel.window_, id), &r);
            MapWindowPoints(nullptr, panel.window_, reinterpret_cast<POINT *>(&r), 2);
            require(r.left >= 0 && r.top >= 0 && r.right <= client.right && r.bottom <= client.bottom, "Translation control escaped client area.");
        }
    });
    test(L"翻译设置掩码显示密钥并可关闭联网功能", [&]
    {
        panel.showSettings();
        auto secret = GetDlgItem(panel.settings_, 422);
        require((GetWindowLongPtr(secret, GWL_STYLE) & ES_PASSWORD) != 0, "Secret was not masked.");
        SendDlgItemMessage(panel.settings_, 423, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessage(panel.settings_, WM_COMMAND, IDOK, 0);
        require(!panel.settings_ && !panel.credentials_.enabled && !panel.store_.load().enabled, "Disable consent did not persist.");
        unsigned before = calls.load(); panel.start(); pump();
        require(!panel.busy_ && calls == before, "Disabled translation contacted provider.");
        panel.credentials_ = exampleCredentials();
    });
    test(L"图片编辑会清除翻译内容并拒绝旧请求结果", [&]
    {
        panel.open(source->id_, source->doc_->revision, L"old text");
        auto stale = panel.generation_;
        source->invalidateOcr();
        require(!panel.busy_ && windowText(GetDlgItem(panel.window_, 401)).empty() && panel.result_.empty(), "Changed image retained stale text.");
        require(panel.generation_ != stale, "Changed image retained old generation.");
        pump(); require(panel.result_.empty(), "Old result reappeared after image edit.");
    });
    test(L"按译后等 OCR 完成才翻译且拒绝错误图片版本", [&]
    {
        panel.open(source->id_, source->doc_->revision, {}, true);
        auto snapshot = sampleSnapshot();
        unsigned before = calls.load();
        panel.ocrReady(source->id_, source->doc_->revision + 1, snapshot); pump();
        require(panel.waitingOcr_ && calls == before, "Stale OCR initiated translation.");
        panel.ocrReady(source->id_, source->doc_->revision, snapshot);
        until([&] { return !panel.busy_; });
        require(calls == before + 1 && panel.result_ == L"测试译文" && panel.rows_.size() == 1, "Requested OCR did not trigger paragraph translation once.");
    });
    test(L"取消等待 OCR 后不会自动发送文字", [&]
    {
        panel.open(source->id_, source->doc_->revision, {}, true); panel.cancel();
        auto before = calls.load(); panel.ocrReady(source->id_, source->doc_->revision, sampleSnapshot()); pump();
        require(!panel.busy_ && calls == before, "Cancelled OCR waiting uploaded text.");
    });
    test(L"没有选字时翻译全文且原图保持不变", [&]
    {
        source->ocr_ = std::make_shared<OcrSnapshot>(sampleSnapshot()); source->anchor_ = source->caret_ = -1;
        auto revision = source->doc_->revision; source->command(23); until([&] { return !panel.busy_; });
        require(windowText(GetDlgItem(panel.window_, 401)) == L"This is a\r\nsimple test." && source->doc_->revision == revision, "Full translation modified image or line breaks.");
    });
    test(L"状态刷新保留译文的选字和滚动位置", [&]
    {
        auto edit = GetDlgItem(panel.window_, 402); SendMessage(edit, EM_SETSEL, 1, 3);
        panel.status_ = L"状态提示更新"; panel.update();
        DWORD first{}, last{}; SendMessage(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        require(first == 1 && last == 3, "Status update reset translated-text selection.");
    });
    test(L"恢复上下两个始终可见的纯文本框，无表格和行号", [&]
    {
        RECT original{}, translated{};
        GetWindowRect(GetDlgItem(panel.window_, 401), &original); GetWindowRect(GetDlgItem(panel.window_, 402), &translated);
        require(original.bottom < translated.top && !GetDlgItem(panel.window_, 412), "Reading panel retained the table toggle or side-by-side layout.");
        for (int id : {401, 402})
        {
            auto control = GetDlgItem(panel.window_, id); wchar_t name[32]{}; GetClassName(control, name, 32);
            require(IsWindowVisible(control) && _wcsicmp(name, L"Edit") == 0, "Reading text box was hidden or not plain text.");
        }
    });
    test(L"长中文在整块文本框自动折行，显示和复制内容完全一致", [&]
    {
        panel.rows_ = translationParagraphs(translationRows(L"First paragraph.\n\nSecond paragraph."));
        panel.rows_[0].translated = L"这是一段较长的中文译文，用于连续阅读。文字在整个文本框中自动折行，同一段内保持连贯，不再被表格拆成一个个单元格，也不会带上行号或英文。";
        panel.rows_[1].translated = L"这是第二段中文，与第一段之间空一行。";
        for (auto &row : panel.rows_) row.complete = true;
        panel.setSource(translationText(panel.rows_)); panel.result_ = translationText(panel.rows_, true); panel.update();
        auto edit = GetDlgItem(panel.window_, 402);
        auto text = windowText(edit); SendMessage(panel.window_, WM_COMMAND, 404, 0);
        require(text == copied && text == panel.result_ && text.find(L"\r\n\r\n") != std::wstring::npos &&
                SendMessage(edit, EM_GETLINECOUNT, 0, 0) > 3 && !(GetWindowLongPtr(edit, GWL_STYLE) & ES_AUTOHSCROLL),
                "Chinese reading area did not wrap, preserve paragraphs or copy plain text.");
        require(GetWindowLongPtr(edit, GWL_STYLE) & ES_READONLY, "Translated reading area was editable.");
    });
    test(L"中文框作为纯文本保留符号、Unicode 与原样选区", [&]
    {
        panel.result_ = L"中文 {\\field example} \U0001f600\r\n\r\n第二段 & <text>"; panel.update();
        auto edit = GetDlgItem(panel.window_, 402); SendMessage(edit, EM_SETSEL, 0, -1);
        DWORD first{}, last{}; SendMessage(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        SendMessage(panel.window_, WM_COMMAND, 404, 0);
        require(windowText(edit) == panel.result_ && copied == panel.result_ && first == 0 && last == panel.result_.size(),
                "Plain-text symbols were transformed or could not be selected together.");
    });
    test(L"密集段落在窄窗口折行，状态刷新不重置滚动和文字选区", [&]
    {
        panel.rows_.clear();
        for (int i = 1; i <= 80; ++i)
            panel.rows_.push_back({L"List item " + std::to_wstring(i) + L": English text on this row.",
                L"第 " + std::to_wstring(i) + L" 段中文译文在窄窗口中自动折行，支持大块连续选择和复制。", {}, i != 1, true});
        panel.result_ = translationText(panel.rows_, true);
        SetWindowPos(panel.window_, nullptr, 0, 0, 560, 490, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); panel.layout(); panel.update();
        auto edit = GetDlgItem(panel.window_, 402);
        SendMessage(edit, EM_SETSEL, 1, 45); SendMessage(edit, EM_LINESCROLL, 0, 30);
        auto before = SendMessage(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
        panel.status_ = L"状态更新保留阅读位置"; panel.update();
        DWORD first{}, last{}; SendMessage(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        require(before > 0 && SendMessage(edit, EM_GETFIRSTVISIBLELINE, 0, 0) == before && first == 1 && last == 45,
                "Status refresh reset scrolling or selection.");
        SendMessage(panel.window_, WM_COMMAND, 404, 0);
        require(copied == panel.result_ && std::count(copied.begin(), copied.end(), L'\n') == 158,
                "Copied reading text lost paragraph breaks.");
        SendMessage(edit, EM_SETSEL, 0, -1); SendMessage(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        require(first == 0 && last == copied.size(), "Whole reading area could not be selected as a block.");
    });
    test(L"修改原文空行立即清除旧译文，重译按新段落分隔", [&]
    {
        panel.open(source->id_, source->doc_->revision, translationRows(sampleSnapshot())); until([&] { return !panel.busy_; });
        auto edit = GetDlgItem(panel.window_, 401); SendMessage(edit, EM_SETSEL, 0, -1);
        SendMessage(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"One paragraph.\r\n\r\nSecond paragraph."));
        require(windowText(GetDlgItem(panel.window_, 402)).empty(), "Editing paragraph boundaries retained stale translation.");
        panel.start(); until([&] { return !panel.busy_; });
        require(panel.rows_.size() == 2 && panel.result_ == L"测试译文\r\n\r\n测试译文", "New paragraph translation did not preserve blank line.");
    });
    test(L"输入法组合中的确认与取消按键优先交给输入法", [&]
    {
        auto edit = GetDlgItem(panel.window_, 401); SendMessage(edit, WM_IME_STARTCOMPOSITION, 0, 0);
        MSG message{}; message.hwnd = edit; message.message = WM_KEYDOWN; message.wParam = VK_ESCAPE;
        require(!panel.preTranslate(message) && panel.window_, "IME Escape closed translation.");
        message.wParam = VK_RETURN; require(!panel.preTranslate(message), "IME Enter was intercepted.");
        SendMessage(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    });
    test(L"紧凑设置窗口和 DPI 变更消息后主要控件仍可见", [&]
    {
        panel.showSettings();
        SetWindowPos(panel.settings_, nullptr, 0, 0, 540, 360, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT next{}; GetWindowRect(panel.settings_, &next);
        SendMessage(panel.settings_, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&next));
        RECT client{}; GetClientRect(panel.settings_, &client);
        for (int id : {421, 422, 423, 424, 425, 426, IDOK, IDCANCEL})
        {
            RECT r{}; GetWindowRect(GetDlgItem(panel.settings_, id), &r);
            MapWindowPoints(nullptr, panel.settings_, reinterpret_cast<POINT *>(&r), 2);
            require(r.left >= 0 && r.top >= 0 && r.right <= client.right && r.bottom <= client.bottom, "Translation setting escaped client area.");
        }
        DestroyWindow(panel.settings_);
    });
    test(L"截图前隐藏翻译面板及设置避免截入图片", [&]
    {
        panel.showSettings(); panel.pauseForCapture();
        require(!IsWindowVisible(panel.window_) && !IsWindowVisible(panel.settings_), "Translation UI remained in capture.");
        DestroyWindow(panel.settings_);
        ShowWindow(panel.window_, SW_SHOWNOACTIVATE);
    });
    test(L"翻译面板可生成实际界面预览", [&]
    {
        SetWindowPos(panel.window_, nullptr, 0, 0, 640, 680, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); panel.layout();
        SendDlgItemMessage(panel.window_, 402, EM_SETSEL, 0, 0); SendDlgItemMessage(panel.window_, 402, EM_SCROLLCARET, 0, 0);
        RECT client{}; GetClientRect(panel.window_, &client);
        Image image(client.right, client.bottom, 0xffffff); Dib dib(image);
        require(PrintWindow(panel.window_, dib.dc, PW_CLIENTONLY) != FALSE, "Translation preview render failed.");
        savePng(graphics, dib.image(), file(L"translation-panel.local.png").wstring());
    });
    test(L"关闭图片立即关闭关联翻译面板", [&]
    {
        std::atomic_bool entered{}, stopped{};
        panel.cancel(); panel.service_.reset();
        panel.service_ = std::make_unique<TranslationService>(root_, [&](auto const &body, TranslationCancel const &cancelled)
        { entered = true; while (!cancelled->load()) Sleep(5); stopped = true; return translatedResponse(body); });
        panel.open(source->id_, source->doc_->revision, L"pending text"); until([&] { return entered.load(); });
        source->close(); pump(); until([&] { return stopped.load(); });
        require(!panel.window_ && !panel.busy_ && !panel.waitingOcr_, "Translation survived source closure.");
        panel.service_.reset();
        panel.service_ = std::make_unique<TranslationService>(root_, [](auto const &body, TranslationCancel const &) { return translatedResponse(body); });
    });
    panel.close();
    copyText_ = previousCopy;
    if (previousForeground && IsWindow(previousForeground)) SetForegroundWindow(previousForeground);
    writeReport(report, "{\"suite\":\"translation\",\"passed\":" + std::to_string(passed) + ",\"failed\":" +
        std::to_string(failed) + ",\"networkUsed\":false,\"userClipboardChanged\":false,\"cases\":" + cases + "]}");
    return failed ? 1 : 0;
}
int App::translationLiveTest(std::wstring const &report)
{
    auto started = Clock::now();
    TranslationResult result;
    bool finished{}, chinese{}, configured{}, enabled{}, aligned{}, ocrAligned{}, preview{}, readable{};
    std::vector<TranslationRow> rows;
    std::wstring configurationIssue;
    try
    {
        auto credentials = TranslationCredentialStore().load();
        configured = credentials.valid(); enabled = credentials.enabled;
        if (!credentials.valid() || !credentials.enabled) throw std::runtime_error("Configure local translation credentials first.");
        // End-to-end fixture: local OCR, paragraph grouping, batched network
        // translation and two actual native text boxes. All text is fixed
        // synthetic content, independent of the user's screen and clipboard.
        std::array<std::wstring, 6> lines = {
            L"Screenshot translation helps you read English text.",
            L"Keep the sentences together for comfortable reading.",
            L"1. Select text in the image.",
            L"2. Copy the translated paragraphs.",
            L"The English source and Chinese translation",
            L"are displayed in two separate text boxes."};
        constexpr int y[] = {24, 62, 126, 164, 228, 266};
        Dib input(Image(1150, 330, 0xffffff));
        auto font = CreateFont(-24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        auto old = SelectObject(input.dc, font); SetBkMode(input.dc, TRANSPARENT); SetTextColor(input.dc, RGB(32, 43, 56));
        for (int i = 0; i < 6; ++i) TextOut(input.dc, 24, y[i], lines[i].c_str(), static_cast<int>(lines[i].size()));
        SelectObject(input.dc, old); DeleteObject(font);
        auto image = std::make_shared<Image>(input.image());
        auto snapshot = recognize({1, 1, image, {}, L"en-US"});
        auto sourceRows = translationRows(*snapshot);
        ocrAligned = sourceRows.size() == 6 && sourceRows[4].paragraphBefore && !sourceRows.back().paragraphBefore &&
            std::all_of(sourceRows.begin(), sourceRows.end(), [](auto const &row) { return row.quad.size() == 4; });
        if (!ocrAligned) { result.error = L"合成截图的 OCR 行与位置检查未通过。"; throw std::runtime_error("Synthetic OCR line alignment failed."); }
        auto paragraphs = translationParagraphs(sourceRows);
        TranslationService service(nullptr);
        service.submit(1, paragraphs, credentials);
        auto deadline = Clock::now() + std::chrono::seconds(35);
        while (!finished && Clock::now() < deadline)
        {
            for (auto &completion : service.take())
                if (completion.finished) { finished = true; result = std::move(completion.result); rows = std::move(completion.rows); }
            Sleep(10);
        }
        if (!finished) { service.cancel(); result.error = L"联网翻译测试超时。"; }
        chinese = std::any_of(result.text.begin(), result.text.end(), [](wchar_t c) { return c >= 0x3400 && c <= 0x9fff; });
        aligned = rows.size() == paragraphs.size() && rows.size() == 4 && std::count(result.text.begin(), result.text.end(), L'\n') == 6;
        for (size_t i = 0; aligned && i < rows.size(); ++i)
            aligned = rows[i].source == paragraphs[i].source && rows[i].complete && !rows[i].translated.empty() &&
                rows[i].paragraphBefore == paragraphs[i].paragraphBefore;
        if (aligned && result.ok())
        {
            auto folder = std::filesystem::path(report).parent_path(); std::filesystem::create_directories(folder);
            auto canvas = std::make_unique<Canvas>(*this, nextId_++, std::make_unique<Document>(*image), false);
            require(canvas->create(false), "Cannot render synthetic translation source.");
            auto source = canvas.get(); windows_.push_back(std::move(canvas));
            auto &panel = *translation_; panel.canvas_ = source->id_; panel.revision_ = source->doc_->revision;
            panel.create(); panel.rows_ = rows; panel.setSource(translationText(sourceRows));
            panel.result_ = result.text; panel.status_ = L"翻译完成，已按段落分行，可拖选文字或复制全部译文。";
            SetWindowPos(panel.window_, nullptr, 0, 0, 640, 680, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            panel.layout(); panel.update();
            ShowWindow(panel.window_, SW_SHOWNOACTIVATE); UpdateWindow(panel.window_);
            readable = IsWindowVisible(GetDlgItem(panel.window_, 401)) && IsWindowVisible(GetDlgItem(panel.window_, 402)) &&
                windowText(GetDlgItem(panel.window_, 402)) == result.text && windowText(GetDlgItem(panel.window_, 401)) == translationText(sourceRows);
            RECT client{}; GetClientRect(panel.window_, &client); Dib output(Image(client.right, client.bottom, 0xffffff));
            preview = PrintWindow(panel.window_, output.dc, PW_CLIENTONLY) &&
                GetPixel(output.dc, 8, 8) == RGB(255, 255, 255);
            if (preview)
            {
                savePng(graphics, output.image(), (folder / L"translation-live-panel.local.png").wstring());
                source->renderUiPreview((folder / L"translation-live-source.local.png").wstring());
            }
            panel.close(); source->close();
        }
    }
    catch (winrt::hresult_error const &e)
    { configurationIssue = L"Windows API " + std::to_wstring(e.code().value); result.error = L"本机翻译配置不可用，请先配置 APPID 与密钥。"; }
    catch (std::exception const &e)
    {
        std::string message = e.what();
        configurationIssue = message.find("decrypt") != std::string::npos ? L"decrypt" :
            message.find("credentials") != std::string::npos ? L"credential validation" : L"configuration read";
        if (result.error.empty()) result.error = L"本机翻译配置不可用，请先配置 APPID 与密钥。";
    }
    bool passed = finished && result.ok() && chinese && aligned && ocrAligned && preview && readable;
    if (!passed && result.error.empty()) result.error = L"分段翻译或双文本框视图检查未通过。";
    writeReport(report, "{\"suite\":\"translation-live\",\"passed\":" + std::string(passed ? "true" : "false") +
        ",\"fixedSyntheticSampleOnly\":true,\"configurationValid\":" + (configured ? "true" : "false") +
        ",\"consentEnabled\":" + (enabled ? "true" : "false") + ",\"configurationIssue\":" + json(configurationIssue) + ",\"localDataDirectory\":" + json(localDirectory()) +
        ",\"chineseResult\":" + (chinese ? "true" : "false") +
        ",\"ocrLineGeometryConfirmed\":" + (ocrAligned ? "true" : "false") + ",\"paragraphBoundariesPreserved\":" + (aligned ? "true" : "false") +
        ",\"paragraphCount\":" + std::to_string(rows.size()) + ",\"plainTextBoxesConfirmed\":" + (readable ? "true" : "false") +
        ",\"nativePreviewRendered\":" + (preview ? "true" : "false") +
        ",\"elapsedMs\":" + std::to_string(std::chrono::duration<double, std::milli>(Clock::now() - started).count()) +
        ",\"error\":" + json(result.error) + "}");
    return passed ? 0 : 1;
}
} // namespace snap
