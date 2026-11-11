// lightweight-http-server
// A minimal HTTP/1.1 server built directly on top of BSD sockets / Winsock.
//
// Design goals:
//   * No third-party dependencies, only the C++17 standard library and the
//     operating system socket API.
//   * Readable code that shows how an HTTP request travels from raw bytes on
//     a TCP connection to a typed request object and back to bytes again.
//   * Safe defaults: bounded header/body sizes, idle timeouts and a bounded
//     connection queue.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace lhs {

// Platform-neutral socket handle. SOCKET on Windows is an unsigned pointer-sized
// integer and a file descriptor on POSIX is an int; both fit into intptr_t.
using NativeSocket = std::intptr_t;
inline constexpr NativeSocket kInvalidSocket = -1;

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

// Decodes percent-encoded sequences (%41 -> 'A'). When plusAsSpace is true the
// '+' character is decoded as a space, as required for form-encoded data.
std::string urlDecode(std::string_view input, bool plusAsSpace);

// Parses "a=1&b=two" style strings (query strings and form bodies).
std::map<std::string, std::string> parseQueryString(std::string_view input);

// Returns the standard reason phrase for an HTTP status code.
std::string_view statusReason(int status);

// ---------------------------------------------------------------------------
// JSON output helpers
// ---------------------------------------------------------------------------

// Small builder for JSON objects. It is intentionally output-only: the server
// produces JSON responses but does not need a full JSON parser for that.
class JsonObject {
public:
    JsonObject& add(const std::string& key, const std::string& value);
    JsonObject& add(const std::string& key, const char* value);
    JsonObject& add(const std::string& key, bool value);
    JsonObject& addNull(const std::string& key);

    template <typename T,
              std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    JsonObject& add(const std::string& key, T value) {
        return addRaw(key, std::to_string(value));
    }

    template <typename T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
    JsonObject& add(const std::string& key, T value) {
        return addRaw(key, formatNumber(static_cast<double>(value)));
    }

    JsonObject& add(const std::string& key, const JsonObject& nested);

    // Inserts an already serialized JSON value (array, object, literal).
    JsonObject& addRaw(const std::string& key, std::string rawJson);

    std::string str() const;

    static std::string escape(std::string_view text);
    static std::string formatNumber(double value);

private:
    std::vector<std::pair<std::string, std::string>> fields_;
};

// ---------------------------------------------------------------------------
// Request / response model
// ---------------------------------------------------------------------------

struct HttpRequest {
    std::string method;   // "GET", "POST", ...
    std::string target;   // Raw request target as sent: "/api/x?y=1"
    std::string path;     // Percent-decoded path without the query: "/api/x"
    std::string version;  // "HTTP/1.1" or "HTTP/1.0"
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;  // Keys are lower-cased.
    std::string body;
    std::string remoteAddress;

    // Header lookup is case-insensitive, as mandated by RFC 9110.
    std::optional<std::string> header(std::string_view name) const;
    std::optional<std::string> queryParam(const std::string& name) const;
};

struct HttpResponse {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;

    static HttpResponse text(std::string body, int status = 200);
    static HttpResponse json(std::string body, int status = 200);
    static HttpResponse json(const JsonObject& object, int status = 200);
    static HttpResponse error(int status, const std::string& message);

    HttpResponse& setHeader(const std::string& name, const std::string& value);

    // Converts the response into the exact bytes that go on the wire.
    // Content-Length, Connection, Date and Server headers are managed here.
    std::string serialize(bool keepAlive, bool omitBody) const;
};

// ---------------------------------------------------------------------------
// Request parser
// ---------------------------------------------------------------------------

enum class ParseStatus { Complete, Incomplete, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::Incomplete;
    HttpRequest request;
    std::size_t consumedBytes = 0;  // Bytes that belong to this request.
    int errorStatus = 0;            // HTTP status to answer with on Error.
    std::string errorMessage;
};

// Incremental-friendly HTTP/1.x request parser. It is stateless: callers feed
// the whole receive buffer and get back either a complete request, a request
// that needs more bytes, or a protocol error with a matching HTTP status.
class HttpRequestParser {
public:
    HttpRequestParser(std::size_t maxHeaderBytes, std::size_t maxBodyBytes);

    ParseResult parse(std::string_view buffer) const;

private:
    std::size_t maxHeaderBytes_;
    std::size_t maxBodyBytes_;
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

struct ServerConfig {
    std::string host = "0.0.0.0";
    std::uint16_t port = 8080;
    std::size_t workerThreads = 4;
    std::size_t maxPendingConnections = 256;
    std::size_t maxHeaderBytes = 8 * 1024;
    std::size_t maxBodyBytes = 1024 * 1024;
    std::size_t maxRequestsPerConnection = 100;
    int keepAliveTimeoutMs = 5000;
    int sendTimeoutMs = 10000;
    bool accessLog = true;
};

struct ServerStats {
    std::uint64_t totalConnections = 0;
    std::uint64_t activeConnections = 0;
    std::uint64_t rejectedConnections = 0;
    std::uint64_t totalRequests = 0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t responses2xx = 0;
    std::uint64_t responses3xx = 0;
    std::uint64_t responses4xx = 0;
    std::uint64_t responses5xx = 0;
    double uptimeSeconds = 0.0;
};

using Handler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
public:
    explicit HttpServer(ServerConfig config);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Route registration. Paths are matched exactly against the decoded path.
    void route(const std::string& method, const std::string& path, Handler handler);
    void get(const std::string& path, Handler handler);
    void post(const std::string& path, Handler handler);
    void put(const std::string& path, Handler handler);
    void del(const std::string& path, Handler handler);

    // Binds, listens and serves until stop() is called. Blocks the caller.
    void run();

    // Requests a graceful shutdown. Only flips an atomic flag, so it is safe to
    // call from a signal handler or from any thread.
    void stop() noexcept;

    bool isRunning() const noexcept;
    ServerStats stats() const;
    const ServerConfig& config() const noexcept { return config_; }

    // Thread-safe logging helper used for the access log and lifecycle messages.
    void log(const std::string& message) const;

private:
    struct PendingConnection {
        NativeSocket socket = kInvalidSocket;
        std::string remoteAddress;
    };

    struct Counters {
        std::atomic<std::uint64_t> totalConnections{0};
        std::atomic<std::uint64_t> activeConnections{0};
        std::atomic<std::uint64_t> rejectedConnections{0};
        std::atomic<std::uint64_t> totalRequests{0};
        std::atomic<std::uint64_t> bytesReceived{0};
        std::atomic<std::uint64_t> bytesSent{0};
        std::atomic<std::uint64_t> responses2xx{0};
        std::atomic<std::uint64_t> responses3xx{0};
        std::atomic<std::uint64_t> responses4xx{0};
        std::atomic<std::uint64_t> responses5xx{0};
    };

    void openListener();
    void acceptLoop();
    void workerLoop();
    void handleConnection(NativeSocket client, const std::string& remoteAddress);
    HttpResponse dispatch(const HttpRequest& request) const;
    void recordStatus(int status);

    ServerConfig config_;
    HttpRequestParser parser_;

    // path -> (method -> handler)
    std::map<std::string, std::map<std::string, Handler>> routes_;
    mutable std::mutex routesMutex_;

    std::atomic<bool> running_{false};
    NativeSocket listener_ = kInvalidSocket;
    std::vector<std::thread> workers_;

    std::queue<PendingConnection> pending_;
    std::mutex pendingMutex_;
    std::condition_variable pendingCv_;

    Counters counters_;
    std::chrono::steady_clock::time_point startedAt_{};
    mutable std::mutex logMutex_;
};

}  // namespace lhs
