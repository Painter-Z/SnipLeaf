#pragma once
#include "document.h"
namespace snap
{
struct OcrRequest
{
    uint64_t window{}, revision{};
    std::shared_ptr<const Image> image;
    std::vector<Box> redactions;
    std::wstring language;
    uint64_t epoch{};
    std::shared_ptr<std::atomic_bool> cancelled;
    bool forceEnhance{};
};
struct OcrCompletion
{
    uint64_t window{};
    std::shared_ptr<OcrSnapshot> result;
};
std::shared_ptr<OcrSnapshot> recognize(OcrRequest const &request, bool requireIdentity = true);
std::vector<std::wstring> ocrLanguages();
int ocrQualityTest(Graphics &graphics, std::wstring const &report);
class OcrService
{
    HWND notify_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<OcrRequest> pending_;
    std::deque<OcrCompletion> completed_;
    bool stopping_{};
    uint64_t activeWindow_{};
    std::shared_ptr<std::atomic_bool> activeCancel_;
    std::thread thread_;
    void run();

  public:
    explicit OcrService(HWND notify);
    ~OcrService();
    void submit(OcrRequest request);
    void cancel(uint64_t window);
    std::vector<OcrCompletion> take();
};
} // namespace snap
