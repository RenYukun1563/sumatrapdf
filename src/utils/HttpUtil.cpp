/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/ThreadUtil.h"
#include "utils/FileUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"
#include "utils/HttpUtil.h"

#include "utils/Log.h"

// per RFC 1945 10.15 and 3.7, a user agent product token shouldn't contain whitespace
constexpr const WCHAR* kUserAgent = L"SumatraPdfHTTP";

bool IsHttpRspOk(const HttpRsp* rsp) {
    if (rsp->error != ERROR_SUCCESS) {
        logf("HttpRspOk: rsp->error %d, should be %d (ERROR_SUCCESS)\n", (int)rsp->error, (int)ERROR_SUCCESS);
        return false;
    }
    if (rsp->httpStatusCode >= 300) {
        logf("HttpRspOk: rsp->httpStatusCode: %d\n", (int)rsp->httpStatusCode);
        return false;
    }
    return true;
}

// returns false if failed to download or status code is not 200
// for other scenarios, check HttpRsp
bool HttpGet(const char* urlA, HttpRsp* rspOut) {
    logf("HttpGet: url: '%s'\n", urlA);
    HINTERNET hReq = nullptr;
    DWORD infoLevel;
    DWORD headerBuffSize = sizeof(DWORD);
    WCHAR* url = ToWStrTemp(urlA);
    DWORD flags = INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_IGNORE_CERT_CN_INVALID;

    if (str::StartsWithI(urlA, "https")) {
        flags |= INTERNET_FLAG_SECURE;
    }

    rspOut->error = ERROR_SUCCESS;
    HINTERNET hInet = InternetOpenW(kUserAgent, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) {
        logf("HttpGet: InternetOpen failed\n");
        LogLastError();
        goto Error;
    }

    hReq = InternetOpenUrlW(hInet, url, nullptr, 0, flags, 0);
    if (!hReq) {
        logf("HttpGet: InternetOpenUrl failed\n");
        LogLastError();
        goto Error;
    }

    infoLevel = HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER;
    if (!HttpQueryInfoW(hReq, infoLevel, &rspOut->httpStatusCode, &headerBuffSize, nullptr)) {
        logf("HttpGet: HttpQueryInfoW failed\n");
        LogLastError();
        goto Error;
    }

    for (;;) {
        char buf[1024];
        DWORD dwRead = 0;
        if (!InternetReadFile(hReq, buf, sizeof(buf), &dwRead)) {
            logf("HttpGet: InternetReadFile failed\n");
            LogLastError();
            goto Error;
        }
        if (0 == dwRead) {
            break;
        }
        InterlockedIncrement(&gAllowAllocFailure);
        bool ok = rspOut->data.Append(buf, dwRead);
        InterlockedDecrement(&gAllowAllocFailure);
        if (!ok) {
            logf("HttpGet: data.Append failed\n");
            goto Error;
        }
    }

Exit:
    if (hReq) {
        InternetCloseHandle(hReq);
    }
    if (hInet) {
        InternetCloseHandle(hInet);
    }
    return IsHttpRspOk(rspOut);

Error:
    rspOut->error = GetLastError();
    if (0 == rspOut->error) {
        rspOut->error = ERROR_GEN_FAILURE;
    }
    goto Exit;
}

constexpr const int kBufSize = 256 * 1024;

// Download content of a url to a file
bool HttpGetToFile(const char* urlA, const char* destFilePath, const Func1<HttpProgress*>& cbProgress) {
    logf("HttpGetToFile: url: '%s', file: '%s'\n", urlA, destFilePath);
    bool ok = false;
    HINTERNET hReq = nullptr, hInet = nullptr;
    DWORD dwRead = 0;
    DWORD headerBuffSize = sizeof(DWORD);
    DWORD statusCode = 0;
    WCHAR* url = ToWStrTemp(urlA);
    char* buf = nullptr;

    HttpProgress progress{};

    WCHAR* pathW = ToWStrTemp(destFilePath);
    HANDLE hf =
        CreateFileW(pathW, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (INVALID_HANDLE_VALUE == hf) {
        logf("HttpGetToFile: CreateFileW('%s') failed\n", destFilePath);
        LogLastError();
        goto Exit;
    }

    buf = AllocArray<char>(kBufSize);
    if (!buf) {
        goto Exit;
    }

    hInet = InternetOpenW(kUserAgent, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) {
        goto Exit;
    }

    hReq = InternetOpenUrlW(hInet, url, nullptr, 0, 0, 0);
    if (!hReq) {
        goto Exit;
    }

    if (!HttpQueryInfoW(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &headerBuffSize, nullptr)) {
        goto Exit;
    }

    if (statusCode != 200) {
        goto Exit;
    }

    for (;;) {
        if (!InternetReadFile(hReq, buf, kBufSize, &dwRead)) {
            goto Exit;
        }
        if (dwRead == 0) {
            break;
        }
        DWORD size;
        BOOL wroteOk = WriteFile(hf, buf, (DWORD)dwRead, &size, nullptr);
        if (!wroteOk) {
            goto Exit;
        }
        progress.nDownloaded += (i64)dwRead;
        cbProgress.Call(&progress);

        if (size != dwRead) {
            goto Exit;
        }
    }

    ok = true;
Exit:
    CloseHandle(hf);
    if (hReq) {
        InternetCloseHandle(hReq);
    }
    if (hInet) {
        InternetCloseHandle(hInet);
    }
    if (!ok) {
        file::Delete(destFilePath);
    }
    free(buf);
    return ok;
}

static bool HttpPostImpl(const char* serverA, int port, const char* urlA, StrBuilder* headers, StrBuilder* data,
                         HttpRsp* rspOut, bool secure) {
    HttpRsp localRsp;
    if (!rspOut) {
        rspOut = &localRsp;
    }
    rspOut->error = ERROR_SUCCESS;
    rspOut->httpStatusCode = (DWORD)-1;
    rspOut->data.Reset();

    bool ok = false;
    char* hdr = nullptr;
    DWORD hdrLen = 0;
    HINTERNET hConn = nullptr, hReq = nullptr;
    void* d = nullptr;
    DWORD dLen = 0;
    unsigned int timeoutMs = 15 * 1000;
    DWORD respHttpCodeSize = sizeof(rspOut->httpStatusCode);
    DWORD dwRead = 0;
    DWORD flags;
    DWORD dwService;
    WCHAR* server = ToWStrTemp(serverA);
    WCHAR* url = ToWStrTemp(urlA);
    DWORD infoLevel;

    DWORD accessType = INTERNET_OPEN_TYPE_PRECONFIG;
    HINTERNET hInet = InternetOpenW(kUserAgent, accessType, nullptr, nullptr, 0);
    if (!hInet) {
        goto Error;
    }
    dwService = INTERNET_SERVICE_HTTP;
    hConn = InternetConnectW(hInet, server, (INTERNET_PORT)port, nullptr, nullptr, dwService, 0, 1);
    if (!hConn) {
        goto Error;
    }

    flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD |
            INTERNET_FLAG_IGNORE_CERT_CN_INVALID;
    if (secure) {
        flags |= INTERNET_FLAG_SECURE;
    }
    hReq = HttpOpenRequestW(hConn, L"POST", url, nullptr, nullptr, nullptr, flags, 0);
    if (!hReq) {
        goto Error;
    }

    if (headers && headers->size() > 0) {
        hdr = headers->Get();
        hdrLen = (DWORD)headers->size();
    }
    if (data && data->size() > 0) {
        d = data->Get();
        dLen = (DWORD)data->size();
    }

    InternetSetOptionW(hReq, INTERNET_OPTION_SEND_TIMEOUT, &timeoutMs, sizeof(timeoutMs));
    InternetSetOptionW(hReq, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeoutMs, sizeof(timeoutMs));

    if (!HttpSendRequestA(hReq, hdr, hdrLen, d, dLen)) {
        goto Error;
    }

    infoLevel = HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER;
    if (!HttpQueryInfoW(hReq, infoLevel, &rspOut->httpStatusCode, &respHttpCodeSize, nullptr)) {
        goto Error;
    }

    do {
        char buf[1024];
        if (!InternetReadFile(hReq, buf, sizeof(buf), &dwRead)) {
            goto Error;
        }
        ok = rspOut->data.Append(buf, dwRead);
        if (!ok) {
            goto Error;
        }
    } while (dwRead > 0);

#if 0
    // it looks like I should be calling HttpEndRequest(), but it always claims
    // a timeout even though the data has been sent, received and we get HTTP 200
    if (!HttpEndRequest(hReq, nullptr, 0, 0)) {
        LogLastError();
        goto Exit;
    }
#endif
    ok = IsHttpRspOk(rspOut);
Exit:
    if (hReq) {
        InternetCloseHandle(hReq);
    }
    if (hConn) {
        InternetCloseHandle(hConn);
    }
    if (hInet) {
        InternetCloseHandle(hInet);
    }
    return ok;

Error:
    rspOut->error = GetLastError();
    if (0 == rspOut->error) {
        rspOut->error = ERROR_GEN_FAILURE;
    }
    goto Exit;
}

bool HttpPost(const char* serverA, int port, const char* urlA, StrBuilder* headers, StrBuilder* data, HttpRsp* rspOut) {
    bool secure = port == 443;
    return HttpPostImpl(serverA, port, urlA, headers, data, rspOut, secure);
}

bool HttpPost(const char* serverA, int port, const char* urlA, StrBuilder* headers, StrBuilder* data) {
    return HttpPost(serverA, port, urlA, headers, data, nullptr);
}

bool HttpPost(const char* urlA, StrBuilder* headers, StrBuilder* data, HttpRsp* rspOut) {
    if (!urlA || !rspOut) {
        return false;
    }
    rspOut->url.SetCopy(urlA);

    TempWStr url = ToWStrTemp(urlA);
    if (!url) {
        return false;
    }

    WCHAR host[512]{};
    WCHAR path[4096]{};
    WCHAR extra[2048]{};
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = dimof(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = dimof(path);
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = dimof(extra);

    if (!InternetCrackUrlW(url, 0, ICU_ESCAPE, &parts)) {
        rspOut->error = GetLastError();
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) {
        rspOut->error = ERROR_INTERNET_UNRECOGNIZED_SCHEME;
        return false;
    }

    WStrBuilder targetPath;
    if (parts.dwUrlPathLength > 0) {
        targetPath.Append(parts.lpszUrlPath, parts.dwUrlPathLength);
    } else {
        targetPath.Append(L"/");
    }
    if (parts.dwExtraInfoLength > 0) {
        targetPath.Append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    TempStr hostA = ToUtf8Temp(parts.lpszHostName, parts.dwHostNameLength);
    TempStr targetPathA = ToUtf8Temp(targetPath.Get(), targetPath.size());
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return HttpPostImpl(hostA, parts.nPort, targetPathA, headers, data, rspOut, secure);
}
