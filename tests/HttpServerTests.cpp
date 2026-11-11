// Unit and integration tests for lightweight-http-server.
// Uses a tiny self-contained harness so the project stays dependency free.

#include "HttpServer.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " #condition "\n"; \
        }                                                                                  \
    } while (false)

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        ++g_checks;                                                                   \
        const auto& actualValue = (actual);                                           \
        const auto& expectedValue = (expected);                                       \
        if (!(actualValue == expectedValue)) {                                        \
            ++g_failures;                                                             \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << ": " #actual   \
                      << " == " #expected "\n    actual:   " << actualValue           \
                      << "\n    expected: " << expectedValue << "\n";                 \
        }                                                                             \
    } while (false)

struct TestCase {
    const char* name;
    std::function<void()> body;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back({name, std::move(body)});
    }
};

#define TEST(name)                                       \
    void name();                                         \
    const Registrar registrar_##name(#name, &name);      \
    void name()

const lhs::HttpRequestParser kParser(8 * 1024, 1024);

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

TEST(urlDecodeHandlesPercentAndPlus) {
    CHECK_EQ(lhs::urlDecode("a%20b", false), std::string("a b"));
    CHECK_EQ(lhs::urlDecode("a+b", true), std::string("a b"));
    CHECK_EQ(lhs::urlDecode("a+b", false), std::string("a+b"));
    CHECK_EQ(lhs::urlDecode("%41%42%43", false), std::string("ABC"));
    // Invalid or truncated escapes are kept literally.
    CHECK_EQ(lhs::urlDecode("100%", false), std::string("100%"));
    CHECK_EQ(lhs::urlDecode("%zz", false), std::string("%zz"));
    CHECK_EQ(lhs::urlDecode("%4", false), std::string("%4"));
}

TEST(parseQueryStringSplitsPairs) {
    const auto query = lhs::parseQueryString("a=1&b=two+words&empty=&flag&=ignored");
    CHECK_EQ(query.size(), std::size_t(4));
    CHECK_EQ(query.at("a"), std::string("1"));
    CHECK_EQ(query.at("b"), std::string("two words"));
    CHECK_EQ(query.at("empty"), std::string(""));
    CHECK_EQ(query.at("flag"), std::string(""));
}

TEST(statusReasonKnowsCommonCodes) {
    CHECK_EQ(lhs::statusReason(200), std::string_view("OK"));
    CHECK_EQ(lhs::statusReason(404), std::string_view("Not Found"));
    CHECK_EQ(lhs::statusReason(999), std::string_view("Unknown"));
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

TEST(jsonObjectSerializesAllTypes) {
    lhs::JsonObject nested;
    nested.add("x", 1);
    lhs::JsonObject object;
    object.add("s", "text").add("i", 42).add("d", 1.5).add("b", true).addNull("n").add("o", nested);
    CHECK_EQ(object.str(),
             std::string(R"({"s":"text","i":42,"d":1.5,"b":true,"n":null,"o":{"x":1}})"));
}

TEST(jsonEscapesSpecialCharacters) {
    CHECK_EQ(lhs::JsonObject::escape("a\"b\\c\nd\te"), std::string(R"(a\"b\\c\nd\te)"));
    CHECK_EQ(lhs::JsonObject::escape(std::string(1, '\x01')), std::string("\\u0001"));
}

TEST(jsonWritesNonFiniteNumbersAsNull) {
    CHECK_EQ(lhs::JsonObject::formatNumber(std::numeric_limits<double>::quiet_NaN()),
             std::string("null"));
    CHECK_EQ(lhs::JsonObject::formatNumber(std::numeric_limits<double>::infinity()),
             std::string("null"));
    CHECK_EQ(lhs::JsonObject::formatNumber(2.25), std::string("2.25"));
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

TEST(parsesSimpleGet) {
    const auto result = kParser.parse("GET /api/status?verbose=1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(result.status == lhs::ParseStatus::Complete);
    CHECK_EQ(result.request.method, std::string("GET"));
    CHECK_EQ(result.request.path, std::string("/api/status"));
    CHECK_EQ(result.request.target, std::string("/api/status?verbose=1"));
    CHECK_EQ(result.request.query.at("verbose"), std::string("1"));
    CHECK_EQ(result.request.version, std::string("HTTP/1.1"));
}

TEST(headersAreCaseInsensitive) {
    const auto result =
        kParser.parse("GET / HTTP/1.1\r\nHOST: example\r\nX-Custom:   padded value  \r\n\r\n");
    CHECK(result.status == lhs::ParseStatus::Complete);
    CHECK_EQ(result.request.header("host").value_or(""), std::string("example"));
    CHECK_EQ(result.request.header("x-custom").value_or(""), std::string("padded value"));
    CHECK_EQ(result.request.header("X-CUSTOM").value_or(""), std::string("padded value"));
}

TEST(duplicateHeadersAreJoined) {
    const auto result = kParser.parse("GET / HTTP/1.1\r\nHost: x\r\nAccept: a\r\nAccept: b\r\n\r\n");
    CHECK(result.status == lhs::ParseStatus::Complete);
    CHECK_EQ(result.request.header("accept").value_or(""), std::string("a, b"));
}

TEST(parsesBodyUsingContentLength) {
    const std::string raw = "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    const auto result = kParser.parse(raw);
    CHECK(result.status == lhs::ParseStatus::Complete);
    CHECK_EQ(result.request.body, std::string("hello"));
    CHECK_EQ(result.consumedBytes, raw.size());
}

TEST(reportsIncompleteHeadersAndBody) {
    CHECK(kParser.parse("GET / HTTP/1.1\r\nHost: x\r\n").status == lhs::ParseStatus::Incomplete);
    CHECK(kParser.parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc").status ==
          lhs::ParseStatus::Incomplete);
}

TEST(handlesPipelinedRequests) {
    const std::string first = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    const std::string second = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    const auto result = kParser.parse(first + second);
    CHECK(result.status == lhs::ParseStatus::Complete);
    CHECK_EQ(result.request.path, std::string("/a"));
    CHECK_EQ(result.consumedBytes, first.size());
}

TEST(skipsLeadingEmptyLines) {
    const auto result = kParser.parse("\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(result.status == lhs::ParseStatus::Complete);
}

TEST(rejectsMalformedRequests) {
    struct Case {
        const char* raw;
        int status;
    };
    const Case cases[] = {
        {"GARBAGE\r\n\r\n", 400},
        {"GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\n\r\n", 400},  // Missing Host
        {"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505},
        {"GET http://x/ HTTP/1.1\r\nHost: x\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\nBad Header: 1\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na", 400},
        {"POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
        {"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 999999\r\n\r\n", 413},
    };
    for (const auto& testCase : cases) {
        const auto result = kParser.parse(testCase.raw);
        CHECK(result.status == lhs::ParseStatus::Error);
        CHECK_EQ(result.errorStatus, testCase.status);
    }
}

TEST(rejectsOversizedHeaderBlock) {
    const lhs::HttpRequestParser smallParser(64, 1024);
    const std::string raw = "GET / HTTP/1.1\r\nHost: x\r\nX-Long: " + std::string(100, 'a');
    const auto result = smallParser.parse(raw);
    CHECK(result.status == lhs::ParseStatus::Error);
    CHECK_EQ(result.errorStatus, 431);
}

TEST(http10DoesNotRequireHost) {
    const auto result = kParser.parse("GET / HTTP/1.0\r\n\r\n");
    CHECK(result.status == lhs::ParseStatus::Complete);
}

// ---------------------------------------------------------------------------
// Response serialization
// ---------------------------------------------------------------------------

TEST(serializesStatusLineAndFraming) {
    const auto wire = lhs::HttpResponse::text("hi").serialize(true, false);
    CHECK(wire.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(wire.find("Content-Length: 2\r\n") != std::string::npos);
    CHECK(wire.find("Connection: keep-alive\r\n") != std::string::npos);
    CHECK(wire.find("Content-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
    CHECK(wire.size() >= 6 && wire.compare(wire.size() - 6, 6, "\r\n\r\nhi") == 0);
}

TEST(headResponseKeepsLengthButOmitsBody) {
    const auto wire = lhs::HttpResponse::text("hello").serialize(false, true);
    CHECK(wire.find("Content-Length: 5\r\n") != std::string::npos);
    CHECK(wire.find("Connection: close\r\n") != std::string::npos);
    CHECK(wire.size() >= 4 && wire.compare(wire.size() - 4, 4, "\r\n\r\n") == 0);
}

TEST(noContentResponseHasNoBodyOrLength) {
    lhs::HttpResponse response = lhs::HttpResponse::text("ignored", 204);
    const auto wire = response.serialize(true, false);
    CHECK(wire.find("Content-Length") == std::string::npos);
    CHECK(wire.find("ignored") == std::string::npos);
}

TEST(userCannotOverrideFramingHeaders) {
    lhs::HttpResponse response = lhs::HttpResponse::text("abc");
    response.setHeader("Content-Length", "999").setHeader("X-Trace", "1");
    const auto wire = response.serialize(true, false);
    CHECK(wire.find("Content-Length: 999") == std::string::npos);
    CHECK(wire.find("Content-Length: 3\r\n") != std::string::npos);
    CHECK(wire.find("X-Trace: 1\r\n") != std::string::npos);
}

TEST(errorResponseIsJson) {
    const auto response = lhs::HttpResponse::error(404, "missing");
    CHECK_EQ(response.status, 404);
    CHECK_EQ(response.body, std::string(R"({"error":"Not Found","message":"missing","status":404})"));
    CHECK_EQ(response.headers.at("Content-Type"), std::string("application/json; charset=utf-8"));
}

// ---------------------------------------------------------------------------
// End-to-end: real sockets against a running server
// ---------------------------------------------------------------------------

#ifdef _WIN32
using ClientSocket = SOCKET;
void closeClient(ClientSocket s) { ::closesocket(s); }
#else
using ClientSocket = int;
void closeClient(ClientSocket s) { ::close(s); }
#endif

// Sends a raw request and reads until the server closes the connection.
std::string roundTrip(std::uint16_t port, const std::string& request) {
    const ClientSocket client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closeClient(client);
        return {};
    }
    ::send(client, request.data(), static_cast<int>(request.size()), 0);

    std::string response;
    char buffer[4096];
    for (;;) {
        const auto received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }
    closeClient(client);
    return response;
}

std::size_t countOccurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

TEST(endToEndRoutingOverRealSockets) {
    lhs::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;  // Let the OS choose a free port.
    config.workerThreads = 2;
    config.accessLog = false;

    lhs::HttpServer server(config);
    server.get("/ping", [](const lhs::HttpRequest&) { return lhs::HttpResponse::text("pong"); });
    server.post("/echo", [](const lhs::HttpRequest& request) {
        return lhs::HttpResponse::text(request.body, 201);
    });
    server.get("/boom", [](const lhs::HttpRequest&) -> lhs::HttpResponse {
        throw std::runtime_error("handler failure");
    });

    std::thread serverThread([&server] { server.run(); });
    for (int i = 0; i < 200 && server.boundPort() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::uint16_t port = server.boundPort();
    CHECK(port != 0);

    if (port != 0) {
        const auto ping = roundTrip(port, "GET /ping HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        CHECK(ping.rfind("HTTP/1.1 200 OK", 0) == 0);
        CHECK(ping.find("\r\n\r\npong") != std::string::npos);

        const auto echo = roundTrip(
            port, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\nConnection: close\r\n\r\ndata");
        CHECK(echo.rfind("HTTP/1.1 201 Created", 0) == 0);
        CHECK(echo.find("\r\n\r\ndata") != std::string::npos);

        const auto missing = roundTrip(port, "GET /nope HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        CHECK(missing.rfind("HTTP/1.1 404", 0) == 0);

        const auto wrongMethod =
            roundTrip(port, "DELETE /ping HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        CHECK(wrongMethod.rfind("HTTP/1.1 405", 0) == 0);
        CHECK(wrongMethod.find("Allow: GET, HEAD, OPTIONS\r\n") != std::string::npos);

        const auto head = roundTrip(port, "HEAD /ping HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        CHECK(head.find("Content-Length: 4\r\n") != std::string::npos);
        CHECK(head.find("pong") == std::string::npos);

        const auto failing = roundTrip(port, "GET /boom HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        CHECK(failing.rfind("HTTP/1.1 500", 0) == 0);

        const auto malformed = roundTrip(port, "NOT A REQUEST\r\n\r\n");
        CHECK(malformed.rfind("HTTP/1.1 400", 0) == 0);

        // Two pipelined requests on one connection must yield two responses.
        const auto pipelined = roundTrip(port,
                                         "GET /ping HTTP/1.1\r\nHost: t\r\n\r\n"
                                         "GET /ping HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        CHECK_EQ(countOccurrences(pipelined, "HTTP/1.1 200 OK"), std::size_t(2));

        const auto stats = server.stats();
        CHECK(stats.totalRequests >= 8);
        CHECK(stats.responses5xx >= 1);
    }

    server.stop();
    serverThread.join();
    CHECK(!server.isRunning());
}

}  // namespace

int main() {
    for (const auto& test : registry()) {
        const int failuresBefore = g_failures;
        test.body();
        std::cout << (g_failures == failuresBefore ? "[ PASS ] " : "[ FAIL ] ") << test.name << '\n';
    }
    std::cout << '\n'
              << registry().size() << " tests, " << g_checks << " checks, " << g_failures
              << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}
