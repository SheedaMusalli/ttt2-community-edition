#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include "http_client.h"
#include <vector>

namespace test_http {
namespace {
std::wstring Utf8(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);
    if(!n) return {};
    std::wstring w((size_t)n,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),w.data(),n); return w;
}
}
bool PostJson(const std::wstring& url,const std::wstring& bearer,const std::string& body,
              std::string& response,unsigned& status,std::wstring& error) {
    response.clear(); error.clear(); status=0;
    URL_COMPONENTS u{}; u.dwStructSize=sizeof(u); u.dwSchemeLength=u.dwHostNameLength=u.dwUrlPathLength=u.dwExtraInfoLength=(DWORD)-1;
    if(!WinHttpCrackUrl(url.c_str(),0,0,&u) || u.nScheme!=INTERNET_SCHEME_HTTP) { error=L"only valid HTTP URLs are supported by this local harness"; return false; }
    std::wstring host(u.lpszHostName,u.dwHostNameLength), path(u.lpszUrlPath,u.dwUrlPathLength);
    path.append(u.lpszExtraInfo,u.dwExtraInfoLength);
    HINTERNET session=WinHttpOpen(L"TTT2 integration harness/1",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session) { error=L"WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(session,5000,5000,5000,5000);
    HINTERNET connection=WinHttpConnect(session,host.c_str(),u.nPort,0);
    HINTERNET request=connection?WinHttpOpenRequest(connection,L"POST",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0):nullptr;
    std::wstring headers=L"Content-Type: application/json\r\nAuthorization: Bearer "+bearer+L"\r\n";
    BOOL ok=request && WinHttpSendRequest(request,headers.c_str(),(DWORD)headers.size(),(LPVOID)body.data(),(DWORD)body.size(),(DWORD)body.size(),0) && WinHttpReceiveResponse(request,nullptr);
    if(!ok) error=L"WinHTTP request failed";
    if(ok) {
        DWORD size=sizeof(status);
        ok=WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)!=FALSE;
        if(!ok) error=L"could not read HTTP status";
        while(ok) { DWORD available=0; if(!WinHttpQueryDataAvailable(request,&available)) {ok=FALSE;break;} if(!available) break;
            std::vector<char> b(available); DWORD read=0; if(!WinHttpReadData(request,b.data(),available,&read)){ok=FALSE;break;} response.append(b.data(),read); }
        if(!ok && error.empty()) error=L"could not read HTTP response";
    }
    if(request) WinHttpCloseHandle(request); if(connection) WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
    return ok && status>=200 && status<300;
}
bool JsonString(const std::string& json,const std::string& key,std::string& value) {
    const std::string needle="\""+key+"\""; size_t p=json.find(needle); if(p==std::string::npos)return false;
    p=json.find(':',p+needle.size()); if(p==std::string::npos)return false; ++p; while(p<json.size()&&(json[p]==' '||json[p]=='\t'))++p;
    if(p>=json.size()||json[p++]!='\"')return false; value.clear();
    while(p<json.size()) { char c=json[p++]; if(c=='\"')return true; if(c=='\\') { if(p>=json.size())return false; c=json[p++]; if(c!='\\'&&c!='\"'&&c!='/')return false; } value.push_back(c); }
    return false;
}
}
