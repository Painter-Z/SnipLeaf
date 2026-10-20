#pragma once
#include "common.h"
namespace snap
{
inline constexpr UINT WM_OCR_SETUP_READY = WM_APP + 6;
struct OcrReadiness
{
    bool identity{}, chinese{}, english{};
    std::wstring error;
    bool ready() const { return identity && chinese && english && error.empty(); }
};
struct OcrPreparationProgress
{
    bool valid{}, success{}, restartNeeded{};
    std::wstring phase, activeCapability, error;
    std::array<std::wstring, 4> states;
    unsigned installed() const;
};
OcrReadiness inspectOcrReadiness();
OcrPreparationProgress parseOcrPreparation(std::string const &utf8);
bool offerOcrPreparation(OcrReadiness const &readiness, bool alreadyOffered);
std::wstring ocrCapabilityLabel(std::wstring const &name);

class OcrSetup
{
    HINSTANCE instance_{};
    HWND owner_{}, window_{};
    HFONT font_{}, titleFont_{};
    bool testing_{}, checking_{}, automatic_{}, offered_{}, awaitingVerification_{};
    std::thread probe_;
    std::mutex probeMutex_;
    std::optional<OcrReadiness> pending_;
    OcrReadiness readiness_;
    OcrPreparationProgress progress_;
    HANDLE process_{};
    std::filesystem::path report_, diagnostics_;
    std::string lastReport_;
    std::wstring notice_;
    Clock::time_point started_{}, updated_{}, countdown_{};
    std::function<void(HWND)> repair_;
    std::function<void()> refreshed_;
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    void update();
    void prepare();
    void close();
    void persistSession();
    void resumeSession();
    void markOffered();
    void startProbe();

  public:
    OcrSetup(HINSTANCE, HWND, bool testing, std::function<void(HWND)> repair, std::function<void()> refreshed);
    ~OcrSetup();
    void start(bool offerAutomatically = true);
    void show(bool activate = true);
    void receive();
    void tick();
    void pauseForCapture();
    bool preTranslate(MSG const &);
    friend class App;
};
} // namespace snap
