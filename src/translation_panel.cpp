#include "app.h"
#include "translation.h"
#include <commctrl.h>
namespace snap
{
namespace
{
constexpr int Source = 401, Result = 402, Begin = 403, CopyResult = 404, Cancel = 405, Configure = 406,
              Status = 407, Privacy = 408, SourceLabel = 409, ResultLabel = 410, Header = 411;
constexpr int AppId = 421, Secret = 422, Enabled = 423, Clear = 424, Documentation = 425, SetupStatus = 426;
std::wstring textOf(HWND window)
{
    int length = GetWindowTextLength(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowText(window, text.data(), length + 1);
    text.resize(length);
    return text;
}
void placeWindow(HWND window, HWND beside, int width, int height)
{
    RECT other{};
    if (!beside || !GetWindowRect(beside, &other)) { POINT p{}; GetCursorPos(&p); other = {p.x, p.y, p.x, p.y}; }
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfo(MonitorFromRect(&other, MONITOR_DEFAULTTONEAREST), &monitor);
    auto work = monitor.rcWork;
    width = std::min(width, static_cast<int>(work.right - work.left));
    height = std::min(height, static_cast<int>(work.bottom - work.top));
    int x = other.right + 10;
    if (x + width > work.right) x = other.left - width - 10;
    x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
    int y = std::clamp(static_cast<int>(other.top), static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    SetWindowPos(window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
}
HWND child(HWND parent, wchar_t const *type, wchar_t const *text, DWORD style, int id, HINSTANCE instance, DWORD extended = 0)
{
    return CreateWindowEx(extended, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}
LRESULT CALLBACK inputProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (message == WM_IME_STARTCOMPOSITION) SetProp(window, L"SnipLeaf.Translation.Composing", reinterpret_cast<HANDLE>(1));
    if (message == WM_IME_ENDCOMPOSITION || message == WM_NCDESTROY) RemoveProp(window, L"SnipLeaf.Translation.Composing");
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, inputProc, 1);
    return DefSubclassProc(window, message, wp, lp);
}
} // namespace
TranslationPanel::TranslationPanel(App &app) : app_(app), service_(std::make_unique<TranslationService>(app.root()))
{
    try { credentials_ = store_.load(); }
    catch (...) { credentialError_ = L"翻译配置无法读取，请重新设置。"; }
}
TranslationPanel::~TranslationPanel()
{
    close();
    if (settings_) DestroyWindow(settings_);
    service_.reset();
}
void TranslationPanel::fontChanged(HWND window, HFONT &font)
{
    auto next = CreateFont(-MulDiv(14, GetDpiForWindow(window), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    for (HWND control = GetWindow(window, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT))
        SendMessage(control, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
    if (font) DeleteObject(font);
    font = next;
}
void TranslationPanel::create()
{
    if (window_) return;
    WNDCLASSEX wc{sizeof(wc)};
    wc.hInstance = app_.instance();
    wc.lpfnWndProc = windowProc;
    wc.lpszClassName = L"SnipLeaf.Translation";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = app_.icon_;
    RegisterClassEx(&wc);
    HWND owner = app_.find(canvas_) ? app_.find(canvas_)->hwnd() : app_.root();
    UINT dpi = GetDpiForWindow(owner);
    window_ = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"SnipLeaf · 英译中",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, MulDiv(640, dpi, 96), MulDiv(680, dpi, 96), app_.root(), nullptr, app_.instance(), this);
    if (!window_) throw std::runtime_error("Cannot create translation panel.");
    placeWindow(window_, owner, MulDiv(640, dpi, 96), MulDiv(680, dpi, 96));
}
void TranslationPanel::layout()
{
    if (!window_) return;
    RECT client{};
    GetClientRect(window_, &client);
    float s = GetDpiForWindow(window_) / 96.f;
    int width = static_cast<int>(client.right / s), height = static_cast<int>(client.bottom / s);
    auto position = [&](int id, int x, int y, int w, int h)
    {
        SetWindowPos(GetDlgItem(window_, id), nullptr, static_cast<int>(x * s), static_cast<int>(y * s),
                     std::max(1, static_cast<int>(w * s)), std::max(1, static_cast<int>(h * s)), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    int sourceHeight = std::max(45, (height - 236) / 2), resultY = 92 + sourceHeight;
    position(Header, 18, 14, width - 36, 24);
    position(SourceLabel, 18, 43, width - 36, 22);
    position(Source, 18, 68, width - 36, sourceHeight);
    position(ResultLabel, 18, resultY, width - 36, 22);
    position(Result, 18, resultY + 25, width - 36, std::max(45, height - 143 - resultY));
    position(Status, 18, height - 113, width - 36, 36);
    position(Privacy, 18, height - 76, width - 36, 28);
    position(Begin, 18, height - 42, 92, 30);
    position(Cancel, 118, height - 42, 64, 30);
    position(Configure, width - 218, height - 42, 94, 30);
    position(CopyResult, width - 116, height - 42, 98, 30);
}
void TranslationPanel::setSource(std::wstring const &text)
{
    filling_ = true;
    SetDlgItemText(window_, Source, text.c_str());
    filling_ = false;
    SendDlgItemMessage(window_, Source, EM_SETSEL, 0, 0);
    SendDlgItemMessage(window_, Source, EM_SCROLLCARET, 0, 0);
}
void TranslationPanel::update()
{
    if (!window_) return;
    SetDlgItemText(window_, Status, status_.c_str());
    if (textOf(GetDlgItem(window_, Result)) != result_) SetDlgItemText(window_, Result, result_.c_str());
    EnableWindow(GetDlgItem(window_, Begin), !busy_ && !waitingOcr_);
    EnableWindow(GetDlgItem(window_, Cancel), busy_ || waitingOcr_);
    EnableWindow(GetDlgItem(window_, CopyResult), !result_.empty() && !busy_);
    SetDlgItemText(window_, Begin, result_.empty() ? L"翻译" : L"重新翻译");
}
void TranslationPanel::cancel(std::wstring const &message)
{
    service_->cancel();
    ++generation_;
    busy_ = waitingOcr_ = false;
    result_.clear();
    submitted_.clear();
    rows_.clear();
    status_ = message;
    update();
}
void TranslationPanel::close()
{
    cancel(L"");
    canvas_ = revision_ = 0;
    if (window_) DestroyWindow(window_);
}
void TranslationPanel::open(uint64_t canvas, uint64_t revision, std::wstring text, bool waiting)
{
    cancel(L"");
    canvas_ = canvas;
    revision_ = revision;
    create();
    setSource(text);
    waitingOcr_ = waiting;
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    if (waiting) { status_ = L"正在本机识别文字，完成后翻译…"; update(); }
    else if (text.empty()) { status_ = L"没有识别到文字。可重新识别，也可在上方输入英文。"; update(); }
    else start();
}
void TranslationPanel::open(uint64_t canvas, uint64_t revision, std::vector<TranslationRow> rows)
{
    open(canvas, revision, translationText(rows));
}
void TranslationPanel::start()
{
    if (!window_ || busy_ || waitingOcr_) return;
    auto text = textOf(GetDlgItem(window_, Source));
    if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos)
    { status_ = L"请先选择或输入需要翻译的英文。"; update(); return; }
    if (text.size() > TranslationTextLimit)
    { status_ = L"一次最多翻译 12000 个字符，请缩小选区。"; update(); return; }
    if (!credentials_.valid() || !credentials_.enabled)
    {
        status_ = credentialError_.empty() ? L"请打开“翻译设置”，填写百度 APPID 与密钥，并启用联网翻译。" : credentialError_;
        update();
        return;
    }
    service_->cancel();
    ++generation_;
    result_.clear();
    rows_ = translationParagraphs(translationRows(text));
    submitted_ = text;
    busy_ = true;
    status_ = L"正在联网翻译…可随时取消，原图仍可操作。";
    service_->submit(generation_, rows_, credentials_);
    update();
}
void TranslationPanel::ocrReady(uint64_t canvas, uint64_t revision, OcrSnapshot const &snapshot)
{
    if (!window_ || !waitingOcr_ || canvas_ != canvas || revision_ != revision) return;
    waitingOcr_ = false;
    auto text = translationText(snapshot);
    setSource(text);
    if (text.empty()) { status_ = snapshot.status.empty() ? L"没有识别到文字。可在上方输入英文后翻译。" : snapshot.status; update(); }
    else start();
}
void TranslationPanel::sourceChanged(uint64_t canvas)
{
    if (canvas_ != canvas || !window_) return;
    cancel(L"图片内容已修改，请在图片中重新选择翻译内容。");
    setSource(L"");
}
void TranslationPanel::sourceClosed(uint64_t canvas)
{
    if (canvas_ == canvas) close();
}
void TranslationPanel::receive()
{
    for (auto &completion : service_->take())
    {
        if (!window_ || !busy_ || completion.generation != generation_) continue;
        if (!completion.rows.empty()) rows_ = std::move(completion.rows);
        if (!completion.finished)
        {
            result_ = translationText(rows_, true);
            status_ = L"正在翻译：已完成 " + std::to_wstring(std::count_if(rows_.begin(), rows_.end(), [](auto const &row) { return row.complete; })) +
                      L" / " + std::to_wstring(rows_.size()) + L" 段，可随时取消。";
        }
        else
        {
            busy_ = false;
            result_ = std::move(completion.result.text);
            if (!completion.result.error.empty())
                status_ = result_.empty() ? completion.result.error : L"仅完成部分翻译。" + completion.result.error;
            else status_ = L"翻译完成，已按段落分行，可拖选文字或复制全部译文。";
        }
        update();
    }
}
void TranslationPanel::pauseForCapture()
{
    if (window_) ShowWindow(window_, SW_HIDE);
    if (settings_) ShowWindow(settings_, SW_HIDE);
}
bool TranslationPanel::preTranslate(MSG const &message)
{
    for (auto window : {settings_, window_})
    {
        if (!window || (message.hwnd != window && !IsChild(window, message.hwnd))) continue;
        if (message.message == WM_KEYDOWN && (message.wParam == VK_ESCAPE || message.wParam == VK_RETURN) &&
            GetProp(message.hwnd, L"SnipLeaf.Translation.Composing")) return false;
        if (message.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && message.wParam == 'A')
        {
            wchar_t type[32]{}; GetClassName(message.hwnd, type, 32);
            if (wcscmp(type, L"Edit") == 0) { SendMessage(message.hwnd, EM_SETSEL, 0, -1); return true; }
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE)
        {
            if (window == settings_) DestroyWindow(settings_);
            else if (busy_ || waitingOcr_) cancel();
            else close();
            return true;
        }
        if (window == window_ && message.message == WM_KEYDOWN && message.wParam == VK_RETURN && GetKeyState(VK_CONTROL) < 0)
        { start(); return true; }
        MSG copy = message;
        return IsDialogMessage(window, &copy) != FALSE;
    }
    return false;
}
LRESULT CALLBACK TranslationPanel::windowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto self = reinterpret_cast<TranslationPanel *>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    { self = static_cast<TranslationPanel *>(reinterpret_cast<CREATESTRUCT *>(lp)->lpCreateParams); self->window_ = window;
      SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    if (!self) return DefWindowProc(window, message, wp, lp);
    try
    {
        switch (message)
        {
        case WM_CREATE:
            child(window, L"STATIC", L"英文 → 简体中文 · 百度翻译", 0, Header, self->app_.instance());
            child(window, L"STATIC", L"英文原文（可修改，空行分段）", 0, SourceLabel, self->app_.instance());
            child(window, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                  Source, self->app_.instance(), WS_EX_CLIENTEDGE);
            SetWindowSubclass(GetDlgItem(window, Source), inputProc, 1, 0);
            SendDlgItemMessage(window, Source, EM_SETLIMITTEXT, TranslationTextLimit, 0);
            child(window, L"STATIC", L"中文译文（按段落换行）", 0, ResultLabel, self->app_.instance());
            child(window, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                  Result, self->app_.instance(), WS_EX_CLIENTEDGE);
            SendDlgItemMessage(window, Result, EM_SETLIMITTEXT, 500000, 0);
            child(window, L"STATIC", L"", 0, Status, self->app_.instance());
            child(window, L"STATIC", L"联网翻译仅发送上方文字至百度，不上传图片。", 0, Privacy, self->app_.instance());
            child(window, L"BUTTON", L"翻译", WS_TABSTOP | BS_DEFPUSHBUTTON, Begin, self->app_.instance());
            child(window, L"BUTTON", L"取消", WS_TABSTOP, Cancel, self->app_.instance());
            child(window, L"BUTTON", L"翻译设置", WS_TABSTOP, Configure, self->app_.instance());
            child(window, L"BUTTON", L"复制译文", WS_TABSTOP, CopyResult, self->app_.instance());
            self->fontChanged(window, self->font_);
            self->layout();
            self->update();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        {
            auto dc = reinterpret_cast<HDC>(wp);
            SetBkColor(dc, RGB(255, 255, 255)); SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == Header ? RGB(22, 120, 185) : RGB(52, 64, 84));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        case WM_SIZE: self->layout(); return 0;
        case WM_GETMINMAXINFO:
        {
            auto info = reinterpret_cast<MINMAXINFO *>(lp);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            info->ptMinTrackSize = {std::min<LONG>(MulDiv(450, GetDpiForWindow(window), 96), monitor.rcWork.right - monitor.rcWork.left),
                                   std::min<LONG>(MulDiv(490, GetDpiForWindow(window), 96), monitor.rcWork.bottom - monitor.rcWork.top)};
            return 0;
        }
        case WM_DPICHANGED:
        {
            auto rect = reinterpret_cast<RECT *>(lp);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->fontChanged(window, self->font_); self->layout(); return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case Begin: self->start(); break;
            case Cancel: self->cancel(); break;
            case Configure: self->showSettings(); break;
            case CopyResult:
                if (!self->result_.empty() && !self->busy_)
                { self->status_ = self->app_.copyText_(window, self->result_) ? L"译文已复制。" : L"剪贴板暂时不可用，请重试。"; self->update(); }
                break;
            case Source:
                if (HIWORD(wp) == EN_CHANGE && !self->filling_)
                    self->cancel(L"原文已修改。点击“翻译”或按 Ctrl+Enter 重新翻译。");
                break;
            }
            return 0;
        case WM_CLOSE: self->close(); return 0;
        case WM_DESTROY:
            if (self->font_) DeleteObject(self->font_);
            self->font_ = nullptr; self->window_ = nullptr;
            return 0;
        }
    }
    catch (...) { self->status_ = L"翻译面板操作失败，请重试。"; self->update(); return message == WM_CREATE ? -1 : 0; }
    return DefWindowProc(window, message, wp, lp);
}
void TranslationPanel::showSettings()
{
    if (settings_)
    { ShowWindow(settings_, SW_SHOW); SetForegroundWindow(settings_); return; }
    WNDCLASSEX wc{sizeof(wc)};
    wc.hInstance = app_.instance(); wc.lpfnWndProc = settingsProc; wc.lpszClassName = L"SnipLeaf.TranslationSettings";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); wc.hIcon = app_.icon_;
    RegisterClassEx(&wc);
    HWND owner = window_ ? window_ : app_.root();
    UINT dpi = GetDpiForWindow(owner);
    settings_ = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"SnipLeaf · 翻译设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, MulDiv(540, dpi, 96), MulDiv(410, dpi, 96),
        app_.root(), nullptr, app_.instance(), this);
    if (!settings_) throw std::runtime_error("Cannot create translation settings.");
    placeWindow(settings_, owner, MulDiv(540, dpi, 96), MulDiv(410, dpi, 96));
    ShowWindow(settings_, SW_SHOW); SetForegroundWindow(settings_);
}
LRESULT CALLBACK TranslationPanel::settingsProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto self = reinterpret_cast<TranslationPanel *>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    { self = static_cast<TranslationPanel *>(reinterpret_cast<CREATESTRUCT *>(lp)->lpCreateParams); self->settings_ = window;
      SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    if (!self) return DefWindowProc(window, message, wp, lp);
    auto position = [&]
    {
        float s = GetDpiForWindow(window) / 96.f;
        RECT client{}; GetClientRect(window, &client);
        int w = static_cast<int>(client.right / s), h = static_cast<int>(client.bottom / s);
        auto move = [&](int id, int x, int y, int width, int height)
        { SetWindowPos(GetDlgItem(window, id), nullptr, static_cast<int>(x * s), static_cast<int>(y * s),
                       std::max(1, static_cast<int>(width * s)), std::max(1, static_cast<int>(height * s)), SWP_NOZORDER | SWP_NOACTIVATE); };
        bool compact = h < 350;
        move(430, 20, 12, w - 40, compact ? 36 : 46);
        move(431, 20, compact ? 60 : 76, 88, 26); move(AppId, 118, compact ? 56 : 72, w - 138, 28);
        move(432, 20, compact ? 100 : 120, 88, 26); move(Secret, 118, compact ? 96 : 116, w - 138, 28);
        move(Enabled, 20, compact ? 133 : 161, w - 40, 42);
        move(433, 20, compact ? 182 : 215, w - 40, compact ? 38 : 48);
        move(SetupStatus, 20, compact ? h - 91 : 268, w - 40, compact ? 31 : 38);
        move(Documentation, 20, h - 48, 104, 30); move(Clear, 132, h - 48, 96, 30);
        move(IDCANCEL, w - 204, h - 48, 84, 30); move(IDOK, w - 112, h - 48, 92, 30);
    };
    try
    {
        switch (message)
        {
        case WM_CREATE:
            child(window, L"STATIC", L"百度通用文本翻译\r\n请填写“开发者信息”页面的 APPID 和密钥。", 0, 430, self->app_.instance());
            child(window, L"STATIC", L"APPID", 0, 431, self->app_.instance());
            child(window, L"EDIT", self->credentials_.appId.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, AppId, self->app_.instance(), WS_EX_CLIENTEDGE);
            child(window, L"STATIC", L"密钥", 0, 432, self->app_.instance());
            child(window, L"EDIT", self->credentials_.secret.c_str(), WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, Secret, self->app_.instance(), WS_EX_CLIENTEDGE);
            SendDlgItemMessage(window, AppId, EM_SETLIMITTEXT, 32, 0);
            SendDlgItemMessage(window, Secret, EM_SETLIMITTEXT, 256, 0);
            child(window, L"BUTTON", L"启用联网翻译：点击“译”或“翻译”时，将待译文字发送至百度。", WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE, Enabled, self->app_.instance());
            SendDlgItemMessage(window, Enabled, BM_SETCHECK, self->credentials_.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            child(window, L"STATIC", L"截图和 OCR 仍在本机运行。密钥仅供当前 Windows 用户加密保存；翻译额度和费用以你的百度账户为准。", 0, 433, self->app_.instance());
            child(window, L"STATIC", self->credentialError_.c_str(), 0, SetupStatus, self->app_.instance());
            child(window, L"BUTTON", L"申请与说明", WS_TABSTOP, Documentation, self->app_.instance());
            child(window, L"BUTTON", L"清除凭据", WS_TABSTOP, Clear, self->app_.instance());
            child(window, L"BUTTON", L"取消", WS_TABSTOP, IDCANCEL, self->app_.instance());
            child(window, L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, self->app_.instance());
            self->fontChanged(window, self->settingsFont_); position(); return 0;
        case WM_SIZE: position(); return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            SetBkColor(reinterpret_cast<HDC>(wp), RGB(255, 255, 255));
            SetTextColor(reinterpret_cast<HDC>(wp), RGB(52, 64, 84));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        case WM_DPICHANGED:
        {
            auto rect = reinterpret_cast<RECT *>(lp);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->fontChanged(window, self->settingsFont_); position(); return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp))
            {
            case Documentation:
                ShellExecute(window, L"open", L"https://fanyi-api.baidu.com/doc/23", nullptr, nullptr, SW_SHOWNORMAL); break;
            case Clear:
                self->store_.clear(); self->credentials_ = {}; self->credentialError_.clear();
                self->cancel(L"翻译凭据已清除，联网翻译已关闭。");
                SetDlgItemText(window, AppId, L""); SetDlgItemText(window, Secret, L"");
                SendDlgItemMessage(window, Enabled, BM_SETCHECK, BST_UNCHECKED, 0);
                SetDlgItemText(window, SetupStatus, L"已清除本机保存的翻译凭据。"); break;
            case IDOK:
            {
                TranslationCredentials next{textOf(GetDlgItem(window, AppId)), textOf(GetDlgItem(window, Secret)),
                    SendDlgItemMessage(window, Enabled, BM_GETCHECK, 0, 0) == BST_CHECKED};
                if (!next.valid())
                { SetDlgItemText(window, SetupStatus, L"请填写有效的数字 APPID 和开发者密钥。不要填写 API Key。"); return 0; }
                self->store_.save(next); self->credentials_ = next; self->credentialError_.clear();
                self->cancel(next.enabled ? L"翻译配置已保存。点击“翻译”开始。" : L"配置已保存，联网翻译当前已关闭。");
                DestroyWindow(window); break;
            }
            case IDCANCEL: DestroyWindow(window); break;
            }
            return 0;
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY:
            if (self->settingsFont_) DeleteObject(self->settingsFont_);
            self->settingsFont_ = nullptr; self->settings_ = nullptr; return 0;
        }
    }
    catch (...) { SetDlgItemText(window, SetupStatus, L"翻译配置保存失败，请检查用户目录权限后重试。"); return message == WM_CREATE ? -1 : 0; }
    return DefWindowProc(window, message, wp, lp);
}
} // namespace snap
