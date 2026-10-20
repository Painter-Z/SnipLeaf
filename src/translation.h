#pragma once
#include "document.h"
namespace snap
{
inline constexpr UINT WM_TRANSLATION_READY = WM_APP + 7;
inline constexpr size_t TranslationTextLimit = 12000;
struct TranslationCredentials
{
    std::wstring appId, secret;
    bool enabled{};
    bool valid() const;
    ~TranslationCredentials();
};
class TranslationCredentialStore
{
    std::filesystem::path path_;
  public:
    explicit TranslationCredentialStore(std::filesystem::path path = {});
    TranslationCredentials load() const;
    void save(TranslationCredentials const &) const;
    void clear() const;
};
struct TranslationRow
{
    std::wstring source, translated;
    std::vector<Point> quad;
    bool paragraphBefore{}, complete{};
};
std::vector<TranslationRow> translationRows(OcrSnapshot const &, int first = -1, int last = -1);
std::vector<TranslationRow> translationRows(std::wstring const &);
std::vector<TranslationRow> translationParagraphs(std::vector<TranslationRow> const &);
std::wstring translationText(std::vector<TranslationRow> const &, bool translated = false);
std::wstring translationText(OcrSnapshot const &, int first = -1, int last = -1);
std::vector<std::wstring> translationChunks(std::wstring const &, size_t limit = 900);
std::string translationForm(TranslationCredentials const &, std::wstring const &, std::string const &salt);
struct TranslationResult
{
    std::wstring text, error;
    bool ok() const { return error.empty() && !text.empty(); }
};
TranslationResult parseTranslationResponse(unsigned status, std::string const &body);
std::vector<std::wstring> alignedTranslationResponse(unsigned status, std::string const &body,
    std::vector<std::wstring> const &sources, std::wstring &error);
struct TranslationHttpResponse
{
    unsigned status{};
    std::string body;
};
using TranslationCancel = std::shared_ptr<std::atomic_bool>;
using TranslationTransport = std::function<TranslationHttpResponse(std::string const &, TranslationCancel const &)>;
TranslationHttpResponse translationHttpPost(std::string const &, TranslationCancel const &);
struct TranslationCompletion
{
    uint64_t generation{};
    unsigned completed{}, total{};
    bool finished{};
    TranslationResult result;
    std::vector<TranslationRow> rows;
};
class TranslationService
{
    struct Request
    {
        uint64_t generation{};
        std::vector<TranslationRow> rows;
        TranslationCredentials credentials;
        TranslationCancel cancelled;
    };
    HWND notify_{};
    TranslationTransport transport_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Request> pending_;
    TranslationCancel active_;
    std::vector<TranslationCompletion> completions_;
    bool stopping_{};
    std::thread thread_;
    void run();
    void publish(TranslationCompletion, TranslationCancel const &);
  public:
    explicit TranslationService(HWND notify, TranslationTransport transport = translationHttpPost);
    ~TranslationService();
    void submit(uint64_t generation, std::wstring text, TranslationCredentials credentials);
    void submit(uint64_t generation, std::vector<TranslationRow> rows, TranslationCredentials credentials);
    void cancel();
    std::vector<TranslationCompletion> take();
};
class App;
class TranslationPanel
{
    App &app_;
    TranslationCredentialStore store_;
    TranslationCredentials credentials_;
    std::unique_ptr<TranslationService> service_;
    HWND window_{}, settings_{};
    HFONT font_{}, settingsFont_{};
    uint64_t canvas_{}, revision_{}, generation_{};
    bool waitingOcr_{}, busy_{}, filling_{};
    std::wstring status_, result_, submitted_;
    std::wstring credentialError_;
    std::vector<TranslationRow> rows_;
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK settingsProc(HWND, UINT, WPARAM, LPARAM);
    void create();
    void layout();
    void update();
    void start();
    void cancel(std::wstring const &message = L"已取消。可修改原文后重新翻译。");
    void close();
    void setSource(std::wstring const &);
    void fontChanged(HWND, HFONT &);
  public:
    explicit TranslationPanel(App &);
    ~TranslationPanel();
    void open(uint64_t canvas, uint64_t revision, std::wstring text, bool waitingOcr = false);
    void open(uint64_t canvas, uint64_t revision, std::vector<TranslationRow> rows);
    void ocrReady(uint64_t canvas, uint64_t revision, OcrSnapshot const &);
    void sourceChanged(uint64_t canvas);
    void sourceClosed(uint64_t canvas);
    void showSettings();
    void receive();
    bool preTranslate(MSG const &);
    void pauseForCapture();
    friend class App;
};
} // namespace snap
