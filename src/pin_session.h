#pragma once
#include "document.h"
namespace snap
{
struct PinPlacement
{
    POINT imageOrigin{};
    float zoom{1.f};
};
struct SavedPin
{
    std::shared_ptr<const Image> base;
    std::vector<Annotation> marks;
    PinPlacement placement;
    // Actual viewport, including panning. An open toolbar is excluded on save.
    RECT window{};
    Point viewOrigin{1.f, 1.f};
    bool smoothZoom{true}, visible{true};
};
struct PinSnapshot
{
    std::vector<SavedPin> pins;
    bool pinsVisible{true};
};
// Leaves every position unchanged while any useful part is on a connected screen.
// Only an unreachable viewport is moved onto the nearest remaining work area.
SavedPin reachablePin(SavedPin pin, std::span<const RECT> workAreas);
std::vector<RECT> pinWorkAreas();

class PinSessionStore
{
    struct CachedPng
    {
        std::weak_ptr<const Image> source;
        std::vector<uint8_t> bytes;
    };
    std::filesystem::path directory_;
    std::vector<CachedPng> cache_;

  public:
    explicit PinSessionStore(std::filesystem::path directory) : directory_(std::move(directory)) {}
    std::filesystem::path file() const { return directory_ / L"session.dat"; }
    PinSnapshot load(Graphics &graphics) const;
    void save(Graphics &graphics, PinSnapshot const &snapshot);
};

// PNG compression, encryption and disk writes run on one worker. A newer pending
// snapshot replaces older pending work; flush waits for its own generation.
class PinSession
{
    PinSessionStore store_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::optional<PinSnapshot> pending_;
    uint64_t submitted_{}, completed_{};
    bool stopping_{};
    std::wstring error_;
    HWND notify_{};
    std::thread worker_;
    void work();

  public:
    explicit PinSession(std::filesystem::path directory, HWND notify = nullptr);
    ~PinSession();
    uint64_t submit(PinSnapshot snapshot);
    bool flush(uint64_t generation, std::wstring &error,
               std::chrono::milliseconds timeout = std::chrono::seconds(15));
    std::wstring error();
};
} // namespace snap
