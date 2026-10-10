#include "app.h"
namespace snap
{
std::string json(std::wstring const &);
void writeReport(std::wstring const &, std::string const &);
void require(bool, char const *);
namespace
{
Image pattern(int w, int h)
{
    Image image(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            auto p = image.pixels.data() + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = static_cast<uint8_t>((x * 13 + y * 7) % 256);
            p[1] = static_cast<uint8_t>((x * 3 + y * 11) % 256);
            p[2] = static_cast<uint8_t>((x + y) % 256);
        }
    return image;
}
bool closeEnough(float a, float b)
{
    return std::abs(a - b) < .05f;
}
bool nearColor(uint8_t const *pixel, unsigned rgb, int tolerance = 12)
{
    return std::abs(static_cast<int>(pixel[0]) - static_cast<int>(rgb & 255)) <= tolerance &&
           std::abs(static_cast<int>(pixel[1]) - static_cast<int>((rgb >> 8) & 255)) <= tolerance &&
           std::abs(static_cast<int>(pixel[2]) - static_cast<int>((rgb >> 16) & 255)) <= tolerance;
}
double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
struct KeyboardState
{
    BYTE original[256]{};
    KeyboardState()
    {
        GetKeyboardState(original);
    }
    ~KeyboardState()
    {
        SetKeyboardState(original);
    }
    void set(int key = 0)
    {
        BYTE state[256]{};
        if (key)
            state[key] = 0x80;
        SetKeyboardState(state);
    }
};
} // namespace
int App::regressionTests(std::wstring const &report)
{
    int passed = 0, failed = 0;
    std::string cases = "[", performance = "[";
    auto folder = std::filesystem::path(report).parent_path();
    std::filesystem::create_directories(folder);
    auto file = [&](wchar_t const *name) { return (folder / name).wstring(); };
    HWND previousForeground = GetForegroundWindow();
    UINT defaultFont = config.textSizeTenths;
    double vectorReferenceMse = 0, bitmapReferenceMse = 0;
    bool cubicZoomVerified = false;
    auto pump = [&]
    {
        MSG msg{};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            if (msg.message != WM_QUIT && !preTranslate(msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
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
        windows_.clear();
        pump();
        config.autoOcr = false;
        config.textSizeTenths = defaultFont;
        copyText_ = snap::copyText;
    };
    auto pin = [&](Image image, bool tools = false)
    {
        auto canvas = std::make_unique<Canvas>(*this, nextId_++, std::make_unique<Document>(std::move(image)), tools);
        require(canvas->create(false), "Pin creation failed.");
        auto w = canvas.get();
        windows_.push_back(std::move(canvas));
        w->view_.zoom = 1.f;
        w->fitPin(true);
        w->layout();
        return w;
    };
    auto capture = [&](int width, int height, int x = 0, int y = 0)
    {
        auto canvas =
            std::make_unique<Canvas>(*this, nextId_++, pattern(width, height), RECT{x, y, x + width, y + height});
        require(canvas->create(false), "Capture creation failed.");
        auto w = canvas.get();
        windows_.push_back(std::move(canvas));
        return w;
    };
    auto pointer = [&](Canvas *w, UINT msg, Point p)
    {
        SendMessage(w->hwnd_, msg, msg == WM_LBUTTONUP ? 0 : MK_LBUTTON,
                    MAKELPARAM(static_cast<int>(std::round(p.x)), static_cast<int>(std::round(p.y))));
    };
    auto drag = [&](Canvas *w, Point a, Point b)
    {
        pointer(w, WM_LBUTTONDOWN, w->view_.toView(a));
        pointer(w, WM_MOUSEMOVE, w->view_.toView(b));
        pointer(w, WM_LBUTTONUP, w->view_.toView(b));
    };
    auto keyboard = [&](HWND hwnd, UINT key)
    {
        PostMessage(hwnd, WM_KEYDOWN, key, 1 | (MapVirtualKey(key, MAPVK_VK_TO_VSC) << 16));
        pump();
        PostMessage(hwnd, WM_KEYUP, key, 0xc0000001 | (MapVirtualKey(key, MAPVK_VK_TO_VSC) << 16));
        pump();
    };
    auto toolbarVisible = [&](Canvas *w)
    {
        auto area = w->availableArea();
        for (auto b : {w->toolbar_, w->statusBox_})
            require(b.x >= area.x - .5f && b.y >= area.y - .5f && b.x + b.w <= area.x + area.w + .5f &&
                        b.y + b.h <= area.y + area.h + .5f,
                    "Toolbar/status escaped the visible monitor client intersection.");
        require(w->buttons_.size() == 17, "A tool disappeared from the responsive toolbar.");
        for (auto const &b : w->buttons_)
            require(w->toolbar_.contains({b.box.x + .1f, b.box.y + .1f}) &&
                        w->toolbar_.contains({b.box.x + b.box.w - .1f, b.box.y + b.box.h - .1f}),
                    "A tool is not fully reachable inside the toolbar.");
    };
    for (auto kind : {Tool::Arrow, Tool::Line, Tool::Text, Tool::Mosaic, Tool::Marker, Tool::Rectangle, Tool::Ellipse})
    {
        auto name = L"贴图隐藏工具栏锁定标注，Tab 解锁并保留移动与撤销 / " +
                    std::to_wstring(static_cast<int>(kind));
        test(name.c_str(),
             [&]
             {
                 KeyboardState state;
                 state.set();
                 auto w = pin(pattern(650, 420));
                 Annotation mark{kind, {100, 100}, {250, 180}, {}, L"锁定文字 ABC", 0xe6525c, 3};
                 if (kind == Tool::Marker)
                     mark.path = {mark.from, mark.to};
                 if (kind == Tool::Text)
                     measureText(graphics, mark, 400);
                 w->doc_->add(mark);
                 w->changed();
                 auto pixels = w->doc_->rendered->pixels;
                 auto revision = w->doc_->revision, history = w->doc_->undoEdits.size();
                 auto bounds = annotationBounds(mark);
                 Point hit = kind == Tool::Rectangle ? Point{bounds.x + bounds.w / 2, bounds.y}
                             : kind == Tool::Ellipse ? Point{bounds.x + bounds.w, bounds.y + bounds.h / 2}
                                                     : Point{bounds.x + bounds.w / 2, bounds.y + bounds.h / 2};
                 require(w->objectAt(hit) == 0 && !w->showTools_, "Locked annotation fixture does not hit.");
                 auto p = w->view_.toView(hit);
                 pointer(w, WM_LBUTTONDOWN, p);
                 require(w->moving_ && !w->objectDragging_ && !w->stroke_ && w->selected_ < 0,
                         "Hidden-toolbar pin selected or began moving an annotation instead of the whole image.");
                 pointer(w, WM_LBUTTONUP, p);
                 pointer(w, WM_LBUTTONDBLCLK, p);
                 require(!w->edit_ && !w->closing_, "Locked annotation double-click edited text or closed the pin.");
                 // An obsolete selection must not bypass the lock through a key or a control point.
                 w->selected_ = 0;
                 require(w->handleAt(w->view_.toView(w->objectHandles(mark).back())) < 0,
                         "A locked annotation exposes resize handles.");
                 for (UINT key : {VK_DELETE, VK_BACK, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN})
                     w->key(key, false, false);
                 w->key('Z', true, false);
                 w->key('Y', true, false);
                 for (UINT key : {'V', 'A', 'L', 'R', 'E', 'T', 'M', 'P'})
                     w->key(key, false, false);
                 w->command(13);
                 require(!w->setFontPoints(30) && !w->fontPopup_, "A locked pin can still change annotation style.");
                 w->beginObject(0, -1, p);
                 w->beginText({300, 200});
                 require(!w->edit_ && !w->stroke_ && !w->objectDragging_ && w->doc_->marks.size() == 1 &&
                             w->doc_->revision == revision && w->doc_->undoEdits.size() == history &&
                             w->doc_->rendered->pixels == pixels,
                         "Locked editing mutated annotation pixels or history.");
                 w->selected_ = -1;
                 w->key(VK_TAB, false, false);
                 require(w->showTools_ && w->doc_->rendered->pixels == pixels,
                         "Showing the toolbar changed the committed image.");
                 w->command(1);
                 drag(w, hit, {hit.x + 25, hit.y + 15});
                 require(w->selected_ == 0 && closeEnough(w->doc_->marks[0].from.x, mark.from.x + 25) &&
                             closeEnough(w->doc_->marks[0].from.y, mark.from.y + 15),
                         "Showing the toolbar did not unlock annotation movement.");
                 revision = w->doc_->revision;
                 history = w->doc_->undoEdits.size();
                 w->key(VK_TAB, false, false);
                 require(!w->showTools_ && w->selected_ < 0 && w->tool_ == Tool::Select && !w->stroke_,
                         "Hiding the toolbar did not clear editing state and restore OCR selection.");
                 w->key('Z', true, false);
                 require(w->doc_->revision == revision && w->doc_->undoEdits.size() == history,
                         "Undo changed a locked pin.");
                 w->command(20); // Same transition used by the context-menu toolbar item.
                 w->command(10);
                 require(w->doc_->rendered->pixels == pixels, "Unlocking lost the previous annotation history.");
                 if (kind == Tool::Text)
                 {
                     pointer(w, WM_LBUTTONDBLCLK, w->view_.toView(hit));
                     require(w->edit_ != nullptr, "Unlocked text can no longer be reopened.");
                     w->finishText(false);
                 }
             });
    }
    test(L"隐藏工具栏提交正在绘制的对象一次，迟到的鼠标释放不能继续修改",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->command(2);
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{100, 100}));
             pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{250, 180}));
             w->key(VK_TAB, false, false);
             require(!w->showTools_ && w->doc_->marks.size() == 1 && w->doc_->undoEdits.size() == 1 &&
                         !w->stroke_ && !w->dragging_ && w->selected_ < 0 && GetCapture() != w->hwnd_,
                     "Hiding the toolbar lost or retained a live drawing.");
             auto revision = w->doc_->revision;
             auto pixels = w->doc_->rendered->pixels;
             pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{350, 220}));
             pointer(w, WM_LBUTTONUP, w->view_.toView(Point{350, 220}));
             require(w->doc_->revision == revision && w->doc_->rendered->pixels == pixels,
                     "A late pointer release changed the locked drawing.");
             w->key(VK_TAB, false, false);
             w->key('Z', true, false);
             require(w->doc_->marks.empty(), "Committed drawing cannot be undone after unlocking.");
         });
    test(L"移动或缩放标注途中隐藏工具栏提交一次，锁定不显示残留控制点",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->doc_->add({Tool::Rectangle, {100, 100}, {250, 180}, {}, L"", 0xe6525c, 3});
             w->changed();
             auto original = w->doc_->rendered->pixels;
             w->command(1);
             auto p = w->view_.toView(Point{175, 100});
             pointer(w, WM_LBUTTONDOWN, p);
             pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{200, 120}));
             require(w->objectDragging_, "Move fixture did not begin editing an existing annotation.");
             w->key(VK_TAB, false, false);
             require(!w->showTools_ && !w->objectDragging_ && !w->stroke_ && w->selected_ < 0 &&
                         w->doc_->undoEdits.size() == 2 && closeEnough(w->doc_->marks[0].from.x, 125) &&
                         closeEnough(w->doc_->marks[0].from.y, 120),
                     "Hiding tools did not commit the live annotation move exactly once.");
             pointer(w, WM_LBUTTONUP, w->view_.toView(Point{300, 220}));
             w->key(VK_TAB, false, false);
             w->selected_ = 0;
             auto handle = w->objectHandles(w->doc_->marks[0])[4];
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(handle));
             pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{handle.x + 20, handle.y + 30}));
             require(w->objectDragging_ && w->objectHandle_ == 4, "Resize fixture missed the control point.");
             w->key(VK_TAB, false, false);
             auto resized = annotationBounds(w->doc_->marks[0]);
             require(!w->showTools_ && !w->objectDragging_ && !w->stroke_ && w->doc_->undoEdits.size() == 3 &&
                         closeEnough(resized.w, 170) && closeEnough(resized.h, 110),
                     "Hiding tools did not commit the live resize exactly once.");
             auto revision = w->doc_->revision;
             pointer(w, WM_LBUTTONUP, w->view_.toView(Point{400, 320}));
             require(w->doc_->revision == revision, "Late mouse-up changed a locked resized annotation.");
             auto lockedPath = file(L"regression-pin-locked.local.png");
             w->renderUiPreview(lockedPath);
             w->selected_ = 0;
             auto stalePath = file(L"regression-pin-locked-stale-selection.local.png");
             w->renderUiPreview(stalePath);
             require(loadImage(graphics, lockedPath).pixels == loadImage(graphics, stalePath).pixels,
                     "A stale object selection still draws edit handles on a locked pin.");
             w->selected_ = -1;
             w->key(VK_TAB, false, false);
             w->selected_ = 0;
             w->renderUiPreview(file(L"regression-pin-unlocked.local.png"));
             w->command(10);
             w->command(10);
             require(w->doc_->rendered->pixels == original, "Unlocking lost the live move/resize undo history.");
         });
    test(L"隐藏工具栏提交输入文字并关闭字号面板，再显示后可修改文字",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->command(4);
             w->beginText({100, 100});
             SetWindowText(w->edit_, L"完成后锁定");
             w->command(13);
             require(w->edit_ && w->fontPopup_, "Text editor and size picker were not opened.");
             w->command(20);
             require(!w->showTools_ && !w->edit_ && !w->fontPopup_ && !w->stroke_ && w->selected_ < 0 &&
                         w->doc_->marks.size() == 1 && w->doc_->marks[0].text == L"完成后锁定",
                     "Hiding tools failed to commit text or left an editing window open.");
             auto revision = w->doc_->revision;
             w->editText(0);
             SendMessage(w->hwnd_, WM_FINISH_TEXT, TRUE, 0);
             require(!w->edit_ && w->doc_->revision == revision, "Text editor reopened after locking.");
             w->key(VK_TAB, false, false);
             w->editText(0);
             require(w->edit_ != nullptr, "Text editor did not unlock.");
             SetWindowText(w->edit_, L"重新编辑成功");
             w->command(20);
             require(w->doc_->marks[0].text == L"重新编辑成功" && w->doc_->undoEdits.size() == 2,
                     "Reedited text did not commit exactly once before locking.");
         });
    test(L"锁定贴图仍可在文字标注上 OCR 拖选复制，缩放与切换工具栏保留选字",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             Annotation mark{Tool::Text, {80, 80}, {}, {}, L"锁定文字 ABC", 0xe6525c, 3};
             measureText(graphics, mark, 400);
             w->doc_->add(mark);
             w->changed();
             w->command(4);
             auto snapshot = std::make_shared<OcrSnapshot>();
             snapshot->revision = w->doc_->revision;
             snapshot->epoch = w->ocrEpoch_;
             snapshot->width = 650;
             snapshot->height = 420;
             snapshot->tokens = {{L"锁定", {82, 82, 40, 20}, 0}, {L"文字", {126, 82, 40, 20}, 0}};
             w->receive(snapshot);
             auto revision = w->doc_->revision;
             w->command(20);
             require(w->ocr_ == snapshot && w->tool_ == Tool::Select, "Locking discarded OCR or kept the Text tool.");
             std::wstring copied;
             copyText_ = [&](HWND, std::wstring const &value)
             {
                 copied = value;
                 return true;
             };
             for (float scale : {.5f, 1.f, 2.f})
             {
                 w->zoom(scale / w->view_.zoom, {2, 2});
                 drag(w, {90, 92}, {150, 92});
                 w->key('C', true, false);
                 require(copied == L"锁定文字" && w->selected_ < 0 && w->ocr_ == snapshot &&
                             w->doc_->revision == revision && !w->edit_,
                         "Locked annotations blocked OCR selection/copy or scaling invalidated it.");
                 auto p = w->view_.toView(Point{90, 92});
                 pointer(w, WM_LBUTTONDBLCLK, p);
                 require(w->anchor_ == 0 && !w->edit_ && !w->closing_, "Double-click on locked OCR text began editing.");
                 pointer(w, WM_LBUTTONUP, p);
                 w->command(20);
                 w->command(20);
                 require(w->anchor_ == 0 && w->ocr_ == snapshot, "Toolbar toggling cleared valid OCR selection.");
                 p = w->view_.toView(Point{350, 250});
                 pointer(w, WM_LBUTTONDOWN, p);
                 require(w->moving_ && w->anchor_ < 0 && w->caret_ < 0, "Blank pin click no longer clears OCR selection.");
                 pointer(w, WM_LBUTTONUP, p);
             }
         });
    test(L"截图完成文字后 F3 生成锁定贴图，解锁修改不影响原截图",
         [&]
         {
             auto w = capture(900, 700);
             pointer(w, WM_LBUTTONDOWN, {100, 100});
             pointer(w, WM_LBUTTONUP, {750, 520});
             w->command(2);
             drag(w, {50, 50}, {200, 130});
             w->command(4);
             w->beginText({300, 100});
             SetWindowText(w->edit_, L"F3 后锁定");
             paste(); // The global F3 handler commits the active editor before pinning.
             auto pinned = windows_.back().get();
             require(pinned != w && pinned->pinned_ && !pinned->showTools_ && pinned->selected_ < 0 &&
                         pinned->doc_->marks.size() == 2 && pinned->doc_->marks[1].text == L"F3 后锁定",
                     "F3 pin lost live text or started with unlocked annotations.");
             auto original = w->doc_->rendered->pixels;
             pinned->key(VK_TAB, false, false);
             pinned->command(1);
             drag(pinned, {125, 90}, {150, 105});
             require(!pinned->objectDragging_ && pinned->doc_->marks[0].from.x > 50 &&
                         w->doc_->rendered->pixels == original,
                     "Unlocking a new pin changed its source document or failed to enable editing.");
         });
    test(L"OCR 空白单击立即撤销高亮，截图与贴图都保留原识别缓存",
         [&]
         {
             for (bool pinned : {true, false})
             {
                 auto w = pinned ? pin(Image(650, 420)) : capture(900, 700);
                 if (!pinned)
                 {
                     pointer(w, WM_LBUTTONDOWN, {100, 100});
                     pointer(w, WM_LBUTTONUP, {750, 520});
                 }
                 ShowWindow(w->hwnd_, SW_SHOWNOACTIVATE);
                 auto s = std::make_shared<OcrSnapshot>();
                 s->revision = w->doc_->revision;
                 s->epoch = w->ocrEpoch_;
                 s->tokens = {{L"一击取消", {60, 80, 90, 20}, 0}};
                 w->receive(s);
                 drag(w, {65, 90}, {140, 90});
                 require(w->anchor_ == 0, "OCR fixture was not selected.");
                 w->paint();
                 auto paints = w->paintCount_, revision = w->doc_->revision, epoch = w->ocrEpoch_;
                 auto blank = w->view_.toView(Point{350, 250});
                 pointer(w, WM_LBUTTONDOWN, blank);
                 require(w->anchor_ == -1 && w->caret_ == -1 &&
                             (GetUpdateRect(w->hwnd_, nullptr, FALSE) || w->paintCount_ > paints),
                         "Cleared OCR highlight was not invalidated on the first blank click.");
                 w->ocr_ = nullptr;
                 w->receive(s);
                 require(w->ocr_ == s, "A pending OCR result was dropped during an unchanged blank click.");
                 pointer(w, WM_LBUTTONUP, blank);
                 pump();
                 require(w->ocr_ == s && w->doc_->revision == revision && w->ocrEpoch_ == epoch,
                         "A blank click discarded OCR or changed the captured image.");
                 drag(w, {65, 90}, {140, 90});
                 require(w->anchor_ == 0, "Text could not be selected again immediately.");
             }
         });
    test(L"文字输入框按内容双向伸缩，输入位置和提交位置一致",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->fontPt_ = 18;
             w->beginText({80, 90});
             RECT initial{}, shortText{}, longText{}, shortened{};
             GetWindowRect(w->edit_, &initial);
             require(initial.right - initial.left < 100, "Empty editor starts with a long fixed width.");
             SetWindowText(w->edit_, L"短文");
             GetWindowRect(w->edit_, &shortText);
             SetWindowText(w->edit_, L"这是一段长度明显增加的输入文字 SnipLeaf");
             GetWindowRect(w->edit_, &longText);
             SetWindowText(w->edit_, L"短文");
             GetWindowRect(w->edit_, &shortened);
             require(longText.right - longText.left > shortText.right - shortText.left + 100 &&
                         shortened.right - shortened.left == shortText.right - shortText.left,
                     "Editor width does not follow inserted and deleted text.");
             RECT formatting{};
             SendMessage(w->edit_, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&formatting));
             MapWindowPoints(HWND_DESKTOP, w->hwnd_, reinterpret_cast<POINT *>(&shortened), 2);
             auto expected = w->view_.toImage(Point{static_cast<float>(shortened.left + formatting.left),
                                                    static_cast<float>(shortened.top + formatting.top)});
             w->finishText(true);
             require(closeEnough(w->doc_->marks[0].from.x, expected.x) &&
                         closeEnough(w->doc_->marks[0].from.y, expected.y),
                     "Text jumped when the editor was committed.");
             auto narrowCapture = capture(900, 700);
             pointer(narrowCapture, WM_LBUTTONDOWN, {100, 100});
             pointer(narrowCapture, WM_LBUTTONUP, {340, 300});
             narrowCapture->beginText({20, 30});
             SetWindowText(narrowCapture->edit_,
                           L"小截图中的文字也应该限制在图片范围内，避免输入时单行、完成后才突然换行。");
             RECT constrained{};
             GetWindowRect(narrowCapture->edit_, &constrained);
             MapWindowPoints(HWND_DESKTOP, narrowCapture->hwnd_, reinterpret_cast<POINT *>(&constrained), 2);
             require(constrained.right - constrained.left <= 248 && constrained.left >= 96 &&
                         constrained.right <= 344 && SendMessage(narrowCapture->edit_, EM_GETLINECOUNT, 0, 0) > 1,
                     "A small screenshot uses the entire desktop as its text-entry width.");
         });
    test(L"文字工具中直接拖动已提交文字，双击仍可编辑且位置支持撤销",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->command(4);
             w->beginText({80, 90});
             SetWindowText(w->edit_, L"可以直接拖动");
             w->finishText(true);
             auto original = w->doc_->marks[0];
             auto p = Point{original.from.x + 30, original.from.y + 15};
             drag(w, p, {p.x + 60, p.y + 40});
             require(!w->edit_ && w->tool_ == Tool::Text && w->doc_->marks.size() == 1 &&
                         closeEnough(w->doc_->marks[0].from.x, original.from.x + 60) &&
                         closeEnough(w->doc_->marks[0].from.y, original.from.y + 40),
                     "Dragging text reopened the editor or required the Move tool.");
             w->command(10);
             require(closeEnough(w->doc_->marks[0].from.x, original.from.x), "Text position cannot be undone.");
             pointer(w, WM_LBUTTONDBLCLK, w->view_.toView(p));
             wchar_t actual[32]{};
             if (w->edit_)
                 GetWindowText(w->edit_, actual, 32);
             require(w->edit_ && std::wstring(actual) == L"可以直接拖动", "Double click no longer edits the text.");
         });
    test(L"透明输入保留图片和遮挡图层，删除文字后无残影，缩放后背景仍对齐",
         [&]
         {
             for (auto [pinned, scale] :
                  {std::pair{true, 1.f}, std::pair{true, 1.5f}, std::pair{true, 2.f}, std::pair{false, 1.f}})
             {
                 auto w = pinned ? pin(pattern(650, 420), true) : capture(900, 700);
                 if (!pinned)
                 {
                     pointer(w, WM_LBUTTONDOWN, {100, 100});
                     pointer(w, WM_LBUTTONUP, {750, 520});
                 }
                 w->doc_->add({Tool::Marker, {20, 95}, {560, 95}, {}, L"", 0xffcc00, 3});
                 w->doc_->add({Tool::Mosaic, {90, 70}, {170, 170}});
                 w->changed();
                 w->zoom(scale / w->view_.zoom, {2, 2});
                 w->selected_ = -1;
                 w->status_.clear();
                 ShowWindow(w->hwnd_, SW_SHOWNOACTIVATE);
                 w->renderUiPreview(file(L"regression-editor-underlay.local.png"));
                 auto expected = loadImage(graphics, file(L"regression-editor-underlay.local.png"));
                 w->fontPt_ = 18;
                 w->beginText(pinned ? Point{80, 90} : Point{0, 0});
                 auto compareEmpty = [&]
                 {
                     w->paint();
                     RedrawWindow(w->edit_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
                     HideCaret(w->edit_);
                     RECT box{};
                     GetWindowRect(w->edit_, &box);
                     MapWindowPoints(HWND_DESKTOP, w->hwnd_, reinterpret_cast<POINT *>(&box), 2);
                     Dib surface(box.right - box.left, box.bottom - box.top);
                     SendMessage(w->edit_, WM_PRINT, reinterpret_cast<WPARAM>(surface.dc),
                                 PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
                     auto actual = surface.image();
                     size_t different = 0;
                     for (int y = 0; y < actual.height; ++y)
                         for (int x = 0; x < actual.width; ++x)
                         {
                             // Screenshot selection outlines are painted above the picture;
                             // the editor replaces that small outline while it is open.
                             if (!pinned && (std::abs(x + box.left - 100) <= 2 || std::abs(y + box.top - 100) <= 2))
                                 continue;
                             auto p =
                                 &expected
                                      .pixels[(static_cast<size_t>(y + box.top) * expected.width + x + box.left) * 4];
                             auto q = &actual.pixels[(static_cast<size_t>(y) * actual.width + x) * 4];
                             different +=
                                 std::abs(p[0] - q[0]) > 1 || std::abs(p[1] - q[1]) > 1 || std::abs(p[2] - q[2]) > 1;
                         }
                     require(different < actual.width * actual.height / 200 + 1,
                             "Empty EDIT background obscures the image, redactions, or retains deleted glyphs.");
                     ShowCaret(w->edit_);
                 };
                 compareEmpty();
                 for (int i = 0; i < 3; ++i)
                 {
                     SetWindowText(w->edit_, L"透明输入 ABC\r\n第二行");
                     w->renderUiPreview(file(L"regression-editor-transparent.local.png"));
                     SendMessage(w->edit_, EM_SETSEL, 0, -1);
                     keyboard(w->edit_, i % 2 ? VK_DELETE : VK_BACK);
                     require(w->edit_ && GetWindowTextLength(w->edit_) == 0,
                             "Delete/Backspace in the native EDIT did not remove characters.");
                     compareEmpty();
                 }
                 w->finishText(true);
                 require(w->doc_->marks.size() == 2, "Deleting input text removed a committed annotation.");
             }
         });
    test(L"输入时拖动细边框保持文字、颜色和输入焦点，提交位置可撤销",
         [&]
         {
             auto w = pin(pattern(650, 420), true);
             w->ink_ = 0x2578e8;
             w->fontPt_ = 24;
             w->beginText({80, 90});
             SetWindowText(w->edit_, L"输入中移动");
             auto editor = w->edit_;
             auto original = w->stroke_->from;
             RECT box{};
             GetWindowRect(editor, &box);
             MapWindowPoints(HWND_DESKTOP, w->hwnd_, reinterpret_cast<POINT *>(&box), 2);
             SendMessage(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
             require(w->textEditorDragging_ && GetCapture() == w->hwnd_, "Editor padding cannot start a move.");
             Point end{static_cast<float>(box.left + 51), static_cast<float>(box.top + 36)};
             pointer(w, WM_MOUSEMOVE, end);
             pointer(w, WM_LBUTTONUP, end);
             require(w->edit_ == editor && !w->textEditorDragging_ && GetFocus() == editor &&
                         closeEnough(w->stroke_->from.x, original.x + 50) &&
                         closeEnough(w->stroke_->from.y, original.y + 35) && w->textEditorInk() == 0x2578e8,
                     "Live editor move lost the input, ink, focus, or position.");
             auto length = GetWindowTextLength(editor);
             SendMessage(editor, EM_SETSEL, length, length);
             SendMessage(editor, WM_CHAR, L'好', 1);
             w->finishText(true);
             require(w->doc_->marks.size() == 1 && w->doc_->marks[0].text == L"输入中移动好" &&
                         closeEnough(w->doc_->marks[0].from.x, original.x + 50),
                     "Moved input did not commit at the displayed position.");
             w->command(10);
             require(w->doc_->marks.empty(), "Input and movement were not one undo operation.");
             w->command(11);
             auto committed = w->doc_->marks[0].from;
             w->editText(0);
             GetWindowRect(w->edit_, &box);
             MapWindowPoints(HWND_DESKTOP, w->hwnd_, reinterpret_cast<POINT *>(&box), 2);
             SendMessage(w->edit_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
             end = {static_cast<float>(box.left + 26), static_cast<float>(box.top + 16)};
             pointer(w, WM_MOUSEMOVE, end);
             pointer(w, WM_LBUTTONUP, end);
             auto history = w->doc_->undoEdits.size();
             w->finishText(true);
             require(w->doc_->undoEdits.size() == history + 1 &&
                         closeEnough(w->doc_->marks[0].from.x, committed.x + 25),
                     "Moving existing input without changing its text was discarded.");
             w->command(10);
             require(closeEnough(w->doc_->marks[0].from.x, committed.x), "Existing text move cannot be undone.");
         });
    test(L"输入框拖动被 Esc 或捕获丢失取消时恢复位置并保留未提交文字",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->beginText({80, 90});
             SetWindowText(w->edit_, L"保留输入");
             auto editor = w->edit_;
             auto original = w->stroke_->from;
             for (bool escape : {true, false})
             {
                 RECT box{};
                 GetWindowRect(editor, &box);
                 MapWindowPoints(HWND_DESKTOP, w->hwnd_, reinterpret_cast<POINT *>(&box), 2);
                 SendMessage(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(1, 1));
                 pointer(w, WM_MOUSEMOVE, {static_cast<float>(box.left + 41), static_cast<float>(box.top + 21)});
                 if (escape)
                     keyboard(editor, VK_ESCAPE);
                 else
                     ReleaseCapture();
                 require(w->edit_ == editor && !w->textEditorDragging_ && closeEnough(w->stroke_->from.x, original.x) &&
                             closeEnough(w->stroke_->from.y, original.y) && GetWindowTextLength(editor) == 4 &&
                             w->doc_->undoEdits.empty(),
                         "Cancelling a live move discarded input or recorded an edit.");
             }
         });
    test(L"透明原生输入框即时显示六种选定颜色，父窗口重绘不覆盖文字",
         [&]
         {
             for (unsigned rgb : {0xe53935u, 0x2578e8u, 0x128846u, 0xffcc00u, 0xffffffu, 0x000000u})
             {
                 auto w = pin(Image(650, 420, rgb == 0xffffff || rgb == 0xffcc00 ? 0x253047 : 0xffffff), true);
                 ShowWindow(w->hwnd_, SW_SHOWNOACTIVATE);
                 w->command(4);
                 w->fontPt_ = 24;
                 w->ink_ = rgb;
                 w->beginText({80, 90});
                 for (wchar_t character : std::wstring(L"颜色 Color 123"))
                     SendMessage(w->edit_, WM_CHAR, character, 1);
                 SendMessage(w->edit_, EM_SETSEL, static_cast<WPARAM>(-1), -1);
                 for (int i = 0; i < 3; ++i)
                     w->paint();
                 auto name = L"regression-editor-color-" + std::to_wstring(rgb) + L".local.png";
                 w->renderUiPreview(file(name.c_str()));
                 w->paint();
                 DwmFlush();
                 RECT box{};
                 GetClientRect(w->edit_, &box);
                 Dib surface(box.right, box.bottom);
                 HDC dc = GetDC(w->edit_);
                 require(dc != nullptr, "Native editor surface unavailable.");
                 BOOL copied = BitBlt(surface.dc, 0, 0, surface.width, surface.height, dc, 0, 0, SRCCOPY);
                 ReleaseDC(w->edit_, dc);
                 require(copied != FALSE, "Native editor surface copy failed.");
                 auto actual = surface.image();
                 size_t glyphs = 0;
                 for (int y = 4; y < actual.height - 4; ++y)
                     for (int x = 4; x < actual.width - 4; ++x)
                         glyphs += nearColor(&actual.pixels[(static_cast<size_t>(y) * actual.width + x) * 4], rgb);
                 require(glyphs > 80 && glyphs < actual.width * actual.height / 2,
                         "Editor ink is missing, black, or indistinguishable from its background.");
                 w->finishText(true);
                 require(w->doc_->marks.back().rgb == rgb, "Input and committed annotation colors differ.");
             }
         });
    test(L"重新编辑文字保留该对象颜色，取消与未修改提交不污染历史或默认颜色",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             Annotation text{Tool::Text, {80.25f, 90.75f}, {}, {}, L"已有红色文字", 0xe53935, 3, 24};
             measureText(graphics, text, 500);
             w->doc_->add(text);
             w->changed();
             w->zoom(1.1f, {2, 2});
             w->ink_ = 0x2578e8;
             auto history = w->doc_->undoEdits.size();
             for (bool accept : {false, true})
             {
                 w->editText(0);
                 SendMessage(w->edit_, EM_SETSEL, static_cast<WPARAM>(-1), -1);
                 HDC dc = GetDC(w->edit_);
                 SendMessage(w->hwnd_, WM_CTLCOLOREDIT, reinterpret_cast<WPARAM>(dc),
                             reinterpret_cast<LPARAM>(w->edit_));
                 COLORREF actual = GetTextColor(dc);
                 ReleaseDC(w->edit_, dc);
                 require(actual == RGB(0xe5, 0x39, 0x35), "Existing text inherited the next annotation's ink.");
                 w->renderUiPreview(file(L"regression-editor-existing-color.local.png"));
                 w->finishText(accept);
                 require(w->doc_->undoEdits.size() == history && w->ink_ == 0x2578e8,
                         "Unchanged text changed history or default ink.");
             }
             w->editText(0);
             SetWindowText(w->edit_, L"修改后仍为红色");
             w->finishText(true);
             require(w->doc_->marks[0].rgb == 0xe53935 && w->ink_ == 0x2578e8,
                     "Editing changed the annotation's ink or the default ink.");
             w->command(10);
             require(w->doc_->marks[0].text == text.text && w->doc_->marks[0].rgb == text.rgb,
                     "Text undo lost the original content or ink.");
         });
    test(L"长条与竖条马赛克使用细密方格，线宽与区域比例不改变像素格",
         [&]
         {
             auto base = pattern(640, 400);
             Annotation horizontal{Tool::Mosaic, {80, 120}, {480, 136}};
             Annotation vertical{Tool::Mosaic, {520, 64}, {536, 352}};
             horizontal.width = vertical.width = 8;
             std::array marks{horizontal, vertical};
             auto actual = renderAnnotations(graphics, base, marks);
             for (auto const &mark : marks)
             {
                 auto area = between(mark.from, mark.to);
                 for (int y = static_cast<int>(area.y); y < area.y + area.h; y += 8)
                     for (int x = static_cast<int>(area.x); x < area.x + area.w; x += 8)
                     {
                         unsigned sum[3]{};
                         for (int yy = y; yy < y + 8; ++yy)
                             for (int xx = x; xx < x + 8; ++xx)
                                 for (int c = 0; c < 3; ++c)
                                     sum[c] += base.pixels[(static_cast<size_t>(yy) * base.width + xx) * 4 + c];
                         for (int yy = y; yy < y + 8; ++yy)
                             for (int xx = x; xx < x + 8; ++xx)
                                 for (int c = 0; c < 3; ++c)
                                     require(actual.pixels[(static_cast<size_t>(yy) * actual.width + xx) * 4 + c] ==
                                                 sum[c] / 64,
                                             "Thin mosaic cells are oversized or stretched.");
                     }
             }
             horizontal.width = vertical.width = 1.5f;
             require(renderAnnotations(graphics, base, std::array{horizontal, vertical}).pixels == actual.pixels,
                     "Line width still changes mosaic density.");
             auto demo = demoImage(graphics);
             Annotation strip{Tool::Mosaic, {350, 217}, {840, 245}};
             auto old = demo;
             pixelate(old, between(strip.from, strip.to), 15); // Previous default: 3 px line width * 5.
             auto updated = renderAnnotations(graphics, demo, std::span(&strip, 1));
             savePng(graphics, old.crop({40, 190, 960, 300}), file(L"regression-mosaic-before.local.png"));
             savePng(graphics, updated.crop({40, 190, 960, 300}), file(L"regression-mosaic-after.local.png"));
         });
    test(L"马赛克疏密独立切换，已选对象修改可撤销且移动缩放保留设置",
         [&]
         {
             auto w = pin(pattern(640, 400), true);
             w->width_ = 8;
             w->command(5);
             for (int size : {12, 16, 6, 8})
             {
                 w->command(13);
                 require(w->mosaicPixels_ == size && w->width_ == 8 && w->doc_->marks.empty(),
                         "Density cycling changed line width or added history.");
             }
             drag(w, {80, 120}, {480, 144});
             auto original = w->doc_->rendered->pixels;
             w->selected_ = 0;
             w->command(1);
             w->selected_ = 0;
             w->command(13);
             require(w->doc_->marks[0].mosaicPixels == 12 && w->doc_->marks[0].width == 8,
                     "Selected mosaic density was not stored independently.");
             auto denser = w->doc_->rendered->pixels;
             require(denser != original, "Changing density did not redraw the mosaic.");
             w->command(10);
             require(w->doc_->rendered->pixels == original && w->doc_->marks[0].mosaicPixels == 8,
                     "Mosaic density undo failed.");
             w->command(11);
             require(w->doc_->rendered->pixels == denser && w->doc_->marks[0].mosaicPixels == 12,
                     "Mosaic density redo failed.");
             w->beginObject(0, 4, w->view_.toView(Point{480, 144}));
             w->pointerMove(w->view_.toView(Point{500, 156}), MK_LBUTTON);
             w->pointerUp(w->view_.toView(Point{500, 156}));
             require(w->doc_->marks[0].mosaicPixels == 12, "Resizing changed mosaic density.");
             w->zoom(2.f, {100, 100});
             require(w->doc_->marks[0].mosaicPixels == 12 && w->doc_->base->width == 640,
                     "Zoom altered the mosaic grid or source image.");
         });
    test(L"四档马赛克横竖长条正反向预览与保存一致，扩展时原有方格稳定",
         [&]
         {
             for (int size : {6, 8, 12, 16})
                 for (bool vertical : {false, true})
                     for (bool reverse : {false, true})
                     {
                         auto w = pin(pattern(640, 400), true);
                         w->command(5);
                         w->mosaicPixels_ = size;
                         Point a = vertical ? Point{100, 70} : Point{100, 120};
                         Point b = vertical ? Point{118, 310} : Point{480, 138};
                         if (reverse)
                             std::swap(a, b);
                         pointer(w, WM_LBUTTONDOWN, w->view_.toView(a));
                         pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{(a.x + b.x) / 2, (a.y + b.y) / 2}));
                         auto cache = w->mosaicImage_;
                         pointer(w, WM_MOUSEMOVE, w->view_.toView(b));
                         require(w->mosaicImage_ == cache && w->doc_->flattenCount == 0,
                                 "Extending a strip rebuilt the mosaic cache or flattened the document.");
                         auto path = file(L"regression-mosaic-strips-live.local.png");
                         w->renderUiPreview(path);
                         auto live = loadImage(graphics, path);
                         pointer(w, WM_LBUTTONUP, w->view_.toView(b));
                         auto area = between(a, b);
                         auto const &output = *w->doc_->rendered;
                         for (int y = 0; y < output.height; ++y)
                             for (int x = 0; x < output.width; ++x)
                             {
                                 size_t index = (static_cast<size_t>(y) * output.width + x) * 4;
                                 if (!area.contains({static_cast<float>(x), static_cast<float>(y)}))
                                     require(std::memcmp(&output.pixels[index], &w->doc_->base->pixels[index], 4) == 0,
                                             "Mosaic changed pixels outside the selected strip.");
                                 if (x <= area.x + 2 || y <= area.y + 2 || x >= area.x + area.w - 2 ||
                                     y >= area.y + area.h - 2)
                                     continue;
                                 auto p = w->view_.toView(Point{static_cast<float>(x), static_cast<float>(y)});
                                 size_t liveIndex = (static_cast<size_t>(p.y) * live.width + static_cast<int>(p.x)) * 4;
                                 require(std::memcmp(&output.pixels[index], &live.pixels[liveIndex], 3) == 0,
                                         "Live mosaic cells changed color or size on commit.");
                             }
                         windows_.clear();
                         pump();
                     }
         });
    test(L"pt 字号独立于线宽，兼容旧标注并拒绝非法数值",
         [&]
         {
             Annotation legacy{Tool::Text, {}, {}, {}, L"字号", 0, 3};
             require(closeEnough(textFontPoints(legacy), 18) && closeEnough(textFontPixels(legacy), 24),
                     "Existing width-based text changed its apparent size.");
             legacy.fontPt = 12.5f;
             legacy.width = 8;
             require(closeEnough(textFontPixels(legacy), 12.5f * 96 / 72), "Font points still depend on line width.");
             require(closeEnough(annotationBounds(legacy).h, textFontPixels(legacy) * 1.5f),
                     "Unmeasured text bounds still depend on line width instead of points.");
             auto w = pin(Image(500, 300), true);
             auto size = w->fontPt_;
             for (float invalid :
                  {0.f, 5.9f, 288.1f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
                 require(!w->setFontPoints(invalid), "Invalid font size was accepted.");
             require(w->fontPt_ == size && w->doc_->undoEdits.empty(), "Rejected input changed document state.");
         });
    test(L"可编辑字号面板接受自定义 pt，Enter 应用且 Esc 只关闭面板",
         [&]
         {
             auto w = pin(Image(650, 350), true);
             w->command(4);
             w->command(13);
             require(w->fontPopup_ && w->fontCombo_, "Text size button still cycles four widths.");
             COMBOBOXINFO info{sizeof(info)};
             require(GetComboBoxInfo(w->fontCombo_, &info) != FALSE, "Font combo has no editable field.");
             SetWindowText(w->fontCombo_, L"12.5 pt");
             RECT panel{};
             GetWindowRect(w->fontPopup_, &panel);
             Dib panelImage(Image(panel.right - panel.left, panel.bottom - panel.top));
             require(PrintWindow(w->fontPopup_, panelImage.dc, PW_RENDERFULLCONTENT) != FALSE,
                     "Font picker could not render its own preview.");
             for (HWND child = GetWindow(w->fontPopup_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
             {
                 RECT area{};
                 GetWindowRect(child, &area);
                 int saved = SaveDC(panelImage.dc);
                 SetViewportOrgEx(panelImage.dc, area.left - panel.left, area.top - panel.top, nullptr);
                 SendMessage(child, WM_PRINT, reinterpret_cast<WPARAM>(panelImage.dc),
                             PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
                 RestoreDC(panelImage.dc, saved);
             }
             savePng(graphics, panelImage.image(), file(L"regression-font-picker.local.png"));
             keyboard(info.hwndItem, VK_RETURN);
             require(!w->fontPopup_ && closeEnough(w->fontPt_, 12.5f) && w->doc_->marks.empty(),
                     "Enter did not apply custom pt size without creating an annotation.");
             auto next = pin(Image(320, 200));
             require(closeEnough(next->fontPt_, 12.5f), "New canvas lost the selected default font size.");
             w->command(13);
             GetComboBoxInfo(w->fontCombo_, &info);
             for (auto invalid : {L"", L"abc", L"nan", L"9999", L"18 px"})
             {
                 SetWindowText(w->fontCombo_, invalid);
                 keyboard(info.hwndItem, VK_RETURN);
                 require(w->fontPopup_ && closeEnough(w->fontPt_, 12.5f), "Invalid point input closed or applied.");
             }
             keyboard(info.hwndItem, VK_ESCAPE);
             require(!w->fontPopup_ && !w->closing_, "Esc closed the image instead of only the size panel.");
             w->command(13);
             GetComboBoxInfo(w->fontCombo_, &info);
             SendMessage(w->fontCombo_, CB_SETCURSEL, 8, 0); // 18 pt
             SendMessage(w->fontCombo_, CB_SHOWDROPDOWN, TRUE, 0);
             require(w->fontPopup_ != nullptr, "Opening the preset list deactivated and destroyed its panel.");
             keyboard(info.hwndItem, VK_DOWN);
             keyboard(info.hwndItem, VK_RETURN);
             require(!w->fontPopup_ && closeEnough(w->fontPt_, 20), "Keyboard preset selection did not apply.");
         });
    test(L"输入文字期间调整字号不提前提交，字体修改和取消进入正确历史",
         [&]
         {
             auto w = pin(Image(650, 400), true);
             w->beginText({30, 35});
             SetWindowText(w->edit_, L"中文字号 SnapLite");
             HWND editor = w->edit_;
             auto button = std::find_if(w->buttons_.begin(), w->buttons_.end(),
                                        [](auto const &item) { return item.command == 13; });
             pointer(w, WM_LBUTTONDOWN, {button->box.x + 8, button->box.y + 8});
             require(w->edit_ == editor && w->doc_->marks.empty(), "Size button prematurely committed the editor.");
             SetWindowText(w->fontCombo_, L"24.5");
             w->applyFontPicker();
             LOGFONT font{};
             GetObject(w->editFont_, sizeof(font), &font);
             require(font.lfHeight == -33 && GetWindowTextLength(editor) == 13,
                     "Font update discarded text or used the old four-level size.");
             w->finishText(true);
             require(w->doc_->marks.size() == 1 && closeEnough(w->doc_->marks[0].fontPt, 24.5f),
                     "The entered point size was not stored on the annotation.");
             auto original = w->doc_->rendered->pixels;
             w->editText(0);
             w->setFontPoints(32.7f);
             w->finishText(true);
             require(w->doc_->undoEdits.size() == 2 && closeEnough(w->doc_->marks[0].fontPt, 32.7f),
                     "Font-only edit was treated as unchanged text.");
             w->command(10);
             require(w->doc_->rendered->pixels == original, "Undo did not restore the original text size.");
             w->command(11);
             auto committed = w->doc_->rendered->pixels;
             auto history = w->doc_->undoEdits.size();
             w->editText(0);
             w->setFontPoints(20);
             w->finishText(false);
             require(w->doc_->rendered->pixels == committed && w->doc_->undoEdits.size() == history,
                     "Cancelling a font edit changed the committed annotation.");
             w->renderUiPreview(file(L"regression-font-points.local.png"));
         });
    test(L"已选文本按 pt 改大小，撤销恢复且不改变线条粗细",
         [&]
         {
             auto w = pin(Image(650, 350), true);
             w->setFontPoints(18);
             w->beginText({30, 25});
             SetWindowText(w->edit_, L"调整文本大小");
             w->finishText(true);
             auto before = w->doc_->marks[0];
             auto width = w->width_;
             w->setFontPoints(23.5f);
             auto after = w->doc_->marks[0];
             require(after.fontPt == 23.5f && after.from.x == before.from.x && after.from.y == before.from.y &&
                         w->width_ == width && after.width == before.width &&
                         annotationBounds(after).h > annotationBounds(before).h,
                     "Changing text size changed the anchor or stroke width.");
             auto box = annotationBounds(after);
             auto scaled = transformAnnotation(after, box, {box.x, box.y, box.w * 2, box.h * 2});
             require(closeEnough(scaled.fontPt, 47) && scaled.width == after.width,
                     "Object resizing did not preserve the independent font unit.");
             w->command(10);
             require(w->doc_->marks[0].fontPt == 18, "Selected text size could not be undone.");
             w->command(11);
             require(w->doc_->marks[0].fontPt == 23.5f, "Selected text size could not be redone.");
         });
    test(L"pt 在 DPI 和视图缩放之间保持一致，大字号编辑不再限制为 160 像素",
         [&]
         {
             for (float dpi : {1.f, 1.5f, 2.f})
             {
                 auto w = pin(Image(320, 200), true);
                 w->ui_ = dpi;
                 w->view_.zoom = 2;
                 w->fitPin(true);
                 w->layout();
                 w->setFontPoints(12.5f);
                 w->beginText({25, 25});
                 LOGFONT font{};
                 GetObject(w->editFont_, sizeof(font), &font);
                 require(font.lfHeight == -33 && w->stroke_->fontPt == 12.5f,
                         "Window DPI was applied twice to image-space points.");
                 w->setFontPoints(288);
                 GetObject(w->editFont_, sizeof(font), &font);
                 require(font.lfHeight == -768 && !(GetWindowLongPtr(w->edit_, GWL_STYLE) & WS_VSCROLL),
                         "Large text is still capped or restores old scrollbar UI.");
                 w->showFontPicker();
                 RECT popup{};
                 GetWindowRect(w->fontPopup_, &popup);
                 MONITORINFO monitor{sizeof(monitor)};
                 GetMonitorInfo(MonitorFromWindow(w->fontPopup_, MONITOR_DEFAULTTONEAREST), &monitor);
                 require(popup.left >= monitor.rcWork.left && popup.right <= monitor.rcWork.right &&
                             popup.top >= monitor.rcWork.top && popup.bottom <= monitor.rcWork.bottom,
                         "Font picker escaped the monitor work area.");
                 w->closeFontPicker();
                 w->finishText(false);
             }
         });
    test(L"放大的文字标注按显示尺寸重绘，接近高分辨率参考而非拉伸位图",
         [&]
         {
             auto w = pin(Image(240, 100));
             Annotation mark{Tool::Text, {15, 14}, {}, {}, L"清晰文字 SnapLite", 0x172333, 3, 12};
             measureText(graphics, mark, 220);
             w->doc_->add(mark);
             w->changed();
             w->view_.zoom = 3;
             w->fitPin(true);
             w->layout();
             auto path = file(L"regression-zoom-vector.local.png");
             w->renderUiPreview(path);
             auto actual = loadImage(graphics, path).crop({1, 1, 721, 301});
             auto referenceMark = mark;
             referenceMark.from = {mark.from.x * 3, mark.from.y * 3};
             referenceMark.to = {mark.to.x * 3, mark.to.y * 3};
             referenceMark.fontPt *= 3;
             auto reference = renderAnnotations(graphics, Image(720, 300), std::span(&referenceMark, 1));
             auto source = w->doc_->rendered;
             Image old(720, 300);
             for (int y = 0; y < old.height; ++y)
                 for (int x = 0; x < old.width; ++x)
                     std::memcpy(old.pixels.data() + y * old.stride() + x * 4,
                                 source->pixels.data() + y / 3 * source->stride() + x / 3 * 4, 4);
             auto error = [&](Image const &image)
             {
                 double sum = 0;
                 for (size_t i = 0; i < image.pixels.size(); ++i)
                 {
                     double delta = image.pixels[i] - reference.pixels[i];
                     sum += delta * delta;
                 }
                 return sum / image.pixels.size();
             };
             savePng(graphics, old, file(L"regression-zoom-vector-old.local.png"));
             savePng(graphics, reference, file(L"regression-zoom-vector-reference.local.png"));
             vectorReferenceMse = error(actual);
             bitmapReferenceMse = error(old);
             require(vectorReferenceMse < bitmapReferenceMse * .5,
                     "Committed text still scales its low-resolution bitmap.");
         });
    test(L"原截图使用 GPU 清晰缩放，可切换像素显示且重复缩放不改变原图",
         [&]
         {
             Dib dib(Image(240, 70));
             auto font = CreateFont(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                    CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
             auto previous = SelectObject(dib.dc, font);
             SetBkMode(dib.dc, TRANSPARENT);
             std::wstring text = L"放大阅读文字 SnapLite 12345";
             TextOut(dib.dc, 12, 20, text.c_str(), static_cast<int>(text.size()));
             SelectObject(dib.dc, previous);
             DeleteObject(font);
             auto w = pin(dib.image());
             auto source = w->doc_->base;
             w->zoom(2.7f, {1, 1});
             w->paint();
             require(w->cubicZoomUsed_, "Native window did not use supported GPU cubic interpolation.");
             cubicZoomVerified = true;
             auto path = file(L"regression-zoom-raster-clear.local.png");
             w->renderUiPreview(path);
             auto clear = loadImage(graphics, path);
             w->command(22);
             w->renderUiPreview(file(L"regression-zoom-raster-pixel.local.png"));
             auto pixel = loadImage(graphics, file(L"regression-zoom-raster-pixel.local.png"));
             require(clear.pixels != pixel.pixels && !w->smoothZoom_, "Zoom display choices produced the same output.");
             w->command(21);
             auto flattened = w->doc_->flattenCount;
             for (int i = 0; i < 100; ++i)
             {
                 w->zoom(1.1f, {1, 1});
                 w->zoom(1.f / 1.1f, {1, 1});
             }
             w->zoom(1.f / w->view_.zoom, {1, 1});
             w->renderUiPreview(file(L"regression-zoom-raster-restored.local.png"));
             auto restored = loadImage(graphics, file(L"regression-zoom-raster-restored.local.png"));
             require(restored.crop({1, 1, 241, 71}).pixels == source->pixels && w->doc_->base == source &&
                         w->doc_->flattenCount == flattened,
                     "Repeated zoom progressively resampled the source or blurred 100% display.");
         });
    test(L"矢量显示保持多层马赛克和半透明标记的次序",
         [&]
         {
             auto w = pin(pattern(240, 150));
             w->doc_->add({Tool::Text, {30, 20}, {200, 80}, {}, L"下面的文字", 0, 3, 18});
             w->doc_->add({Tool::Mosaic, {24, 16}, {180, 112}, {}, L"", 0, 1.6f});
             w->doc_->add({Tool::Marker, {30, 60}, {200, 60}, {{30, 60}, {200, 60}}, L"", 0xff0000, 3});
             w->doc_->add({Tool::Mosaic, {56, 40}, {128, 88}, {}, L"", 0, 1.6f});
             w->changed();
             require(w->doc_->rasterLayers.size() == 2 && w->doc_->rasterLayers[1].index == 3,
                     "Mosaic snapshots lost their layer indices.");
             for (float zoom : {1.f, 2.5f})
             {
                 w->view_.zoom = zoom;
                 w->fitPin(true);
                 w->layout();
                 auto path = file(L"regression-zoom-layer-mask.local.png");
                 w->renderUiPreview(path);
                 auto image = loadImage(graphics, path);
                 for (auto p : {Point{60, 52}, Point{92, 68}, Point{148, 60}, Point{36, 92}})
                 {
                     auto v = w->view_.toView(p);
                     auto actual =
                         image.pixels.data() + static_cast<int>(v.y) * image.stride() + static_cast<int>(v.x) * 4;
                     auto expected = w->doc_->rendered->pixels.data() +
                                     static_cast<int>(p.y) * w->doc_->rendered->stride() + static_cast<int>(p.x) * 4;
                     for (int c = 0; c < 3; ++c)
                         require(std::abs(actual[c] - expected[c]) <= 1,
                                 "Vector redraw changed mosaic order or applied a translucent layer twice.");
                 }
             }
         });
    test(L"平滑缩放不将马赛克内的原始像素采样到边界外",
         [&]
         {
             Image first(160, 140), second(160, 140);
             for (int y = 40; y < 104; ++y)
                 for (int x = 40; x < 120; ++x)
                     for (int c = 0; c < 3; ++c)
                     {
                         size_t i = y * first.stride() + x * 4 + c;
                         first.pixels[i] = (x + y) % 2 ? 20 : 220;
                         second.pixels[i] = (x + y) % 2 ? 220 : 20;
                     }
             auto render = [&](Image image, wchar_t const *name)
             {
                 auto w = pin(std::move(image));
                 w->doc_->add({Tool::Mosaic, {40, 40}, {120, 104}, {}, L"", 0, 1.6f});
                 w->changed();
                 w->view_.zoom = 2.7f;
                 w->fitPin(true);
                 w->layout();
                 w->renderUiPreview(file(name));
                 return loadImage(graphics, file(name));
             };
             auto a = render(std::move(first), L"regression-zoom-mask-edge-a.local.png");
             auto b = render(std::move(second), L"regression-zoom-mask-edge-b.local.png");
             require(a.pixels == b.pixels, "Hidden source pattern leaked through cubic sampling outside its mask.");
         });
    test(L"字号面板与放大渲染资源能反复释放，预览切换不跨 Direct2D 资源域",
         [&]
         {
             auto w = pin(pattern(260, 160), true);
             w->command(4);
             w->doc_->add({Tool::Mosaic, {20, 20}, {120, 100}, {}, L"", 0, 3});
             w->changed();
             w->view_.zoom = 2;
             w->fitPin(true);
             w->layout();
             w->paint();
             w->showFontPicker();
             w->closeFontPicker();
             DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
                   user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
             for (int i = 0; i < 30; ++i)
             {
                 w->showFontPicker();
                 w->closeFontPicker();
                 if (i % 5 == 0)
                 {
                     w->renderUiPreview(file(L"regression-zoom-resource.local.png"));
                     w->rt_ = nullptr;
                     w->discardTargetBitmaps();
                     w->paint();
                 }
             }
             require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi + 3 &&
                         GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= user + 3,
                     "Font picker leaked fonts, controls, or render resources.");
         });
    test(L"贴图采用一像素渐变边框，悬停不覆盖右上角图片",
         [&]
         {
             auto w = pin(Image(360, 210));
             w->hovering_ = true;
             auto path = file(L"regression-pin-frame.local.png");
             w->renderUiPreview(path);
             auto image = loadImage(graphics, path);
             require(image.width == 362 && image.height == 212, "Pin retains a thick exterior gutter.");
             auto rgb = [&](int x, int y)
             {
                 auto p = image.pixels.data() + y * image.stride() + x * 4;
                 return (p[2] << 16) | (p[1] << 8) | p[0];
             };
             require(rgb(12, 0) != rgb(350, 0) && rgb(12, 0) != 0x17191c, "Pin border is not a colored gradient.");
             require(rgb(348, 12) == 0xffffff && rgb(1, 80) == 0xffffff,
                     "Floating close UI or a thick gutter obscures the image.");
             require(w->document()->rendered->pixels == Image(360, 210).pixels,
                     "Decorative frame modified screenshot pixels.");
         });
    test(L"旧贴图关闭按钮位置可以选字，Esc 仍可关闭",
         [&]
         {
             auto w = pin(Image(360, 210));
             w->hovering_ = true;
             auto snapshot = std::make_shared<OcrSnapshot>();
             snapshot->revision = w->doc_->revision;
             snapshot->epoch = w->ocrEpoch_;
             snapshot->width = 360;
             snapshot->height = 210;
             snapshot->tokens = {{L"文字", {332, 3, 24, 20}, 0}};
             w->receive(snapshot);
             drag(w, {340, 12}, {348, 12});
             require(!w->closing_ && w->anchor_ == 0 && w->caret_ == 0,
                     "Old invisible close hit target still intercepts text selection.");
             w->key(VK_ESCAPE, false, false);
             require(!w->closing_ && w->anchor_ < 0, "Esc did not clear active selection.");
             w->key(VK_ESCAPE, false, false);
             require(w->closing_, "Esc no longer closes an idle pin.");
         });
    test(L"取消 OCR 请求不启动识别，空图片安全返回",
         [&]
         {
             auto cancelled = std::make_shared<std::atomic_bool>(true);
             auto result = recognize({1, 1, std::make_shared<Image>(800, 600), {}, L"", 0, cancelled});
             require(result->passCount == 0 && result->tokens.empty(), "Cancelled OCR still ran the engine.");
             require(recognize({})->tokens.empty(), "Empty OCR request was not handled.");
         });
    test(L"超过引擎上限的原图分块识别，跨块文字不重复且马赛克过滤有效",
         [&]
         {
             Dib dib(Image(12040, 460));
             auto font = CreateFontW(-22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
             require(font != nullptr, "Large OCR fixture font unavailable.");
             auto old = SelectObject(dib.dc, font);
             SetBkMode(dib.dc, TRANSPARENT);
             SetTextColor(dib.dc, RGB(20, 30, 40));
             std::wstring text = L"Boundary selection stays accurate across tiles.";
             TextOutW(dib.dc, 1990, 40, text.c_str(), static_cast<int>(text.size()));
             std::wstring secret = L"PRIVATE REDACTED CONTENT";
             TextOutW(dib.dc, 1990, 140, secret.c_str(), static_cast<int>(secret.size()));
             TextOutW(dib.dc, 10050, 240, L"Far edge text remains selectable.", 33);
             SelectObject(dib.dc, old);
             DeleteObject(font);
             auto image = std::make_shared<Image>(dib.image());
             Box mask{1980, 132, 500, 45};
             pixelate(*image, mask, 18);
             auto result = recognize({1, 1, image, {mask}, L""});
             auto actual = result->selection(0, static_cast<int>(result->tokens.size()) - 1);
             require(actual.find(L"Boundary selection stays accurate across tiles.") != std::wstring::npos &&
                         actual.find(L"Far edge text remains selectable.") != std::wstring::npos,
                     "A tile seam or large-image shrink lost text.");
             require(result->enhanced && result->tileCount >= 2, "Large OCR did not take the tiled path.");
             require(actual.find(L"Boundary") == actual.rfind(L"Boundary"), "Tile overlap duplicated a word.");
             for (auto const &token : result->tokens)
             {
                 auto quad = result->quad(token);
                 float left = quad[0].x, right = left, top = quad[0].y, bottom = top;
                 for (auto p : quad)
                 {
                     left = std::min(left, p.x);
                     right = std::max(right, p.x);
                     top = std::min(top, p.y);
                     bottom = std::max(bottom, p.y);
                 }
                 require(!mask.intersects({left, top, right - left, bottom - top}),
                         "Tiled coordinate remapping exposed a redacted word.");
             }
         });
    test(L"密集小字增强后英文复制仍保留词间空格",
         [&]
         {
             Dib dib(Image(1100, 180));
             auto font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
             require(font != nullptr, "Small-font fixture unavailable.");
             auto old = SelectObject(dib.dc, font);
             SetBkMode(dib.dc, TRANSPARENT);
             TextOutW(dib.dc, 24, 20, L"中文小字识别与英文复制", 11);
             std::wstring text = L"Dense English words keep their spaces.";
             TextOutW(dib.dc, 24, 60, text.c_str(), static_cast<int>(text.size()));
             SelectObject(dib.dc, old);
             DeleteObject(font);
             auto result = recognize({1, 1, std::make_shared<Image>(dib.image()), {}, L""});
             require(result->enhanced && result->selection(0, static_cast<int>(result->tokens.size()) - 1).find(text) !=
                                             std::wstring::npos,
                     "Small-font enhancement merged English words or lost the sentence.");
         });
    test(L"跨块倾斜文字的识别坐标回到实际字形位置",
         [&]
         {
             Dib dib(Image(12040, 460));
             auto font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
             require(font != nullptr, "Rotated fixture font unavailable.");
             auto old = SelectObject(dib.dc, font);
             SetBkMode(dib.dc, TRANSPARENT);
             SetGraphicsMode(dib.dc, GM_ADVANCED);
             float radians = 6.f * 3.14159265f / 180.f;
             XFORM transform{std::cos(radians), std::sin(radians), -std::sin(radians), std::cos(radians), 1990, 80};
             SetWorldTransform(dib.dc, &transform);
             std::wstring text = L"Rotated text remains aligned.";
             SIZE size{};
             GetTextExtentPoint32W(dib.dc, text.c_str(), static_cast<int>(text.size()), &size);
             TextOutW(dib.dc, 0, 0, text.c_str(), static_cast<int>(text.size()));
             SelectObject(dib.dc, old);
             DeleteObject(font);
             auto result = recognize({1, 1, std::make_shared<Image>(dib.image()), {}, L"en-US"});
             require(result->enhanced && result->tokens.size() >= 3, "Rotated tiled text was not recognized.");
             for (auto const &token : result->tokens)
             {
                 auto quad = result->quad(token);
                 Point center{};
                 for (auto p : quad)
                 {
                     center.x += p.x / 4;
                     center.y += p.y / 4;
                 }
                 auto local = rotate({center.x - 1990, center.y - 80}, {}, -6);
                 require(local.x >= -4 && local.x <= size.cx + 4 && local.y >= -4 && local.y <= size.cy + 4,
                         "Tile-local text angle was rotated around the wrong image center.");
                 require(result->hit(center) >= 0, "Remapped tilted words cannot be selected.");
             }
         });
    test(L"贴图增强识别菜单通过后台队列强制处理并保持文档像素",
         [&]
         {
             auto w = pin(demoImage(graphics));
             auto pixels = w->doc_->rendered->pixels;
             auto epoch = w->ocrEpoch_;
             w->command(19);
             require(!w->ocr_ && w->ocrEpoch_ == epoch + 1 && w->status_ == L"正在增强识别…",
                     "Enhancement did not invalidate the old OCR generation.");
             auto deadline = Clock::now() + std::chrono::seconds(10);
             while (!w->ocr_ && Clock::now() < deadline)
             {
                 pump();
                 MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
             }
             require(w->ocr_ && w->ocr_->enhanced && w->ocr_->epoch == epoch + 1 && !w->ocr_->tokens.empty(),
                     "The context command did not force background enhancement.");
             require(w->doc_->rendered->pixels == pixels, "Enhanced OCR changed pinned image pixels.");
         });
    test(L"默认选字模式拖动空白移动截图，文字区域保留选字",
         [&]
         {
             auto w = capture(900, 700);
             pointer(w, WM_LBUTTONDOWN, {100, 100});
             pointer(w, WM_LBUTTONUP, {600, 450});
             require(w->tool_ == Tool::Select, "Default mode is not OCR selection.");
             auto snapshot = std::make_shared<OcrSnapshot>();
             snapshot->revision = w->doc_->revision;
             snapshot->epoch = w->ocrEpoch_;
             snapshot->tokens = {{L"文字", {30, 40, 60, 25}, 0}};
             w->receive(snapshot);
             drag(w, {45, 50}, {75, 50});
             require(w->anchor_ == 0 && w->crop_.left == 100, "Text dragged the crop.");
             auto before = w->doc_->flattenCount;
             pointer(w, WM_LBUTTONDOWN, {180, 220});
             pointer(w, WM_MOUSEMOVE, {210, 240});
             require(w->moving_ && w->doc_->flattenCount == before && !w->ocr_,
                     "Blank crop move did not start or synchronously flattened.");
             pointer(w, WM_LBUTTONUP, {210, 240});
             require(w->crop_.left == 130 && w->crop_.top == 120, "Default crop move failed.");
             require(w->doc_->rendered->pixels == w->desktop_->crop(w->crop_).pixels, "Moved crop pixels mismatch.");
             w->receive(snapshot);
             require(!w->ocr_, "Old OCR accepted after moving the crop.");
         });
    test(L"记号笔忽略中途弯曲轨迹，预览与提交均为起终点直线",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->command(6);
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{60, 100}));
             pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{160, 190}));
             pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{260, 100}));
             require(w->stroke_ && annotationBounds(*w->stroke_).h < 1,
                     "Marker preview retained the curved pointer path.");
             pointer(w, WM_LBUTTONUP, w->view_.toView(Point{260, 100}));
             auto const &m = w->doc_->marks.at(0);
             require(hitAnnotation(m, {160, 100}, 1) && !hitAnnotation(m, {160, 190}, 1),
                     "Committed marker is not a straight highlight.");
         });
    test(L"文字输入去除三维边框和滚动条，换行时自动增加高度",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->beginText({80, 70});
             auto style = GetWindowLongPtr(w->edit_, GWL_STYLE);
             auto exStyle = GetWindowLongPtr(w->edit_, GWL_EXSTYLE);
             require(!(style & (WS_VSCROLL | WS_HSCROLL)) && !(exStyle & WS_EX_CLIENTEDGE),
                     "Text editor still exposes legacy scrollbars or a recessed frame.");
             RECT before{}, after{};
             GetWindowRect(w->edit_, &before);
             SetWindowText(w->edit_, L"第一行\r\n第二行\r\n第三行");
             GetWindowRect(w->edit_, &after);
             require(after.bottom - after.top > before.bottom - before.top, "Multiline text did not grow the editor.");
             w->renderUiPreview(file(L"regression-text-card.local.png"));
         });
    test(L"文字边缘与词间空隙可以开始拖选，远处空白仍移动贴图",
         [&]
         {
             auto w = pin(Image(650, 420));
             auto snapshot = std::make_shared<OcrSnapshot>();
             snapshot->revision = w->doc_->revision;
             snapshot->epoch = w->ocrEpoch_;
             snapshot->tokens = {
                 {L"Hello", {60, 80, 50, 20}, 0}, {L"world", {124, 80, 55, 20}, 0}, {L"第二行", {60, 120, 80, 20}, 1}};
             w->receive(snapshot);
             for (Point p : {Point{56, 90}, Point{82, 76}, Point{117, 90}, Point{182, 102}})
             {
                 pointer(w, WM_LBUTTONDOWN, w->view_.toView(p));
                 require(w->selecting_ && !w->moving_, "A nearby word edge or gap moved the image.");
                 pointer(w, WM_LBUTTONUP, w->view_.toView(p));
             }
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{350, 250}));
             require(w->moving_ && !w->selecting_, "Distant whitespace no longer moves the image.");
             w->cancelGesture();
         });
    test(L"贴图移动后保留 OCR，仍可选字，移动期间到达的结果也可用",
         [&]
         {
             auto w = pin(Image(650, 420));
             auto snapshot = std::make_shared<OcrSnapshot>();
             snapshot->revision = w->doc_->revision;
             snapshot->epoch = w->ocrEpoch_;
             snapshot->tokens = {{L"贴图选字", {60, 80, 90, 20}, 0}};
             w->receive(snapshot);
             auto epoch = w->ocrEpoch_;
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{350, 250}));
             require(w->moving_ && w->ocr_ == snapshot && w->ocrEpoch_ == epoch,
                     "Moving a pin discarded its OCR snapshot.");
             pointer(w, WM_LBUTTONUP, w->view_.toView(Point{350, 250}));
             drag(w, {62, 85}, {140, 90});
             require(w->anchor_ == 0 && w->caret_ == 0 && w->ocr_->selection(0, 0) == L"贴图选字",
                     "Cannot select pin text after moving it.");
             w->ocr_.reset();
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{350, 250}));
             w->receive(snapshot);
             pointer(w, WM_LBUTTONUP, w->view_.toView(Point{350, 250}));
             require(w->ocr_ == snapshot, "An unchanged pin rejected OCR received during its move.");
         });
    test(L"直线记号笔方向吸附、零长度不入历史、移动和撤销",
         [&]
         {
             KeyboardState state;
             auto w = pin(Image(650, 420), true);
             w->command(6);
             state.set(VK_SHIFT);
             drag(w, {60, 100}, {260, 115});
             state.set();
             require(w->doc_->marks.size() == 1 && closeEnough(w->doc_->marks[0].from.y, w->doc_->marks[0].to.y),
                     "Shift failed to keep a near-horizontal marker horizontal.");
             w->tool_ = Tool::Move;
             drag(w, {160, 100}, {190, 125});
             require(hitAnnotation(w->doc_->marks[0], {190, 125}, 1), "Straight marker cannot be moved.");
             w->command(10);
             require(hitAnnotation(w->doc_->marks[0], {160, 100}, 1), "Marker move cannot be undone.");
             require(w->objectHandles(w->doc_->marks[0]).size() == 2, "Straight marker still has box handles.");
             drag(w, {160, 100}, {160, 100});
             drag(w, {260, 100}, {290, 160});
             require(closeEnough(w->doc_->marks[0].to.x, 290) && closeEnough(w->doc_->marks[0].to.y, 160) &&
                         hitAnnotation(w->doc_->marks[0], {175, 130}, 1),
                     "Resizing a marker endpoint detached its displayed path.");
             w->command(10);
             auto history = w->doc_->undoEdits.size();
             w->command(6);
             drag(w, {400, 200}, {400, 200});
             require(w->doc_->undoEdits.size() == history, "A zero-length marker created an invisible object.");
         });
    test(L"选字容错适配缩放和 DPI，旋转文字仍以正确方向命中",
         [&]
         {
             for (float zoom : {.5f, 1.f, 2.f})
                 for (float dpi : {1.f, 1.5f, 2.f})
                 {
                     auto w = pin(Image(650, 420));
                     w->view_ = {zoom, {2, 2}};
                     w->ui_ = dpi;
                     auto s = std::make_shared<OcrSnapshot>();
                     s->revision = w->doc_->revision;
                     s->epoch = w->ocrEpoch_;
                     s->width = 650;
                     s->height = 420;
                     s->tokens = {{L"Text", {90, 90, 60, 20}, 0, 12.f}, {L"次行", {90, 135, 60, 20}, 1, 0.f}};
                     w->receive(s);
                     auto local = Point{90 - 5 * dpi / zoom, 100};
                     auto point = w->view_.toView(rotate(local, {325, 210}, 12.f));
                     require(w->textAt(point) == 0, "Zoom/DPI/rotation made the forgiving text target too small.");
                     pointer(w, WM_LBUTTONDOWN, point);
                     require(w->selecting_ && w->anchor_ == 0, "Rotated text target did not start selection.");
                     pointer(w, WM_LBUTTONUP, point);
                     require(w->textAt(w->view_.toView(Point{450, 270})) < 0,
                             "Forgiving targets expanded into a distant column.");
                 }
         });
    test(L"文字附近点击不跨行误选，马赛克内不吸附邻近文本",
         [&]
         {
             auto w = pin(Image(650, 420));
             auto s = std::make_shared<OcrSnapshot>();
             s->revision = w->doc_->revision;
             s->epoch = w->ocrEpoch_;
             s->tokens = {{L"上一行", {60, 80, 90, 20}, 0}, {L"下一行", {60, 120, 90, 20}, 1}};
             w->receive(s);
             require(w->textAt(w->view_.toView(Point{100, 116})) == 1, "Near next line selected the previous line.");
             require(w->textAt(w->view_.toView(Point{100, 110})) < 0, "Large line whitespace became a text target.");
             w->doc_->add({Tool::Mosaic, {151, 70}, {210, 110}, {}, L"", 0, 3});
             s->revision = w->doc_->revision;
             w->receive(s);
             require(w->textAt(w->view_.toView(Point{155, 90})) < 0,
                     "A redacted pixel snapped to text next to the mosaic.");
         });
    test(L"靠近截图内边缘优先选字，外边缘仍可调整截图",
         [&]
         {
             auto w = capture(900, 700);
             pointer(w, WM_LBUTTONDOWN, {100, 100});
             pointer(w, WM_LBUTTONUP, {600, 450});
             auto s = std::make_shared<OcrSnapshot>();
             s->revision = w->doc_->revision;
             s->epoch = w->ocrEpoch_;
             s->tokens = {{L"边缘文字", {1, 40, 80, 20}, 0}};
             w->receive(s);
             pointer(w, WM_LBUTTONDOWN, {103, 150});
             require(w->selecting_ && !w->cropping_, "The crop edge stole a click on text.");
             pointer(w, WM_LBUTTONUP, {145, 150});
             pointer(w, WM_LBUTTONDOWN, {98, 150});
             require(w->cropping_, "Outside crop edge no longer resizes.");
             w->cancelGesture();
         });
    test(L"贴图双击 OCR 文本不关闭窗口，空白移动取消仍保留 OCR",
         [&]
         {
             auto w = pin(Image(650, 420));
             auto s = std::make_shared<OcrSnapshot>();
             s->revision = w->doc_->revision;
             s->epoch = w->ocrEpoch_;
             s->tokens = {{L"保留贴图", {60, 80, 90, 20}, 0}};
             w->receive(s);
             auto p = w->view_.toView(Point{70, 90});
             pointer(w, WM_LBUTTONDBLCLK, p);
             pointer(w, WM_LBUTTONUP, p);
             require(IsWindow(w->hwnd_) && w->anchor_ == 0, "Double click on OCR text closed the pin.");
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{350, 250}));
             w->cancelGesture();
             require(w->ocr_ == s, "Cancelling a pin move invalidated OCR.");
             w->zoom(.5f, w->view_.toView(Point{100, 100}));
             w->show(false);
             w->show(true);
             require(w->ocr_ == s, "Zooming or hiding a pin lost OCR.");
             drag(w, {60, 90}, {140, 90});
             require(w->anchor_ == 0 && w->caret_ == 0, "Cannot select after zoom/hide/show.");
         });
    test(L"F3 与贴图按钮复用当前 OCR，标注后不复用过期文字",
         [&]
         {
             for (bool hotkey : {false, true})
             {
                 auto w = capture(900, 700);
                 pointer(w, WM_LBUTTONDOWN, {100, 100});
                 pointer(w, WM_LBUTTONUP, {600, 450});
                 auto s = std::make_shared<OcrSnapshot>();
                 s->revision = w->doc_->revision;
                 s->epoch = w->ocrEpoch_;
                 s->tokens = {{L"立刻选字", {60, 80, 90, 20}, 0}};
                 w->receive(s);
                 if (hotkey)
                     SendMessage(root_, WM_HOTKEY, 2, 0);
                 else
                     w->command(15);
                 auto pinned = windows_.back().get();
                 require(pinned->pinned_ && !pinned->showTools_ && pinned->tool_ == Tool::Select && pinned->ocr_ == s,
                         "Pin did not reuse the current OCR/default selection mode without its toolbar.");
                 pump();
                 drag(pinned, {56, 90}, {140, 90});
                 require(pinned->anchor_ == 0, "New pin is not immediately selectable.");
                 pinned->doc_->add({Tool::Mosaic, {40, 60}, {180, 115}, {}, L"", 0, 3});
                 pinned->changed();
                 this->pin(*pinned->doc_, s);
                 require(!windows_.back()->ocr_, "A changed image inherited stale text.");
                 windows_.clear();
                 pump();
             }
         });
    test(L"真实中英文异步 OCR：截图、F3、移动贴图、拖选、Ctrl+C",
         [&]
         {
             config.autoOcr = true;
             auto image = demoImage(graphics);
             int width = image.width, height = image.height;
             auto source = std::make_unique<Canvas>(*this, nextId_++, std::move(image), RECT{0, 0, width, height});
             require(source->create(false), "Cannot create the actual OCR capture.");
             auto raw = source.get();
             windows_.push_back(std::move(source));
             pointer(raw, WM_LBUTTONDOWN, {0, 0});
             pointer(raw, WM_LBUTTONUP, {static_cast<float>(width), static_cast<float>(height)});
             SendMessage(root_, WM_HOTKEY, 2, 0);
             auto w = windows_.back().get();
             require(w->pinned_ && !w->showTools_, "F3 did not make a toolbar-free pin.");
             auto deadline = Clock::now() + std::chrono::seconds(10);
             while (!w->ocr_ && Clock::now() < deadline)
             {
                 pump();
                 MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
             }
             require(w->ocr_ && !w->ocr_->tokens.empty(), "The pinned image did not receive actual async OCR.");
             auto text = w->ocr_->selection(0, static_cast<int>(w->ocr_->tokens.size()) - 1);
             require(text.find(L"直接在图片上拖选文字") != std::wstring::npos &&
                         text.find(L"Select words on this image and copy them.") != std::wstring::npos,
                     "Pinned OCR did not recognize Chinese and English.");
             auto s = w->ocr_;
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{35, 35}));
             pointer(w, WM_LBUTTONUP, w->view_.toView(Point{35, 35}));
             require(w->ocr_ == s, "Moving the actual OCR pin cleared its result.");
             auto const &t = s->tokens.front();
             auto p = rotate({t.box.x - 4 / w->view_.zoom, t.box.y + t.box.h / 2}, {s->width / 2.f, s->height / 2.f},
                             t.angle.value_or(s->angle));
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(p));
             pointer(w, WM_LBUTTONUP, w->view_.toView(p));
             require(w->anchor_ == 0 && w->caret_ == 0, "Cannot select actual OCR from the glyph edge.");
             std::wstring copied;
             int copies = 0;
             copyText_ = [&](HWND owner, std::wstring const &payload)
             {
                 require(owner == w->hwnd_, "Copy used another window's clipboard ownership.");
                 copied = payload;
                 ++copies;
                 return true;
             };
             {
                 KeyboardState state;
                 state.set(VK_CONTROL);
                 keyboard(w->hwnd_, 'C');
             }
             require(copies == 1 && copied == t.text && IsWindow(w->hwnd_) && w->status_ == L"文字已复制",
                     "Ctrl+C did not route the selected pin text to the clipboard boundary.");
             w->renderUiPreview(file(L"regression-pinned-ocr-selection.local.png"));
         });
    test(L"多行输入自动增高和缩小，长文不显示滚动条且保持在窗口内",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->beginText({630, 405});
             RECT initial{}, expanded{}, shrunk{}, client{};
             GetWindowRect(w->edit_, &initial);
             std::wstring text;
             for (int i = 0; i < 30; ++i)
                 text += L"输入多行内容\r\n";
             SetWindowText(w->edit_, text.c_str());
             GetWindowRect(w->edit_, &expanded);
             RECT local = expanded;
             MapWindowPoints(HWND_DESKTOP, w->hwnd_, reinterpret_cast<POINT *>(&local), 2);
             GetClientRect(w->hwnd_, &client);
             require(local.left >= 0 && local.top >= 0 && local.right <= client.right && local.bottom <= client.bottom,
                     "Autogrowing input escaped the canvas.");
             require(!(GetWindowLongPtr(w->edit_, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL)),
                     "Long text brought back the scrollbars.");
             SetWindowText(w->edit_, L"缩短内容");
             GetWindowRect(w->edit_, &shrunk);
             require(shrunk.bottom - shrunk.top == initial.bottom - initial.top &&
                         expanded.bottom - expanded.top > shrunk.bottom - shrunk.top,
                     "The input did not shrink after deleting multiline text.");
             w->finishText(true);
             w->editText(0);
             require(GetWindowTextLength(w->edit_) == 4, "Reopening text lost its contents.");
         });
    test(L"裁切移动与边缘调整取消时恢复原图和标注历史",
         [&]
         {
             auto w = capture(700, 550);
             pointer(w, WM_LBUTTONDOWN, {100, 100});
             pointer(w, WM_LBUTTONUP, {550, 380});
             w->doc_->add({Tool::Rectangle, {40, 30}, {160, 100}, {}, L"", 0xff0000, 3});
             w->changed();
             auto original = w->doc_->rendered->pixels;
             pointer(w, WM_LBUTTONDOWN, {550, 270});
             pointer(w, WM_MOUSEMOVE, {620, 270});
             w->key(VK_ESCAPE, false, false);
             require(w->crop_.right == 550 && w->doc_->rendered->pixels == original, "Resize cancel changed pixels.");
             pointer(w, WM_LBUTTONDOWN, {300, 300});
             pointer(w, WM_MOUSEMOVE, {330, 310});
             ReleaseCapture();
             require(w->crop_.left == 100 && w->doc_->rendered->pixels == original, "Capture loss changed the crop.");
             drag(w, {210, 170}, {230, 190});
             require(w->crop_.left == 120 && closeEnough(w->doc_->marks[0].from.x, 20),
                     "Annotation did not keep screen position.");
             w->command(10);
             w->command(11);
             require(closeEnough(w->doc_->marks[0].from.x, 20), "Crop translation corrupted undo/redo coordinates.");
         });
    for (int width : {320, 800, 1920, 3840})
        for (float dpi : {1.f, 1.5f, 2.f})
        {
            auto name = L"全屏与窄屏工具栏 " + std::to_wstring(width) + L" px / " +
                        std::to_wstring(static_cast<int>(dpi * 100)) + L"%";
            test(name.c_str(),
                 [&]
                 {
                     auto w = capture(width, width == 320 ? 640 : 1080);
                     w->ui_ = dpi;
                     w->crop_ = {0, 0, w->desktop_->width, w->desktop_->height};
                     w->doc_ = std::make_unique<Document>(*w->desktop_);
                     w->layout();
                     toolbarVisible(w);
                     if (width == 1920 && dpi == 1.f)
                         w->renderUiPreview(file(L"regression-fullscreen.local.png"));
                     for (auto position :
                          {Point{0, 0}, Point{100.f, 80.f}, Point{static_cast<float>(width - 100), 880}})
                     {
                         w->crop_ = {static_cast<LONG>(position.x), static_cast<LONG>(position.y),
                                     static_cast<LONG>(position.x + 95), static_cast<LONG>(position.y + 90)};
                         w->layout();
                         toolbarVisible(w);
                     }
                 });
        }
    test(L"负坐标副屏及跨屏工具栏保持可点击",
         [&]
         {
             auto w = capture(2400, 1200, -1200, -200);
             w->crop_ = {0, 0, 2400, 1200};
             w->doc_ = std::make_unique<Document>(*w->desktop_);
             w->layout();
             toolbarVisible(w);
             w->crop_ = {20, 50, 1180, 1150};
             w->layout();
             toolbarVisible(w);
         });
    test(L"10%～800% 贴图缩放、展开工具栏和 DPI 调整",
         [&]
         {
             auto w = pin(Image(800, 500), true);
             for (float scale : {.1f, .5f, 1.f, 2.f, 8.f})
             {
                 w->zoom(scale / w->view_.zoom, {2, 2});
                 toolbarVisible(w);
                 require(closeEnough(w->view_.zoom, scale), "Pin zoom lost its requested scale.");
                 w->command(20);
                 w->command(20);
                 toolbarVisible(w);
             }
             RECT proposed{};
             GetWindowRect(w->hwnd_, &proposed);
             SendMessage(w->hwnd_, WM_DPICHANGED, MAKELPARAM(144, 144), reinterpret_cast<LPARAM>(&proposed));
             toolbarVisible(w);
         });
    for (auto kind : {Tool::Arrow, Tool::Line, Tool::Rectangle, Tool::Ellipse, Tool::Marker, Tool::Mosaic, Tool::Text})
    {
        auto name = L"标注对象移动、缩放、删除和完整撤销重做 / " + std::to_wstring(static_cast<int>(kind));
        test(name.c_str(),
             [&]
             {
                 auto w = pin(pattern(600, 400), true);
                 Annotation m{kind, {100, 100}, {250, 180}, {}, L"中文 ABC", 0xe6525c, 3};
                 if (kind == Tool::Marker)
                     m.path = {{100, 100}, {175, 140}, {250, 180}};
                 if (kind == Tool::Text)
                     measureText(graphics, m, 400);
                 w->doc_->add(m);
                 w->changed();
                 auto original = w->doc_->rendered->pixels;
                 w->command(1);
                 auto b = annotationBounds(m);
                 Point hit = kind == Tool::Rectangle || kind == Tool::Ellipse ? Point{b.x + b.w / 2, b.y}
                                                                              : Point{b.x + b.w / 2, b.y + b.h / 2};
                 drag(w, hit, {hit.x + 25, hit.y + 15});
                 require(w->selected_ == 0 && closeEnough(w->doc_->marks[0].from.x, 125) &&
                             closeEnough(w->doc_->marks[0].from.y, 115) && w->doc_->undoEdits.size() == 2,
                         "Object move failed.");
                 auto handles = w->objectHandles(w->doc_->marks[0]);
                 auto end = handles[kind == Tool::Arrow || kind == Tool::Line ? 1 : 4];
                 drag(w, end, {end.x + 35, end.y + 25});
                 auto finalPixels = w->doc_->rendered->pixels;
                 require(w->doc_->undoEdits.size() == 3 && finalPixels != original, "Object resize not recorded once.");
                 w->command(10);
                 w->command(10);
                 require(w->doc_->rendered->pixels == original, "Move/resize undo did not restore original pixels.");
                 w->command(11);
                 w->command(11);
                 require(w->doc_->rendered->pixels == finalPixels, "Move/resize redo did not restore pixels.");
                 w->selected_ = 0;
                 w->key(VK_DELETE, false, false);
                 require(w->doc_->marks.empty(), "Delete failed.");
                 w->command(10);
                 require(w->doc_->rendered->pixels == finalPixels, "Delete undo changed the layer order.");
                 w->command(11);
                 require(w->doc_->marks.empty(), "Delete redo failed.");
                 keyboard(w->hwnd_, VK_BACK);
                 require(w->doc_->marks.empty(), "Backspace without selection changed the document.");
                 w->command(10);
                 w->selected_ = 0;
                 keyboard(w->hwnd_, VK_BACK);
                 require(w->doc_->marks.empty(), "Backspace did not delete the selected annotation.");
                 w->command(10);
                 require(w->doc_->rendered->pixels == finalPixels, "Backspace undo lost the annotation.");
                 w->command(11);
                 require(w->doc_->marks.empty(), "Backspace redo failed.");
                 w->command(10);
                 w->selected_ = 0;
                 auto bounds = annotationBounds(w->doc_->marks[0]);
                 Point inside{bounds.x + bounds.w / 2, bounds.y + bounds.h / 2};
                 pointer(w, WM_LBUTTONDOWN, w->view_.toView(inside));
                 pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{inside.x + 10, inside.y + 10}));
                 require(w->objectDragging_, "Selected annotation did not enter the live move.");
                 keyboard(w->hwnd_, VK_BACK);
                 pointer(w, WM_LBUTTONUP, w->view_.toView(inside));
                 require(w->doc_->marks.empty() && !w->stroke_ && !w->objectDragging_,
                         "Delete during a live move resurrected the annotation on mouse-up.");
                 w->command(10);
                 require(w->doc_->rendered->pixels == finalPixels,
                         "Deleting during a live move failed to undo to the committed object.");
             });
    }
    test(L"空心矩形与椭圆只命中边界，线段端点和箭头头部可命中",
         [&]
         {
             Annotation m{Tool::Rectangle, {100, 100}, {220, 180}, {}, L"", 0, 3};
             require(hitAnnotation(m, {160, 100}, 3) && !hitAnnotation(m, {160, 140}, 3), "Rectangle hit area wrong.");
             m.kind = Tool::Ellipse;
             require(hitAnnotation(m, {220, 140}, 3) && !hitAnnotation(m, {160, 140}, 3), "Ellipse hit area wrong.");
             m = {Tool::Arrow, {50, 50}, {150, 50}, {}, L"", 0, 4};
             require(hitAnnotation(m, {136, 44}, 3) && !hitAnnotation(m, {100, 90}, 3), "Arrow head hit area wrong.");
         });
    test(L"Shift 约束正方形、圆形、45 度直线并处理反向绘制",
         [&]
         {
             KeyboardState state;
             auto w = pin(Image(600, 400), true);
             for (int command : {8, 9, 3})
             {
                 w->command(command);
                 state.set(VK_SHIFT);
                 drag(w, {300, 250}, {160, 180});
                 state.set();
                 auto b = between(w->doc_->marks.back().from, w->doc_->marks.back().to);
                 require(closeEnough(b.w, b.h), "Constrained shape is not square / 45 degrees.");
             }
         });
    test(L"零面积形状不提交，Shift 线段在边缘保持角度且可抓取",
         [&]
         {
             auto w = pin(Image(600, 400), true);
             for (int command : {5, 8, 9})
             {
                 w->command(command);
                 drag(w, {100, 100}, {250, 100});
                 require(w->doc_->marks.empty() && w->doc_->undoEdits.empty(),
                         "Invisible area shape polluted undo history.");
             }
             KeyboardState state;
             state.set(VK_SHIFT);
             w->command(3);
             drag(w, {10, 200}, {599, 399});
             auto const &line = w->doc_->marks[0];
             require(line.to.x <= 600 && line.to.y <= 400 && closeEnough(line.to.y, 200),
                     "Angle snapping pushed the endpoint outside the image.");
         });
    test(L"对象拖动 Esc、鼠标捕获丢失和未移动点击不会污染历史",
         [&]
         {
             auto w = pin(Image(600, 400), true);
             w->doc_->add({Tool::Rectangle, {100, 100}, {250, 200}, {}, L"", 0xff0000, 3});
             w->changed();
             auto original = w->doc_->rendered->pixels;
             for (int cancel : {0, 1, 2})
             {
                 w->command(1);
                 pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{180, 100}));
                 if (cancel != 2)
                     pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{210, 120}));
                 if (cancel == 0)
                     w->key(VK_ESCAPE, false, false);
                 else if (cancel == 1)
                     ReleaseCapture();
                 else
                     pointer(w, WM_LBUTTONUP, w->view_.toView(Point{180, 100}));
                 require(!w->stroke_ && !w->objectDragging_ && w->doc_->undoEdits.size() == 1 &&
                             w->doc_->rendered->pixels == original,
                         "Cancelled / empty edit changed the document.");
             }
         });
    test(L"马赛克实时预览像素与导出一致且不会改动区域外",
         [&]
         {
             auto w = pin(pattern(600, 400), true);
             w->command(5);
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{101, 103}));
             pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{247, 189}));
             w->renderUiPreview(file(L"regression-mosaic-live.local.png"));
             w->paint(); // Return from a DC preview target to the HWND resource domain.
             auto live = loadImage(graphics, file(L"regression-mosaic-live.local.png"));
             pointer(w, WM_LBUTTONUP, w->view_.toView(Point{247, 189}));
             require(w->doc_->rendered->pixels[0] == w->doc_->base->pixels[0], "Mosaic modified outside region.");
             for (int y = 104; y < 189; ++y)
                 for (int x = 102; x < 247; ++x)
                 {
                     auto v = w->view_.toView(Point{static_cast<float>(x), static_cast<float>(y)});
                     auto a = &live.pixels[(static_cast<size_t>(v.y) * live.width + static_cast<int>(v.x)) * 4];
                     auto b = &w->doc_->rendered->pixels[(static_cast<size_t>(y) * w->doc_->base->width + x) * 4];
                     require(a[0] == b[0] && a[1] == b[1] && a[2] == b[2], "Mosaic live/export pixels diverged.");
                 }
         });
    test(L"下层对象调整时上层马赛克仍遮挡，提交保持图层次序",
         [&]
         {
             auto w = pin(pattern(600, 400), true);
             Annotation arrow{Tool::Arrow, {80, 80}, {320, 240}, {}, L"", 0xff0000, 4};
             Annotation mosaic{Tool::Mosaic, {130, 120}, {290, 230}, {}, L"", 0, 3};
             mosaic.mosaicPixels = 12;
             w->doc_->add(arrow);
             w->doc_->add(mosaic);
             w->changed();
             w->command(1);
             w->beginObject(0, -1, w->view_.toView(Point{90, 87}));
             w->pointerMove(w->view_.toView(Point{110, 117}), MK_LBUTTON);
             w->renderUiPreview(file(L"regression-layer-mask.local.png"));
             auto live = loadImage(graphics, file(L"regression-layer-mask.local.png"));
             auto expected = renderAnnotations(graphics, *w->doc_->base, std::span(&mosaic, 1));
             for (int y = 150; y < 200; ++y)
                 for (int x = 160; x < 260; ++x)
                 {
                     auto v = w->view_.toView(Point{static_cast<float>(x), static_cast<float>(y)});
                     auto a = &live.pixels[(static_cast<size_t>(v.y) * live.width + static_cast<int>(v.x)) * 4];
                     auto b = &expected.pixels[(static_cast<size_t>(y) * expected.width + x) * 4];
                     require(a[0] == b[0] && a[1] == b[1] && a[2] == b[2],
                             "Upper mosaic was bypassed during a live edit.");
                 }
             w->pointerUp(w->view_.toView(Point{110, 117}));
             require(w->doc_->marks[0].kind == Tool::Arrow && w->doc_->marks[1].kind == Tool::Mosaic,
                     "Editing brought an object above a redaction.");
         });
    test(L"重叠对象优先选中上层，修改后清除重做分支",
         [&]
         {
             auto w = pin(Image(600, 400), true);
             Annotation a{Tool::Rectangle, {100, 100}, {250, 200}, {}, L"", 0xff0000, 3};
             w->doc_->add(a);
             a.rgb = 0x0000ff;
             w->doc_->add(a);
             w->changed();
             require(w->objectAt({180, 100}) == 1, "Hit order is not topmost-first.");
             w->doc_->replace(1, transformAnnotation(a, annotationBounds(a), {120, 110, 150, 100}));
             w->command(10);
             w->selected_ = 1;
             w->key(VK_RIGHT, false, true);
             require(w->doc_->redoEdits.empty() && closeEnough(w->doc_->marks[1].from.x, 110),
                     "A new edit retained redo.");
         });
    test(L"原生键盘消息输入、中文字符、焦点恢复和输入框绘制",
         [&]
         {
             KeyboardState state;
             state.set();
             auto w = pin(Image(650, 420), true);
             ShowWindow(w->hwnd_, SW_SHOWNOACTIVATE);
             w->command(4);
             pointer(w, WM_LBUTTONDOWN, w->view_.toView(Point{100, 80}));
             require(w->edit_ && GetFocus() == w->edit_, "Text editor did not receive keyboard focus.");
             HWND edit = w->edit_;
             keyboard(edit, 'A');
             keyboard(edit, 'B');
             keyboard(edit, 'C');
             PostMessage(edit, WM_CHAR, L'中', 1);
             PostMessage(edit, WM_CHAR, L'文', 1);
             pump();
             SetFocus(w->hwnd_);
             require(GetFocus() == edit, "Parent activation stole text input focus.");
             for (int i = 0; i < 3; ++i)
                 w->paint();
             wchar_t text[128]{};
             GetWindowText(edit, text, 128);
             require(std::wstring(text) == L"abc中文", "Keyboard translation / Unicode input was lost.");
             w->renderUiPreview(file(L"regression-text-input.local.png"));
             auto ui = loadImage(graphics, file(L"regression-text-input.local.png"));
             RECT box{};
             GetWindowRect(edit, &box);
             MapWindowPoints(HWND_DESKTOP, w->hwnd_, reinterpret_cast<POINT *>(&box), 2);
             size_t dark = 0;
             for (int y = box.top + 8; y < std::min(box.bottom - 8, box.top + 40); ++y)
                 for (int x = box.left + 8; x < std::min(box.right - 20, box.left + 200); ++x)
                 {
                     auto p = &ui.pixels[(static_cast<size_t>(y) * ui.width + x) * 4];
                     dark += nearColor(p, w->ink_, 30);
                 }
             require(dark > 60, "Native editor has text but its rendered glyphs are missing.");
             DwmFlush();
             Dib surface(box.right - box.left, box.bottom - box.top);
             HDC windowDc = GetWindowDC(edit);
             require(windowDc != nullptr, "Native editor surface unavailable.");
             BOOL copied = BitBlt(surface.dc, 0, 0, surface.width, surface.height, windowDc, 0, 0, SRCCOPY);
             ReleaseDC(edit, windowDc);
             require(copied != FALSE, "Native editor surface copy failed.");
             auto actual = surface.image();
             size_t visibleGlyphs = 0;
             for (int y = 8; y < std::min(actual.height - 8, 40); ++y)
                 for (int x = 8; x < std::min(actual.width - 20, 200); ++x)
                 {
                     auto p = &actual.pixels[(static_cast<size_t>(y) * actual.width + x) * 4];
                     visibleGlyphs += nearColor(p, w->ink_, 30);
                 }
             require(visibleGlyphs > 60, "Parent repaint hid the native editor's actual window glyphs.");
             savePng(graphics, actual, file(L"regression-native-edit-surface.local.png"));
             keyboard(edit, VK_RETURN);
             require(!w->edit_ && w->doc_->marks.size() == 1 && w->doc_->marks[0].text == L"abc中文",
                     "Enter failed through the production preTranslate pipeline.");
         });
    test(L"文字换行、IME 组合回车保护、Esc 取消和边缘输入框",
         [&]
         {
             KeyboardState state;
             state.set();
             auto w = pin(Image(650, 420), true);
             ShowWindow(w->hwnd_, SW_SHOWNOACTIVATE);
             w->beginText({630, 405});
             RECT box{}, client{};
             GetWindowRect(w->edit_, &box);
             MapWindowPoints(HWND_DESKTOP, w->hwnd_, reinterpret_cast<POINT *>(&box), 2);
             GetClientRect(w->hwnd_, &client);
             require(box.left >= 0 && box.top >= 0 && box.right <= client.right && box.bottom <= client.bottom,
                     "Text editor escaped the canvas near an edge.");
             HWND edit = w->edit_;
             SendMessage(edit, WM_IME_STARTCOMPOSITION, 0, 0);
             MSG enter{edit, WM_KEYDOWN, VK_RETURN};
             require(!w->preTranslate(enter), "IME confirmation committed the annotation.");
             SendMessage(edit, WM_IME_ENDCOMPOSITION, 0, 0);
             SendMessage(edit, WM_KEYDOWN, VK_RETURN, 0);
             pump();
             require(w->edit_ == edit, "Composition-end race committed the IME confirmation key.");
             SendMessage(edit, WM_KEYUP, VK_RETURN, 0);
             keyboard(edit, 'A');
             state.set(VK_SHIFT);
             keyboard(edit, VK_RETURN);
             state.set();
             keyboard(edit, 'B');
             keyboard(edit, VK_RETURN);
             require(w->doc_->marks.size() == 1 && w->doc_->marks[0].text == L"a\r\nb",
                     "Shift+Enter did not preserve newline.");
             auto original = w->doc_->rendered->pixels;
             w->beginText({80, 90});
             keyboard(w->edit_, 'C');
             keyboard(w->edit_, VK_ESCAPE);
             require(w->doc_->marks.size() == 1 && w->doc_->rendered->pixels == original,
                     "Esc committed cancelled text.");
         });
    test(L"贴图文字双击修改、空文本删除和撤销，双击工具栏不关闭",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             Annotation m{Tool::Text, {100, 80}, {}, {}, L"原始文字", 0xff0000, 3};
             measureText(graphics, m, 400);
             w->doc_->add(m);
             w->changed();
             auto point = w->view_.toView(Point{110, 90});
             SendMessage(w->hwnd_, WM_LBUTTONDBLCLK, 0,
                         MAKELPARAM(static_cast<int>(point.x), static_cast<int>(point.y)));
             require(w->edit_ && w->textIndex_ == 0 && IsWindow(w->hwnd_), "Double click closed the text's pin.");
             SetWindowText(w->edit_, L"修改文字");
             w->finishText(true);
             require(w->doc_->marks.size() == 1 && w->doc_->marks[0].text == L"修改文字",
                     "Text edit inserted another object.");
             w->command(10);
             require(w->doc_->marks[0].text == L"原始文字", "Text edit undo failed.");
             w->editText(0);
             SetWindowText(w->edit_, L"");
             w->finishText(true);
             require(w->doc_->marks.empty(), "Empty text did not remove the object.");
             w->command(10);
             require(w->doc_->marks.size() == 1, "Text removal undo failed.");
             auto b = w->buttons_[1].box;
             SendMessage(w->hwnd_, WM_LBUTTONDBLCLK, 0,
                         MAKELPARAM(static_cast<int>(b.x + 5), static_cast<int>(b.y + 5)));
             require(IsWindow(w->hwnd_), "Double clicking a tool closed the pin.");
             w->renderUiPreview(file(L"regression-objects.local.png"));
         });
    test(L"对象编辑立即作废旧 OCR，结束后拒绝旧 epoch",
         [&]
         {
             auto w = pin(Image(600, 400), true);
             w->doc_->add({Tool::Mosaic, {100, 100}, {220, 180}, {}, L"", 0, 3});
             w->changed();
             auto snapshot = std::make_shared<OcrSnapshot>();
             snapshot->revision = w->doc_->revision;
             snapshot->epoch = w->ocrEpoch_;
             snapshot->tokens = {{L"旧结果", {40, 30, 60, 20}, 0}};
             w->receive(snapshot);
             w->anchor_ = w->caret_ = 0;
             w->beginObject(0, -1, w->view_.toView(Point{150, 140}));
             require(!w->ocr_ && w->anchor_ < 0, "OCR selection survived the beginning of an object edit.");
             w->receive(snapshot);
             require(!w->ocr_, "Old result arrived during live editing.");
             w->pointerUp(w->view_.toView(Point{170, 150}));
             w->receive(snapshot);
             require(!w->ocr_, "Old result arrived after editing.");
         });
    test(L"选中边界框内拖动、文本缩窄换行和未修改提交",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             Annotation arrow{Tool::Arrow, {100, 100}, {250, 200}, {}, L"", 0xff0000, 3};
             w->doc_->add(arrow);
             w->changed();
             w->selected_ = 0;
             w->tool_ = Tool::Move;
             drag(w, {120, 170}, {140, 185});
             require(closeEnough(w->doc_->marks[0].from.x, 120),
                     "Selected boundary-box interior did not move the object.");
             Annotation text{Tool::Text, {100, 230}, {}, {}, L"abcdef 中文测试", 0xff0000, 3};
             measureText(graphics, text, 500);
             w->doc_->add(text);
             w->changed();
             w->editText(1);
             auto history = w->doc_->undoEdits.size();
             w->finishText(true);
             require(w->doc_->undoEdits.size() == history && w->doc_->marks.size() == 2,
                     "Unchanged text edit changed history or deleted text.");
             auto b = annotationBounds(w->doc_->marks[1]);
             w->selected_ = 1;
             drag(w, {b.x + b.w, b.y + b.h / 2}, {b.x + 45, b.y + b.h / 2});
             auto resized = w->doc_->marks[1];
             auto measured = resized;
             measureText(graphics, measured, annotationBounds(resized).w);
             require(resized.to.y >= measured.to.y - 3 && resized.text == text.text,
                     "Wrapped text clipped its final line.");
         });
    test(L"绘制期间切换工具或撤销能提交或取消完整对象",
         [&]
         {
             auto w = pin(Image(650, 420), true);
             w->command(2);
             w->pointerDown(w->view_.toView(Point{100, 100}), 0);
             w->pointerMove(w->view_.toView(Point{220, 160}), MK_LBUTTON);
             w->command(8);
             require(w->doc_->marks.size() == 1 && !w->stroke_ && !w->dragging_, "Switching tools lost a live stroke.");
             w->pointerDown(w->view_.toView(Point{300, 100}), 0);
             w->pointerMove(w->view_.toView(Point{420, 180}), MK_LBUTTON);
             w->command(10);
             require(w->doc_->marks.size() == 1 && !w->stroke_ && w->doc_->undoEdits.size() == 1,
                     "Undo of a live stroke removed an earlier object.");
         });
    test(L"F3 提交正在框选或绘制的画面，新增贴图不使窗口引用失效",
         [&]
         {
             auto w = capture(700, 550);
             pointer(w, WM_LBUTTONDOWN, {100, 100});
             pointer(w, WM_MOUSEMOVE, {550, 380});
             windows_.shrink_to_fit(); // Force pin() to reallocate the owning vector.
             paste();
             pump();
             require(windows_.size() == 1 && windows_[0]->pinned_ && windows_[0]->doc_->base->width == 450,
                     "F3 lost a live crop or invalidated the source pointer.");
             windows_.clear();
             auto c = capture(700, 550);
             pointer(c, WM_LBUTTONDOWN, {100, 100});
             pointer(c, WM_LBUTTONUP, {550, 380});
             c->command(2);
             c->pointerDown(c->view_.toView(Point{50, 60}), 0);
             c->pointerMove(c->view_.toView(Point{200, 140}), MK_LBUTTON);
             windows_.shrink_to_fit();
             paste();
             pump();
             require(windows_.size() == 1 && windows_[0]->doc_->marks.size() == 1 &&
                         windows_[0]->doc_->rendered->pixels != windows_[0]->doc_->base->pixels,
                     "F3 pinned stale pixels while drawing.");
         });
    test(L"持续鼠标消息排队时可见窗口仍立即更新标注",
         [&]
         {
             auto w = pin(Image(800, 500), true);
             ShowWindow(w->hwnd_, SW_SHOWNOACTIVATE);
             struct Restore
             {
                 bool &flag;
                 bool value;
                 ~Restore()
                 {
                     flag = value;
                 }
             } restore{testing_, testing_};
             testing_ = false;
             w->command(2);
             w->pointerDown(w->view_.toView(Point{100, 100}), 0);
             w->paint();
             auto count = w->paintCount_;
             for (int i = 0; i < 100; ++i)
                 PostMessage(w->hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(250 + i, 180));
             w->lastPaint_ = Clock::now() - std::chrono::milliseconds(10);
             pointer(w, WM_MOUSEMOVE, w->view_.toView(Point{220, 160}));
             require(w->paintCount_ > count && w->doc_->marks.empty(),
                     "Live drawing waited behind queued mouse messages instead of painting.");
             pump();
             w->pointerUp(w->last_);
         });
    test(L"500 次混合图层历史操作与独立快照模型一致",
         [&]
         {
             Document doc(Image(100, 100));
             std::vector<std::vector<Annotation>> snapshots(1);
             size_t cursor = 0;
             unsigned seed = 0x12345678;
             auto random = [&]
             {
                 seed = seed * 1664525u + 1013904223u;
                 return seed;
             };
             for (int i = 0; i < 500; ++i)
             {
                 auto expected = snapshots[cursor];
                 auto action = random() % 5;
                 if (action == 3)
                 {
                     require(doc.undo() == (cursor > 0), "History undo availability mismatch.");
                     if (cursor > 0)
                         --cursor;
                 }
                 else if (action == 4)
                 {
                     require(doc.redo() == (cursor + 1 < snapshots.size()), "History redo availability mismatch.");
                     if (cursor + 1 < snapshots.size())
                         ++cursor;
                 }
                 else
                 {
                     Annotation mark{Tool::Rectangle,
                                     {static_cast<float>(random() % 80), 10},
                                     {90, 80},
                                     {},
                                     L"",
                                     random() % 0xffffff,
                                     3};
                     if (expected.empty() || action == 0)
                     {
                         doc.add(mark);
                         expected.push_back(mark);
                     }
                     else
                     {
                         size_t index = random() % expected.size();
                         if (action == 1)
                         {
                             doc.replace(index, mark);
                             expected[index] = mark;
                         }
                         else
                         {
                             doc.remove(index);
                             expected.erase(expected.begin() + index);
                         }
                     }
                     snapshots.resize(cursor + 1);
                     snapshots.push_back(std::move(expected));
                     ++cursor;
                 }
                 require(doc.marks.size() == snapshots[cursor].size(), "History layer count mismatch.");
                 for (size_t j = 0; j < doc.marks.size(); ++j)
                     require(doc.marks[j].from.x == snapshots[cursor][j].from.x &&
                                 doc.marks[j].rgb == snapshots[cursor][j].rgb,
                             "History restored the wrong layer or geometry.");
             }
         });
    for (int tool : {2, 3, 5, 6, 8, 9})
    {
        auto name = L"4K 连续绘制无逐帧整图合成 / tool " + std::to_wstring(tool);
        test(name.c_str(),
             [&]
             {
                 auto w = pin(pattern(3840, 2160), true);
                 w->view_.zoom = .3f;
                 w->fitPin(true);
                 w->layout();
                 w->command(tool);
                 auto setup = Clock::now();
                 w->pointerDown(w->view_.toView(Point{100, 100}), 0);
                 double setupMs = elapsed(setup);
                 auto count = w->doc_->flattenCount;
                 auto pixels = w->doc_->rendered;
                 std::vector<double> handlers, frames;
                 for (int i = 0; i < 100; ++i)
                 {
                     Point p = w->view_.toView(Point{200.f + i * 12.f, 200.f + i * 4.f});
                     auto start = Clock::now();
                     w->pointerMove(p, MK_LBUTTON);
                     double handler = elapsed(start);
                     w->paint();
                     if (i >= 10)
                     {
                         handlers.push_back(handler);
                         frames.push_back(elapsed(start));
                     }
                 }
                 require(w->doc_->flattenCount == count && w->doc_->rendered == pixels,
                         "Pointer move rebuilt the full-resolution image.");
                 std::sort(handlers.begin(), handlers.end());
                 std::sort(frames.begin(), frames.end());
                 if (performance.size() > 1)
                     performance += ",";
                 performance += "{\"tool\":" + std::to_string(tool) +
                                ",\"imagePixels\":8294400,\"samples\":90,"
                                "\"setupMs\":" +
                                std::to_string(setupMs) + ",\"pointerP95Ms\":" + std::to_string(handlers[85]) +
                                ",\"paintP50Ms\":" + std::to_string(frames[45]) +
                                ",\"paintP95Ms\":" + std::to_string(frames[85]) +
                                ",\"paintMaxMs\":" + std::to_string(frames.back()) + "}";
                 require(handlers[85] < 5, "Warm pointer handling exceeds the 5 ms budget.");
                 w->pointerUp(w->last_);
                 require(w->doc_->flattenCount == count + 1 && w->doc_->marks.size() == 1,
                         "Drawing did not flatten exactly once on commit.");
             });
    }
    test(L"100 次文字与对象编辑取消，窗口与 GDI 资源无持续增长",
         [&]
         {
             DWORD gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
                   user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
             for (int i = 0; i < 100; ++i)
             {
                 auto w = pin(Image(300, 200), true);
                 w->beginText({20, 20});
                 SendMessage(w->edit_, WM_CHAR, L'字', 1);
                 w->finishText(false);
                 w->doc_->add({Tool::Ellipse, {30, 40}, {150, 120}, {}, L"", 0xff0000, 3});
                 w->changed();
                 w->beginObject(0, 4, w->view_.toView(Point{150, 120}));
                 w->pointerMove(w->view_.toView(Point{180, 150}), MK_LBUTTON);
                 w->cancelGesture();
                 windows_.clear();
                 pump();
             }
             require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi + 3 &&
                         GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= user + 3,
                     "Editing leaked native windows, fonts, or GDI handles.");
         });
    if (previousForeground && IsWindow(previousForeground))
        SetForegroundWindow(previousForeground);
    writeReport(report, "{\"suite\":\"regression\",\"passed\":" + std::to_string(passed) +
                            ",\"failed\":" + std::to_string(failed) +
                            ",\"syntheticImages\":true,"
                            "\"clipboardUntouched\":true,\"realImeCandidateUiTested\":false,\"cases\":" +
                            cases +
                            "],\"performanceContext\":\"4K source, 30% view, hidden HWND, 10 warmup + 90 samples; "
                            "paint includes submission, not physical input-to-present latency\",\"performance\":" +
                            performance +
                            "],\"typography\":{\"fontPointRange\":[6,288],\"pointDecimals\":1,"
                            "\"nativeCubicVerified\":" +
                            (cubicZoomVerified ? "true" : "false") +
                            ",\"vectorReferenceMSE\":" + std::to_string(vectorReferenceMse) +
                            ",\"oldBitmapReferenceMSE\":" + std::to_string(bitmapReferenceMse) + "}}");
    return failed ? 1 : 0;
}
} // namespace snap
