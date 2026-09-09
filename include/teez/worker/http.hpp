#pragma once

#include <string>

namespace teez::worker {

struct HttpResponse {
    long status = 0;
    std::string body;
};

HttpResponse http_get(const std::string& url);
HttpResponse http_post(const std::string& url, const std::string& body);

} // namespace teez::worker
