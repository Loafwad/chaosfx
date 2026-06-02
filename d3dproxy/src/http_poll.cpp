#include "http_poll.h"
#include "effects.h"
#include "effect_types.h"
#include "log.h"
#include <windows.h>
#include <winhttp.h>
#include <cstring>
#include <cstdlib>

#pragma comment(lib, "winhttp.lib")

// Extracts the string value of a JSON key from a flat (non-nested) JSON object.
// Returns false if the key is absent or the value is null.
static bool JsonGetString(const char* json, const char* key, char* outBuf, int outLen)
{
    char pattern[128];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    int i = 0;
    while (i < outLen - 1) {
        char c = p[i];
        if (c == '\0' || c == '"') break;
        outBuf[i] = c;
        i++;
    }
    outBuf[i] = '\0';
    return i > 0;
}

static int JsonGetInt(const char* json, const char* key, int defaultVal)
{
    char pattern[128];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return defaultVal;
    p += strlen(pattern);
    if (*p == 'n') return defaultVal; // null
    return atoi(p);
}

static chaosfx::EffectType RewardKeyToEffect(const char* key)
{
    if (strcmp(key, "pink_mode")       == 0) return chaosfx::EffectType::PinkMode;
    if (strcmp(key, "kaleidoscope")    == 0) return chaosfx::EffectType::Kaleidoscope;
    if (strcmp(key, "mirrored_screen") == 0) return chaosfx::EffectType::Mirror;
    if (strcmp(key, "flipped_screen")  == 0) return chaosfx::EffectType::FlippedScreen;
    return chaosfx::EffectType::None;
}

static DWORD WINAPI PollThread(LPVOID)
{
    CFXLOG("PollThread: starting");

    HINTERNET hSession = WinHttpOpen(
        L"ChaosFX/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) {
        CFXLOG("PollThread: WinHttpOpen failed %lu", GetLastError());
        return 1;
    }

    // Set a short connect/send/receive timeout so we don't hang if bridge is down
    DWORD timeout = 2000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT,    &timeout, sizeof(timeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT,       &timeout, sizeof(timeout));
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT,    &timeout, sizeof(timeout));

    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", 18244, 0);
    if (!hConnect) {
        CFXLOG("PollThread: WinHttpConnect failed %lu", GetLastError());
        WinHttpCloseHandle(hSession);
        return 1;
    }

    CFXLOG("PollThread: connected, entering poll loop");

    while (true) {
        Sleep(1000);

        HINTERNET hRequest = WinHttpOpenRequest(
            hConnect, L"GET", L"/rewards/next",
            nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!hRequest) continue;

        BOOL ok = WinHttpSendRequest(hRequest,
            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);

        if (ok) {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

            if (statusCode == 200) {
                char buf[512] = {};
                DWORD totalRead = 0;
                DWORD bytesRead = 0;
                // WinHTTP may need multiple reads to deliver the full body
                while (totalRead < sizeof(buf) - 1) {
                    bytesRead = 0;
                    if (!WinHttpReadData(hRequest, buf + totalRead,
                                        sizeof(buf) - 1 - totalRead, &bytesRead) || bytesRead == 0)
                        break;
                    totalRead += bytesRead;
                }
                buf[totalRead] = '\0';

                CFXLOG("PollThread: got reward JSON: %s", buf);

                char rewardKey[64] = {};
                if (JsonGetString(buf, "rewardKey", rewardKey, sizeof(rewardKey))) {
                    chaosfx::EffectType effect = RewardKeyToEffect(rewardKey);
                    if (effect != chaosfx::EffectType::None) {
                        int durationMs = JsonGetInt(buf, "durationMs", 15000);
                        if (durationMs < 1000)   durationMs = 1000;
                        if (durationMs > 300000) durationMs = 300000;

                        CFXLOG("PollThread: activating effect %d for %dms", (int)effect, durationMs);
                        chaosfx::effects::SetEffect(effect, 1.0f);
                        WinHttpCloseHandle(hRequest);

                        Sleep(durationMs);
                        chaosfx::effects::ClearEffect();
                        CFXLOG("PollThread: effect cleared");
                        continue; // skip the close below, already done
                    }
                }
            }
            // 204 = empty queue — nothing to do, continue polling
        }

        WinHttpCloseHandle(hRequest);
    }

    // Unreachable, but tidy
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return 0;
}

void Proxy_StartPolling()
{
    HANDLE hThread = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
        CFXLOG("Proxy_StartPolling: polling thread started");
    } else {
        CFXLOG("Proxy_StartPolling: CreateThread failed %lu", GetLastError());
    }
}
