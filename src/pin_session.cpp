#include "pin_session.h"
#include <bit>
#include <wincrypt.h>
namespace snap
{
namespace
{
constexpr uint32_t Magic = 0x31504c53, Schema = 1;
constexpr size_t MaxBytes = 512ull * 1024 * 1024;
constexpr uint64_t MaxPixels = 134217728;
constexpr uint32_t MaxPins = 128, MaxMarks = 10000, MaxText = 2 * 1024 * 1024, MaxPath = 200000;
struct SecretBytes
{
    std::vector<uint8_t> value;
    ~SecretBytes() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
};
struct LocalData
{
    DATA_BLOB blob{};
    ~LocalData()
    {
        if (blob.pbData)
        {
            SecureZeroMemory(blob.pbData, blob.cbData);
            LocalFree(blob.pbData);
        }
    }
};
void valid(bool condition)
{
    if (!condition)
        throw std::runtime_error("Invalid or oversized pin session data.");
}
bool coordinate(float x) { return std::isfinite(x) && std::abs(x) <= 1000000.f; }
bool point(Point p) { return coordinate(p.x) && coordinate(p.y); }
bool annotationKind(Tool tool)
{
    return tool == Tool::Arrow || tool == Tool::Line || tool == Tool::Text || tool == Tool::Mosaic ||
           tool == Tool::Marker || tool == Tool::Rectangle || tool == Tool::Ellipse;
}
void validateMark(Annotation const &mark)
{
    valid(annotationKind(mark.kind) && point(mark.from) && point(mark.to) && mark.rgb <= 0xffffff &&
          std::isfinite(mark.width) && mark.width > 0 && mark.width <= 4096 &&
          std::isfinite(mark.fontPt) && mark.fontPt >= 0 && mark.fontPt <= 4096 &&
          mark.mosaicPixels >= 1 && mark.mosaicPixels <= 1024 && mark.text.size() <= MaxText &&
          mark.path.size() <= MaxPath);
    for (auto p : mark.path) valid(point(p));
}
void validateGeometry(SavedPin const &pin)
{
    auto width = static_cast<int64_t>(pin.window.right) - pin.window.left,
         height = static_cast<int64_t>(pin.window.bottom) - pin.window.top;
    valid(coordinate(static_cast<float>(pin.window.left)) && coordinate(static_cast<float>(pin.window.top)) &&
          width >= 2 && height >= 2 && width <= 262146 && height <= 262146 && point(pin.viewOrigin) &&
          std::isfinite(pin.placement.zoom) && pin.placement.zoom >= .01f && pin.placement.zoom <= 8.f &&
          coordinate(static_cast<float>(pin.placement.imageOrigin.x)) &&
          coordinate(static_cast<float>(pin.placement.imageOrigin.y)));
    valid(std::abs(static_cast<double>(pin.window.left) + pin.viewOrigin.x - pin.placement.imageOrigin.x) <= 1 &&
          std::abs(static_cast<double>(pin.window.top) + pin.viewOrigin.y - pin.placement.imageOrigin.y) <= 1);
}
struct Writer
{
    std::vector<uint8_t> &data;
    void bytes(std::span<const uint8_t> b)
    {
        valid(b.size() <= MaxBytes && data.size() <= MaxBytes - b.size());
        data.insert(data.end(), b.begin(), b.end());
    }
    void u32(uint32_t value) { bytes({reinterpret_cast<uint8_t const *>(&value), sizeof(value)}); }
    void i32(int32_t value) { u32(std::bit_cast<uint32_t>(value)); }
    void real(float value) { u32(std::bit_cast<uint32_t>(value)); }
    void pt(Point p) { real(p.x); real(p.y); }
    void text(std::wstring const &value)
    {
        u32(static_cast<uint32_t>(value.size()));
        bytes({reinterpret_cast<uint8_t const *>(value.data()), value.size() * sizeof(wchar_t)});
    }
};
struct Reader
{
    std::span<const uint8_t> remaining;
    std::span<const uint8_t> bytes(size_t count)
    {
        valid(count <= remaining.size());
        auto result = remaining.first(count);
        remaining = remaining.subspan(count);
        return result;
    }
    uint32_t u32()
    {
        auto b = bytes(4);
        uint32_t value{};
        std::memcpy(&value, b.data(), 4);
        return value;
    }
    int32_t i32() { return std::bit_cast<int32_t>(u32()); }
    float real() { return std::bit_cast<float>(u32()); }
    Point pt() { return {real(), real()}; }
    uint32_t count(uint32_t maximum)
    {
        auto n = u32();
        valid(n <= maximum);
        return n;
    }
    std::wstring text()
    {
        auto n = count(MaxText);
        auto b = bytes(static_cast<size_t>(n) * sizeof(wchar_t));
        std::wstring result(n, L'\0');
        std::memcpy(result.data(), b.data(), b.size());
        return result;
    }
};
void writeAtomic(std::filesystem::path const &file, std::span<const uint8_t> bytes)
{
    std::filesystem::create_directories(file.parent_path());
    GUID id{};
    check(CoCreateGuid(&id));
    wchar_t name[40]{};
    StringFromGUID2(id, name, 40);
    auto temp = file.parent_path() / (std::wstring(L"session-") + name + L".tmp");
    HANDLE handle = CreateFile(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        winrt::throw_last_error();
    try
    {
        DWORD written{};
        if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size())
            throw std::runtime_error("Cannot write the encrypted pin session.");
        if (!FlushFileBuffers(handle)) winrt::throw_last_error();
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        if (!MoveFileEx(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            winrt::throw_last_error();
    }
    catch (...)
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        DeleteFile(temp.c_str());
        throw;
    }
}
} // namespace
PinSnapshot PinSessionStore::load(Graphics &graphics) const
{
    if (!std::filesystem::exists(file())) return {};
    auto size = std::filesystem::file_size(file());
    valid(size > 0 && size <= MaxBytes + 65536);
    std::vector<uint8_t> encrypted(static_cast<size_t>(size));
    std::ifstream input(file(), std::ios::binary);
    valid(input && static_cast<bool>(input.read(reinterpret_cast<char *>(encrypted.data()), encrypted.size())));
    DATA_BLOB source{static_cast<DWORD>(encrypted.size()), encrypted.data()};
    LocalData plain;
    if (!CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain.blob))
        winrt::throw_last_error();
    valid(plain.blob.cbData <= MaxBytes);
    Reader r{{plain.blob.pbData, plain.blob.cbData}};
    valid(r.u32() == Magic && r.u32() == Schema);
    PinSnapshot snapshot;
    auto flags = r.u32();
    valid(flags <= 1);
    snapshot.pinsVisible = flags != 0;
    auto count = r.count(MaxPins);
    uint64_t pixels{};
    uint32_t totalMarks{}, totalPath{}, totalText{};
    for (uint32_t i = 0; i < count; ++i)
    {
        SavedPin pin;
        flags = r.u32();
        valid(flags <= 3);
        pin.visible = (flags & 1) != 0;
        pin.smoothZoom = (flags & 2) != 0;
        pin.placement.imageOrigin = {r.i32(), r.i32()};
        pin.placement.zoom = r.real();
        pin.window = {r.i32(), r.i32(), r.i32(), r.i32()};
        pin.viewOrigin = r.pt();
        validateGeometry(pin);
        auto n = r.count(MaxMarks);
        valid((totalMarks += n) <= MaxMarks);
        for (uint32_t j = 0; j < n; ++j)
        {
            Annotation mark;
            mark.kind = static_cast<Tool>(r.u32());
            mark.from = r.pt(); mark.to = r.pt();
            mark.rgb = r.u32(); mark.width = r.real(); mark.fontPt = r.real();
            mark.mosaicPixels = r.i32();
            mark.text = r.text();
            valid((totalText += static_cast<uint32_t>(mark.text.size())) <= 4 * MaxText);
            auto pathSize = r.count(MaxPath);
            valid((totalPath += pathSize) <= 5 * MaxPath);
            for (uint32_t k = 0; k < pathSize; ++k) mark.path.push_back(r.pt());
            validateMark(mark);
            pin.marks.push_back(std::move(mark));
        }
        auto width = r.count(32768), height = r.count(32768);
        valid(width && height && pixels + static_cast<uint64_t>(width) * height <= MaxPixels);
        auto png = r.bytes(r.count(static_cast<uint32_t>(MaxBytes)));
        auto image = imageFromPng(graphics, png, MaxPixels - pixels);
        valid(image.width == static_cast<int>(width) && image.height == static_cast<int>(height));
        pixels += static_cast<uint64_t>(width) * height;
        pin.base = std::make_shared<Image>(std::move(image));
        snapshot.pins.push_back(std::move(pin));
    }
    valid(r.remaining.empty());
    return snapshot;
}
void PinSessionStore::save(Graphics &graphics, PinSnapshot const &snapshot)
{
    valid(snapshot.pins.size() <= MaxPins);
    SecretBytes plain;
    Writer w{plain.value};
    w.u32(Magic); w.u32(Schema); w.u32(snapshot.pinsVisible ? 1 : 0);
    w.u32(static_cast<uint32_t>(snapshot.pins.size()));
    std::erase_if(cache_, [](auto const &item) { return item.source.expired(); });
    uint64_t pixels{};
    size_t marks{}, text{}, path{};
    for (auto const &pin : snapshot.pins)
    {
        validateGeometry(pin);
        valid(pin.base && !pin.base->empty() && pin.base->width <= 32768 && pin.base->height <= 32768 &&
              pin.base->pixels.size() == static_cast<size_t>(pin.base->width) * pin.base->height * 4);
        pixels += static_cast<uint64_t>(pin.base->width) * pin.base->height;
        valid(pixels <= MaxPixels && (marks += pin.marks.size()) <= MaxMarks);
        w.u32((pin.visible ? 1 : 0) | (pin.smoothZoom ? 2 : 0));
        w.i32(pin.placement.imageOrigin.x); w.i32(pin.placement.imageOrigin.y); w.real(pin.placement.zoom);
        w.i32(pin.window.left); w.i32(pin.window.top); w.i32(pin.window.right); w.i32(pin.window.bottom);
        w.pt(pin.viewOrigin);
        w.u32(static_cast<uint32_t>(pin.marks.size()));
        for (auto const &mark : pin.marks)
        {
            validateMark(mark);
            valid((text += mark.text.size()) <= 4 * MaxText && (path += mark.path.size()) <= 5 * MaxPath);
            w.u32(static_cast<uint32_t>(mark.kind)); w.pt(mark.from); w.pt(mark.to);
            w.u32(mark.rgb); w.real(mark.width); w.real(mark.fontPt); w.i32(mark.mosaicPixels);
            w.text(mark.text); w.u32(static_cast<uint32_t>(mark.path.size()));
            for (auto p : mark.path) w.pt(p);
        }
        auto found = std::find_if(cache_.begin(), cache_.end(), [&](auto const &item)
                                  { return item.source.lock() == pin.base; });
        if (found == cache_.end())
        {
            cache_.push_back({pin.base, pngBytes(graphics, *pin.base)});
            found = std::prev(cache_.end());
        }
        w.u32(pin.base->width); w.u32(pin.base->height);
        w.u32(static_cast<uint32_t>(found->bytes.size())); w.bytes(found->bytes);
    }
    DATA_BLOB source{static_cast<DWORD>(plain.value.size()), plain.value.data()};
    LocalData encrypted;
    if (!CryptProtectData(&source, L"SnipLeaf saved pins", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &encrypted.blob))
        winrt::throw_last_error();
    writeAtomic(file(), {encrypted.blob.pbData, encrypted.blob.cbData});
}
std::vector<RECT> pinWorkAreas()
{
    std::vector<RECT> areas;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM value) -> BOOL
    {
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfo(monitor, &info))
            reinterpret_cast<std::vector<RECT> *>(value)->push_back(info.rcWork);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&areas));
    return areas;
}
SavedPin reachablePin(SavedPin pin, std::span<const RECT> areas)
{
    if (areas.empty()) return pin;
    auto r = pin.window;
    // A 24-pixel grip is enough to drag a partially offscreen pin back.
    for (auto area : areas)
        if (std::min(r.right, area.right) - std::max(r.left, area.left) >= std::min<LONG>(24, r.right-r.left) &&
            std::min(r.bottom, area.bottom) - std::max(r.top, area.top) >= std::min<LONG>(24, r.bottom-r.top))
            return pin;
    auto nearest = *std::min_element(areas.begin(), areas.end(), [&](RECT a, RECT b)
    {
        auto distance = [&](RECT a)
        {
            double dx = std::max({0L, a.left-r.right, r.left-a.right});
            double dy = std::max({0L, a.top-r.bottom, r.top-a.bottom});
            return dx*dx + dy*dy;
        };
        return distance(a) < distance(b);
    });
    LONG width = std::min(r.right-r.left, nearest.right-nearest.left),
         height = std::min(r.bottom-r.top, nearest.bottom-nearest.top);
    LONG x = std::clamp(r.left, nearest.left, nearest.right-width),
         y = std::clamp(r.top, nearest.top, nearest.bottom-height);
    pin.window = {x, y, x+width, y+height};
    // When a panned crop is unreachable, bring its original image back into view.
    pin.viewOrigin = {1.f, 1.f};
    pin.placement.imageOrigin = {x+1, y+1};
    return pin;
}
PinSession::PinSession(std::filesystem::path directory, HWND notify)
    : store_(std::move(directory)), notify_(notify), worker_([this] { work(); }) {}
PinSession::~PinSession()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    worker_.join();
}
uint64_t PinSession::submit(PinSnapshot snapshot)
{
    std::lock_guard lock(mutex_);
    pending_ = std::move(snapshot);
    auto generation = ++submitted_;
    wake_.notify_one();
    return generation;
}
bool PinSession::flush(uint64_t generation, std::wstring &error, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    if (!done_.wait_for(lock, timeout, [&] { return completed_ >= generation; }))
    {
        error = L"贴图仍在保存，请稍后重试退出。";
        return false;
    }
    error = error_;
    return error.empty();
}
std::wstring PinSession::error()
{
    std::lock_guard lock(mutex_);
    return error_;
}
void PinSession::work()
{
    auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_ptr<Graphics> graphics;
    std::wstring initializationError;
    try { check(apartment); graphics = std::make_unique<Graphics>(); }
    catch (...) { initializationError = errorText(); }
    for (;;)
    {
        PinSnapshot snapshot;
        uint64_t generation{};
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
            if (!pending_) break;
            snapshot = std::move(*pending_); pending_.reset(); generation = submitted_;
        }
        std::wstring error = initializationError;
        if (graphics)
        {
            try { store_.save(*graphics, snapshot); }
            catch (...) { error = errorText(); }
        }
        {
            std::lock_guard lock(mutex_);
            completed_ = generation; error_ = std::move(error);
        }
        done_.notify_all();
        if (notify_) PostMessage(notify_, WM_PIN_SESSION_READY, 0, 0);
    }
    graphics.reset();
    if (SUCCEEDED(apartment)) CoUninitialize();
}
} // namespace snap
