#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <winrt/Windows.Media.Ocr.h>
#include <iostream>
#include <type_traits>

static_assert(__cplusplus >= 202002L, "C++20 mode is required.");
static_assert(std::is_class_v<winrt::Windows::Media::Ocr::OcrEngine>);

int main()
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        winrt::com_ptr<ID2D1Factory> drawing;
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, drawing.put()));

        winrt::com_ptr<IUnknown> text;
        winrt::check_hresult(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), text.put()));

        winrt::com_ptr<IWICImagingFactory> imaging;
        winrt::check_hresult(CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory), imaging.put_void()));

        std::cout << "PASS: C++20, Win32, C++/WinRT OCR headers, Direct2D, DirectWrite, WIC.\n";
        std::cout << "MSVC version: " << _MSC_VER << "; C++ mode: " << __cplusplus << '\n';
        std::cout << "Toolchain validation only; application OCR deployment remains unverified.\n";
        return 0;
    }
    catch (winrt::hresult_error const& error)
    {
        std::cerr << "Native interface initialization failed, HRESULT="
                  << std::hex << static_cast<unsigned int>(error.code().value) << '\n';
        return 1;
    }
}
