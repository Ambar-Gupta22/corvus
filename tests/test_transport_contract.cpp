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

TEST_CASE("default transport fails fast when the token is already cancelled") {
    const auto transport = defaultHttpTransport();
    CancelToken token;
    token.cancel();
    // Never reaches the network: the pre-flight cancel check fires first.
    const auto r = transport->post(req("http://127.0.0.1:1/x"), nullptr, token);
    CHECK(r.status == 0);
    CHECK(r.error == "cancelled");
}
