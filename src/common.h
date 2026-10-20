#pragma once
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <appmodel.h>
#include <winrt/base.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace snap
{
using Clock = std::chrono::steady_clock;
inline constexpr wchar_t Version[] = L"0.1.11";
inline constexpr UINT WM_OCR_READY = WM_APP + 1, WM_TRAY = WM_APP + 2, WM_CANVAS_CLOSED = WM_APP + 3;
inline constexpr UINT WM_FINISH_TEXT = WM_APP + 4, WM_OVERLAY_DONE = WM_APP + 5;
inline constexpr UINT WM_PREVIEW_READY = WM_APP + 11;
struct Point
{
    float x{}, y{};
};
struct Box
{
    float x{}, y{}, w{}, h{};
    bool contains(Point p) const
    {
        return p.x >= x && p.y >= y && p.x <= x + w && p.y <= y + h;
    }
    bool intersects(Box b) const
    {
        return x < b.x + b.w && x + w > b.x && y < b.y + b.h && y + h > b.y;
    }
};
inline Box between(Point a, Point b)
{
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
}
inline D2D1_POINT_2F dp(Point p)
{
    return {p.x, p.y};
}
inline D2D1_RECT_F dr(Box b)
{
    return {b.x, b.y, b.x + b.w, b.y + b.h};
}
inline D2D1_COLOR_F color(unsigned rgb, float a = 1.f)
{
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
inline Point rotate(Point p, Point center, float degrees)
{
    float r = degrees * 3.14159265358979323846f / 180.f, c = std::cos(r), s = std::sin(r);
    return {center.x + (p.x - center.x) * c - (p.y - center.y) * s,
            center.y + (p.x - center.x) * s + (p.y - center.y) * c};
}
struct View
{
    float zoom{1.f};
    Point origin{};
    Point toImage(Point p) const
    {
        return {(p.x - origin.x) / zoom, (p.y - origin.y) / zoom};
    }
    Point toView(Point p) const
    {
        return {p.x * zoom + origin.x, p.y * zoom + origin.y};
    }
    Box toView(Box b) const
    {
        auto p = toView(Point{b.x, b.y});
        return {p.x, p.y, b.w * zoom, b.h * zoom};
    }
};
inline void check(HRESULT hr)
{
    winrt::check_hresult(hr);
}
inline std::wstring errorText()
{
    try
    {
        throw;
    }
    catch (winrt::hresult_error const &e)
    {
        return std::wstring(e.message());
    }
    catch (std::exception const &e)
    {
        std::string s = e.what();
        return std::wstring(s.begin(), s.end());
    }
    catch (...)
    {
        return L"未知错误";
    }
}
inline std::wstring localDirectory()
{
    PWSTR raw{};
    check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw));
    std::filesystem::path p(raw);
    CoTaskMemFree(raw);
    p /= L"SnapLite";
    std::filesystem::create_directories(p);
    return p.wstring();
}
inline std::wstring packageIdentity()
{
    UINT32 length = 0;
    if (GetCurrentPackageFullName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER)
        return {};
    std::wstring s(length, L'\0');
    if (GetCurrentPackageFullName(&length, s.data()) != ERROR_SUCCESS)
        return {};
    s.resize(length - 1);
    return s;
}
struct Graphics
{
    winrt::com_ptr<ID2D1Factory> d2d;
    winrt::com_ptr<IDWriteFactory> text;
    winrt::com_ptr<IWICImagingFactory> wic;
    Graphics()
    {
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.put()));
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown **>(text.put())));
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                               wic.put_void()));
    }
    winrt::com_ptr<IDWriteTextFormat> font(float size, bool bold = false)
    {
        winrt::com_ptr<IDWriteTextFormat> f;
        check(text->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                     bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", f.put()));
        return f;
    }
};
} // namespace snap
