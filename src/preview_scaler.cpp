#include "preview_scaler.h"
namespace snap
{
bool validPreviewSize(Image const &source, int width, int height)
{
    return !source.empty() && width > 0 && height > 0 && width <= source.width && height <= source.height &&
           (width < source.width || height < source.height) &&
           static_cast<uint64_t>(source.width) * source.height <= 33554432 &&
           static_cast<uint64_t>(width) * height <= 2097152;
}
Image downsamplePreview(IWICImagingFactory *wic, Image const &source, int width, int height)
{
    if (!validPreviewSize(source, width, height))
        throw std::invalid_argument("Preview dimensions exceed the display cache budget.");
    winrt::com_ptr<IWICBitmap> bitmap;
    check(wic->CreateBitmapFromMemory(source.width, source.height, GUID_WICPixelFormat32bppBGRA,
                                      static_cast<UINT>(source.stride()), static_cast<UINT>(source.pixels.size()),
                                      const_cast<BYTE *>(source.pixels.data()), bitmap.put()));
    winrt::com_ptr<IWICBitmapScaler> scaler;
    check(wic->CreateBitmapScaler(scaler.put()));
    check(scaler->Initialize(bitmap.get(), width, height, WICBitmapInterpolationModeFant));
    Image result(width, height);
    check(scaler->CopyPixels(nullptr, static_cast<UINT>(result.stride()), static_cast<UINT>(result.pixels.size()),
                             result.pixels.data()));
    return result;
}
PreviewScaler::PreviewScaler(HWND notify) : notify_(notify), thread_([this] { run(); }) {}
PreviewScaler::~PreviewScaler()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.clear();
        if (activeCancel_)
            activeCancel_->store(true);
    }
    cv_.notify_one();
    thread_.join();
}
void PreviewScaler::submit(PreviewRequest request)
{
    request.cancelled = std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard lock(mutex_);
        if (activeWindow_ == request.window && activeCancel_)
            activeCancel_->store(true);
        std::erase_if(pending_, [&](auto const &p) { return p.window == request.window; });
        std::erase_if(completed_, [&](auto const &p) { return p.request.window == request.window; });
        pending_.push_back(std::move(request));
    }
    cv_.notify_one();
}
void PreviewScaler::cancel(uint64_t window)
{
    std::lock_guard lock(mutex_);
    if (activeWindow_ == window && activeCancel_)
        activeCancel_->store(true);
    std::erase_if(pending_, [&](auto const &p) { return p.window == window; });
    std::erase_if(completed_, [&](auto const &p) { return p.request.window == window; });
}
std::vector<PreviewResult> PreviewScaler::take()
{
    std::lock_guard lock(mutex_);
    std::vector<PreviewResult> results;
    while (!completed_.empty())
    {
        results.push_back(std::move(completed_.front()));
        completed_.pop_front();
    }
    return results;
}
void PreviewScaler::run()
{
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    winrt::com_ptr<IWICImagingFactory> wic;
    if (SUCCEEDED(apartment))
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                         wic.put_void());
    for (;;)
    {
        PreviewResult result;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
            if (stopping_)
                break;
            result.request = std::move(pending_.front());
            pending_.pop_front();
            activeWindow_ = result.request.window;
            activeCancel_ = result.request.cancelled;
        }
        try
        {
            if (wic && !result.request.cancelled->load())
                result.image = std::make_shared<Image>(
                    downsamplePreview(wic.get(), *result.request.source, result.request.width, result.request.height));
        }
        catch (...)
        {
            // Keep the existing realtime renderer when the optional cache cannot be built.
        }
        {
            std::lock_guard lock(mutex_);
            activeWindow_ = 0;
            activeCancel_.reset();
            if (stopping_)
                break;
            if (result.request.cancelled->load())
                continue;
            completed_.push_back(std::move(result));
        }
        PostMessage(notify_, WM_PREVIEW_READY, 0, 0);
    }
    wic = nullptr;
    if (SUCCEEDED(apartment))
        CoUninitialize();
}
} // namespace snap
