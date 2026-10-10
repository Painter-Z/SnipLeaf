#include "app.h"
#include <cwchar>
#include <cwctype>
namespace snap
{
namespace
{
constexpr UINT ApplySize = WM_APP + 10;
constexpr int SizeCombo = 201, ApplyButton = 202, Hint = 203;
std::wstring pointLabel(float value)
{
    wchar_t text[32]{};
    swprintf_s(text, L"%g", static_cast<double>(value));
    return text;
}
} // namespace
bool Canvas::textSizeContext() const
{
    if (!annotationsEditable())
        return false;
    return (edit_ && stroke_ && stroke_->kind == Tool::Text) ||
           (doc_ && selected_ >= 0 && selected_ < static_cast<int>(doc_->marks.size()) &&
            doc_->marks[selected_].kind == Tool::Text) ||
           tool_ == Tool::Text;
}
float Canvas::currentFontPoints() const
{
    if (edit_ && stroke_ && stroke_->kind == Tool::Text)
        return textFontPoints(*stroke_);
    if (doc_ && selected_ >= 0 && selected_ < static_cast<int>(doc_->marks.size()) &&
        doc_->marks[selected_].kind == Tool::Text)
        return textFontPoints(doc_->marks[selected_]);
    return fontPt_;
}
void Canvas::updateEditorFont()
{
    if (!edit_ || !stroke_)
        return;
    float size = textFontPixels(*stroke_) * view_.zoom;
    auto font = CreateFont(-std::max(1, static_cast<int>(std::round(size))), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                           L"Microsoft YaHei UI");
    if (!font)
        winrt::throw_last_error();
    auto previous = editFont_;
    editFont_ = font;
    SendMessage(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (previous)
        DeleteObject(previous);
    sizeTextEditor();
}
bool Canvas::setFontPoints(float points)
{
    if (!annotationsEditable() || !std::isfinite(points) || points < 6 || points > 288 || !doc_)
        return false;
    points = std::round(points * 10.f) / 10.f;
    fontPt_ = points;
    app_.config.textSizeTenths = static_cast<UINT>(std::round(points * 10));
    if (!app_.testing_)
        app_.config.save();
    if (edit_ && stroke_)
    {
        stroke_->fontPt = points;
        updateEditorFont();
    }
    else if (selected_ >= 0 && selected_ < static_cast<int>(doc_->marks.size()) &&
             doc_->marks[selected_].kind == Tool::Text &&
             std::abs(textFontPoints(doc_->marks[selected_]) - points) > .001f)
    {
        auto mark = doc_->marks[selected_];
        mark.fontPt = points;
        measureText(app_.graphics, mark, std::max(16.f, doc_->base->width - mark.from.x));
        doc_->replace(selected_, std::move(mark));
        changed();
    }
    status_ = L"字号 " + pointLabel(points) + L" pt";
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}
void Canvas::showFontPicker()
{
    if (!annotationsEditable())
        return;
    if (fontPopup_)
    {
        SetFocus(fontCombo_);
        return;
    }
    WNDCLASSEX wc{sizeof(wc)};
    wc.lpfnWndProc = fontPopupProc;
    wc.hInstance = app_.instance();
    wc.lpszClassName = L"SnapLite.FontSize";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        winrt::throw_last_error();
    Box anchor = toolbar_;
    for (auto const &button : buttons_)
        if (button.command == 13)
            anchor = button.box;
    POINT position{static_cast<LONG>(anchor.x), static_cast<LONG>(anchor.y)};
    ClientToScreen(hwnd_, &position);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfo(MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST), &monitor);
    int width = static_cast<int>(244 * ui_), height = static_cast<int>(110 * ui_);
    int x = std::clamp(position.x, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
    int y = position.y - height - static_cast<int>(6 * ui_);
    if (y < monitor.rcWork.top)
        y = position.y + static_cast<int>(anchor.h + 6 * ui_);
    y = std::clamp(y, static_cast<int>(monitor.rcWork.top),
                   std::max(static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height));
    fontPopup_ = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"文字字号", WS_POPUP | WS_BORDER,
                                x, y, width, height, hwnd_, nullptr, app_.instance(), this);
    if (!fontPopup_)
        winrt::throw_last_error();
    ShowWindow(fontPopup_, SW_SHOW);
    SetFocus(fontCombo_);
    SendMessage(fontCombo_, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
}
void Canvas::closeFontPicker(bool restoreFocus)
{
    if (!fontPopup_)
        return;
    DestroyWindow(fontPopup_);
    if (restoreFocus && !closing_ && IsWindow(hwnd_))
        SetFocus(edit_ ? edit_ : hwnd_);
}
void Canvas::applyFontPicker()
{
    wchar_t buffer[80]{};
    GetWindowText(fontCombo_, buffer, static_cast<int>(std::size(buffer)));
    wchar_t *end{};
    double points = std::wcstod(buffer, &end);
    while (iswspace(*end))
        ++end;
    if ((end[0] == L'p' || end[0] == L'P') && (end[1] == L't' || end[1] == L'T'))
        end += 2;
    while (iswspace(*end))
        ++end;
    if (end == buffer || *end || !setFontPoints(static_cast<float>(points)))
    {
        SetWindowText(GetDlgItem(fontPopup_, Hint), L"请输入 6–288 pt 的数字");
        SetFocus(fontCombo_);
        SendMessage(fontCombo_, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
        return;
    }
    closeFontPicker();
}
LRESULT CALLBACK Canvas::fontPopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto self = reinterpret_cast<Canvas *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        self = static_cast<Canvas *>(reinterpret_cast<CREATESTRUCT *>(lp)->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->fontPopup_ = hwnd;
    }
    if (!self)
        return DefWindowProc(hwnd, msg, wp, lp);
    try
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            float scale = self->ui_;
            self->pickerFont_ = CreateFont(-static_cast<int>(14 * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
            auto control =
                [&](wchar_t const *type, wchar_t const *text, DWORD style, int x, int y, int w, int h, int id)
            {
                HWND child = CreateWindowEx(
                    0, type, text, WS_CHILD | WS_VISIBLE | style, static_cast<int>(x * scale),
                    static_cast<int>(y * scale), static_cast<int>(w * scale), static_cast<int>(h * scale), hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), self->app_.instance(), nullptr);
                if (!child)
                    winrt::throw_last_error();
                SendMessage(child, WM_SETFONT, reinterpret_cast<WPARAM>(self->pickerFont_), TRUE);
                return child;
            };
            control(L"STATIC", L"文字大小", 0, 12, 10, 200, 22, 0);
            self->fontCombo_ = control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, 12,
                                       36, 110, 230, SizeCombo);
            SendMessage(self->fontCombo_, CB_LIMITTEXT, 32, 0);
            for (auto size : {6, 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 60, 72, 96, 144, 288})
            {
                auto text = std::to_wstring(size);
                SendMessage(self->fontCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            }
            SetWindowText(self->fontCombo_, pointLabel(self->currentFontPoints()).c_str());
            control(L"STATIC", L"pt", 0, 130, 40, 24, 24, 0);
            control(L"BUTTON", L"应用", WS_TABSTOP | BS_DEFPUSHBUTTON, 162, 35, 68, 28, ApplyButton);
            control(L"STATIC", L"6–288 pt · 可输入小数", 0, 12, 77, 222, 22, Hint);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == ApplyButton || (LOWORD(wp) == SizeCombo && HIWORD(wp) == CBN_SELENDOK))
                PostMessage(hwnd, ApplySize, 0, 0);
            return 0;
        case ApplySize:
            self->applyFontPicker();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE)
                self->closeFontPicker(false);
            return 0;
        case WM_CLOSE:
            self->closeFontPicker();
            return 0;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wp), RGB(71, 84, 103));
            SetBkColor(reinterpret_cast<HDC>(wp), RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        case WM_NCDESTROY:
            self->fontPopup_ = self->fontCombo_ = nullptr;
            if (self->pickerFont_)
                DeleteObject(self->pickerFont_);
            self->pickerFont_ = nullptr;
            break;
        }
    }
    catch (...)
    {
        self->status_ = L"字号调整失败：" + errorText();
        if (msg == WM_CREATE)
            return -1;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
} // namespace snap
