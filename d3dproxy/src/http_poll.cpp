#include "http_poll.h"
#include "effects.h"
#include "effect_types.h"
#include "proxy_drawcall.h"
#include "log.h"
#include <windows.h>
#include <winhttp.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>

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

        // ── Sync skipDrawN from bridge ──────────────────────────────────────
        {
            HINTERNET hDbgReq = WinHttpOpenRequest(
                hConnect, L"GET", L"/debug/state",
                nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (hDbgReq) {
                BOOL dbgOk = WinHttpSendRequest(hDbgReq,
                    WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
                if (dbgOk) dbgOk = WinHttpReceiveResponse(hDbgReq, nullptr);
                if (dbgOk) {
                    char dbgBuf[4096] = {};
                    DWORD dbgRead = 0, dbgBytes = 0;
                    while (dbgRead < sizeof(dbgBuf) - 1) {
                        dbgBytes = 0;
                        if (!WinHttpReadData(hDbgReq, dbgBuf + dbgRead,
                                            sizeof(dbgBuf) - 1 - dbgRead, &dbgBytes) || dbgBytes == 0)
                            break;
                        dbgRead += dbgBytes;
                    }
                    dbgBuf[dbgRead] = '\0';
                    g_SkipDrawN.store(JsonGetInt(dbgBuf, "skipDrawN", -1), std::memory_order_relaxed);
                    // Parse skipPairs: "0xVSHASH1,0xPSHASH1;0xVSHASH2,0xPSHASH2;..."
                    char pairsStr[2048] = {};
                    uint64_t vsArr[32], psArr[32];
                    int pairCount = 0;
                    if (JsonGetString(dbgBuf, "skipPairs", pairsStr, sizeof(pairsStr))) {
                        char* tok = pairsStr;
                        while (*tok && pairCount < 32) {
                            char* semi = strchr(tok, ';');
                            if (semi) *semi = '\0';
                            char* comma = strchr(tok, ',');
                            if (comma) {
                                *comma = '\0';
                                vsArr[pairCount] = (uint64_t)strtoull(tok, nullptr, 16);
                                psArr[pairCount] = (uint64_t)strtoull(comma + 1, nullptr, 16);
                                pairCount++;
                            }
                            if (!semi) break;
                            tok = semi + 1;
                        }
                    }
                    Proxy_SetSkipPairs(vsArr, psArr, pairCount);
                    // Parse skipIndexCounts: "3840,1200,6000"
                    {
                        char idxStr[512] = {};
                        unsigned int idxArr[32];
                        int idxCount = 0;
                        if (JsonGetString(dbgBuf, "skipIndexCounts", idxStr, sizeof(idxStr))) {
                            char* tok = idxStr;
                            while (*tok && idxCount < 32) {
                                char* comma = strchr(tok, ',');
                                if (comma) *comma = '\0';
                                idxArr[idxCount] = (unsigned int)atoi(tok);
                                idxCount++;
                                if (!comma) break;
                                tok = comma + 1;
                            }
                        }
                        Proxy_SetSkipIndexCounts(idxArr, idxCount);
                    }
                }
                WinHttpCloseHandle(hDbgReq);
            }
        }

        // ── POST any pending skip info back to bridge ───────────────────────
        {
            DrawSkipInfo info {};
            if (Proxy_TakeSkipInfo(&info)) {
                char body[256];
                int bodyLen = _snprintf_s(body, sizeof(body),
                    "{\"indexCount\":%u,\"startIndex\":%u,\"vs\":\"0x%llx\",\"ps\":\"0x%llx\"}",
                    info.indexCount, info.startIndex,
                    (unsigned long long)info.vsPtr,
                    (unsigned long long)info.psPtr);
                if (bodyLen > 0) {
                    HINTERNET hPostReq = WinHttpOpenRequest(
                        hConnect, L"POST", L"/debug/last-skip",
                        nullptr, WINHTTP_NO_REFERER,
                        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                    if (hPostReq) {
                        static const wchar_t* kCtJson =
                            L"Content-Type: application/json\r\n";
                        WinHttpSendRequest(hPostReq,
                            kCtJson, (DWORD)-1L,
                            body, (DWORD)bodyLen, (DWORD)bodyLen, 0);
                        WinHttpReceiveResponse(hPostReq, nullptr);
                        WinHttpCloseHandle(hPostReq);
                        CFXLOG("PollThread: posted last-skip %s", body);
                    }
                }
            }
        }

        // ── POST shader histogram ──────────────────────────────────────────────────────
        {
            const int kMax = 256;
            ShaderPairEntry hist[kMax];
            int count = Proxy_TakeShaderHist(hist, kMax);
            if (count > 0) {
                int bufSize = 16 + count * 80;
                char* body = (char*)malloc(bufSize);
                if (body) {
                    int pos = 0;
                    pos += _snprintf_s(body + pos, bufSize - pos, _TRUNCATE, "{\"pairs\":[");
                    for (int i = 0; i < count; ++i)
                        pos += _snprintf_s(body + pos, bufSize - pos, _TRUNCATE,
                            "%s{\"vs\":\"0x%llx\",\"ps\":\"0x%llx\",\"n\":%d}",
                            i ? "," : "",
                            (unsigned long long)hist[i].vsHash,
                            (unsigned long long)hist[i].psHash,
                            hist[i].count);
                    pos += _snprintf_s(body + pos, bufSize - pos, _TRUNCATE, "]}");
                    HINTERNET hReq = WinHttpOpenRequest(
                        hConnect, L"POST", L"/debug/shader-hist",
                        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                    if (hReq) {
                        WinHttpSendRequest(hReq, L"Content-Type: application/json\r\n", (DWORD)-1L,
                            body, (DWORD)pos, (DWORD)pos, 0);
                        WinHttpReceiveResponse(hReq, nullptr);
                        WinHttpCloseHandle(hReq);
                    }
                    free(body);
                }
            }
        }

        // ── POST index-count histogram ──────────────────────────────────────
        {
            const int kMaxIdx = 256;
            IndexCountEntry hist[kMaxIdx];
            int count = Proxy_TakeIndexCountHist(hist, kMaxIdx);
            CFXLOG("PollThread: index-count hist count=%d", count);
            if (count > 0) {
                int bufSize = 16 + count * 48;
                char* body = (char*)malloc(bufSize);
                if (body) {
                    int pos = 0;
                    pos += _snprintf_s(body + pos, bufSize - pos, _TRUNCATE, "{\"pairs\":[");
                    for (int i = 0; i < count; ++i)
                        pos += _snprintf_s(body + pos, bufSize - pos, _TRUNCATE,
                            "%s{\"c\":%u,\"n\":%d}",
                            i ? "," : "",
                            hist[i].indexCount,
                            hist[i].drawCount);
                    pos += _snprintf_s(body + pos, bufSize - pos, _TRUNCATE, "]}");
                    HINTERNET hReq = WinHttpOpenRequest(
                        hConnect, L"POST", L"/debug/index-count-hist",
                        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                    if (hReq) {
                        WinHttpSendRequest(hReq, L"Content-Type: application/json\r\n", (DWORD)-1L,
                            body, (DWORD)pos, (DWORD)pos, 0);
                        WinHttpReceiveResponse(hReq, nullptr);
                        WinHttpCloseHandle(hReq);
                    }
                    free(body);
                }
            }
        }

        // ── POST frame stats (always) ────────────────────────────────────────
        {
            char fsBody[128];
            int fsLen = _snprintf_s(fsBody, sizeof(fsBody),
                "{\"drawsPerFrame\":%d,\"activeSkipN\":%d,\"hooksInstalled\":%s,\"presents\":%d}",
                g_DrawsLastFrame.load(std::memory_order_relaxed),
                g_SkipDrawN.load(std::memory_order_relaxed),
                g_HooksInstalled.load(std::memory_order_relaxed) ? "true" : "false",
                g_PresentCount.load(std::memory_order_relaxed));
            if (fsLen > 0) {
                HINTERNET hFsReq = WinHttpOpenRequest(
                    hConnect, L"POST", L"/debug/frame-stats",
                    nullptr, WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
                if (hFsReq) {
                    static const wchar_t* kCtJson =
                        L"Content-Type: application/json\r\n";
                    WinHttpSendRequest(hFsReq,
                        kCtJson, (DWORD)-1L,
                        fsBody, (DWORD)fsLen, (DWORD)fsLen, 0);
                    WinHttpReceiveResponse(hFsReq, nullptr);
                    WinHttpCloseHandle(hFsReq);
                }
            }
        }
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
