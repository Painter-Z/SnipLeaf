#include "translation.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
namespace snap
{
namespace
{
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
struct Wipe
{
    std::string &text;
    ~Wipe() { if (!text.empty()) SecureZeroMemory(text.data(), text.size()); }
};
std::wstring trim(std::wstring text)
{
    auto start = text.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return {};
    return text.substr(start, text.find_last_not_of(L" \t\r\n") - start + 1);
}
std::string formEncode(std::string const &text)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    return out;
}
std::string md5(std::string const &text)
{
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Translation signing is unavailable.");
    std::array<unsigned char, 16> digest{};
    auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(text.data())),
                             static_cast<ULONG>(text.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Translation signing failed.");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
}
std::string salt()
{
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Cannot generate translation request ID.");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (auto byte : bytes) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
}
struct LocalBlob
{
    DATA_BLOB data{};
    ~LocalBlob()
    {
        if (data.pbData) { SecureZeroMemory(data.pbData, data.cbData); LocalFree(data.pbData); }
    }
};
struct HttpHandle
{
    HINTERNET value{};
    explicit HttpHandle(HINTERNET handle) : value(handle)
    { if (!value) throw std::runtime_error("HTTP initialization failed."); }
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
};
struct HttpState
{
    HANDLE event{CreateEvent(nullptr, FALSE, FALSE, nullptr)};
    std::mutex mutex;
    DWORD status{}, error{}, count{};
    std::array<char, 8192> buffer{};
    std::string body;
    ~HttpState() { if (event) CloseHandle(event); if (!body.empty()) SecureZeroMemory(body.data(), body.size()); }
};
void CALLBACK httpCallback(HINTERNET, DWORD_PTR context, DWORD status, void *info, DWORD length)
{
    if (!context) return;
    auto owner = reinterpret_cast<std::shared_ptr<HttpState> *>(context);
    auto state = *owner;
    // This is the final callback. Pending reads and their buffers remain alive
    // through this separately owned context after cancellation returns to UI.
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { delete owner; return; }
    if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE && status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE &&
        status != WINHTTP_CALLBACK_STATUS_READ_COMPLETE && status != WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) return;
    {
        std::lock_guard lock(state->mutex);
        state->status = status;
        state->count = length;
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && info && length >= sizeof(WINHTTP_ASYNC_RESULT))
            state->error = static_cast<WINHTTP_ASYNC_RESULT *>(info)->dwError;
    }
    SetEvent(state->event);
}
void beginHttp(std::shared_ptr<HttpState> const &state)
{
    ResetEvent(state->event);
    std::lock_guard lock(state->mutex);
    state->status = state->error = state->count = 0;
}
DWORD awaitHttp(std::shared_ptr<HttpState> const &state, DWORD expected, TranslationCancel const &cancelled,
                Clock::time_point deadline)
{
    for (;;)
    {
        if (cancelled && cancelled->load()) throw std::runtime_error("Translation cancelled.");
        if (Clock::now() >= deadline) throw std::runtime_error("Translation network timeout.");
        auto wait = WaitForSingleObject(state->event, 50);
        if (wait == WAIT_TIMEOUT) continue;
        if (wait != WAIT_OBJECT_0) throw std::runtime_error("Translation network wait failed.");
        std::lock_guard lock(state->mutex);
        if (state->error == ERROR_WINHTTP_TIMEOUT) throw std::runtime_error("Translation network timeout.");
        if (state->error) throw std::runtime_error("Translation network request failed.");
        if (state->status == expected) return state->count;
    }
}
void initiated(BOOL result)
{
    if (!result && GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Translation network request failed.");
}
} // namespace
TranslationCredentials::~TranslationCredentials()
{
    if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
}
bool TranslationCredentials::valid() const
{
    return appId.size() >= 6 && appId.size() <= 32 &&
        std::all_of(appId.begin(), appId.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }) &&
        secret.size() >= 8 && secret.size() <= 256 &&
        std::all_of(secret.begin(), secret.end(), [](wchar_t c) { return c > L' ' && c < 127; });
}
TranslationCredentialStore::TranslationCredentialStore(std::filesystem::path path)
    : path_(path.empty() ? std::filesystem::path(localDirectory()) / L"translation.dat" : std::move(path)) {}
TranslationCredentials TranslationCredentialStore::load() const
{
    if (!std::filesystem::exists(path_)) return {};
    if (std::filesystem::file_size(path_) > 16384) throw std::runtime_error("Invalid encrypted translation settings.");
    std::ifstream in(path_, std::ios::binary);
    std::string encrypted(std::istreambuf_iterator<char>(in), {});
    DATA_BLOB source{static_cast<DWORD>(encrypted.size()), reinterpret_cast<BYTE *>(encrypted.data())};
    LocalBlob clear;
    if (!CryptUnprotectData(&source, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &clear.data))
        throw std::runtime_error("Cannot decrypt translation settings for this Windows user.");
    std::string raw(reinterpret_cast<char *>(clear.data.pbData), clear.data.cbData);
    Wipe wipe{raw};
    auto object = JsonObject::Parse(winrt::to_hstring(raw));
    if (object.GetNamedNumber(L"schema", 0) != 1) throw std::runtime_error("Unknown translation settings format.");
    TranslationCredentials result;
    result.appId = object.GetNamedString(L"appId");
    result.secret = object.GetNamedString(L"secret");
    result.enabled = object.GetNamedBoolean(L"enabled", false);
    if (!result.valid()) throw std::runtime_error("Invalid translation credentials.");
    return result;
}
void TranslationCredentialStore::save(TranslationCredentials const &credentials) const
{
    if (!credentials.valid()) throw std::runtime_error("Invalid translation credentials.");
    JsonObject object;
    object.Insert(L"schema", JsonValue::CreateNumberValue(1));
    object.Insert(L"appId", JsonValue::CreateStringValue(credentials.appId));
    object.Insert(L"secret", JsonValue::CreateStringValue(credentials.secret));
    object.Insert(L"enabled", JsonValue::CreateBooleanValue(credentials.enabled));
    std::string raw = winrt::to_string(object.Stringify());
    Wipe wipe{raw};
    DATA_BLOB source{static_cast<DWORD>(raw.size()), reinterpret_cast<BYTE *>(raw.data())};
    LocalBlob protectedData;
    if (!CryptProtectData(&source, L"SnipLeaf Translation", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                           &protectedData.data)) throw std::runtime_error("Cannot encrypt translation settings.");
    std::filesystem::create_directories(path_.parent_path());
    auto temporary = path_;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<char *>(protectedData.data.pbData), protectedData.data.cbData);
        if (!out) throw std::runtime_error("Cannot save encrypted translation settings.");
    }
    if (!MoveFileEx(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFile(temporary.c_str());
        throw std::runtime_error("Cannot replace encrypted translation settings.");
    }
}
void TranslationCredentialStore::clear() const
{
    if (std::filesystem::exists(path_) && !DeleteFile(path_.c_str())) throw std::runtime_error("Cannot remove translation settings.");
}
std::wstring translationText(OcrSnapshot const &snapshot, int first, int last)
{
    return translationText(translationRows(snapshot, first, last));
}
std::vector<std::wstring> translationChunks(std::wstring const &text, size_t limit)
{
    if (text.size() > TranslationTextLimit) throw std::runtime_error("Translation text exceeds 12000 characters.");
    if (limit < 4 || limit > 2000) throw std::runtime_error("Invalid translation chunk limit.");
    std::vector<std::wstring> chunks;
    for (size_t at = 0; at < text.size();)
    {
        size_t end = std::min(at + limit, text.size());
        if (end < text.size())
        {
            size_t split = text.find_last_of(L"\n.!?; ", end - 1);
            if (split != std::wstring::npos && split >= at + limit / 2) end = split + 1;
            if (end > at && text[end - 1] >= 0xd800 && text[end - 1] <= 0xdbff) --end;
            if (end > at && text[end - 1] == L'\r' && text[end] == L'\n') --end;
        }
        auto chunk = trim(text.substr(at, end - at));
        if (!chunk.empty()) chunks.push_back(std::move(chunk));
        at = end;
    }
    return chunks;
}
std::string translationForm(TranslationCredentials const &credentials, std::wstring const &text, std::string const &nonce)
{
    if (!credentials.valid() || text.empty() || text.size() > 2000) throw std::runtime_error("Invalid translation request.");
    auto query = winrt::to_string(text), id = winrt::to_string(credentials.appId), secret = winrt::to_string(credentials.secret);
    Wipe wipeSecret{secret};
    std::string signedText = id + query + nonce + secret;
    Wipe wipeSigned{signedText};
    auto signature = md5(signedText);
    return "q=" + formEncode(query) + "&from=en&to=zh&appid=" + id + "&salt=" + formEncode(nonce) + "&sign=" + signature;
}
TranslationResult parseTranslationResponse(unsigned status, std::string const &body)
{
    if (status == 401 || status == 403) return {{}, L"翻译服务拒绝访问，请检查 APPID、密钥和服务开通状态。"};
    if (status == 429) return {{}, L"请求过于频繁，请稍后重试。"};
    if (status != 200) return {{}, L"翻译服务暂时不可用（HTTP " + std::to_wstring(status) + L"），请稍后重试。"};
    if (body.empty() || body.size() > 524288) return {{}, L"翻译服务返回了空内容或过大的响应。"};
    try
    {
        auto object = JsonObject::Parse(winrt::to_hstring(body));
        if (object.HasKey(L"error_code"))
        {
            auto value = object.GetNamedValue(L"error_code");
            std::wstring code;
            if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String)
            {
                code = value.GetString();
                if (code.empty() || code.size() > 12 || !std::all_of(code.begin(), code.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                    return {{}, L"翻译服务响应格式异常，请稍后重试。"};
            }
            else
            {
                double number = value.GetNumber();
                if (!std::isfinite(number) || number < 0 || number > 999999 || std::floor(number) != number)
                    return {{}, L"翻译服务响应格式异常，请稍后重试。"};
                code = std::to_wstring(static_cast<unsigned>(number));
            }
            if (code != L"52000" && code != L"0")
            {
                if (code == L"54001") return {{}, L"APPID 或密钥不匹配（54001）。请填写“开发者信息”中的密钥，不能使用 API Key。"};
                if (code == L"52003" || code == L"58002" || code == L"90107") return {{}, L"百度通用文本翻译尚未开通，或开发者认证未生效。请在百度控制台检查。"};
                if (code == L"54003" || code == L"54005" || code == L"59004") return {{}, L"百度限制了请求频率，请稍后重试。"};
                if (code == L"54004") return {{}, L"百度翻译账户余额或可用额度不足，请查看百度控制台。"};
                if (code == L"52001") return {{}, L"百度翻译处理超时，请重试或减少所选文字。"};
                if (code == L"58000" || code == L"58003") return {{}, L"百度翻译拒绝了当前网络地址，请检查账户的 IP 限制。"};
                // Do not echo an untrusted provider message, request text or credentials.
                return {{}, L"百度翻译未能完成请求，请检查服务状态和输入长度。"};
            }
        }
        if (object.GetNamedString(L"to", L"") != L"zh") return {{}, L"翻译服务返回的目标语言不正确。"};
        auto results = object.GetNamedArray(L"trans_result");
        if (!results.Size() || results.Size() > 2000) return {{}, L"翻译服务没有返回有效译文。"};
        std::wstring translated;
        for (auto const &value : results)
        {
            auto text = trim(std::wstring(value.GetObject().GetNamedString(L"dst")));
            if (text.empty()) return {{}, L"翻译服务返回了不完整的译文，请重试。"};
            if (!translated.empty()) translated += L"\r\n";
            translated += text;
        }
        return {std::move(translated), {}};
    }
    catch (...) { return {{}, L"翻译服务响应格式异常，请稍后重试。"}; }
}
TranslationHttpResponse translationHttpPost(std::string const &body, TranslationCancel const &cancelled)
{
    if (cancelled && cancelled->load()) throw std::runtime_error("Translation cancelled.");
    auto state = std::make_shared<HttpState>();
    if (!state->event) throw std::runtime_error("HTTP event initialization failed.");
    state->body = body;
    HttpHandle session(WinHttpOpen(L"SnipLeaf/0.1.10", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 15000)) throw std::runtime_error("HTTP timeout setup failed.");
    HttpHandle connection(WinHttpConnect(session.value, L"fanyi-api.baidu.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    HttpHandle request(WinHttpOpenRequest(connection.value, L"POST", L"/api/trans/vip/translate", nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon, sizeof(autologon)))
        throw std::runtime_error("HTTP privacy setup failed.");
    auto callback = WinHttpSetStatusCallback(request.value, httpCallback,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0);
    if (callback == WINHTTP_INVALID_STATUS_CALLBACK) throw std::runtime_error("HTTP callback setup failed.");
    auto owner = std::make_unique<std::shared_ptr<HttpState>>(state);
    auto context = reinterpret_cast<DWORD_PTR>(owner.get());
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
        throw std::runtime_error("HTTP context setup failed.");
    owner.release();
    auto deadline = Clock::now() + std::chrono::seconds(25);
    constexpr wchar_t headers[] = L"Content-Type: application/x-www-form-urlencoded; charset=utf-8\r\nAccept: application/json\r\n";
    beginHttp(state);
    initiated(WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1), state->body.data(),
                                  static_cast<DWORD>(state->body.size()), static_cast<DWORD>(state->body.size()), context));
    awaitHttp(state, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, cancelled, deadline);
    beginHttp(state);
    initiated(WinHttpReceiveResponse(request.value, nullptr));
    awaitHttp(state, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, cancelled, deadline);
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("HTTP response status unavailable.");
    TranslationHttpResponse result{status, {}};
    for (;;)
    {
        beginHttp(state);
        initiated(WinHttpReadData(request.value, state->buffer.data(), static_cast<DWORD>(state->buffer.size()), nullptr));
        DWORD count = awaitHttp(state, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, cancelled, deadline);
        if (!count) break;
        if (count > state->buffer.size() || result.body.size() + count > 524288)
            throw std::runtime_error("Translation response too large.");
        result.body.append(state->buffer.data(), count);
    }
    return result;
}
TranslationService::TranslationService(HWND notify, TranslationTransport transport)
    : notify_(notify), transport_(std::move(transport)), thread_([this] { run(); }) {}
TranslationService::~TranslationService()
{
    { std::lock_guard lock(mutex_); stopping_ = true; if (active_) active_->store(true); pending_.reset(); }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}
void TranslationService::submit(uint64_t generation, std::wstring text, TranslationCredentials credentials)
{
    submit(generation, translationRows(text), std::move(credentials));
}
void TranslationService::submit(uint64_t generation, std::vector<TranslationRow> rows, TranslationCredentials credentials)
{
    { std::lock_guard lock(mutex_); if (active_) active_->store(true);
      pending_ = Request{generation, std::move(rows), std::move(credentials), std::make_shared<std::atomic_bool>(false)};
      completions_.clear(); }
    cv_.notify_all();
}
void TranslationService::cancel()
{
    { std::lock_guard lock(mutex_); if (active_) active_->store(true); pending_.reset(); completions_.clear(); }
    cv_.notify_all();
}
std::vector<TranslationCompletion> TranslationService::take()
{
    std::lock_guard lock(mutex_);
    auto results = std::move(completions_);
    completions_.clear();
    return results;
}
void TranslationService::publish(TranslationCompletion completion, TranslationCancel const &cancelled)
{
    { std::lock_guard lock(mutex_); if (stopping_ || cancelled->load()) return; completions_.push_back(std::move(completion)); }
    if (notify_) PostMessage(notify_, WM_TRANSLATION_READY, 0, 0);
}
void TranslationService::run()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    Clock::time_point nextAllowed{};
    for (;;)
    {
        Request request;
        { std::unique_lock lock(mutex_); cv_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
          if (stopping_) break; request = std::move(*pending_); pending_.reset(); active_ = request.cancelled; }
        TranslationCompletion completion{request.generation};
        try
        {
            if (!request.credentials.enabled || !request.credentials.valid())
                throw std::runtime_error("Translation credentials missing or disabled.");
            if (translationText(request.rows).size() > TranslationTextLimit) throw std::runtime_error("Translation text too long.");
            struct Unit { size_t row{}; std::wstring text; };
            struct Batch { std::wstring text; std::vector<Unit> units; };
            std::vector<Batch> batches;
            std::vector<unsigned> remaining(request.rows.size());
            for (size_t row = 0; row < request.rows.size(); ++row)
                for (auto &chunk : translationChunks(request.rows[row].source))
                {
                    if (batches.empty() || batches.back().text.size() + chunk.size() + 1 > 900) batches.emplace_back();
                    auto &batch = batches.back();
                    if (!batch.text.empty()) batch.text += L'\n';
                    batch.text += chunk;
                    batch.units.push_back({row, std::move(chunk)});
                    ++remaining[row];
                }
            if (batches.empty()) throw std::runtime_error("No text to translate.");
            completion.rows = request.rows;
            for (auto &row : completion.rows) { row.translated.clear(); row.complete = false; }
            completion.total = static_cast<unsigned>(batches.size());
            auto overallDeadline = Clock::now() + std::chrono::seconds(180);
            for (size_t i = 0; i < batches.size(); ++i)
            {
                { std::unique_lock lock(mutex_); cv_.wait_until(lock, nextAllowed, [&] { return stopping_ || request.cancelled->load(); });
                  if (stopping_ || request.cancelled->load()) break; }
                if (Clock::now() > overallDeadline) throw std::runtime_error("Translation network timeout.");
                auto body = translationForm(request.credentials, batches[i].text, salt());
                Wipe wipe{body};
                nextAllowed = Clock::now() + std::chrono::milliseconds(1100);
                auto response = transport_(body, request.cancelled);
                std::vector<std::wstring> sources;
                for (auto const &unit : batches[i].units) sources.push_back(unit.text);
                auto translated = alignedTranslationResponse(response.status, response.body, sources, completion.result.error);
                if (!completion.result.error.empty()) break;
                for (size_t part = 0; part < translated.size(); ++part)
                {
                    auto rowIndex = batches[i].units[part].row;
                    auto &row = completion.rows[rowIndex];
                    if (!row.translated.empty() && !translated[part].empty() &&
                        row.translated.back() < 128 && iswalnum(row.translated.back()) &&
                        translated[part].front() < 128 && iswalnum(translated[part].front())) row.translated += L' ';
                    row.translated += translated[part];
                    row.complete = --remaining[rowIndex] == 0;
                }
                completion.result.text = translationText(completion.rows, true);
                completion.completed = static_cast<unsigned>(i + 1);
                publish({request.generation, completion.completed, completion.total, false, {}, completion.rows}, request.cancelled);
            }
        }
        catch (std::exception const &e)
        {
            std::string message = e.what();
            completion.result.error = message.find("timeout") != std::string::npos
                ? L"联网翻译超时，请检查网络或减少所选文字后重试。"
                : L"联网翻译失败，请检查网络、代理设置和翻译账户后重试。";
        }
        catch (...) { completion.result.error = L"联网翻译暂时失败，请稍后重试。"; }
        completion.finished = true;
        publish(std::move(completion), request.cancelled);
    }
    winrt::uninit_apartment();
}
} // namespace snap
