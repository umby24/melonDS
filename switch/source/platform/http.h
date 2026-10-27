#pragma once
// Asynchronous HTTP(S) on libcurl (TLS through libnx's ssl service). Requests run one at
// a time, in order, on a worker thread; their callbacks run on that thread too.

#include <functional>
#include <string>

namespace http {

struct Response {
    int status = -1;    // HTTP status, or -1 if the request never completed
    std::string body;
    std::string error;  // transport error text when status is -1
};

bool init(const std::string& user_agent);  // starts the worker; any thread, once
void shutdown();                           // drops queued requests, joins the worker

// POST when `post_data` is non-empty, GET otherwise. `content_type` may be empty.
void request(const std::string& url, const std::string& post_data, const std::string& content_type,
             std::function<void(const Response&)> done);

// Blocking GET on the calling thread (its own connection; doesn't wait for the queue).
// For bulk downloads that shouldn't hold up request().
Response get(const std::string& url);

}  // namespace http
