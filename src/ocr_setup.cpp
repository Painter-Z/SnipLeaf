#include "ocr_setup.h"
#include <commctrl.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.Ocr.h>
namespace snap
{
using winrt::Windows::Data::Json::JsonObject;
namespace
{
constexpr int Begin = 301, Refresh = 302, Logs = 303, Later = 304;
constexpr std::array<wchar_t const *, 4> Capabilities{
    L"Language.Basic~~~zh-CN~0.0.1.0", L"Language.OCR~~~zh-CN~0.0.1.0",
    L"Language.Basic~~~en-US~0.0.1.0", L"Language.OCR~~~en-US~0.0.1.0"};
std::filesystem::path programDirectory()
{
    std::wstring executable(32768, L'\0');
    DWORD n = GetModuleFileName(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!n || n >= executable.size())
        throw std::runtime_error("Cannot locate program directory.");
    executable.resize(n);
    return std::filesystem::path(executable).parent_path();
}
std::filesystem::path powershellPath()
{
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectory(system, MAX_PATH))
        throw std::runtime_error("Cannot locate Windows PowerShell.");
    return std::filesystem::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
}
std::string readSmallFile(std::filesystem::path const &path)
{
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size > 65536)
        return {};
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
JsonObject readJson(std::string text)
{
    if (text.starts_with("\xef\xbb\xbf"))
        text.erase(0, 3);
    return JsonObject::Parse(winrt::to_hstring(text));
}
std::wstring setupIni()
{
    return (std::filesystem::path(localDirectory()) / L"settings.ini").wstring();
}
std::wstring stateText(std::wstring const &state)
{
    if (state == L"Installed") return L"已就绪";
    if (state == L"InstallPending") return L"等待重启";
    if (state == L"NotPresent") return L"未安装";
    if (state == L"Checking" || state.empty()) return L"待检查";
    if (state == L"QueryFailed") return L"检查失败";
    return L"尚未就绪";
}
uint64_t creationTime(HANDLE process)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user))
        return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
} // namespace
unsigned OcrPreparationProgress::installed() const
{
    return static_cast<unsigned>(std::count(states.begin(), states.end(), L"Installed"));
}
std::wstring ocrCapabilityLabel(std::wstring const &name)
{
    for (size_t i = 0; i < Capabilities.size(); ++i)
        if (name == Capabilities[i])
            return std::wstring(i < 2 ? L"中文" : L"英文") + (i % 2 ? L" OCR" : L"基础语言");
    return L"Windows 语言组件";
}
OcrReadiness inspectOcrReadiness()
{
    OcrReadiness result;
    result.identity = !packageIdentity().empty();
    if (!result.identity)
        return result;
    try
    {
        using winrt::Windows::Media::Ocr::OcrEngine;
        using winrt::Windows::Globalization::Language;
        result.chinese = static_cast<bool>(OcrEngine::TryCreateFromLanguage(Language(L"zh-Hans-CN")));
        result.english = static_cast<bool>(OcrEngine::TryCreateFromLanguage(Language(L"en-US")));
    }
    catch (...)
    {
        result.error = errorText();
    }
    return result;
}
bool offerOcrPreparation(OcrReadiness const &readiness, bool alreadyOffered)
{
    return !alreadyOffered && !readiness.ready();
}
OcrPreparationProgress parseOcrPreparation(std::string const &utf8)
{
    OcrPreparationProgress p;
    try
    {
        auto object = readJson(utf8);
        if (object.GetNamedNumber(L"schema", 0) != 1)
            return p;
        p.phase = object.GetNamedString(L"phase", L"");
        if (p.phase != L"checking" && p.phase != L"installing" && p.phase != L"completed" &&
            p.phase != L"failed" && p.phase != L"restart-required")
            return {};
        p.restartNeeded = object.GetNamedBoolean(L"restartNeeded", false);
        p.activeCapability = object.GetNamedString(L"activeCapability", L"");
        auto capabilities = object.GetNamedArray(L"capabilities");
        if (capabilities.Size() > 4)
            return {};
        std::array<bool, 4> seen{};
        for (auto const &value : capabilities)
        {
            auto cap = value.GetObject();
            auto name = cap.GetNamedString(L"name");
            bool known = false;
            for (size_t i = 0; i < Capabilities.size(); ++i)
                if (name == Capabilities[i])
                {
                    if (seen[i]) return {};
                    seen[i] = true;
                    p.states[i] = cap.GetNamedString(L"state", L"Checking");
                    known = true;
                }
            if (!known) return {};
        }
        auto errors = object.GetNamedArray(L"errors");
        if (errors.Size())
            p.error = errors.GetAt(0).GetObject().GetNamedString(L"message", L"Windows 语言准备失败");
        if (p.error.size() > 500) p.error.resize(500);
        p.success = object.GetNamedBoolean(L"success", false) && p.phase == L"completed" &&
                    p.installed() == 4 && errors.Size() == 0;
        p.valid = true;
    }
    catch (...)
    {
        return {};
    }
    return p;
}
OcrSetup::OcrSetup(HINSTANCE instance, HWND owner, bool testing, std::function<void(HWND)> repair,
                   std::function<void()> refreshed)
    : instance_(instance), owner_(owner), testing_(testing), repair_(std::move(repair)), refreshed_(std::move(refreshed))
{
    diagnostics_ = std::filesystem::path(localDirectory()).parent_path() / L"SnipLeaf" / L"Diagnostics";
    if (!testing_)
        offered_ = GetPrivateProfileInt(L"OCR", L"PreparationOffered", 0, setupIni().c_str()) != 0;
}
OcrSetup::~OcrSetup()
{
    KillTimer(owner_, 2);
    if (probe_.joinable()) probe_.join();
    if (window_) DestroyWindow(window_);
    // Windows servicing is deliberately independent of the app lifetime.
    if (process_) CloseHandle(process_);
}
void OcrSetup::startProbe()
{
    if (checking_) return;
    if (probe_.joinable()) probe_.join();
    checking_ = true;
    probe_ = std::thread([this]
    {
        OcrReadiness result;
        try
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            result = inspectOcrReadiness();
            winrt::uninit_apartment();
        }
        catch (...) { result.error = errorText(); }
        {
            std::lock_guard lock(probeMutex_);
            pending_ = std::move(result);
        }
        PostMessage(owner_, WM_OCR_SETUP_READY, 0, 0);
    });
    update();
}
void OcrSetup::start(bool offerAutomatically)
{
    automatic_ = offerAutomatically;
    if (!testing_) resumeSession();
    startProbe();
}
void OcrSetup::markOffered()
{
    offered_ = true;
    if (!testing_)
        WritePrivateProfileString(L"OCR", L"PreparationOffered", L"1", setupIni().c_str());
}
void OcrSetup::receive()
{
    {
        std::lock_guard lock(probeMutex_);
        if (!pending_) return;
        readiness_ = std::move(*pending_);
        pending_.reset();
    }
    checking_ = false;
    if (probe_.joinable()) probe_.join();
    if (automatic_ && (process_ || offerOcrPreparation(readiness_, offered_)))
    {
        bool canPrepare = !process_ && readiness_.identity && readiness_.error.empty();
        show(true);
        if (canPrepare)
            countdown_ = Clock::now() + std::chrono::seconds(8);
        markOffered();
    }
    automatic_ = false;
    if (awaitingVerification_)
    {
        awaitingVerification_ = false;
        if (readiness_.ready())
        {
            if (progress_.success)
                notice_ = L"中英文 OCR 引擎验证通过，可以直接在截图和贴图上选字复制。";
            else
                notice_ += L"\n当前中英文 OCR 引擎可用；组件准备的错误仍保留在日志中。";
            refreshed_();
        }
        else if (progress_.success)
            notice_ = L"语言组件已安装，当前进程尚未取得全部 OCR 引擎。请保存工作后退出并重开 SnipLeaf；Windows 要求时再重启电脑。";
    }
    update();
    if (!window_ && !process_) KillTimer(owner_, 2);
}
void OcrSetup::show(bool activate)
{
    if (!window_)
    {
        WNDCLASSEX wc{sizeof(wc)};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = instance_;
        wc.lpszClassName = L"SnipLeaf.OcrSetup";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.hIcon = LoadIcon(instance_, MAKEINTRESOURCE(1));
        RegisterClassEx(&wc);
        window_ = CreateWindowEx(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, wc.lpszClassName, L"SnipLeaf · OCR 准备",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                  580, 470, nullptr, nullptr, instance_, this);
    }
    if (!window_) return;
    SetTimer(owner_, 2, 1000, nullptr);
    if (!testing_)
    {
        ShowWindow(window_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        if (activate) SetForegroundWindow(window_);
    }
    update();
}
void OcrSetup::pauseForCapture()
{
    automatic_ = false;
    countdown_ = {};
    if (window_ && IsWindowVisible(window_))
    {
        ShowWindow(window_, SW_HIDE);
        DwmFlush();
    }
}
void OcrSetup::close()
{
    countdown_ = {};
    markOffered();
    if (window_) DestroyWindow(window_);
    if (!process_) KillTimer(owner_, 2);
}
bool OcrSetup::preTranslate(MSG const &msg)
{
    if (window_ && (msg.hwnd == window_ || IsChild(window_, msg.hwnd)))
    {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            close();
            return true;
        }
        return IsDialogMessage(window_, const_cast<MSG *>(&msg)) != FALSE;
    }
    return false;
}
void OcrSetup::prepare()
{
    countdown_ = {};
    markOffered();
    if (process_ || checking_ || testing_) return;
    if (!readiness_.identity)
    {
        repair_(window_);
        notice_ = L"身份修复完成后，请退出并重开 SnipLeaf，再检查 OCR。";
        update();
        return;
    }
    try
    {
        auto helper = programDirectory() / L"Installer" / L"Initialize-OcrLanguages.ps1";
        if (!std::filesystem::is_regular_file(helper))
        {
            notice_ = L"当前目录没有语言准备组件。已打开 Windows 语言设置，可在那里补齐中文和英文 OCR。";
            ShellExecute(window_, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL);
            update();
            return;
        }
        GUID guid{};
        check(CoCreateGuid(&guid));
        wchar_t id[40]{};
        StringFromGUID2(guid, id, 40);
        std::wstring runId = std::wstring(id + 1, 36);
        auto run = diagnostics_ / runId;
        std::filesystem::create_directories(run);
        report_ = run / L"ocr-prerequisites.json";
        auto ps = powershellPath().wstring();
        std::wstring parameters = L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
            helper.wstring() + L"\" -ReportDirectory \"" + diagnostics_.wstring() + L"\" -RunId " + runId;
        SHELLEXECUTEINFO info{sizeof(info)};
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        info.hwnd = window_;
        info.lpVerb = L"runas";
        info.lpFile = ps.c_str();
        info.lpParameters = parameters.c_str();
        info.nShow = SW_HIDE;
        if (!ShellExecuteEx(&info))
        {
            DWORD error = GetLastError();
            notice_ = error == ERROR_CANCELLED ? L"已取消管理员授权。截图和贴图仍可使用，稍后可再次准备 OCR。"
                : L"Windows 未能启动语言准备（错误 " + std::to_wstring(error) + L"）。请查看系统或安全软件的阻止记录。";
        }
        else if (info.hProcess)
        {
            process_ = info.hProcess;
            progress_ = {};
            lastReport_.clear();
            started_ = updated_ = Clock::now();
            notice_.clear();
            try { persistSession(); } catch (...) { /* The live process remains observable without persistence. */ }
            SetTimer(owner_, 2, 1000, nullptr);
        }
        else notice_ = L"未取得准备进程的状态，请在 Windows 语言设置中检查 OCR。";
    }
    catch (...) { notice_ = L"无法准备 OCR：" + errorText(); }
    update();
}
void OcrSetup::persistSession()
{
    JsonObject object;
    object.Insert(L"runId", winrt::Windows::Data::Json::JsonValue::CreateStringValue(report_.parent_path().filename().wstring()));
    object.Insert(L"pid", winrt::Windows::Data::Json::JsonValue::CreateNumberValue(GetProcessId(process_)));
    // Store the 64-bit creation time as a string, avoiding JSON number rounding.
    object.Insert(L"created", winrt::Windows::Data::Json::JsonValue::CreateStringValue(std::to_wstring(creationTime(process_))));
    std::ofstream stream(diagnostics_ / L"active-ocr-setup.json", std::ios::binary | std::ios::trunc);
    stream << winrt::to_string(object.Stringify());
}
void OcrSetup::resumeSession()
{
    HANDLE candidate{};
    try
    {
        auto object = readJson(readSmallFile(diagnostics_ / L"active-ocr-setup.json"));
        std::wstring runId(object.GetNamedString(L"runId"));
        if (runId.size() != 36 || !std::all_of(runId.begin(), runId.end(), [](wchar_t c)
            { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F') || c == L'-'; })) return;
        report_ = diagnostics_ / runId / L"ocr-prerequisites.json";
        lastReport_ = readSmallFile(report_);
        progress_ = parseOcrPreparation(lastReport_);
        if (progress_.valid && (progress_.phase == L"failed" || progress_.phase == L"restart-required"))
            notice_ = progress_.error.empty() ? L"上次语言准备未完成，可重新检查或查看日志。" : L"上次语言准备未完成：" + progress_.error;
        auto pidValue = object.GetNamedNumber(L"pid");
        if (pidValue < 1 || pidValue > MAXDWORD || pidValue != std::floor(pidValue)) return;
        candidate = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pidValue));
        if (!candidate) return;
        wchar_t name[32768]{};
        DWORD length = 32768;
        if (WaitForSingleObject(candidate, 0) != WAIT_TIMEOUT ||
            !QueryFullProcessImageName(candidate, 0, name, &length) ||
            _wcsicmp(name, powershellPath().c_str()) != 0 ||
            std::to_wstring(creationTime(candidate)) != std::wstring(object.GetNamedString(L"created")))
        {
            CloseHandle(candidate);
            return;
        }
        process_ = candidate;
        candidate = nullptr;
        started_ = updated_ = Clock::now();
        notice_ = L"已重新连接上次仍在运行的语言准备，不会重复下载。";
        SetTimer(owner_, 2, 1000, nullptr);
    }
    catch (...) { if (candidate) CloseHandle(candidate); }
}
void OcrSetup::tick()
{
    if (countdown_ != Clock::time_point{} && !process_ && !checking_ && Clock::now() >= countdown_)
        prepare();
    if (process_)
    {
        auto text = readSmallFile(report_);
        if (!text.empty() && text != lastReport_)
        {
            auto parsed = parseOcrPreparation(text);
            if (parsed.valid)
            {
                progress_ = std::move(parsed);
                lastReport_ = std::move(text);
                updated_ = Clock::now();
            }
        }
        if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0)
        {
            DWORD exitCode{1};
            GetExitCodeProcess(process_, &exitCode);
            CloseHandle(process_);
            process_ = nullptr;
            if (exitCode != 0) progress_.success = false;
            if (!progress_.valid || !progress_.success || exitCode != 0)
                notice_ = progress_.error.empty() ? L"语言准备未完成。可打开日志查看原因；已有语言仍可使用，软件不会回滚。"
                                                  : L"语言准备未完成：" + progress_.error;
            else notice_ = L"语言组件已就绪，正在验证中英文 OCR 引擎…";
            awaitingVerification_ = true;
            startProbe();
        }
    }
    update();
    if (!window_ && !process_ && !checking_) KillTimer(owner_, 2);
}
void OcrSetup::update()
{
    if (!window_) return;
    auto set = [&](int id, std::wstring const &text) { SetDlgItemText(window_, id, text.c_str()); };
    unsigned ready = static_cast<unsigned>(readiness_.chinese) + static_cast<unsigned>(readiness_.english);
    set(310, checking_ ? L"正在检查离线 OCR…" : L"离线 OCR：" + std::to_wstring(ready) + L" / 2 种语言就绪");
    set(311, L"只补齐 Windows 的中文、英文基础语言和 OCR 组件。\n初次下载需要联网，之后识别在本机离线完成。截图和贴图始终可用。");
    set(312, L"中文 OCR：" + std::wstring(checking_ ? L"检查中" : readiness_.chinese ? L"已就绪" : L"尚未就绪"));
    set(313, L"英文 OCR：" + std::wstring(checking_ ? L"检查中" : readiness_.english ? L"已就绪" : L"尚未就绪"));
    std::wstring components;
    for (size_t i = 0; i < Capabilities.size(); ++i)
    {
        if (i) components += L"\n";
        components += ocrCapabilityLabel(Capabilities[i]) + L"：" +
            (progress_.valid ? stateText(progress_.states[i]) : L"开始准备后检查");
    }
    set(314, components);
    SendDlgItemMessage(window_, 315, PBM_SETRANGE32, 0, 4);
    SendDlgItemMessage(window_, 315, PBM_SETPOS, progress_.valid ? progress_.installed() : 0, 0);
    std::wstring status;
    if (process_)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_).count();
        auto idle = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - updated_).count();
        status = L"Windows 组件已核实 " + std::to_wstring(progress_.installed()) + L" / 4；本次观察已用时 " +
            std::to_wstring(elapsed / 60) + L" 分 " + std::to_wstring(elapsed % 60) + L" 秒。\n";
        status += progress_.phase == L"installing" ? L"正在下载或安装：" + ocrCapabilityLabel(progress_.activeCapability)
                                                 : L"正在检查 Windows 语言组件…";
        status += idle >= 90 ? L"\nWindows 暂无新的进度，准备进程仍在运行。可稍后查看，不必重复安装。"
                            : L"\n组件计数代表已安装状态，不是下载百分比。";
    }
    else if (checking_) status = L"正在后台验证身份和中英文 OCR 引擎，不会阻塞截图。";
    else if (countdown_ != Clock::time_point{})
    {
        auto seconds = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::seconds>(countdown_ - Clock::now()).count() + 1);
        status = std::to_wstring(seconds) + L" 秒后自动准备缺少的语言，Windows 将请求管理员确认。\n可选择“稍后”，以后从托盘的“OCR 准备…”继续。";
    }
    else if (!notice_.empty()) status = notice_;
    else if (!readiness_.identity) status = L"OCR 本地身份尚未就绪，请先修复身份；无需注册在线账号。\n截图、标注和贴图仍可使用。";
    else if (!readiness_.error.empty()) status = L"OCR 检查失败：" + readiness_.error;
    else if (readiness_.ready()) status = L"中英文 OCR 引擎均已验证，可以直接在截图和贴图上拖选文字，按 Ctrl+C 复制。";
    else status = L"准备缺少的语言即可启用完整中英文 OCR。已就绪的语言不会重复下载。";
    if (progress_.restartNeeded && !process_) status += L"\nWindows 报告需要重启，请保存工作后按系统提示操作。";
    set(316, status);
    set(317, process_ ? L"关闭此窗口或退出 SnipLeaf，Windows 仍会继续准备；再次打开可查看状态。"
                      : L"准备失败会保留软件和已有语言；不会自动重启电脑，也不会更改安全防护设置。");
    set(Begin, !readiness_.identity ? L"修复 OCR 身份…" : readiness_.ready() ? L"OCR 已就绪" : L"立即准备缺少的语言");
    EnableWindow(GetDlgItem(window_, Begin), !process_ && !checking_ && !readiness_.ready());
    EnableWindow(GetDlgItem(window_, Refresh), !process_ && !checking_);
    set(Later, process_ ? L"后台继续" : L"稍后 / 关闭");
}
LRESULT CALLBACK OcrSetup::windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto self = reinterpret_cast<OcrSetup *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        self = static_cast<OcrSetup *>(reinterpret_cast<CREATESTRUCT *>(lp)->lpCreateParams);
        self->window_ = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProc(hwnd, msg, wp, lp);
    if (msg == WM_CREATE)
    {
        UINT dpi = GetDpiForWindow(hwnd);
        auto font = [&](int size, int weight) { return CreateFont(-MulDiv(size, dpi, 96), 0, 0, 0, weight,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI"); };
        self->font_ = font(14, FW_NORMAL);
        self->titleFont_ = font(21, FW_SEMIBOLD);
        SetProp(hwnd, L"SnipLeaf.Dpi", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(dpi)));
        RECT rect{0, 0, MulDiv(580, dpi, 96), MulDiv(466, dpi, 96)};
        AdjustWindowRectExForDpi(&rect, static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE)), FALSE,
                                static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE)), dpi);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        int w = rect.right - rect.left, h = rect.bottom - rect.top;
        SetWindowPos(hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
            mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        auto control = [&](wchar_t const *cls, DWORD style, int x, int y, int w1, int h1, int id, wchar_t const *label = L"")
        {
            auto child = CreateWindowEx(0, cls, label, WS_CHILD | WS_VISIBLE | style, MulDiv(x, dpi, 96),
                MulDiv(y, dpi, 96), MulDiv(w1, dpi, 96), MulDiv(h1, dpi, 96), hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), self->instance_, nullptr);
            SendMessage(child, WM_SETFONT, reinterpret_cast<WPARAM>(id == 310 ? self->titleFont_ : self->font_), TRUE);
        };
        control(L"STATIC", SS_NOPREFIX, 24, 20, 532, 30, 310);
        control(L"STATIC", SS_NOPREFIX, 24, 60, 532, 42, 311);
        control(L"STATIC", SS_NOPREFIX, 24, 111, 532, 22, 312);
        control(L"STATIC", SS_NOPREFIX, 24, 138, 532, 22, 313);
        control(L"STATIC", SS_NOPREFIX, 24, 176, 532, 76, 314);
        control(PROGRESS_CLASS, PBS_SMOOTH, 24, 264, 532, 12, 315);
        control(L"STATIC", SS_NOPREFIX, 24, 291, 532, 78, 316);
        control(L"STATIC", SS_NOPREFIX, 24, 377, 532, 32, 317);
        control(L"BUTTON", BS_DEFPUSHBUTTON | WS_TABSTOP, 24, 426, 170, 30, Begin);
        control(L"BUTTON", WS_TABSTOP, 206, 426, 98, 30, Refresh, L"重新检查");
        control(L"BUTTON", WS_TABSTOP, 316, 426, 102, 30, Logs, L"打开日志");
        control(L"BUTTON", WS_TABSTOP, 430, 426, 126, 30, Later);
        return 0;
    }
    if (msg == WM_COMMAND)
    {
        switch (LOWORD(wp))
        {
        case Begin: self->prepare(); return 0;
        case Refresh: self->countdown_ = {}; self->notice_.clear(); self->startProbe(); return 0;
        case Logs:
            if (!self->testing_)
            {
                std::error_code ec;
                std::filesystem::create_directories(self->diagnostics_, ec);
                ShellExecute(hwnd, L"open", self->diagnostics_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        case Later: self->close(); return 0;
        }
    }
    if (msg == WM_DPICHANGED)
    {
        UINT old = static_cast<UINT>(reinterpret_cast<UINT_PTR>(GetProp(hwnd, L"SnipLeaf.Dpi"))), dpi = HIWORD(wp);
        if (!old) old = 96;
        auto nextFont = CreateFont(-MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        auto nextTitle = CreateFont(-MulDiv(21, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        {
            RECT r{};
            GetWindowRect(child, &r);
            MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT *>(&r), 2);
            SetWindowPos(child, nullptr, MulDiv(r.left, dpi, old), MulDiv(r.top, dpi, old),
                MulDiv(r.right - r.left, dpi, old), MulDiv(r.bottom - r.top, dpi, old), SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessage(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetDlgCtrlID(child) == 310 ? nextTitle : nextFont), TRUE);
        }
        DeleteObject(self->font_); DeleteObject(self->titleFont_);
        self->font_ = nextFont; self->titleFont_ = nextTitle;
        SetProp(hwnd, L"SnipLeaf.Dpi", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(dpi)));
        auto r = reinterpret_cast<RECT *>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    if (msg == WM_CLOSE) { self->close(); return 0; }
    if (msg == WM_CTLCOLORSTATIC)
    {
        auto dc = reinterpret_cast<HDC>(wp);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    if (msg == WM_DESTROY)
    {
        DeleteObject(self->font_); DeleteObject(self->titleFont_);
        self->font_ = self->titleFont_ = nullptr;
        RemoveProp(hwnd, L"SnipLeaf.Dpi");
        self->window_ = nullptr;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
} // namespace snap
