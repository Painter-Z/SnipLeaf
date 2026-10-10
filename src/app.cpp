#include "app.h"
#include <commctrl.h>
namespace snap
{
namespace
{
constexpr UINT HotCapture = 1, HotPaste = 2, HotHide = 3;
constexpr UINT MenuCapture = 101, MenuPaste = 102, MenuHide = 103, MenuSettings = 104, MenuHelp = 105, MenuExit = 106,
               MenuRetry = 107, MenuDemo = 108;
std::wstring ini()
{
    return localDirectory() + L"\\settings.ini";
}
std::wstring number(UINT n)
{
    return std::to_wstring(n);
}
std::wstring hotName(UINT key, UINT mods)
{
    std::wstring s;
    if (mods & MOD_CONTROL)
        s += L"Ctrl+";
    if (mods & MOD_ALT)
        s += L"Alt+";
    if (mods & MOD_SHIFT)
        s += L"Shift+";
    UINT scan = MapVirtualKey(key, MAPVK_VK_TO_VSC);
    wchar_t buffer[64]{};
    GetKeyNameText(scan << 16, buffer, 64);
    return s + (buffer[0] ? buffer : L"?");
}
WORD toHot(UINT key, UINT mods)
{
    return MAKEWORD(key, ((mods & MOD_CONTROL) ? HOTKEYF_CONTROL : 0) | ((mods & MOD_ALT) ? HOTKEYF_ALT : 0) |
                             ((mods & MOD_SHIFT) ? HOTKEYF_SHIFT : 0));
}
std::pair<UINT, UINT> fromHot(HWND control)
{
    WORD value = static_cast<WORD>(SendMessage(control, HKM_GETHOTKEY, 0, 0));
    UINT flags = HIBYTE(value);
    return {LOBYTE(value), ((flags & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) | ((flags & HOTKEYF_ALT) ? MOD_ALT : 0) |
                               ((flags & HOTKEYF_SHIFT) ? MOD_SHIFT : 0)};
}
void configureOcr(HWND owner)
{
    std::wstring executable(32768, L'\0');
    auto length = GetModuleFileName(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size())
        return;
    executable.resize(length);
    auto helper = std::filesystem::path(executable).parent_path() / L"Installer" / L"Initialize-OcrLanguages.ps1";
    std::error_code error;
    if (!std::filesystem::exists(helper, error))
    {
        ShellExecute(owner, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectory(system, MAX_PATH))
        return;
    auto powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    auto diagnostics = std::filesystem::path(localDirectory()).parent_path() / L"SnipLeaf" / L"Diagnostics";
    auto parameters = L"-NoLogo -NoProfile -ExecutionPolicy Bypass -File \"" + helper.wstring() +
                      L"\" -ReportDirectory \"" + diagnostics.wstring() + L"\" -Interactive";
    SHELLEXECUTEINFO info{sizeof(info)};
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = powershell.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteEx(&info) && GetLastError() != ERROR_CANCELLED)
        MessageBox(owner, L"无法打开 OCR 配置窗口。请在 Windows 语言设置中检查中文和英文的语言功能。",
                   L"SnipLeaf · OCR 语言", MB_OK | MB_ICONINFORMATION);
}
} // namespace
void Settings::load()
{
    auto path = ini();
    captureKey = GetPrivateProfileInt(L"Hotkeys", L"CaptureKey", VK_F1, path.c_str());
    captureMods = GetPrivateProfileInt(L"Hotkeys", L"CaptureMods", 0, path.c_str());
    pasteKey = GetPrivateProfileInt(L"Hotkeys", L"PasteKey", VK_F3, path.c_str());
    pasteMods = GetPrivateProfileInt(L"Hotkeys", L"PasteMods", 0, path.c_str());
    autoOcr = GetPrivateProfileInt(L"OCR", L"Automatic", 1, path.c_str()) != 0;
    wchar_t buffer[80]{};
    GetPrivateProfileString(L"OCR", L"Language", L"", buffer, 80, path.c_str());
    language = buffer;
    textSizeTenths = GetPrivateProfileInt(L"Annotation", L"FontPtTenths", 180, path.c_str());
    if (textSizeTenths < 60 || textSizeTenths > 2880)
        textSizeTenths = 180;
    if (captureKey == 0 || captureKey > 254)
    {
        captureKey = VK_F1;
        captureMods = 0;
    }
    if (pasteKey == 0 || pasteKey > 254)
    {
        pasteKey = VK_F3;
        pasteMods = 0;
    }
    captureMods &= MOD_ALT | MOD_CONTROL | MOD_SHIFT;
    pasteMods &= MOD_ALT | MOD_CONTROL | MOD_SHIFT;
}
void Settings::save() const
{
    auto path = ini();
    WritePrivateProfileString(L"Hotkeys", L"CaptureKey", number(captureKey).c_str(), path.c_str());
    WritePrivateProfileString(L"Hotkeys", L"CaptureMods", number(captureMods).c_str(), path.c_str());
    WritePrivateProfileString(L"Hotkeys", L"PasteKey", number(pasteKey).c_str(), path.c_str());
    WritePrivateProfileString(L"Hotkeys", L"PasteMods", number(pasteMods).c_str(), path.c_str());
    WritePrivateProfileString(L"OCR", L"Automatic", autoOcr ? L"1" : L"0", path.c_str());
    WritePrivateProfileString(L"OCR", L"Language", language.c_str(), path.c_str());
    WritePrivateProfileString(L"Annotation", L"FontPtTenths", number(textSizeTenths).c_str(), path.c_str());
}
App::App(HINSTANCE instance) : instance_(instance)
{
    config.load();
}
App::~App()
{
    windows_.clear();
    ocr_.reset();
    if (settingsWindow_ && IsWindow(settingsWindow_))
        DestroyWindow(settingsWindow_);
    if (tray_.hWnd)
        Shell_NotifyIcon(NIM_DELETE, &tray_);
    UnregisterHotKey(root_, HotCapture);
    UnregisterHotKey(root_, HotPaste);
    UnregisterHotKey(root_, HotHide);
    if (root_ && IsWindow(root_))
        DestroyWindow(root_);
    if (icon_)
        DestroyIcon(icon_);
}
bool App::create(bool automated)
{
    testing_ = automated;
    WNDCLASSEX wc{sizeof(wc)};
    wc.lpfnWndProc = rootProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"SnapLite.Root";
    if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    root_ =
        CreateWindowEx(0, wc.lpszClassName, L"SnapLite", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance_, this);
    if (!root_)
        return false;
    ocr_ = std::make_unique<OcrService>(root_);
    taskbarCreated_ = RegisterWindowMessage(L"TaskbarCreated");
    icon_ = static_cast<HICON>(LoadImage(instance_, MAKEINTRESOURCE(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!icon_)
        icon_ = CopyIcon(LoadIcon(nullptr, IDI_APPLICATION));
    if (!testing_)
    {
        trayIcon();
        retryHotkeys(false);
        if (!hotkeyStatus_.empty())
            balloon(L"SnipLeaf 已启动", hotkeyStatus_, NIIF_WARNING);
        else
            balloon(L"SnipLeaf 已启动",
                    L"F1 截图 · F3 贴图 · Shift+F3 全部显隐\n右击托盘图标可查看设置和使用说明。");
    }
    return true;
}
void App::trayIcon()
{
    tray_ = {sizeof(tray_)};
    tray_.hWnd = root_;
    tray_.uID = 1;
    tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray_.uCallbackMessage = WM_TRAY;
    tray_.hIcon = icon_;
    std::wstring tip = L"SnipLeaf · " + hotName(config.captureKey, config.captureMods) + L" 截图 / " +
                       hotName(config.pasteKey, config.pasteMods) + L" 贴图";
    if (!hotkeyStatus_.empty())
        tip += L"（热键冲突）";
    wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIcon(NIM_ADD, &tray_);
    tray_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIcon(NIM_SETVERSION, &tray_);
}
void App::balloon(std::wstring const &title, std::wstring const &text, DWORD flags)
{
    if (testing_)
        return;
    NOTIFYICONDATA data{sizeof(data)};
    data.hWnd = root_;
    data.uID = 1;
    data.uFlags = NIF_INFO;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    data.dwInfoFlags = flags;
    Shell_NotifyIcon(NIM_MODIFY, &data);
}
void App::retryHotkeys(bool notify)
{
    if (!captureRegistered_)
        captureRegistered_ =
            RegisterHotKey(root_, HotCapture, config.captureMods | MOD_NOREPEAT, config.captureKey) != FALSE;
    if (!pasteRegistered_)
        pasteRegistered_ = RegisterHotKey(root_, HotPaste, config.pasteMods | MOD_NOREPEAT, config.pasteKey) != FALSE;
    if (!hideRegistered_)
        hideRegistered_ = RegisterHotKey(root_, HotHide, MOD_SHIFT | MOD_NOREPEAT, VK_F3) != FALSE;
    hotkeyStatus_.clear();
    if (!captureRegistered_)
        hotkeyStatus_ += hotName(config.captureKey, config.captureMods) + L" ";
    if (!pasteRegistered_)
        hotkeyStatus_ += hotName(config.pasteKey, config.pasteMods) + L" ";
    if (!hideRegistered_)
        hotkeyStatus_ += L"Shift+F3 ";
    if (!hotkeyStatus_.empty())
    {
        hotkeyStatus_ += L"已被其他软件占用。退出 Snipaste 后会自动重试；也可从托盘菜单截图或设置。";
        SetTimer(root_, 1, 5000, nullptr);
    }
    else
        KillTimer(root_, 1);
    if (tray_.hWnd)
    {
        std::wstring tip = L"SnipLeaf · " + hotName(config.captureKey, config.captureMods) + L" 截图 / " +
                           hotName(config.pasteKey, config.pasteMods) + L" 贴图";
        if (!hotkeyStatus_.empty())
            tip += L"（热键冲突）";
        wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
        tray_.uFlags = NIF_TIP;
        Shell_NotifyIcon(NIM_MODIFY, &tray_);
    }
    if (notify)
        balloon(L"快捷键", hotkeyStatus_.empty() ? L"快捷键已注册成功。" : hotkeyStatus_,
                hotkeyStatus_.empty() ? NIIF_INFO : NIIF_WARNING);
}
Canvas *App::find(uint64_t id)
{
    for (auto &w : windows_)
        if (w->id() == id)
            return w.get();
    return nullptr;
}
void App::capture()
{
    for (auto &w : windows_)
        if (!w->pinned() && w->hwnd())
        {
            SetForegroundWindow(w->hwnd());
            return;
        }
    try
    {
        RECT bounds{};
        auto image = captureDesktop(bounds);
        auto canvas = std::make_unique<Canvas>(*this, nextId_++, std::move(image), bounds);
        if (!canvas->create(true))
            throw std::runtime_error("Cannot create capture window.");
        windows_.push_back(std::move(canvas));
    }
    catch (...)
    {
        balloon(L"截图失败", errorText(), NIIF_ERROR);
    }
}
void App::pin(Document const &document, std::shared_ptr<OcrSnapshot> snapshot)
{
    auto canvas = std::make_unique<Canvas>(*this, nextId_++, std::make_unique<Document>(document));
    if (snapshot && snapshot->revision == document.revision)
        canvas->ocr_ = std::move(snapshot);
    if (!canvas->create(false))
        throw std::runtime_error("Cannot create pin window.");
    windows_.push_back(std::move(canvas));
    pinsVisible_ = true;
}
void App::paste()
{
    for (auto &w : windows_)
        if (!w->pinned() && w->hwnd())
        {
            // pin() may reallocate windows_, so retain the Canvas address rather
            // than the vector element reference across that call.
            Canvas *source = w.get();
            source->finishGesture();
            if (source->document())
            {
                pin(*source->document(), source->ocr_);
                source->close();
            }
            return;
        }
    try
    {
        auto image = pasteImage(root_, graphics);
        if (image.empty())
        {
            balloon(L"没有可贴的图片", L"先截图并复制，或在其他软件中复制一张图片，再按 F3。");
            return;
        }
        Document doc(std::move(image));
        pin(doc);
    }
    catch (...)
    {
        balloon(L"贴图失败", errorText(), NIIF_ERROR);
    }
}
void App::demo()
{
    auto canvas = std::make_unique<Canvas>(*this, nextId_++, std::make_unique<Document>(demoImage(graphics)), true);
    if (canvas->create(true))
        windows_.push_back(std::move(canvas));
}
void App::open(std::wstring const &path)
{
    auto canvas =
        std::make_unique<Canvas>(*this, nextId_++, std::make_unique<Document>(loadImage(graphics, path)), true);
    if (canvas->create(true))
        windows_.push_back(std::move(canvas));
}
void App::togglePins()
{
    pinsVisible_ = !pinsVisible_;
    for (auto &w : windows_)
        if (w->pinned())
            w->show(pinsVisible_);
}
void App::trayMenu()
{
    HMENU menu = CreatePopupMenu();
    auto capture = L"截图\t" + hotName(config.captureKey, config.captureMods),
         paste = L"贴图\t" + hotName(config.pasteKey, config.pasteMods);
    AppendMenu(menu, MF_STRING, MenuCapture, capture.c_str());
    AppendMenu(menu, MF_STRING, MenuPaste, paste.c_str());
    AppendMenu(menu, MF_STRING, MenuHide, pinsVisible_ ? L"隐藏全部贴图\tShift+F3" : L"显示全部贴图\tShift+F3");
    AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(menu, MF_STRING, MenuDemo, L"打开体验图片");
    AppendMenu(menu, MF_STRING, MenuSettings, L"设置…");
    AppendMenu(menu, MF_STRING, MenuRetry, L"重新注册快捷键");
    AppendMenu(menu, MF_STRING, MenuHelp, L"使用说明");
    AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(menu, MF_STRING, MenuExit, L"退出 SnipLeaf");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(root_);
    int result = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, root_, nullptr);
    DestroyMenu(menu);
    if (result)
        SendMessage(root_, WM_COMMAND, result, 0);
    PostMessage(root_, WM_NULL, 0, 0);
}
void App::help()
{
    std::wstring text =
        L"SnipLeaf " + std::wstring(Version) +
        L" · 开发预览版\n\n"
        L"F1 截图：拖动框选，拖动边框调整，空白默认拖动移动。\nF3 "
        L"贴图：截图时直接贴图，平时粘贴剪贴板图片。\nShift+F3：全部贴图隐藏 "
        L"/ 显示。\n\n"
        L"R 矩形 · E 椭圆 · A 箭头 · L 直线 · T 文字 · M 马赛克 · P 记号笔\n"
        L"V 选择标注：拖动边界框或控制点调整，Delete 删除。\n双击文字重新编辑；Enter 完成，Shift+Enter 换行。\n"
        L"O 选字：拖选后 Ctrl+C；Ctrl+A 选全部文字。\nEnter 或双击截图空白：复制图片。\nCtrl+S：保存 PNG；Ctrl+Z / "
        L"Ctrl+Y：撤销 / 重做。\n\n"
        L"贴图：空白处拖动移动，Space+拖动可从任意位置移动。\n滚轮缩放；Ctrl+0 恢复 100%；Tab "
        L"展开工具栏。\nCtrl+拖动平移图片；双击贴图空白 / Ctrl+W 关闭。\nEsc 依次取消编辑、清除选择、关闭。\n\n"
        L"OCR 在本机离线运行，不上传截图。\n退出 Snipaste 后，冲突的默认热键会自动重试。\n\n";
    text += packageIdentity().empty() ? L"应用身份：未注册，OCR 暂不可用。" : L"应用身份：已注册。";
    MessageBox(root_, text.c_str(), L"SnipLeaf · 使用说明", MB_OK | MB_ICONINFORMATION);
}
void App::settings()
{
    if (settingsWindow_ && IsWindow(settingsWindow_))
    {
        ShowWindow(settingsWindow_, SW_SHOW);
        SetForegroundWindow(settingsWindow_);
        return;
    }
    WNDCLASSEX wc{sizeof(wc)};
    wc.lpfnWndProc = settingsProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"SnapLite.Settings";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = icon_;
    RegisterClassEx(&wc);
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfo(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
    float scale = GetDpiForSystem() / 96.f;
    int w = static_cast<int>(510 * scale), h = static_cast<int>(400 * scale);
    settingsWindow_ = CreateWindowEx(
        WS_EX_APPWINDOW, L"SnapLite.Settings", L"SnipLeaf 设置", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
        mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, nullptr, nullptr, instance_, this);
    SetForegroundWindow(settingsWindow_);
}
LRESULT CALLBACK App::settingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto *self = reinterpret_cast<App *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        self = static_cast<App *>(reinterpret_cast<CREATESTRUCT *>(lp)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProc(hwnd, msg, wp, lp);
    if (msg == WM_CREATE)
    {
        float s = GetDpiForWindow(hwnd) / 96.f;
        auto font = CreateFont(-static_cast<int>(14 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                               0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        SetProp(hwnd, L"SnapLite.Font", font);
        SetProp(hwnd, L"SnapLite.Dpi", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(GetDpiForWindow(hwnd))));
        RECT client{0, 0, static_cast<LONG>(490 * s), static_cast<LONG>(370 * s)};
        AdjustWindowRectExForDpi(&client, static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE)), FALSE,
                                 static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd));
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        int outerWidth = client.right - client.left, outerHeight = client.bottom - client.top;
        SetWindowPos(hwnd, nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - outerWidth) / 2,
                     monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - outerHeight) / 2, outerWidth,
                     outerHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        auto control = [&](wchar_t const *cls, wchar_t const *label, DWORD style, int x, int y, int w, int h, int id)
        {
            HWND c = CreateWindowEx(0, cls, label, WS_CHILD | WS_VISIBLE | style, static_cast<int>(x * s),
                                    static_cast<int>(y * s), static_cast<int>(w * s), static_cast<int>(h * s), hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), self->instance_, nullptr);
            SendMessage(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return c;
        };
        control(L"STATIC", L"快捷键", 0, 24, 18, 440, 25, 0);
        control(L"STATIC", L"截图", 0, 24, 63, 110, 24, 0);
        auto capture = control(HOTKEY_CLASS, L"", WS_TABSTOP | WS_BORDER, 155, 58, 295, 30, 201);
        SendMessage(capture, HKM_SETHOTKEY, toHot(self->config.captureKey, self->config.captureMods), 0);
        control(L"STATIC", L"贴图", 0, 24, 108, 110, 24, 0);
        auto paste = control(HOTKEY_CLASS, L"", WS_TABSTOP | WS_BORDER, 155, 103, 295, 30, 202);
        SendMessage(paste, HKM_SETHOTKEY, toHot(self->config.pasteKey, self->config.pasteMods), 0);
        control(L"STATIC", L"全部贴图显隐：Shift+F3", 0, 24, 148, 440, 24, 0);
        auto automatic =
            control(L"BUTTON", L"截图与贴图自动识别文字", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 190, 426, 26, 203);
        SendMessage(automatic, BM_SETCHECK, self->config.autoOcr ? BST_CHECKED : BST_UNCHECKED, 0);
        control(L"STATIC", L"识别语言", 0, 24, 239, 110, 24, 0);
        auto languages = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 155, 232, 295, 140, 204);
        SendMessage(languages, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"自动（优先中文，可识别英文）"));
        std::vector<std::wstring> all;
        bool languageQueryReady = true;
        try
        {
            all = ocrLanguages();
        }
        catch (...)
        {
            languageQueryReady = false;
        }
        int selected = 0;
        for (size_t i = 0; i < all.size(); ++i)
        {
            SendMessage(languages, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(all[i].c_str()));
            if (all[i] == self->config.language)
                selected = static_cast<int>(i) + 1;
        }
        SendMessage(languages, CB_SETCURSEL, selected, 0);
        std::wstring status = !languageQueryReady ? L"OCR 语言检查失败，请配置语言；截图和贴图仍可使用。"
                              : all.empty()      ? L"当前没有 OCR 语言，请先配置；截图和贴图仍可使用。"
                                                 : L"已有 " + std::to_wstring(all.size()) + L" 种 OCR 语言，可离线识别。";
        status += L"\n补齐语言后，请退出再启动 SnipLeaf。";
        control(L"STATIC", status.c_str(), 0, 24, 276, 450, 40, 0);
        control(L"BUTTON", L"恢复默认", WS_TABSTOP, 24, 320, 100, 30, 205);
        control(L"BUTTON", L"配置 OCR 语言…", WS_TABSTOP, 140, 320, 185, 30, 206);
        control(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, 350, 320, 100, 30, IDOK);
        return 0;
    }
    if (msg == WM_COMMAND)
    {
        if (LOWORD(wp) == 206)
        {
            configureOcr(hwnd);
            return 0;
        }
        if (LOWORD(wp) == 205)
        {
            SendDlgItemMessage(hwnd, 201, HKM_SETHOTKEY, toHot(VK_F1, 0), 0);
            SendDlgItemMessage(hwnd, 202, HKM_SETHOTKEY, toHot(VK_F3, 0), 0);
            return 0;
        }
        if (LOWORD(wp) == IDOK)
        {
            auto capture = fromHot(GetDlgItem(hwnd, 201)), paste = fromHot(GetDlgItem(hwnd, 202));
            if (!capture.first || !paste.first || capture == paste ||
                capture == std::pair<UINT, UINT>{VK_F3, MOD_SHIFT} || paste == std::pair<UINT, UINT>{VK_F3, MOD_SHIFT})
            {
                MessageBox(hwnd, L"请设置两个不同的有效快捷键，并保留 Shift+F3 用于显隐贴图。", L"快捷键",
                           MB_OK | MB_ICONWARNING);
                return 0;
            }
            UnregisterHotKey(self->root_, HotCapture);
            UnregisterHotKey(self->root_, HotPaste);
            self->captureRegistered_ = self->pasteRegistered_ = false;
            self->config.captureKey = capture.first;
            self->config.captureMods = capture.second;
            self->config.pasteKey = paste.first;
            self->config.pasteMods = paste.second;
            self->config.autoOcr = SendDlgItemMessage(hwnd, 203, BM_GETCHECK, 0, 0) == BST_CHECKED;
            int index = static_cast<int>(SendDlgItemMessage(hwnd, 204, CB_GETCURSEL, 0, 0));
            wchar_t lang[100]{};
            if (index > 0)
                SendDlgItemMessage(hwnd, 204, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(lang));
            self->config.language = index > 0 ? lang : L"";
            self->config.save();
            self->retryHotkeys();
            if (self->config.autoOcr)
                for (auto &w : self->windows_)
                    w->requestOcr();
            DestroyWindow(hwnd);
            self->settingsWindow_ = nullptr;
            return 0;
        }
    }
    if (msg == WM_DPICHANGED)
    {
        UINT oldDpi = static_cast<UINT>(reinterpret_cast<UINT_PTR>(GetProp(hwnd, L"SnapLite.Dpi"))),
             newDpi = HIWORD(wp);
        if (!oldDpi)
            oldDpi = 96;
        auto font = CreateFont(-MulDiv(14, newDpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                               CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            RECT rect{};
            GetWindowRect(child, &rect);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT *>(&rect), 2);
            SetWindowPos(child, nullptr, MulDiv(rect.left, newDpi, oldDpi), MulDiv(rect.top, newDpi, oldDpi),
                         MulDiv(rect.right - rect.left, newDpi, oldDpi), MulDiv(rect.bottom - rect.top, newDpi, oldDpi),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessage(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        auto previous = static_cast<HFONT>(GetProp(hwnd, L"SnapLite.Font"));
        SetProp(hwnd, L"SnapLite.Font", font);
        if (previous)
            DeleteObject(previous);
        SetProp(hwnd, L"SnapLite.Dpi", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(newDpi)));
        auto rect = reinterpret_cast<RECT *>(lp);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (msg == WM_DESTROY)
    {
        auto font = static_cast<HFONT>(RemoveProp(hwnd, L"SnapLite.Font"));
        RemoveProp(hwnd, L"SnapLite.Dpi");
        if (font)
            DeleteObject(font);
        self->settingsWindow_ = nullptr;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
void App::quit()
{
    if (quitting_)
        return;
    quitting_ = true;
    windows_.clear();
    PostQuitMessage(0);
}
LRESULT CALLBACK App::rootProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto *self = reinterpret_cast<App *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        self = static_cast<App *>(reinterpret_cast<CREATESTRUCT *>(lp)->lpCreateParams);
        self->root_ = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProc(hwnd, msg, wp, lp);
    try
    {
        return self->message(msg, wp, lp);
    }
    catch (...)
    {
        self->balloon(L"SnipLeaf", errorText(), NIIF_ERROR);
        return 0;
    }
}
LRESULT App::message(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == taskbarCreated_ && taskbarCreated_)
    {
        trayIcon();
        return 0;
    }
    switch (msg)
    {
    case WM_HOTKEY:
        if (wp == HotCapture)
            capture();
        else if (wp == HotPaste)
            paste();
        else if (wp == HotHide)
            togglePins();
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp))
        {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            trayMenu();
            break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            capture();
            break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case MenuCapture:
            capture();
            break;
        case MenuPaste:
            paste();
            break;
        case MenuHide:
            togglePins();
            break;
        case MenuSettings:
            settings();
            break;
        case MenuHelp:
            help();
            break;
        case MenuExit:
            quit();
            break;
        case MenuRetry:
            retryHotkeys();
            break;
        case MenuDemo:
            demo();
            break;
        }
        return 0;
    case WM_OCR_READY:
        for (auto &result : ocr_->take())
            if (auto *w = find(result.window))
                w->receive(std::move(result.result));
        return 0;
    case WM_CANVAS_CLOSED:
        std::erase_if(windows_, [&](auto const &w) { return w->id() == wp; });
        return 0;
    case WM_TIMER:
        if (wp == 1)
            retryHotkeys(false);
        return 0;
    case WM_COPYDATA:
    {
        auto data = reinterpret_cast<COPYDATASTRUCT *>(lp);
        if (data->dwData == 1)
            demo();
        else if (data->dwData == 2)
            capture();
        else if (data->dwData == 3)
            paste();
        else if (data->dwData == 5)
            quit();
        else if (data->dwData == 4 && data->lpData && data->cbData >= sizeof(wchar_t) && data->cbData < 65536)
        {
            auto *text = static_cast<wchar_t const *>(data->lpData);
            if (text[data->cbData / sizeof(wchar_t) - 1] == 0)
                open(text);
        }
        return TRUE;
    }
    case WM_CLOSE:
        quit();
        return 0;
    }
    return DefWindowProc(root_, msg, wp, lp);
}
bool App::preTranslate(MSG const &msg)
{
    if (settingsWindow_ && (msg.hwnd == settingsWindow_ || IsChild(settingsWindow_, msg.hwnd)) &&
        IsDialogMessage(settingsWindow_, const_cast<MSG *>(&msg)))
        return true;
    for (auto &w : windows_)
        if (w->preTranslate(msg))
            return true;
    return false;
}
int App::run()
{
    MSG msg{};
    BOOL result{};
    while ((result = GetMessage(&msg, nullptr, 0, 0)) > 0)
    {
        if (!preTranslate(msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return result < 0 ? 1 : static_cast<int>(msg.wParam);
}
} // namespace snap
