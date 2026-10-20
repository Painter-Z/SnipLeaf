#include "app.h"
#include <commctrl.h>
namespace snap
{
std::string json(std::wstring const &);
void writeReport(std::wstring const &, std::string const &);
void require(bool, char const *);
int App::integrationTests(std::wstring const &report)
{
    int passed = 0, failed = 0;
    bool englishDragVerified = false;
    std::string cases = "[";
    auto folder = std::filesystem::path(report).parent_path();
    std::filesystem::create_directories(folder);
    auto file = [&](wchar_t const *name) { return (folder / name).wstring(); };
    auto pump = [&]
    {
        MSG msg{};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message != WM_QUIT)
            {
                if (!preTranslate(msg))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
        }
    };
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
    test(L"OCR 身份修复与语言配置入口在设置窗口内完整显示",
         [&]
         {
             settings();
             pump();
             require(settingsWindow_ != nullptr, "Settings window did not open.");
             HWND window = settingsWindow_;
             RECT client{};
             GetClientRect(window, &client);
             std::vector<RECT> buttons;
             for (int id : {207, 206, IDOK})
             {
                 HWND button = GetDlgItem(window, id);
                 // The command-line test host deliberately starts hidden.
                 // Check the child visibility style, not hidden ancestors.
                 require(button && (GetWindowLongPtr(button, GWL_STYLE) & WS_VISIBLE),
                         "OCR repair/language/save control missing or hidden.");
                 RECT bounds{};
                 GetWindowRect(button, &bounds);
                 MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
                 require(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right &&
                             bounds.bottom <= client.bottom,
                         "Settings control clipped outside client area.");
                 for (auto const &other : buttons)
                 {
                     RECT intersection{};
                     require(!IntersectRect(&intersection, &bounds, &other), "Settings buttons overlap.");
                 }
                 buttons.push_back(bounds);
             }
             // Close without saving or starting any identity/language operation.
             DestroyWindow(window);
             pump();
             require(!settingsWindow_, "Settings window ownership was not released.");
         });
    test(L"首次 OCR 准备会区分已就绪、缺语言、缺身份和已选择稍后",
         [&]
         {
             auto &setup = *ocrSetup_;
             auto result = [&](OcrReadiness readiness, bool offered)
             {
                 setup.automatic_ = true;
                 setup.offered_ = offered;
                 setup.pending_ = std::move(readiness);
                 setup.receive();
             };
             result({true, true, true, {}}, false);
             require(!setup.window_ && setup.countdown_ == Clock::time_point{}, "Ready OCR triggered setup.");
             result({true, true, false, {}}, false);
             require(setup.window_ && setup.countdown_ != Clock::time_point{}, "Missing English did not offer preparation.");
             require(!setup.process_, "Test attempted actual Windows servicing.");
             setup.close();
             result({false, false, false, {}}, false);
             require(setup.window_ && setup.countdown_ == Clock::time_point{}, "Missing identity triggered language installation.");
             setup.close();
             result({true, false, false, L"Simulated API failure"}, false);
             require(setup.window_ && setup.countdown_ == Clock::time_point{}, "Unknown language state triggered automatic download.");
             setup.close();
             result({true, true, false, {}}, true);
             require(!setup.window_, "Deferred preparation nagged again.");
         });
    test(L"OCR 准备面板显示真实组件数量且关闭不发起系统操作",
         [&]
         {
             auto &setup = *ocrSetup_;
             setup.readiness_ = {true, true, false, {}};
             setup.progress_ = {true, false, false, L"installing", L"Language.OCR~~~en-US~0.0.1.0", {},
                                {L"Installed", L"Installed", L"Installed", L"NotPresent"}};
             setup.show(false);
             RECT client{};
             GetClientRect(setup.window_, &client);
             for (int id : {301, 302, 303, 304, 310, 311, 312, 313, 314, 315, 316, 317})
             {
                 auto child = GetDlgItem(setup.window_, id);
                 require(child != nullptr, "Preparation control missing.");
                 RECT rect{};
                 GetWindowRect(child, &rect);
                 MapWindowPoints(nullptr, setup.window_, reinterpret_cast<POINT *>(&rect), 2);
                 require(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom,
                         "Preparation control clipped outside client area.");
             }
             require(SendDlgItemMessage(setup.window_, 315, PBM_GETPOS, 0, 0) == 3,
                     "Preparation used fabricated progress instead of verified components.");
             Dib preview(client.right, client.bottom);
             FillRect(preview.dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
             // Paint only our own controls, without showing or capturing the
             // user's desktop. PrintWindow does not paint a hidden top-level.
             for (HWND child = GetWindow(setup.window_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
             {
                 RECT rect{};
                 GetWindowRect(child, &rect);
                 MapWindowPoints(nullptr, setup.window_, reinterpret_cast<POINT *>(&rect), 2);
                 int saved = SaveDC(preview.dc);
                 SetViewportOrgEx(preview.dc, rect.left, rect.top, nullptr);
                 IntersectClipRect(preview.dc, 0, 0, rect.right - rect.left, rect.bottom - rect.top);
                 SendMessage(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(preview.dc), PRF_CLIENT | PRF_ERASEBKGND);
                 RestoreDC(preview.dc, saved);
             }
             savePng(graphics, preview.image(), file(L"ui-ocr-setup.local.png"));
             SendMessage(setup.window_, WM_CLOSE, 0, 0);
             require(!setup.window_ && !setup.process_, "Closing the preparation panel started an operation.");
         });
    test(L"OCR 面板在 DPI 改变后仍完整显示并支持 Esc 关闭",
         [&]
         {
             auto &setup = *ocrSetup_;
             setup.show(false);
             UINT originalDpi = GetDpiForWindow(setup.window_);
             RECT outer{};
             GetWindowRect(setup.window_, &outer);
             for (UINT dpi : {96u, 144u, 192u, originalDpi})
             {
                 // Synthetic WM_DPICHANGED scales the client controls; the OS
                 // non-client DPI stays real until a genuine monitor change.
                 RECT size{0, 0, MulDiv(580, dpi, 96), MulDiv(466, dpi, 96)};
                 AdjustWindowRectExForDpi(&size, static_cast<DWORD>(GetWindowLongPtr(setup.window_, GWL_STYLE)), FALSE,
                     static_cast<DWORD>(GetWindowLongPtr(setup.window_, GWL_EXSTYLE)), GetDpiForWindow(setup.window_));
                 RECT suggested{outer.left, outer.top, outer.left + size.right - size.left, outer.top + size.bottom - size.top};
                 SendMessage(setup.window_, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
                 GetWindowRect(setup.window_, &outer);
                 RECT client{};
                 GetClientRect(setup.window_, &client);
                 for (int id : {301, 302, 303, 304, 316, 317})
                 {
                     RECT rect{};
                     GetWindowRect(GetDlgItem(setup.window_, id), &rect);
                     MapWindowPoints(nullptr, setup.window_, reinterpret_cast<POINT *>(&rect), 2);
                     require(rect.left >= 0 && rect.top >= 0 && rect.right <= client.right && rect.bottom <= client.bottom,
                             "DPI change clipped preparation content.");
                 }
             }
             MSG message{};
             message.hwnd = setup.window_;
             message.message = WM_KEYDOWN;
             message.wParam = VK_ESCAPE;
             require(setup.preTranslate(message) && !setup.window_, "Esc did not close modeless preparation panel.");
         });
    test(L"截屏会暂停首次 OCR 自动准备并隐藏准备面板",
         [&]
         {
             auto &setup = *ocrSetup_;
             setup.show(false);
             setup.countdown_ = Clock::now() + std::chrono::seconds(8);
             setup.automatic_ = true;
             setup.pauseForCapture();
             require(setup.countdown_ == Clock::time_point{} && !setup.automatic_ && !IsWindowVisible(setup.window_),
                     "OCR setup interrupted region capture.");
             setup.close();
         });
    test(L"OCR 准备后台进程可重连、关闭面板不中断并在结束后验证引擎",
         [&]
         {
             auto &setup = *ocrSetup_;
             auto originalDiagnostics = setup.diagnostics_;
             auto restoreAction = [&]
             {
                 if (setup.process_)
                 {
                     WaitForSingleObject(setup.process_, 5000);
                     CloseHandle(setup.process_);
                     setup.process_ = nullptr;
                 }
                 setup.close();
                 setup.diagnostics_ = originalDiagnostics;
                 setup.progress_ = {};
                 setup.lastReport_.clear();
                 setup.notice_.clear();
             };
             struct Restore
             {
                 std::function<void()> action;
                 ~Restore() { action(); }
             } restore{restoreAction};
             setup.diagnostics_ = folder / L"ocr-worker-fixture";
             auto run = setup.diagnostics_ / L"11111111-2222-3333-4444-555555555555";
             std::filesystem::create_directories(run);
             wchar_t system[MAX_PATH]{};
             GetSystemDirectory(system, MAX_PATH);
             std::wstring ps = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
             auto command = L"\"" + ps + L"\" -NoLogo -NoProfile -NonInteractive -Command \"Start-Sleep -Milliseconds 1800\"";
             STARTUPINFO startup{sizeof(startup)};
             startup.dwFlags = STARTF_USESHOWWINDOW;
             startup.wShowWindow = SW_HIDE;
             PROCESS_INFORMATION process{};
             require(CreateProcess(ps.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                    run.c_str(), &startup, &process), "Read-only worker fixture did not start.");
             CloseHandle(process.hThread);
             setup.process_ = process.hProcess;
             setup.started_ = setup.updated_ = Clock::now();
             setup.report_ = run / L"ocr-prerequisites.json";
             auto reportText = [](bool done)
             {
                 std::string text = "{\"schema\":1,\"phase\":\"" + std::string(done ? "completed" : "installing") +
                     "\",\"success\":" + (done ? "true" : "false") + ",\"capabilities\":[";
                 std::array<wchar_t const *, 4> names{L"Language.Basic~~~zh-CN~0.0.1.0", L"Language.OCR~~~zh-CN~0.0.1.0",
                     L"Language.Basic~~~en-US~0.0.1.0", L"Language.OCR~~~en-US~0.0.1.0"};
                 for (size_t i = 0; i < names.size(); ++i)
                 {
                     if (i) text += ",";
                     text += "{\"name\":" + json(names[i]) + ",\"state\":\"" +
                         (i == 3 && !done ? "NotPresent" : "Installed") + "\"}";
                 }
                 return text + "],\"errors\":[]}";
             };
             std::ofstream(setup.report_, std::ios::binary) << reportText(false);
             setup.persistSession();
             {
                 OcrSetup resumed(instance_, root_, true, [](HWND) {}, [] {});
                 resumed.diagnostics_ = setup.diagnostics_;
                 resumed.resumeSession();
                 require(resumed.process_ && GetProcessId(resumed.process_) == process.dwProcessId &&
                             resumed.progress_.installed() == 3,
                         "Existing preparation could not be observed without restarting it.");
             }
             auto session = setup.diagnostics_ / L"active-ocr-setup.json";
             std::ofstream(session, std::ios::binary | std::ios::trunc) <<
                 "{\"runId\":\"../untrusted\",\"pid\":" + std::to_string(process.dwProcessId) + ",\"created\":\"0\"}";
             {
                 OcrSetup forged(instance_, root_, true, [](HWND) {}, [] {});
                 forged.diagnostics_ = setup.diagnostics_;
                 forged.resumeSession();
                 require(!forged.process_ && forged.report_.empty(), "Unsafe session path was accepted.");
             }
             std::ofstream(session, std::ios::binary | std::ios::trunc) <<
                 "{\"runId\":\"11111111-2222-3333-4444-555555555555\",\"pid\":" +
                 std::to_string(process.dwProcessId) + ",\"created\":\"0\"}";
             {
                 OcrSetup recycled(instance_, root_, true, [](HWND) {}, [] {});
                 recycled.diagnostics_ = setup.diagnostics_;
                 recycled.resumeSession();
                 require(!recycled.process_, "PID without a matching creation time was accepted.");
             }
             setup.persistSession();
             setup.show(false);
             setup.tick();
             require(setup.progress_.installed() == 3 && !setup.progress_.success, "Running progress was not read.");
             setup.close();
             require(setup.process_ && WaitForSingleObject(setup.process_, 0) == WAIT_TIMEOUT,
                     "Closing the panel terminated preparation.");
             std::ofstream(setup.report_, std::ios::binary | std::ios::trunc) << reportText(true);
             auto deadline = Clock::now() + std::chrono::seconds(15);
             while ((setup.process_ || setup.checking_) && Clock::now() < deadline)
             {
                 setup.tick();
                 pump();
                 Sleep(10);
             }
             require(!setup.process_ && !setup.checking_ && setup.progress_.success &&
                         setup.readiness_.identity == !packageIdentity().empty(),
                     "Completed preparation did not finish with a real engine check.");
             command = L"\"" + ps + L"\" -NoLogo -NoProfile -NonInteractive -Command \"exit 7\"";
             process = {};
             require(CreateProcess(ps.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                    run.c_str(), &startup, &process), "Failure fixture did not start.");
             CloseHandle(process.hThread);
             setup.process_ = process.hProcess;
             setup.lastReport_.clear();
             deadline = Clock::now() + std::chrono::seconds(15);
             while ((setup.process_ || setup.checking_) && Clock::now() < deadline)
             {
                 setup.tick();
                 pump();
                 Sleep(10);
             }
             require(!setup.process_ && !setup.checking_ && !setup.progress_.success &&
                         setup.notice_.find(L"未完成") != std::wstring::npos,
                     "Nonzero worker exit was hidden by a previously successful progress report.");
         });
    auto desktop = demoImage(graphics);
    auto capture = std::make_unique<Canvas>(*this, nextId_++, desktop, RECT{-1000, -300, 0, 220});
    require(capture->create(false), "Capture window failed.");
    auto *edit = capture.get();
    windows_.push_back(std::move(capture));
    auto pointer = [&](UINT msg, int x, int y)
    { SendMessage(edit->hwnd(), msg, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(x, y)); };
    test(L"框选拖动期间保持同一高对比十字光标",
         [&]
         {
             auto original = GetCursor();
             pointer(WM_LBUTTONDOWN, 30, 60);
             auto cursor = GetCursor();
             require(cursor == edit->crosshair_.get(GetDpiForWindow(edit->hwnd())), "Capture started with system crosshair.");
             SetCursor(LoadCursor(nullptr, IDC_ARROW));
             pointer(WM_MOUSEMOVE, 60, 80);
             require(GetCursor() == cursor, "Captured mouse movement lost the high-contrast cursor.");
             edit->cancelGesture();
             SetCursor(original);
         });
    test(L"冻结画面框选",
         [&]
         {
             pointer(WM_LBUTTONDOWN, 40, 110);
             pointer(WM_MOUSEMOVE, 960, 400);
             pointer(WM_LBUTTONUP, 960, 400);
             require(edit->doc_ && edit->doc_->base->width == 920 && edit->doc_->base->height == 290,
                     "Region capture dimensions incorrect.");
             edit->renderUiPreview(file(L"ui-capture.local.png"));
         });
    test(L"边框调整选区",
         [&]
         {
             pointer(WM_LBUTTONDOWN, 960, 250);
             pointer(WM_MOUSEMOVE, 980, 250);
             pointer(WM_LBUTTONUP, 980, 250);
             require(edit->doc_->base->width == 940, "Region resize failed.");
         });
    test(L"箭头与直线鼠标交互",
         [&]
         {
             edit->command(2);
             pointer(WM_LBUTTONDOWN, 150, 150);
             pointer(WM_MOUSEMOVE, 350, 175);
             pointer(WM_LBUTTONUP, 350, 175);
             edit->command(3);
             pointer(WM_LBUTTONDOWN, 150, 190);
             pointer(WM_MOUSEMOVE, 350, 190);
             pointer(WM_LBUTTONUP, 350, 190);
             require(edit->doc_->marks.size() == 2, "Pointer release lost a stroke.");
         });
    test(L"记号笔与马赛克鼠标交互",
         [&]
         {
             edit->command(6);
             edit->ink_ = 0xffc13a;
             pointer(WM_LBUTTONDOWN, 150, 310);
             pointer(WM_MOUSEMOVE, 300, 310);
             pointer(WM_LBUTTONUP, 300, 310);
             edit->command(5);
             pointer(WM_LBUTTONDOWN, 650, 310);
             pointer(WM_MOUSEMOVE, 860, 368);
             pointer(WM_LBUTTONUP, 860, 368);
             require(edit->doc_->marks.size() == 4 && edit->doc_->redactions().size() == 1,
                     "Marker/mosaic commit failed.");
         });
    test(L"原生中文文本控件提交",
         [&]
         {
             edit->command(4);
             edit->ink_ = 0xef5a62;
             pointer(WM_LBUTTONDOWN, 400, 150);
             require(edit->edit_ != nullptr, "Native edit missing.");
             SetWindowText(edit->edit_, L"中文标注 ABC");
             SendMessage(edit->hwnd(), WM_FINISH_TEXT, TRUE, 0);
             require(edit->doc_->marks.size() == 5 && edit->doc_->marks.back().text == L"中文标注 ABC",
                     "Text not committed.");
         });
    test(L"窗口撤销重做与导出",
         [&]
         {
             auto before = edit->doc_->rendered->pixels;
             edit->command(10);
             require(edit->doc_->marks.size() == 4, "UI undo failed.");
             edit->command(11);
             require(edit->doc_->rendered->pixels == before, "UI redo differs.");
             edit->renderPreview(file(L"ui-annotated.local.png"));
             edit->renderUiPreview(file(L"ui-toolbar.local.png"));
         });
    auto snapshot = std::make_shared<OcrSnapshot>();
    snapshot->revision = edit->doc_->revision;
    snapshot->width = edit->doc_->base->width;
    snapshot->height = edit->doc_->base->height;
    snapshot->epoch = edit->ocrEpoch_;
    snapshot->language = L"test-fixture";
    snapshot->status = L"拖选文字，Ctrl+C 复制";
    snapshot->tokens = {{L"轻", {50, 100, 20, 22}, 0},     {L"量", {75, 100, 20, 22}, 0},
                        {L"截", {100, 100, 20, 22}, 0},    {L"图", {125, 100, 20, 22}, 0},
                        {L"Select", {50, 145, 65, 25}, 1}, {L"words", {125, 145, 70, 25}, 1}};
    test(L"窗口跨行正向与反向选字",
         [&]
         {
             edit->receive(snapshot);
             edit->command(7);
             auto a = edit->view_.toView(Point{80, 110}), b = edit->view_.toView(Point{150, 155});
             pointer(WM_LBUTTONDOWN, static_cast<int>(a.x), static_cast<int>(a.y));
             pointer(WM_MOUSEMOVE, static_cast<int>(b.x), static_cast<int>(b.y));
             pointer(WM_LBUTTONUP, static_cast<int>(b.x), static_cast<int>(b.y));
             require(edit->ocr_->selection(edit->anchor_, edit->caret_) == L"量截图\r\nSelect words",
                     "UI multiline select failed.");
             pointer(WM_LBUTTONDOWN, static_cast<int>(b.x), static_cast<int>(b.y));
             pointer(WM_MOUSEMOVE, static_cast<int>(a.x), static_cast<int>(a.y));
             pointer(WM_LBUTTONUP, static_cast<int>(a.x), static_cast<int>(a.y));
             require(edit->ocr_->selection(edit->anchor_, edit->caret_) == L"量截图\r\nSelect words",
                     "UI reverse select failed.");
             edit->renderUiPreview(file(L"ui-selection.local.png"));
         });
    test(L"遮挡立即清除旧选字与旧 OCR",
         [&]
         {
             auto stale = std::make_shared<OcrSnapshot>(*snapshot);
             edit->command(5);
             pointer(WM_LBUTTONDOWN, 110, 200);
             require(!edit->ocr_ && edit->anchor_ < 0, "Selection survived redaction start.");
             pointer(WM_MOUSEMOVE, 180, 240);
             pointer(WM_LBUTTONUP, 180, 240);
             edit->receive(stale);
             require(!edit->ocr_, "Stale result accepted.");
         });
    test(L"五张独立贴图与显隐",
         [&]
         {
             for (int i = 0; i < 5; ++i)
                 pin(*edit->doc_);
             require(windows_.size() == 6, "Pin count incorrect.");
             togglePins();
             require(!pinsVisible_, "Hide state failed.");
             togglePins();
             require(pinsVisible_, "Show state failed.");
             for (auto const &w : windows_)
                 if (w->pinned())
                     require(w->doc_->base == edit->doc_->base && w->doc_.get() != edit->doc_.get(),
                             "Pin document independence failed.");
         });
    Canvas *pinWindow = windows_[1].get();
    test(L"贴图 50%、100%、200% 原图选字",
         [&]
         {
             auto s = std::make_shared<OcrSnapshot>(*snapshot);
             s->revision = pinWindow->doc_->revision;
             s->epoch = pinWindow->ocrEpoch_;
             pinWindow->receive(s);
             for (float z : {.5f, 1.f, 2.f})
             {
                 pinWindow->zoom(z / pinWindow->view_.zoom, {2, 2});
                 // Actual OCR filters the mosaic at x=70..140, y=90..130.
                 // Keep this synthetic selection outside that redaction too.
                 auto p = pinWindow->view_.toView(Point{60, 110});
                 pinWindow->pointerDown(p, 0);
                 pinWindow->pointerUp(p);
                 require(pinWindow->anchor_ == 0 && pinWindow->caret_ == 0, "Pin zoom text hit mismatch.");
             }
             pinWindow->zoom(1.f / pinWindow->view_.zoom, {2, 2});
             pinWindow->command(20);
             pinWindow->renderUiPreview(file(L"ui-pin.local.png"));
         });
    test(L"关闭窗口后丢弃 OCR 结果",
         [&]
         {
             uint64_t id = windows_.back()->id();
             windows_.back()->close();
             pump();
             require(find(id) == nullptr, "Closed canvas retained.");
             if (auto *w = find(id))
                 w->receive(snapshot);
             require(windows_.size() == 5, "Closed result reopened window.");
         });
    windows_.clear();
    pump();
    DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
          userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    test(L"连续 100 次框选取消的句柄生命周期",
         [&]
         {
             for (int i = 0; i < 100; ++i)
             {
                 auto canvas = std::make_unique<Canvas>(*this, nextId_++, Image(64, 64), RECT{0, 0, 64, 64});
                 require(canvas->create(false), "Stress capture window failed.");
                 auto *w = canvas.get();
                 windows_.push_back(std::move(canvas));
                 w->pointerDown({2, 2}, 0);
                 w->pointerUp({48, 48});
                 w->close();
                 pump();
             }
             DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
                   userAfter = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
             require(windows_.empty() && gdiAfter <= gdiBefore + 3 && userAfter <= userBefore + 3,
                     "Window/GDI handles grew during capture cycle.");
         });
    if (!packageIdentity().empty())
    {
        test(L"实际 OCR 词框在原图窗口拖选",
             [&]
             {
                 auto canvas =
                     std::make_unique<Canvas>(*this, nextId_++, std::make_unique<Document>(demoImage(graphics)), true);
                 require(canvas->create(false), "OCR workflow window failed.");
                 auto *window = canvas.get();
                 windows_.push_back(std::move(canvas));
                 auto result = recognize(
                     {window->id(), window->doc_->revision, window->doc_->rendered, {}, L"", window->ocrEpoch_});
                 require(result->tokens.size() >= 8, "Actual OCR returned too few tokens.");
                 window->receive(result);
                 auto center = [&](int i)
                 {
                     auto const &token = result->tokens[i];
                     auto b = token.box;
                     return window->view_.toView(rotate({b.x + b.w / 2, b.y + b.h / 2},
                                                        {result->width / 2.f, result->height / 2.f},
                                                        token.angle.value_or(result->angle)));
                 };
                 window->pointerDown(center(1), 0);
                 window->pointerMove(center(4), MK_LBUTTON);
                 window->pointerUp(center(4));
                 require(window->anchor_ == 1 && window->caret_ == 4 &&
                             result->selection(window->anchor_, window->caret_) == result->selection(1, 4),
                         "Actual OCR cannot be selected on its image.");
                 window->renderUiPreview(file(L"ui-ocr-real.local.png"));
                 if (result->language.find(L"en-US") != std::wstring::npos)
                 {
                     int first = -1, last = -1;
                     for (size_t i = 0; i < result->tokens.size(); ++i)
                         if (result->tokens[i].text == L"Select")
                         {
                             first = static_cast<int>(i);
                             last = first;
                             while (last + 1 < static_cast<int>(result->tokens.size()) &&
                                    result->tokens[last + 1].line == result->tokens[first].line)
                                 ++last;
                             break;
                         }
                     require(first >= 0, "English recognizer did not return the sample line.");
                     for (float scale : {.5f, 1.f, 2.f})
                     {
                         window->zoom(scale / window->view_.zoom, {2, 2});
                         for (bool reverse : {false, true})
                         {
                             window->pointerDown(center(reverse ? last : first), 0);
                             window->pointerMove(center(reverse ? first : last), MK_LBUTTON);
                             window->pointerUp(center(reverse ? first : last));
                             require(result->selection(window->anchor_, window->caret_) ==
                                         L"Select words on this image and copy them.",
                                     "English OCR cannot be dragged on its image at this zoom.");
                         }
                     }
                     window->zoom(1.f / window->view_.zoom, {2, 2});
                     window->renderUiPreview(file(L"ui-ocr-english.local.png"));
                     englishDragVerified = true;
                 }
                 window->close();
                 pump();
             });
    }
    cases += "]";
    writeReport(report, "{\"suite\":\"ui\",\"passed\":" + std::to_string(passed) +
                            ",\"failed\":" + std::to_string(failed) +
                            ",\"syntheticDesktop\":true,\"clipboardUntouched\":true,\"englishRecognizerDrag\":" +
                            (englishDragVerified ? "true" : "false") + ",\"cases\":" + cases + "}");
    return failed ? 1 : 0;
}
} // namespace snap
