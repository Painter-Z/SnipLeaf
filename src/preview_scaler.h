#pragma once
#include "image.h"
namespace snap
{
struct PreviewRequest
{
    uint64_t window{}, epoch{}, revision{};
    std::shared_ptr<const Image> source;
    int width{}, height{};
    std::shared_ptr<std::atomic_bool> cancelled;
};
struct PreviewResult
{
    PreviewRequest request;
    std::shared_ptr<const Image> image;
};
// One worker for all pins. Jobs for the same pin coalesce; only UI code creates D2D resources.
class PreviewScaler
{
    HWND notify_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<PreviewRequest> pending_;
    std::deque<PreviewResult> completed_;
    bool stopping_{};
    uint64_t activeWindow_{};
    std::shared_ptr<std::atomic_bool> activeCancel_;
    std::thread thread_;
    void run();

  public:
    explicit PreviewScaler(HWND notify);
    ~PreviewScaler();
    void submit(PreviewRequest request);
    void cancel(uint64_t window);
    std::vector<PreviewResult> take();
};
bool validPreviewSize(Image const &source, int width, int height);
Image downsamplePreview(IWICImagingFactory *wic, Image const &source, int width, int height);
} // namespace snap
