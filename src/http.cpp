#include "teez/worker/http.hpp"

#include <coditary/net/http.hpp>

namespace teez::worker {

HttpResponse http_get(const std::string& url) {
    const auto response = coditary::net::http_get(url);
    return {response.status, response.body};
}

HttpResponse http_post(const std::string& url, const std::string& body) {
    coditary::net::HttpRequestOptions options;
    options.body = body;
    const auto response = coditary::net::http_post(url, options);
    return {response.status, response.body};
}

} // namespace teez::worker
