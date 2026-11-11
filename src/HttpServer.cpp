#include "HttpServer.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <stdexcept>

namespace lhs {

namespace {

// ---------------------------------------------------------------------------
// Platform layer: the only place that knows about Winsock vs. POSIX sockets.
// ---------------------------------------------------------------------------

#ifdef _WIN32
using RawSocket = SOCKET;
using SockLen = int;
#else
using RawSocket = int;
using SockLen = socklen_t;
#endif

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;  // Do not raise SIGPIPE on Linux.
#else
constexpr int kSendFlags = 0;
#endif

constexpr std::string_view kServerName = "lightweight-http-server/1.0";

RawSocket toRaw(NativeSocket socket) {
    return static_cast<RawSocket>(socket);
}

NativeSocket fromRaw(RawSocket socket) {
#ifdef _WIN32
    return socket == INVALID_SOCKET ? kInvalidSocket : static_cast<NativeSocket>(socket);
#else
    return socket < 0 ? kInvalidSocket : static_cast<NativeSocket>(socket);
#endif
}

void closeSocket(NativeSocket socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::closesocket(toRaw(socket));
#else
    ::close(toRaw(socket));
#endif
}

std::string lastSocketError() {
#ifdef _WIN32
    return "WSA error " + std::to_string(::WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

bool interruptedBySignal() {
#ifdef _WIN32
    return false;
#else
    return errno == EINTR;
#endif
}

// Waits until the socket has data (or a pending connection for a listener).
// Returns 1 when readable, 0 on timeout and -1 on error. poll() is used instead
// of select() because select() cannot handle descriptors >= FD_SETSIZE.
int waitReadable(NativeSocket socket, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD descriptor{};
    descriptor.fd = toRaw(socket);
    descriptor.events = POLLRDNORM;
    const int result = ::WSAPoll(&descriptor, 1, timeoutMs);
#else
    pollfd descriptor{};
    descriptor.fd = toRaw(socket);
    descriptor.events = POLLIN;
    const int result = ::poll(&descriptor, 1, timeoutMs);
    if (result < 0 && interruptedBySignal()) {
        return 0;
    }
#endif
    if (result < 0) {
        return -1;
    }
    return result == 0 ? 0 : 1;
}

void setTimeoutOption(NativeSocket socket, int option, int timeoutMs) {
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(timeoutMs);
    ::setsockopt(toRaw(socket), SOL_SOCKET, option, reinterpret_cast<const char*>(&value),
                 sizeof(value));
#else
    timeval value{};
    value.tv_sec = timeoutMs / 1000;
    value.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(toRaw(socket), SOL_SOCKET, option, &value, sizeof(value));
#endif
}

void setNoDelay(NativeSocket socket) {
    int flag = 1;
    ::setsockopt(toRaw(socket), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag),
                 sizeof(flag));
}

// Reads whatever is available. Returns the number of bytes, 0 on orderly
// shutdown by the peer and -1 on error.
long receiveSome(NativeSocket socket, char* buffer, std::size_t capacity) {
    for (;;) {
#ifdef _WIN32
        const int received = ::recv(toRaw(socket), buffer, static_cast<int>(capacity), 0);
#else
        const ssize_t received = ::recv(toRaw(socket), buffer, capacity, 0);
#endif
        if (received < 0 && interruptedBySignal()) {
            continue;
        }
        return static_cast<long>(received);
    }
}

// send() may write fewer bytes than requested, so loop until everything is out.
bool sendAll(NativeSocket socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const std::size_t chunk = std::min<std::size_t>(data.size() - sent, 1 << 20);
#ifdef _WIN32
        const int written =
            ::send(toRaw(socket), data.data() + sent, static_cast<int>(chunk), kSendFlags);
#else
        const ssize_t written = ::send(toRaw(socket), data.data() + sent, chunk, kSendFlags);
#endif
        if (written < 0 && interruptedBySignal()) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

std::string formatAddress(const sockaddr_storage& address) {
    if (address.ss_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
        char text[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text));
        return std::string(text) + ":" + std::to_string(ntohs(ipv4->sin_port));
    }
    return "unknown";
}

std::tm utcTime(std::time_t time) {
    std::tm result{};
#ifdef _WIN32
    ::gmtime_s(&result, &time);
#else
    ::gmtime_r(&time, &result);
#endif
    return result;
}

// RFC 9110 IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
std::string httpDate() {
    const std::tm tm = utcTime(std::time(nullptr));
    static constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                  kDays[tm.tm_wday], tm.tm_mday, kMonths[tm.tm_mon], tm.tm_year + 1900,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

std::string logTimestamp() {
    const std::tm tm = utcTime(std::time(nullptr));
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::string toLower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string_view trim(std::string_view text) {
    const auto isSpace = [](char c) { return c == ' ' || c == '\t'; };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// "tchar" from RFC 9110: the characters allowed in methods and header names.
bool isTokenChar(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
        return true;
    }
    return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool isToken(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), isTokenChar);
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

ParseResult parseError(int status, std::string message) {
    ParseResult result;
    result.status = ParseStatus::Error;
    result.errorStatus = status;
    result.errorMessage = std::move(message);
    return result;
}

bool headerContainsToken(const std::string& value, std::string_view token) {
    // Connection: keep-alive, Upgrade  -> split on commas and compare tokens.
    std::string_view rest(value);
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const std::string_view part = trim(rest.substr(0, comma));
        if (equalsIgnoreCase(part, token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return false;
}

bool shouldKeepAlive(const HttpRequest& request) {
    const auto connection = request.header("connection");
    if (connection && headerContainsToken(*connection, "close")) {
        return false;
    }
    if (request.version == "HTTP/1.0") {
        // HTTP/1.0 closes by default unless the client explicitly opts in.
        return connection && headerContainsToken(*connection, "keep-alive");
    }
    return true;
}

#ifdef _WIN32
// Winsock must be initialised once per process before any socket call.
class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }
    ~WinsockRuntime() { ::WSACleanup(); }
    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
};

void ensureSocketRuntime() {
    static WinsockRuntime runtime;
}
#else
void ensureSocketRuntime() {
    // Writing to a socket the peer already closed would kill the process with
    // SIGPIPE on POSIX systems; we prefer to get an error code from send().
    std::signal(SIGPIPE, SIG_IGN);
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::string urlDecode(std::string_view input, bool plusAsSpace) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '%' && i + 2 < input.size()) {
            const int high = hexValue(input[i + 1]);
            const int low = hexValue(input[i + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        output.push_back(plusAsSpace && c == '+' ? ' ' : c);
    }
    return output;
}

std::map<std::string, std::string> parseQueryString(std::string_view input) {
    std::map<std::string, std::string> result;
    while (!input.empty()) {
        const std::size_t amp = input.find('&');
        const std::string_view pair = input.substr(0, amp);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            std::string key = urlDecode(pair.substr(0, eq), true);
            std::string value =
                eq == std::string_view::npos ? std::string() : urlDecode(pair.substr(eq + 1), true);
            if (!key.empty()) {
                result[std::move(key)] = std::move(value);
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        input.remove_prefix(amp + 1);
    }
    return result;
}

std::string_view statusReason(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// JsonObject
// ---------------------------------------------------------------------------

JsonObject& JsonObject::add(const std::string& key, const std::string& value) {
    return addRaw(key, "\"" + escape(value) + "\"");
}

JsonObject& JsonObject::add(const std::string& key, const char* value) {
    return value ? add(key, std::string(value)) : addNull(key);
}

JsonObject& JsonObject::add(const std::string& key, bool value) {
    return addRaw(key, value ? "true" : "false");
}

JsonObject& JsonObject::addNull(const std::string& key) {
    return addRaw(key, "null");
}

JsonObject& JsonObject::add(const std::string& key, const JsonObject& nested) {
    return addRaw(key, nested.str());
}

JsonObject& JsonObject::addRaw(const std::string& key, std::string rawJson) {
    fields_.emplace_back(key, std::move(rawJson));
    return *this;
}

std::string JsonObject::str() const {
    std::string out = "{";
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        out += '"';
        out += escape(fields_[i].first);
        out += "\":";
        out += fields_[i].second;
    }
    out += '}';
    return out;
}

std::string JsonObject::escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string JsonObject::formatNumber(double value) {
    // JSON has no representation for NaN or infinity.
    if (!std::isfinite(value)) {
        return "null";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    return buffer;
}

// ---------------------------------------------------------------------------
// HttpRequest / HttpResponse
// ---------------------------------------------------------------------------

std::optional<std::string> HttpRequest::header(std::string_view name) const {
    const auto it = headers.find(toLower(name));
    if (it == headers.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> HttpRequest::queryParam(const std::string& name) const {
    const auto it = query.find(name);
    if (it == query.end()) {
        return std::nullopt;
    }
    return it->second;
}

HttpResponse HttpResponse::text(std::string body, int status) {
    HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    response.headers["Content-Type"] = "text/plain; charset=utf-8";
    return response;
}

HttpResponse HttpResponse::json(std::string body, int status) {
    HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    response.headers["Content-Type"] = "application/json; charset=utf-8";
    return response;
}

HttpResponse HttpResponse::json(const JsonObject& object, int status) {
    return json(object.str(), status);
}

HttpResponse HttpResponse::error(int status, const std::string& message) {
    JsonObject body;
    body.add("error", std::string(statusReason(status)))
        .add("message", message)
        .add("status", status);
    return json(body, status);
}

HttpResponse& HttpResponse::setHeader(const std::string& name, const std::string& value) {
    headers[name] = value;
    return *this;
}

std::string HttpResponse::serialize(bool keepAlive, bool omitBody) const {
    // 1xx, 204 and 304 responses must never carry a body (RFC 9110 §6.4.1).
    const bool bodyForbidden = (status >= 100 && status < 200) || status == 204 || status == 304;

    std::string out;
    out.reserve(256 + body.size());
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += statusReason(status);
    out += "\r\n";

    out += "Date: " + httpDate() + "\r\n";
    out += "Server: ";
    out += kServerName;
    out += "\r\n";
    out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    if (!bodyForbidden) {
        // For HEAD the length of the body that *would* be sent is reported.
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }

    for (const auto& [name, value] : headers) {
        // These are owned by the framing logic above.
        if (equalsIgnoreCase(name, "content-length") || equalsIgnoreCase(name, "connection") ||
            equalsIgnoreCase(name, "date") || equalsIgnoreCase(name, "server") ||
            equalsIgnoreCase(name, "transfer-encoding")) {
            continue;
        }
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "\r\n";

    if (!omitBody && !bodyForbidden) {
        out += body;
    }
    return out;
}

// ---------------------------------------------------------------------------
// HttpRequestParser
// ---------------------------------------------------------------------------

HttpRequestParser::HttpRequestParser(std::size_t maxHeaderBytes, std::size_t maxBodyBytes)
    : maxHeaderBytes_(maxHeaderBytes), maxBodyBytes_(maxBodyBytes) {}

ParseResult HttpRequestParser::parse(std::string_view buffer) const {
    // RFC 9112 §2.2: a server SHOULD ignore empty lines received before the
    // request line (some clients send a stray CRLF after a POST body).
    std::size_t offset = 0;
    while (buffer.size() - offset >= 2 && buffer.compare(offset, 2, "\r\n") == 0) {
        offset += 2;
    }
    const std::string_view data = buffer.substr(offset);

    // 1) Wait for the end of the header block: an empty line (CRLF CRLF).
    const std::size_t headerEnd = data.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos) {
        if (data.size() > maxHeaderBytes_) {
            return parseError(431, "Request header block exceeds the configured limit");
        }
        return ParseResult{};  // Incomplete
    }
    if (headerEnd + 4 > maxHeaderBytes_) {
        return parseError(431, "Request header block exceeds the configured limit");
    }

    const std::string_view head = data.substr(0, headerEnd);
    const std::size_t requestLineEnd = head.find("\r\n");
    const std::string_view requestLine = head.substr(0, requestLineEnd);

    // 2) Request line: METHOD SP request-target SP HTTP-version
    const std::size_t firstSpace = requestLine.find(' ');
    const std::size_t secondSpace =
        firstSpace == std::string_view::npos ? std::string_view::npos
                                             : requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos ||
        requestLine.find(' ', secondSpace + 1) != std::string_view::npos) {
        return parseError(400, "Malformed request line");
    }

    HttpRequest request;
    request.method = std::string(requestLine.substr(0, firstSpace));
    request.target = std::string(requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1));
    request.version = std::string(requestLine.substr(secondSpace + 1));

    if (!isToken(request.method)) {
        return parseError(400, "Invalid request method");
    }
    if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
        if (request.version.rfind("HTTP/", 0) == 0) {
            return parseError(505, "Only HTTP/1.0 and HTTP/1.1 are supported");
        }
        return parseError(400, "Invalid HTTP version");
    }
    if (request.target.empty() || request.target.front() != '/') {
        return parseError(400, "Only origin-form request targets are supported");
    }

    const std::size_t questionMark = request.target.find('?');
    const std::string_view rawPath = std::string_view(request.target).substr(0, questionMark);
    request.path = urlDecode(rawPath, false);
    if (request.path.find('\0') != std::string::npos) {
        return parseError(400, "Path contains a NUL byte");
    }
    if (questionMark != std::string::npos) {
        request.query = parseQueryString(std::string_view(request.target).substr(questionMark + 1));
    }

    // 3) Header fields: name ":" OWS value OWS
    std::string_view rest =
        requestLineEnd == std::string_view::npos ? std::string_view() : head.substr(requestLineEnd + 2);
    while (!rest.empty()) {
        const std::size_t lineEnd = rest.find("\r\n");
        const std::string_view line = rest.substr(0, lineEnd);
        rest = lineEnd == std::string_view::npos ? std::string_view() : rest.substr(lineEnd + 2);

        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            return parseError(400, "Obsolete header line folding is not allowed");
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return parseError(400, "Malformed header line");
        }
        const std::string_view name = line.substr(0, colon);
        if (!isToken(name)) {
            return parseError(400, "Invalid header name");
        }
        std::string key = toLower(name);
        const std::string_view value = trim(line.substr(colon + 1));

        auto existing = request.headers.find(key);
        if (existing == request.headers.end()) {
            request.headers.emplace(std::move(key), std::string(value));
        } else if (key == "content-length" || key == "host") {
            // Duplicates of these fields are a classic request smuggling vector.
            return parseError(400, "Duplicate " + key + " header");
        } else {
            existing->second += ", ";
            existing->second += value;
        }
    }

    if (request.version == "HTTP/1.1" && request.headers.count("host") == 0) {
        return parseError(400, "HTTP/1.1 requests must include a Host header");
    }
    if (request.headers.count("transfer-encoding") != 0) {
        return parseError(501, "Transfer-Encoding is not supported, send Content-Length instead");
    }

    // 4) Body framing via Content-Length.
    std::size_t contentLength = 0;
    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
        const std::string& text = it->second;
        unsigned long long parsed = 0;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (text.empty() || ec != std::errc() || ptr != text.data() + text.size()) {
            return parseError(400, "Invalid Content-Length header");
        }
        if (parsed > maxBodyBytes_) {
            return parseError(413, "Request body exceeds the configured limit");
        }
        contentLength = static_cast<std::size_t>(parsed);
    }

    const std::size_t bodyStart = headerEnd + 4;
    if (data.size() - bodyStart < contentLength) {
        return ParseResult{};  // Incomplete: body still arriving.
    }

    request.body = std::string(data.substr(bodyStart, contentLength));

    ParseResult result;
    result.status = ParseStatus::Complete;
    result.request = std::move(request);
    result.consumedBytes = offset + bodyStart + contentLength;
    return result;
}

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

HttpServer::HttpServer(ServerConfig config)
    : config_(std::move(config)), parser_(config_.maxHeaderBytes, config_.maxBodyBytes) {
    if (config_.workerThreads == 0) {
        config_.workerThreads = 1;
    }
    ensureSocketRuntime();
}

HttpServer::~HttpServer() {
    stop();
    closeSocket(listener_);
}

void HttpServer::route(const std::string& method, const std::string& path, Handler handler) {
    std::lock_guard<std::mutex> lock(routesMutex_);
    routes_[path][method] = std::move(handler);
}

void HttpServer::get(const std::string& path, Handler handler) {
    route("GET", path, std::move(handler));
}

void HttpServer::post(const std::string& path, Handler handler) {
    route("POST", path, std::move(handler));
}

void HttpServer::put(const std::string& path, Handler handler) {
    route("PUT", path, std::move(handler));
}

void HttpServer::del(const std::string& path, Handler handler) {
    route("DELETE", path, std::move(handler));
}

bool HttpServer::isRunning() const noexcept {
    return running_.load();
}

void HttpServer::stop() noexcept {
    // Intentionally minimal: the accept loop and the workers poll this flag
    // with short timeouts, which keeps stop() async-signal-safe.
    running_.store(false);
}

ServerStats HttpServer::stats() const {
    ServerStats snapshot;
    snapshot.totalConnections = counters_.totalConnections.load();
    snapshot.activeConnections = counters_.activeConnections.load();
    snapshot.rejectedConnections = counters_.rejectedConnections.load();
    snapshot.totalRequests = counters_.totalRequests.load();
    snapshot.bytesReceived = counters_.bytesReceived.load();
    snapshot.bytesSent = counters_.bytesSent.load();
    snapshot.responses2xx = counters_.responses2xx.load();
    snapshot.responses3xx = counters_.responses3xx.load();
    snapshot.responses4xx = counters_.responses4xx.load();
    snapshot.responses5xx = counters_.responses5xx.load();
    if (startedAt_ != std::chrono::steady_clock::time_point{}) {
        snapshot.uptimeSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_).count();
    }
    return snapshot;
}

void HttpServer::log(const std::string& message) const {
    std::lock_guard<std::mutex> lock(logMutex_);
    std::cout << '[' << logTimestamp() << "] " << message << '\n' << std::flush;
}

void HttpServer::openListener() {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("Invalid IPv4 address: " + config_.host);
    }

    const NativeSocket listener = fromRaw(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener == kInvalidSocket) {
        throw std::runtime_error("socket() failed: " + lastSocketError());
    }

    int enable = 1;
#ifdef _WIN32
    // On Windows SO_REUSEADDR would let another process steal the port.
    ::setsockopt(toRaw(listener), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&enable), sizeof(enable));
#else
    // Allows a quick restart while old connections sit in TIME_WAIT.
    ::setsockopt(toRaw(listener), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#endif

    if (::bind(toRaw(listener), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const std::string reason = lastSocketError();
        closeSocket(listener);
        throw std::runtime_error("bind() to " + config_.host + ":" + std::to_string(config_.port) +
                                 " failed: " + reason);
    }
    if (::listen(toRaw(listener), SOMAXCONN) != 0) {
        const std::string reason = lastSocketError();
        closeSocket(listener);
        throw std::runtime_error("listen() failed: " + reason);
    }
    // With port 0 the kernel assigns an ephemeral port; ask which one we got.
    sockaddr_in bound{};
    SockLen boundLength = sizeof(bound);
    if (::getsockname(toRaw(listener), reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
        boundPort_.store(ntohs(bound.sin_port));
    } else {
        boundPort_.store(config_.port);
    }
    listener_ = listener;
}

void HttpServer::run() {
    if (running_.exchange(true)) {
        throw std::logic_error("HttpServer::run() called while the server is already running");
    }
    try {
        openListener();
    } catch (...) {
        running_.store(false);
        throw;
    }

    startedAt_ = std::chrono::steady_clock::now();
    workers_.reserve(config_.workerThreads);
    for (std::size_t i = 0; i < config_.workerThreads; ++i) {
        workers_.emplace_back(&HttpServer::workerLoop, this);
    }

    log("Listening on http://" + config_.host + ":" + std::to_string(boundPort_.load()) + " with " +
        std::to_string(config_.workerThreads) + " worker thread(s)");

    acceptLoop();

    // Graceful shutdown: stop accepting, let workers finish what they hold.
    closeSocket(listener_);
    listener_ = kInvalidSocket;
    boundPort_.store(0);
    pendingCv_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
    workers_.clear();

    std::lock_guard<std::mutex> lock(pendingMutex_);
    while (!pending_.empty()) {
        closeSocket(pending_.front().socket);
        pending_.pop();
    }
    log("Server stopped");
}

void HttpServer::acceptLoop() {
    while (running_.load()) {
        const int ready = waitReadable(listener_, 250);
        if (ready <= 0) {
            continue;
        }

        sockaddr_storage peer{};
        SockLen peerLength = sizeof(peer);
        const NativeSocket client =
            fromRaw(::accept(toRaw(listener_), reinterpret_cast<sockaddr*>(&peer), &peerLength));
        if (client == kInvalidSocket) {
            continue;
        }
        counters_.totalConnections.fetch_add(1);

        PendingConnection connection{client, formatAddress(peer)};
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (pending_.size() < config_.maxPendingConnections) {
                pending_.push(std::move(connection));
                queued = true;
            }
        }

        if (queued) {
            pendingCv_.notify_one();
        } else {
            // Back-pressure: every worker is busy and the queue is full.
            counters_.rejectedConnections.fetch_add(1);
            const auto response =
                HttpResponse::error(503, "Server is overloaded, please retry later");
            sendAll(client, response.serialize(false, false));
            closeSocket(client);
        }
    }
}

void HttpServer::workerLoop() {
    for (;;) {
        PendingConnection connection;
        {
            std::unique_lock<std::mutex> lock(pendingMutex_);
            // wait_for instead of wait: stop() does not notify (signal safety),
            // so the worker re-checks the running flag periodically.
            while (pending_.empty() && running_.load()) {
                pendingCv_.wait_for(lock, std::chrono::milliseconds(250));
            }
            if (pending_.empty()) {
                return;
            }
            connection = std::move(pending_.front());
            pending_.pop();
        }
        handleConnection(connection.socket, connection.remoteAddress);
    }
}

void HttpServer::handleConnection(NativeSocket client, const std::string& remoteAddress) {
    using Clock = std::chrono::steady_clock;

    counters_.activeConnections.fetch_add(1);
    setTimeoutOption(client, SO_SNDTIMEO, config_.sendTimeoutMs);
    setNoDelay(client);

    std::string buffer;
    char chunk[16 * 1024];
    std::size_t servedRequests = 0;
    auto deadline = Clock::now() + std::chrono::milliseconds(config_.keepAliveTimeoutMs);

    while (running_.load()) {
        // The buffer may already hold a full request (HTTP pipelining), so the
        // parser runs before we block on the socket again.
        ParseResult parsed = parser_.parse(buffer);

        if (parsed.status == ParseStatus::Incomplete) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) {
                if (!buffer.empty()) {
                    // The client started a request but never finished it.
                    const auto response = HttpResponse::error(408, "Request was not completed in time");
                    sendAll(client, response.serialize(false, false));
                }
                break;
            }
            const int ready = waitReadable(client, static_cast<int>(std::min<long long>(remaining, 250)));
            if (ready < 0) {
                break;
            }
            if (ready == 0) {
                continue;
            }
            const long received = receiveSome(client, chunk, sizeof(chunk));
            if (received <= 0) {
                break;  // Peer closed the connection or a socket error occurred.
            }
            buffer.append(chunk, static_cast<std::size_t>(received));
            counters_.bytesReceived.fetch_add(static_cast<std::uint64_t>(received));
            continue;
        }

        if (parsed.status == ParseStatus::Error) {
            const auto response = HttpResponse::error(parsed.errorStatus, parsed.errorMessage);
            const std::string wire = response.serialize(false, false);
            if (sendAll(client, wire)) {
                counters_.bytesSent.fetch_add(wire.size());
            }
            recordStatus(parsed.errorStatus);
            if (config_.accessLog) {
                log(remoteAddress + " <malformed request> " + std::to_string(parsed.errorStatus) +
                    " " + parsed.errorMessage);
            }
            break;
        }

        // Complete request.
        HttpRequest& request = parsed.request;
        request.remoteAddress = remoteAddress;
        buffer.erase(0, parsed.consumedBytes);
        ++servedRequests;

        const bool keepAlive = shouldKeepAlive(request) &&
                               servedRequests < config_.maxRequestsPerConnection &&
                               running_.load();

        const auto startedAt = Clock::now();
        const HttpResponse response = dispatch(request);
        const std::string wire = response.serialize(keepAlive, request.method == "HEAD");
        const bool sent = sendAll(client, wire);
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(Clock::now() - startedAt).count();

        counters_.totalRequests.fetch_add(1);
        recordStatus(response.status);
        if (sent) {
            counters_.bytesSent.fetch_add(wire.size());
        }

        if (config_.accessLog) {
            char timing[32];
            std::snprintf(timing, sizeof(timing), "%.2fms", elapsedMs);
            log(remoteAddress + " \"" + request.method + " " + request.target + " " +
                request.version + "\" " + std::to_string(response.status) + " " +
                std::to_string(response.body.size()) + "B " + timing);
        }

        if (!sent || !keepAlive) {
            break;
        }
        deadline = Clock::now() + std::chrono::milliseconds(config_.keepAliveTimeoutMs);
    }

    closeSocket(client);
    counters_.activeConnections.fetch_sub(1);
}

HttpResponse HttpServer::dispatch(const HttpRequest& request) const {
    Handler handler;
    std::string allowed;
    {
        std::lock_guard<std::mutex> lock(routesMutex_);
        const auto pathIt = routes_.find(request.path);
        if (pathIt == routes_.end()) {
            return HttpResponse::error(404, "No route for " + request.path);
        }

        const auto& methods = pathIt->second;
        // HEAD is answered by the GET handler; the body is dropped on output.
        const std::string lookup = request.method == "HEAD" ? "GET" : request.method;
        const auto methodIt = methods.find(lookup);
        if (methodIt != methods.end()) {
            handler = methodIt->second;
        } else {
            for (const auto& entry : methods) {
                allowed += entry.first + ", ";
                if (entry.first == "GET") {
                    allowed += "HEAD, ";
                }
            }
            allowed += "OPTIONS";
        }
    }

    if (!handler) {
        if (request.method == "OPTIONS") {
            HttpResponse response;
            response.status = 204;
            response.setHeader("Allow", allowed);
            return response;
        }
        auto response = HttpResponse::error(
            405, "Method " + request.method + " is not allowed on " + request.path);
        response.setHeader("Allow", allowed);
        return response;
    }

    // A failing handler must never take down the worker thread.
    try {
        return handler(request);
    } catch (const std::exception& ex) {
        log("Handler for " + request.method + " " + request.path + " threw: " + ex.what());
    } catch (...) {
        log("Handler for " + request.method + " " + request.path + " threw an unknown exception");
    }
    return HttpResponse::error(500, "The request handler failed");
}

void HttpServer::recordStatus(int status) {
    if (status >= 500) {
        counters_.responses5xx.fetch_add(1);
    } else if (status >= 400) {
        counters_.responses4xx.fetch_add(1);
    } else if (status >= 300) {
        counters_.responses3xx.fetch_add(1);
    } else if (status >= 200) {
        counters_.responses2xx.fetch_add(1);
    }
}

}  // namespace lhs
