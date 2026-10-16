#pragma once
#include <string>

namespace test_http {
bool PostJson(const std::wstring& url, const std::wstring& bearer, const std::string& body,
              std::string& response, unsigned& status, std::wstring& error);
bool JsonString(const std::string& json, const std::string& key, std::string& value);
}
