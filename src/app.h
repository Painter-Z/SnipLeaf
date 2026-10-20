#pragma once
#include "canvas.h"
#include "ocr_setup.h"
#include "translation.h"
#include "preview_scaler.h"
namespace snap
{
struct Settings
{
    UINT captureKey{VK_F1}, captureMods{}, pasteKey{VK_F3}, pasteMods{};
    bool autoOcr{true};
    UINT textSizeTenths{180};
    std::wstring language;
    void load();
    void save() const;
};
class App
{
    HINSTANCE instance_{};
    HWND root_{}, settingsWindow_{};
    HICON icon_{};
    NOTIFYICONDATA tray_{};
    UINT taskbarCreated_{};
    std::unique_ptr<OcrService> ocr_;
    std::unique_ptr<PreviewScaler> previewScaler_;
    std::unique_ptr<OcrSetup> ocrSetup_;
    std::unique_ptr<TranslationPanel> translation_;
    // Keep the OS clipboard boundary replaceable in integration tests so they
    // can exercise Ctrl+C without replacing the user's clipboard contents.
    std::function<bool(HWND, std::wstring const &)> copyText_{snap::copyText};
    std::function<bool(HWND, Graphics &, Image const &)> copyImage_{snap::copyImage};
    std::function<std::wstring(HWND, Graphics &, Image const &)> copyImageFile_{snap::copyImageFile};
    std::function<Image(HWND, Graphics &)> pasteImage_{snap::pasteImage};
    std::function<DWORD()> clipboardSequence_{GetClipboardSequenceNumber};
    struct CopiedImagePlacement
    {
        PinPlacement placement;
        int width{}, height{};
        DWORD sequence{};
    };
    std::optional<CopiedImagePlacement> copiedImage_;
    std::vector<std::unique_ptr<Canvas>> windows_;
    uint64_t nextId_{1};
    bool pinsVisible_{true}, captureRegistered_{}, pasteRegistered_{}, hideRegistered_{}, quitting_{}, testing_{};
    std::wstring hotkeyStatus_;
    static LRESULT CALLBACK rootProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK settingsProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT, WPARAM, LPARAM);
    void trayMenu();
    void trayIcon();
    void settings();
    void help();
    void retryHotkeys(bool notify = true);
    void quit();

  public:
    Graphics graphics;
    Settings config;
    App(HINSTANCE instance);
    ~App();
    bool create(bool automated = false);
    int run();
    HINSTANCE instance() const
    {
        return instance_;
    }
    HWND root() const
    {
        return root_;
    }
    OcrService &ocr()
    {
        return *ocr_;
    }
    void capture();
    void paste();
    void pin(Document const &document, std::shared_ptr<OcrSnapshot> snapshot = {},
             std::optional<PinPlacement> placement = {});
    void demo();
    void open(std::wstring const &path);
    void togglePins();
    void balloon(std::wstring const &title, std::wstring const &text, DWORD flags = NIIF_INFO);
    Canvas *find(uint64_t id);
    bool preTranslate(MSG const &message);
    int integrationTests(std::wstring const &report);
    int regressionTests(std::wstring const &report);
    int translationTests(std::wstring const &report);
    int translationLiveTest(std::wstring const &report);
    friend class Canvas;
    friend class TranslationPanel;
};
} // namespace snap
