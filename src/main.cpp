#include "app.h"
#include <commctrl.h>
#include <iostream>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.Ocr.h>
namespace snap
{
std::string json(std::wstring const &value)
{
    auto utf8 = winrt::to_string(value);
    std::string out = "\"";
    for (unsigned char c : utf8)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (c == '\r')
            out += "\\r";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (c >= 32)
            out += c;
    }
    return out + "\"";
}
void writeReport(std::wstring const &path, std::string const &body)
{
    auto p = std::filesystem::path(path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot write report.");
    out << body;
    std::cout << body << std::endl;
}
void require(bool condition, char const *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
int coreTests(Graphics &g, std::wstring const &path)
{
    int passed = 0, failed = 0;
    std::string cases = "[";
    auto test = [&](wchar_t const *name, auto body)
    {
        if (passed + failed)
            cases += ",";
        cases += "{\"name\":" + json(name);
        try
        {
            body();
            ++passed;
            cases += ",\"passed\":true}";
        }
        catch (...)
        {
            ++failed;
            cases += ",\"passed\":false,\"error\":" + json(errorText()) + "}";
        }
    };
    test(L"图像尺寸检查",
         []
         {
             bool threw = false;
             try
             {
                 Image invalid(0, 2);
             }
             catch (...)
             {
                 threw = true;
             }
             require(threw, "Zero-sized image accepted.");
         });
    test(L"裁切物理像素",
         []
         {
             Image image(20, 10, 0x123456);
             auto cropped = image.crop({3, 2, 14, 8});
             require(cropped.width == 11 && cropped.height == 6, "Incorrect crop size.");
             require(cropped.pixels[0] == 0x56 && cropped.pixels[2] == 0x12, "Pixel order changed.");
         });
    test(L"负坐标与缩放逆变换",
         []
         {
             for (float z : {.5f, 1.f, 2.f})
             {
                 View v{z, {-800, 153}};
                 Point p{213, 47};
                 auto q = v.toImage(v.toView(p));
                 require(std::abs(q.x - p.x) < .001 && std::abs(q.y - p.y) < .001, "View inverse mismatch.");
             }
         });
    test(L"旋转文字框命中",
         []
         {
             OcrSnapshot s;
             s.width = 400;
             s.height = 200;
             s.angle = 13;
             s.tokens = {{L"测试", {40, 30, 60, 20}, 0}};
             auto p = rotate({60, 40}, {200, 100}, 13);
             require(s.hit(p) == 0, "Rotated hit failed.");
         });
    OcrSnapshot sample;
    sample.tokens = {{L"轻", {0, 0, 20, 20}, 0},      {L"量", {22, 0, 20, 20}, 0},     {L"截图", {44, 0, 40, 20}, 0},
                     {L"Select", {0, 40, 55, 20}, 1}, {L"words", {65, 40, 50, 20}, 1}, {L"here", {125, 40, 40, 20}, 1}};
    test(L"中文部分句子", [&] { require(sample.selection(1, 2) == L"量截图", "CJK spacing incorrect."); });
    test(L"英文词间空格",
         [&] { require(sample.selection(3, 5) == L"Select words here", "English spacing incorrect."); });
    test(L"反向跨行选字", [&]
         { require(sample.selection(4, 1) == L"量截图\r\nSelect words", "Reverse or multiline selection failed."); });
    test(L"最近词框拖选", [&] { require(sample.hit({120, 50}, true) >= 3, "Nearest token failed."); });
    Image base(220, 140);
    Annotation arrow{Tool::Arrow, {15, 110}, {190, 24}, {}, L"", 0xe6525c, 4};
    Annotation line{Tool::Line, {15, 90}, {190, 90}, {}, L"", 0x268ee9, 3};
    Annotation marker{Tool::Marker, {15, 65}, {190, 65}, {{15, 65}, {100, 65}, {190, 65}}, L"", 0xffc13a, 4};
    test(L"箭头渲染",
         [&]
         {
             auto rendered = renderAnnotations(g, base, std::span(&arrow, 1));
             require(rendered.pixels != base.pixels, "Arrow not drawn.");
         });
    test(L"直线渲染",
         [&]
         {
             auto rendered = renderAnnotations(g, base, std::span(&line, 1));
             require(rendered.pixels[(90 * 220 + 100) * 4] < 245, "Line not drawn.");
         });
    test(L"半透明记号笔",
         [&]
         {
             auto rendered = renderAnnotations(g, base, std::span(&marker, 1));
             auto b = rendered.pixels[(65 * 220 + 100) * 4];
             require(b > 140 && b < 245, "Marker alpha incorrect.");
         });
    test(L"中文文字渲染",
         [&]
         {
             Annotation text{Tool::Text, {10, 5}, {}, {}, L"原图选字 ABC", 0x252a36, 3};
             auto rendered = renderAnnotations(g, base, std::span(&text, 1));
             require(rendered.pixels != base.pixels, "Text not rendered.");
         });
    test(L"马赛克像素块与范围",
         []
         {
             Image image(24, 24);
             for (int y = 0; y < 24; ++y)
                 for (int x = 0; x < 24; ++x)
                     image.pixels[(y * 24 + x) * 4] = static_cast<uint8_t>(x * 7 + y);
             auto before = image;
             pixelate(image, {4, 4, 12, 12}, 6);
             require(image.pixels[(6 * 24 + 6) * 4] == image.pixels[(8 * 24 + 8) * 4], "Mosaic block not uniform.");
             require(image.pixels[0] == before.pixels[0] && image.pixels != before.pixels,
                     "Mosaic modified outside region or did nothing.");
         });
    test(L"撤销重做与版本失效",
         [&]
         {
             Document d(base);
             auto r = d.revision;
             d.add(arrow);
             d.flatten(g);
             auto visible = d.rendered->pixels;
             require(d.revision > r && d.undo(), "Undo failed.");
             d.flatten(g);
             require(d.rendered->pixels == base.pixels, "Undo pixels mismatch.");
             require(d.redo(), "Redo failed.");
             d.flatten(g);
             require(d.rendered->pixels == visible, "Redo pixels mismatch.");
             require(d.undo(), "Second undo failed.");
             d.add(line);
             require(!d.redo(), "Redo not cleared by new edit.");
         });
    test(L"马赛克遮挡范围",
         [&]
         {
             Document d(base);
             d.add({Tool::Mosaic, {11, 22}, {81, 62}, {}, L"", 0, 3});
             require(d.redactions().size() == 1 && d.redactions()[0].w == 70, "Redaction mask missing.");
         });
    test(L"PNG 可见像素往返",
         [&]
         {
             std::array<Annotation, 3> marks{arrow, line, marker};
             auto rendered = renderAnnotations(g, base, marks);
             auto file = (std::filesystem::path(path).parent_path() / L"annotations.local.png").wstring();
             savePng(g, rendered, file);
             auto loaded = loadImage(g, file);
             require(loaded.pixels == rendered.pixels, "PNG output differs from visible pixels.");
         });
    cases += "]";
    writeReport(path, "{\"suite\":\"core\",\"passed\":" + std::to_string(passed) +
                          ",\"failed\":" + std::to_string(failed) + ",\"cases\":" + cases + "}");
    return failed ? 1 : 0;
}
int ocrTest(Graphics &g, std::wstring const &path)
{
    auto image = std::make_shared<Image>(demoImage(g));
    auto result = recognize({1, 7, image, {}, L""});
    bool identity = !packageIdentity().empty(), hasChinese = false, hasEnglish = false;
    auto text = result->selection(0, static_cast<int>(result->tokens.size()) - 1);
    hasChinese = text.find(L"直接在图片上拖选文字") != std::wstring::npos;
    hasEnglish = text.find(L"Select words on this image and copy them.") != std::wstring::npos;
    int partial = -1;
    for (size_t i = 0; i < result->tokens.size(); ++i)
        if (result->tokens[i].text == L"截")
        {
            partial = static_cast<int>(i);
            break;
        }
    bool hit = true;
    for (float z : {.5f, 1.f, 2.f})
    {
        View v{z, {-700, 120}};
        for (auto const &t : result->tokens)
        {
            auto center = rotate({t.box.x + t.box.w / 2, t.box.y + t.box.h / 2},
                                 {image->width / 2.f, image->height / 2.f}, t.angle.value_or(result->angle));
            auto found = result->hit(v.toImage(v.toView(center)));
            if (found < 0)
                hit = false;
        }
    }
    auto file = (std::filesystem::path(path).parent_path() / L"ocr-sample.local.png").wstring();
    savePng(g, *image, file);
    Box mask{50, 211, 880, 45};
    auto redacted = std::make_shared<Image>(*image);
    pixelate(*redacted, mask, 18);
    auto filtered = recognize({1, 8, redacted, {mask}, L""});
    bool noLeaked = true;
    for (auto const &t : filtered->tokens)
    {
        auto quad = filtered->quad(t);
        float left = quad[0].x, top = quad[0].y, right = left, bottom = top;
        for (auto p : quad)
        {
            left = std::min(left, p.x);
            top = std::min(top, p.y);
            right = std::max(right, p.x);
            bottom = std::max(bottom, p.y);
        }
        if (mask.intersects({left, top, right - left, bottom - top}))
            noLeaked = false;
    }
    bool passed = identity && hasChinese && hasEnglish && hit && partial >= 0 && noLeaked && !filtered->tokens.empty();
    writeReport(path, "{\"suite\":\"ocr\",\"passed\":" + std::string(passed ? "true" : "false") +
                          ",\"identity\":" + json(packageIdentity()) + ",\"language\":" + json(result->language) +
                          ",\"elapsedMs\":" + std::to_string(result->elapsedMs) + ",\"tokens\":" +
                          std::to_string(result->tokens.size()) + ",\"chinese\":" + (hasChinese ? "true" : "false") +
                          ",\"english\":" + (hasEnglish ? "true" : "false") + ",\"partialChinese\":" +
                          (partial >= 0 ? "true" : "false") + ",\"zoomHits\":" + (hit ? "true" : "false") +
                          ",\"redactionFiltered\":" + (noLeaked ? "true" : "false") + ",\"text\":" + json(text) +
                          ",\"status\":" + json(result->status) + "}");
    return passed ? 0 : 1;
}
} // namespace snap
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    int count{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> args;
    for (int i = 1; i < count; ++i)
        args.emplace_back(argv[i]);
    LocalFree(argv);
    if (!args.empty() && args[0] == L"--install-check" && args.size() != 2)
        return 2;
    if (!args.empty() && args[0] != L"--self-test" && args[0] != L"--ocr-test" && args[0] != L"--ui-test" &&
        args[0] != L"--regression-test" && args[0] != L"--ocr-quality-test" && args[0] != L"--demo" &&
        args[0] != L"--capture" && args[0] != L"--paste" && args[0] != L"--open" && args[0] != L"--quit" &&
        args[0] != L"--install-check" && args[0] != L"--translation-test" && args[0] != L"--translation-live-test")
        return 2;
    bool test = !args.empty() &&
                (args[0] == L"--self-test" || args[0] == L"--ocr-test" || args[0] == L"--ui-test" ||
                 args[0] == L"--regression-test" || args[0] == L"--ocr-quality-test" || args[0] == L"--install-check" ||
                 args[0] == L"--translation-test" || args[0] == L"--translation-live-test");
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    try
    {
        winrt::init_apartment(test ? winrt::apartment_type::multi_threaded : winrt::apartment_type::single_threaded);
        if (test)
        {
            snap::Graphics graphics;
            auto path = args.size() > 1 ? args[1] : snap::localDirectory() + L"\\test.local.json";
            if (args[0] == L"--install-check")
            {
                std::string languages = "[";
                std::string languageError = "null";
                unsigned available = 0;
                bool identity = !snap::packageIdentity().empty();
                // Report basic initialization separately from optional identity.
                // Do not call identity-only OCR APIs for a basic installation.
                try
                {
                    if (identity)
                    {
                        for (auto const &language : winrt::Windows::Media::Ocr::OcrEngine::AvailableRecognizerLanguages())
                        {
                            if (available++)
                                languages += ",";
                            languages += snap::json(std::wstring(language.LanguageTag()));
                        }
                    }
                }
                catch (...)
                {
                    languageError = snap::json(snap::errorText());
                    languages = "[";
                    available = 0;
                }
                snap::writeReport(path, "{\"identity\":" + snap::json(snap::packageIdentity()) + ",\"languages\":" +
                                            languages + "],\"version\":" + snap::json(snap::Version) + "," +
                                            "\"basicReady\":true," +
                                            "\"identityReady\":" + (identity ? "true" : "false") +
                                            ",\"ocrReady\":" + (identity && available ? "true" : "false") +
                                            ",\"languageCheckError\":" + languageError + "}");
                return identity ? 0 : 3;
            }
            if (args[0] == L"--ocr-test")
                return snap::ocrTest(graphics, path);
            if (args[0] == L"--ocr-quality-test")
                return snap::ocrQualityTest(graphics, path);
            if (args[0] == L"--self-test")
                return snap::coreTests(graphics, path);
            snap::App app(instance);
            app.config.autoOcr = false;
            if (!app.create(true))
                return 2;
            if (args[0] == L"--translation-test") return app.translationTests(path);
            if (args[0] == L"--translation-live-test") return app.translationLiveTest(path);
            return args[0] == L"--regression-test" ? app.regressionTests(path) : app.integrationTests(path);
        }
        HANDLE mutex = CreateMutex(nullptr, FALSE, L"Local\\SnapLite.Desktop.Singleton");
        if (!mutex)
            throw std::runtime_error("Cannot create instance mutex.");
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            HWND root = FindWindow(L"SnapLite.Root", L"SnapLite");
            COPYDATASTRUCT data{};
            data.dwData = !args.empty() && args[0] == L"--demo"    ? 1
                          : !args.empty() && args[0] == L"--paste" ? 3
                          : !args.empty() && args[0] == L"--quit"  ? 5
                                                                   : 2;
            std::wstring file;
            if (args.size() > 1 && args[0] == L"--open")
            {
                file = args[1];
                data.dwData = 4;
                data.cbData = static_cast<DWORD>((file.size() + 1) * sizeof(wchar_t));
                data.lpData = file.data();
            }
            DWORD_PTR ignored{};
            if (root)
                SendMessageTimeout(root, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000,
                                   &ignored);
            CloseHandle(mutex);
            return 0;
        }
        if (!args.empty() && args[0] == L"--quit")
        {
            CloseHandle(mutex);
            return 0;
        }
        int exitCode{};
        {
            snap::App app(instance);
            if (!app.create())
                throw std::runtime_error("Cannot initialize SnipLeaf.");
            if (!args.empty() && args[0] == L"--demo")
                app.demo();
            else if (!args.empty() && args[0] == L"--capture")
                app.capture();
            else if (!args.empty() && args[0] == L"--paste")
                app.paste();
            else if (args.size() > 1 && args[0] == L"--open")
                app.open(args[1]);
            exitCode = app.run();
        }
        CloseHandle(mutex);
        return exitCode;
    }
    catch (...)
    {
        auto message = snap::errorText();
        if (test)
        {
            std::cerr << winrt::to_string(message) << std::endl;
            return 1;
        }
        MessageBox(nullptr, message.c_str(), L"SnipLeaf 启动失败", MB_OK | MB_ICONERROR);
        return 1;
    }
}
