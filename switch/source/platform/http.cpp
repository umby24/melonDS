#include "platform/http.h"

#include <curl/curl.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace http {

namespace {

struct Request {
    std::string url, post_data, content_type;
    std::function<void(const Response&)> done;
};

std::string g_user_agent = "melonds-next";
std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Request> g_queue;
std::thread g_thread;
bool g_quit = false;
bool g_running = false;

size_t write_body(char* data, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

Response perform(CURL* curl, const Request& rq) {
    Response rs;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, rq.url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, g_user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rs.body);
    curl_slist* headers = nullptr;
    if (!rq.post_data.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, rq.post_data.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(rq.post_data.size()));
    }
    if (!rq.content_type.empty()) {
        headers = curl_slist_append(headers, ("Content-Type: " + rq.content_type).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        rs.status = int(code);
    } else {
        rs.error = curl_easy_strerror(res);
        rs.body.clear();
        fprintf(stderr, "[http] %s: %s\n", rq.url.c_str(), rs.error.c_str());
    }
    curl_slist_free_all(headers);
    return rs;
}

void worker() {
    CURL* curl = curl_easy_init();
    for (;;) {
        Request rq;
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_cv.wait(lk, [] { return g_quit || !g_queue.empty(); });
            if (g_quit)
                break;
            rq = std::move(g_queue.front());
            g_queue.pop_front();
        }
        Response rs;
        if (curl)
            rs = perform(curl, rq);
        else
            rs.error = "curl unavailable";
        if (rq.done)
            rq.done(rs);
    }
    if (curl)
        curl_easy_cleanup(curl);
}

}  // namespace

bool init(const std::string& user_agent) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_running)
        return true;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return false;
    g_user_agent = user_agent;
    g_quit = false;
    g_thread = std::thread(worker);
    g_running = true;
    return true;
}

void shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_running)
            return;
        g_quit = true;
        g_queue.clear();
    }
    g_cv.notify_all();
    g_thread.join();
    curl_global_cleanup();
    g_running = false;
}

Response get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        Response rs;
        rs.error = "curl unavailable";
        return rs;
    }
    Response rs = perform(curl, Request{url, "", "", nullptr});
    curl_easy_cleanup(curl);
    return rs;
}

void request(const std::string& url, const std::string& post_data, const std::string& content_type,
             std::function<void(const Response&)> done) {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_running) {
            Response rs;
            rs.error = "network not started";
            if (done)
                done(rs);
            return;
        }
        g_queue.push_back({url, post_data, content_type, std::move(done)});
    }
    g_cv.notify_one();
}

}  // namespace http
