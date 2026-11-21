#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "corvus/http_transport.h"
#include "corvus/mock_http_transport.h"

using namespace corvus;

namespace {
HttpRequest req(std::string url = "https://api.example.com/v1/x", std::string body = "{}") {
    HttpRequest r;
    r.url = std::move(url);
    r.body = std::move(body);
    r.headers = {{"x-api-key", "k"}, {"content-type", "application/json"}};
    return r;
}
}  // namespace

TEST_CASE("mock transport returns scripted responses in FIFO order and records requests") {
    MockHttpTransport mock;
    HttpResponse a;
    a.status = 200;
    a.body = "first";
    HttpResponse b;
    b.status = 429;
    b.body = "second";
    b.headers = {{"retry-after", "7"}};
    mock.enqueue(a);
    mock.enqueue(b);

    const auto r1 = mock.post(req("https://api.example.com/one", "{\"n\":1}"), nullptr, {});
    const auto r2 = mock.post(req("https://api.example.com/two", "{\"n\":2}"), nullptr, {});

    CHECK(r1.status == 200);
    CHECK(r1.body == "first");
    CHECK(r2.status == 429);
    CHECK(r2.body == "second");
    REQUIRE(r2.headers.size() == 1);
    CHECK(r2.headers[0].first == "retry-after");

    REQUIRE(mock.requests().size() == 2);
    CHECK(mock.requests()[0].url == "https://api.example.com/one");
    CHECK(mock.requests()[0].body == "{\"n\":1}");
    CHECK(mock.requests()[1].url == "https://api.example.com/two");
    REQUIRE(mock.requests()[0].headers.size() == 2);
    CHECK(mock.requests()[0].headers[0].second == "k");
}

TEST_CASE("mock transport with an empty queue reports a transport failure, not a throw") {
    MockHttpTransport mock;
    const auto r = mock.post(req(), nullptr, {});
    CHECK(r.status == 0);
    CHECK(r.error == "MockHttpTransport: no scripted response");
}

TEST_CASE("streamed script delivers chunks in order and leaves body empty") {
    MockHttpTransport mock;
    mock.enqueueStream(200, {"data: a\n\n", "data: b\n\n", "data: [DONE]\n\n"});

    std::vector<std::string> seen;
    const auto r = mock.post(
        req(),
        [&](const char* d, std::size_t n) {
            seen.emplace_back(d, n);
            return true;
        },
        {});

    CHECK(r.status == 200);
    CHECK(r.body.empty());
    CHECK(r.error.empty());
    REQUIRE(seen.size() == 3);
    CHECK(seen[0] == "data: a\n\n");
    CHECK(seen[2] == "data: [DONE]\n\n");
}

TEST_CASE("streamed script without an onChunk collapses chunks into the body") {
    MockHttpTransport mock;
    mock.enqueueStream(200, {"ab", "cd"});
    const auto r = mock.post(req(), nullptr, {});
    CHECK(r.status == 200);
    CHECK(r.body == "abcd");
}

TEST_CASE("streamed non-2xx keeps the error payload in body and never calls onChunk") {
    MockHttpTransport mock;
    mock.enqueueStream(429, {"{\"error\":", "\"slow down\"}"});

    bool onChunkCalled = false;
    const auto r = mock.post(
        req(),
        [&](const char*, std::size_t) {
            onChunkCalled = true;
            return true;
        },
        {});

    CHECK(r.status == 429);
    CHECK_FALSE(onChunkCalled);  // matches the real transport: errors go to body
    CHECK(r.body == "{\"error\":\"slow down\"}");
}

TEST_CASE("enqueue()'d 2xx body with onChunk streams through onChunk, leaving body empty") {
    MockHttpTransport mock;
    HttpResponse resp;
    resp.status = 200;
    resp.body = "streamed-payload";
    mock.enqueue(resp);

    std::string got;
    const auto r = mock.post(
        req(),
        [&](const char* d, std::size_t n) {
            got.append(d, n);
            return true;
        },
        {});

    CHECK(r.status == 200);
    CHECK(r.body.empty());     // real transport routes 2xx bytes to onChunk
    CHECK(got == "streamed-payload");
}

TEST_CASE("mock honors a pre-fired cancel like the real transport's pre-flight check") {
    MockHttpTransport mock;
    HttpResponse resp;
    resp.status = 200;
    resp.body = "ignored";
    mock.enqueue(resp);

    CancelToken token;
    token.cancel();
    const auto r = mock.post(req(), nullptr, token);
    CHECK(r.status == 0);
    CHECK(r.error == "cancelled");
    // The scripted response is consumed only on a live call, so it is still queued.
    CHECK(mock.requests().size() == 1);
}

TEST_CASE("receiver returning false aborts the stream") {
    MockHttpTransport mock;
    mock.enqueueStream(200, {"one", "two", "three"});

    int delivered = 0;
    const auto r = mock.post(
        req(),
        [&](const char*, std::size_t) {
            ++delivered;
            return delivered < 2;  // abort after the second chunk arrives
        },
        {});

    CHECK(delivered == 2);
    CHECK(r.status == 0);
    CHECK(r.error == "aborted by receiver");
}

TEST_CASE("cancel fired mid-stream stops delivery with a cancelled error") {
    MockHttpTransport mock;
    mock.enqueueStream(200, {"one", "two", "three"});

    CancelToken token;
    mock.onBeforeChunk = [&](std::size_t i) {
        if (i == 1) token.cancel();
    };

    int delivered = 0;
    const auto r = mock.post(
        req(),
        [&](const char*, std::size_t) {
            ++delivered;
            return true;
        },
        token);

    CHECK(delivered == 1);  // chunk 0 only; cancel fired before chunk 1
    CHECK(r.status == 0);
    CHECK(r.error == "cancelled");
}

TEST_CASE("default transport rejects malformed URLs without touching the network") {
    const auto transport = defaultHttpTransport();
    REQUIRE(transport.get() != nullptr);

    HttpRequest bad;
    bad.url = "not-a-url";
    const auto r = transport->post(bad, nullptr, {});
    CHECK(r.status == 0);
    CHECK(r.error == "invalid url: not-a-url");

    HttpRequest ftp;
    ftp.url = "ftp://example.com/file";
    const auto r2 = transport->post(ftp, nullptr, {});
    CHECK(r2.status == 0);
    CHECK(r2.error == "invalid url: ftp://example.com/file");
}

TEST_CASE("default transport rejects userinfo in the authority without connecting") {
    const auto transport = defaultHttpTransport();
    HttpRequest r;
    r.url = "https://api.anthropic.com@evil.example/v1/messages";
    const auto resp = transport->post(r, nullptr, {});
    CHECK(resp.status == 0);
    // Userinfo is redacted in the error: it may be "user:password".
    CHECK(resp.error == "invalid url: https://<redacted>@evil.example/v1/messages");

    r.url = "https://user:hunter2@evil.example/";
    const auto resp2 = transport->post(r, nullptr, {});
    CHECK(resp2.error == "invalid url: https://<redacted>@evil.example/");
}

TEST_CASE("default transport rejects control bytes and spaces anywhere in the URL") {
    const auto transport = defaultHttpTransport();
    const std::vector<std::string> bad = {
        "http://127.0.0.1:1/x\r\nHost: evil.example",  // request-line split
        std::string("http://127.0.0.1:1/a\0b", 22),    // NUL would truncate the path
        "http://127.0.0.1:1/a b",
        "http://127.0.0.1:1/\x7f",
    };
    for (const auto& url : bad) {
        HttpRequest r;
        r.url = url;
        const auto resp = transport->post(r, nullptr, {});
        CHECK(resp.status == 0);
        CHECK(resp.error.rfind("invalid url: ", 0) == 0);
    }
}

TEST_CASE("invalid-url errors escape control bytes and redact the query") {
    const auto transport = defaultHttpTransport();
    HttpRequest r;
    r.url = "http://127.0.0.1:1/x\r\ny";
    auto resp = transport->post(r, nullptr, {});
    CHECK(resp.error == "invalid url: http://127.0.0.1:1/x\\x0D\\x0Ay");  // no raw CR/LF in logs

    r.url = "ftp://example.com/v1?key=SECRET#frag";
    resp = transport->post(r, nullptr, {});
    CHECK(resp.error == "invalid url: ftp://example.com/v1?<redacted>");
}

TEST_CASE("default transport rejects CR/LF in a header without connecting") {
    const auto transport = defaultHttpTransport();
    HttpRequest r;
    r.url = "http://127.0.0.1:1/x";
    r.headers = {{"x-api-key", "good\r\nHost: evil.example"}};
    const auto resp = transport->post(r, nullptr, {});
    CHECK(resp.status == 0);
    CHECK(resp.error == "invalid header value (control character): x-api-key");
}

TEST_CASE("default transport rejects any control byte in a header value, never echoing it") {
    const auto transport = defaultHttpTransport();
    for (const auto& value : {std::string("sk-secret\0x", 11), std::string("sk-secret\x01"),
                              std::string("sk-secret\x7f")}) {
        HttpRequest r;
        r.url = "http://127.0.0.1:1/x";
        r.headers = {{"x-api-key", value}};
        const auto resp = transport->post(r, nullptr, {});
        CHECK(resp.status == 0);
        CHECK(resp.error == "invalid header value (control character): x-api-key");
        CHECK(resp.error.find("sk-secret") == std::string::npos);
    }
}

TEST_CASE("default transport rejects header names that are not RFC 9110 tokens") {
    const auto transport = defaultHttpTransport();
    for (const auto& name : {"", "x-a: injected", "bad name", "x\r\nHost", "x\x01"}) {
        HttpRequest r;
        r.url = "http://127.0.0.1:1/x";
        r.headers = {{name, "v"}};
        const auto resp = transport->post(r, nullptr, {});
        CHECK(resp.status == 0);
        CHECK(resp.error == "invalid header name");
    }
}

TEST_CASE("a tab in a header value is valid and passes validation") {
    const auto transport = defaultHttpTransport();
    CancelToken token;
    token.cancel();
    HttpRequest r;
    r.url = "http://127.0.0.1:1/x";
    r.headers = {{"x-note", "a\tb"}};
    // Validation runs before the pre-flight cancel, so "cancelled" proves the
    // header was accepted — without opening a socket.
    const auto resp = transport->post(r, nullptr, token);
    CHECK(resp.error == "cancelled");
}

TEST_CASE("malformed input is reported even when the token is already cancelled") {
    const auto transport = defaultHttpTransport();
    CancelToken token;
    token.cancel();
    HttpRequest r;
    r.url = "http://127.0.0.1:1/x";
    r.headers = {{"x-a", "b\nc"}};
    const auto resp = transport->post(r, nullptr, token);
    CHECK(resp.error == "invalid header value (control character): x-a");
}

TEST_CASE("default transport fails fast when the token is already cancelled") {
    const auto transport = defaultHttpTransport();
    CancelToken token;
    token.cancel();
    // Never reaches the network: the pre-flight cancel check fires first.
    const auto r = transport->post(req("http://127.0.0.1:1/x"), nullptr, token);
    CHECK(r.status == 0);
    CHECK(r.error == "cancelled");
}
