// Command line entry point for lightweight-http-server.
//
// Starts the HTTP server and registers a small demo REST API:
//   GET  /                -> plain text index
//   GET  /api/status      -> service health and runtime information
//   GET  /api/telemetry   -> server counters plus aggregated client metrics
//   POST /api/telemetry   -> records a metric sample (JSON or form encoded)
//   POST /api/echo        -> echoes the request back as JSON

#include "HttpServer.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* kServiceName = "lightweight-http-server";
constexpr const char* kVersion = "1.0.0";

std::atomic<lhs::HttpServer*> g_server{nullptr};

extern "C" void handleSignal(int /*signal*/) {
    if (lhs::HttpServer* server = g_server.load()) {
        server->stop();
    }
}

// ---------------------------------------------------------------------------
// Command line parsing
// ---------------------------------------------------------------------------

struct CliOptions {
    lhs::ServerConfig config;
    bool showHelp = false;
    bool showVersion = false;
};

void printUsage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n\n"
        << "Options:\n"
        << "  -H, --host <ipv4>         Address to bind (default: 0.0.0.0)\n"
        << "  -p, --port <number>       TCP port to listen on (default: 8080)\n"
        << "  -t, --threads <number>    Worker thread count (default: CPU cores)\n"
        << "      --max-body <bytes>    Maximum request body size (default: 1048576)\n"
        << "      --keep-alive <ms>     Idle keep-alive timeout (default: 5000)\n"
        << "  -q, --quiet               Disable the access log\n"
        << "  -v, --version             Print version and exit\n"
        << "  -h, --help                Print this help and exit\n";
}

unsigned long long parseNumber(const std::string& option, const std::string& text,
                               unsigned long long min, unsigned long long max) {
    std::size_t consumed = 0;
    unsigned long long value = 0;
    try {
        if (text.empty() || !std::isdigit(static_cast<unsigned char>(text.front()))) {
            throw std::invalid_argument("not a number");
        }
        value = std::stoull(text, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument("Option " + option + " expects a number, got '" + text + "'");
    }
    if (consumed != text.size() || value < min || value > max) {
        throw std::invalid_argument("Option " + option + " must be between " +
                                    std::to_string(min) + " and " + std::to_string(max));
    }
    return value;
}

CliOptions parseArguments(int argc, char** argv) {
    CliOptions options;
    const unsigned cores = std::thread::hardware_concurrency();
    options.config.workerThreads = cores == 0 ? 4 : cores;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto requireValue = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("Option " + arg + " requires a value");
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
        } else if (arg == "-v" || arg == "--version") {
            options.showVersion = true;
        } else if (arg == "-q" || arg == "--quiet") {
            options.config.accessLog = false;
        } else if (arg == "-H" || arg == "--host") {
            options.config.host = requireValue();
        } else if (arg == "-p" || arg == "--port") {
            options.config.port = static_cast<std::uint16_t>(parseNumber(arg, requireValue(), 1, 65535));
        } else if (arg == "-t" || arg == "--threads") {
            options.config.workerThreads = static_cast<std::size_t>(parseNumber(arg, requireValue(), 1, 512));
        } else if (arg == "--max-body") {
            options.config.maxBodyBytes =
                static_cast<std::size_t>(parseNumber(arg, requireValue(), 0, 64ULL * 1024 * 1024));
        } else if (arg == "--keep-alive") {
            options.config.keepAliveTimeoutMs =
                static_cast<int>(parseNumber(arg, requireValue(), 100, 120000));
        } else {
            throw std::invalid_argument("Unknown option: " + arg);
        }
    }
    return options;
}

// ---------------------------------------------------------------------------
// Minimal flat JSON object reader used by POST /api/telemetry.
// Accepts {"key": "string" | number | true | false | null, ...} only.
// ---------------------------------------------------------------------------

class FlatJsonReader {
public:
    explicit FlatJsonReader(std::string_view text) : text_(text) {}

    std::optional<std::map<std::string, std::string>> read() {
        std::map<std::string, std::string> result;
        skipWhitespace();
        if (!consume('{')) {
            return std::nullopt;
        }
        skipWhitespace();
        if (consume('}')) {
            return finish(result);
        }
        for (;;) {
            skipWhitespace();
            auto key = readString();
            if (!key) {
                return std::nullopt;
            }
            skipWhitespace();
            if (!consume(':')) {
                return std::nullopt;
            }
            skipWhitespace();
            auto value = peek() == '"' ? readString() : readLiteral();
            if (!value) {
                return std::nullopt;
            }
            result[*key] = *value;
            skipWhitespace();
            if (consume(',')) {
                continue;
            }
            if (consume('}')) {
                return finish(result);
            }
            return std::nullopt;
        }
    }

private:
    std::optional<std::map<std::string, std::string>> finish(
        std::map<std::string, std::string>& result) {
        skipWhitespace();
        if (pos_ != text_.size()) {
            return std::nullopt;  // Trailing garbage after the object.
        }
        return std::move(result);
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    bool consume(char expected) {
        if (peek() == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void skipWhitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    std::optional<std::string> readString() {
        if (!consume('"')) {
            return std::nullopt;
        }
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return std::nullopt;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                return std::nullopt;
            }
            switch (text_[pos_++]) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: return std::nullopt;  // \uXXXX is not needed here.
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> readLiteral() {
        const std::size_t start = pos_;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
                ++pos_;
            } else {
                break;
            }
        }
        if (pos_ == start) {
            return std::nullopt;
        }
        return std::string(text_.substr(start, pos_ - start));
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// In-memory telemetry aggregation
// ---------------------------------------------------------------------------

class TelemetryStore {
public:
    static constexpr std::size_t kMaxMetrics = 64;

    struct Summary {
        std::uint64_t count = 0;
        double min = std::numeric_limits<double>::infinity();
        double max = -std::numeric_limits<double>::infinity();
        double sum = 0.0;
        double last = 0.0;
    };

    // Returns false when the metric limit is reached for a new metric name.
    bool record(const std::string& metric, double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = metrics_.find(metric);
        if (it == metrics_.end()) {
            if (metrics_.size() >= kMaxMetrics) {
                return false;
            }
            it = metrics_.emplace(metric, Summary{}).first;
        }
        Summary& summary = it->second;
        ++summary.count;
        summary.min = std::min(summary.min, value);
        summary.max = std::max(summary.max, value);
        summary.sum += value;
        summary.last = value;
        return true;
    }

    std::map<std::string, Summary> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return metrics_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, Summary> metrics_;
};

bool isValidMetricName(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

std::optional<double> parseFiniteDouble(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::int64_t unixTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void registerRoutes(lhs::HttpServer& server, TelemetryStore& telemetry) {
    server.get("/", [](const lhs::HttpRequest&) {
        return lhs::HttpResponse::text(
            std::string(kServiceName) + " " + kVersion + "\n\n" +
            "Available endpoints:\n"
            "  GET  /api/status\n"
            "  GET  /api/telemetry\n"
            "  POST /api/telemetry   {\"metric\": \"cpu_temp\", \"value\": 61.5}\n"
            "  POST /api/echo\n");
    });

    server.get("/api/status", [&server](const lhs::HttpRequest&) {
        const lhs::ServerStats stats = server.stats();
        lhs::JsonObject body;
        body.add("status", "ok")
            .add("service", kServiceName)
            .add("version", kVersion)
            .add("uptimeSeconds", stats.uptimeSeconds)
            .add("workerThreads", server.config().workerThreads)
            .add("activeConnections", stats.activeConnections)
            .add("timestamp", unixTimestamp());
        return lhs::HttpResponse::json(body).setHeader("Cache-Control", "no-store");
    });

    server.get("/api/telemetry", [&server, &telemetry](const lhs::HttpRequest&) {
        const lhs::ServerStats stats = server.stats();

        lhs::JsonObject responses;
        responses.add("2xx", stats.responses2xx)
            .add("3xx", stats.responses3xx)
            .add("4xx", stats.responses4xx)
            .add("5xx", stats.responses5xx);

        lhs::JsonObject serverSection;
        serverSection.add("uptimeSeconds", stats.uptimeSeconds)
            .add("totalConnections", stats.totalConnections)
            .add("activeConnections", stats.activeConnections)
            .add("rejectedConnections", stats.rejectedConnections)
            .add("totalRequests", stats.totalRequests)
            .add("bytesReceived", stats.bytesReceived)
            .add("bytesSent", stats.bytesSent)
            .add("responses", responses);

        lhs::JsonObject metrics;
        for (const auto& [name, summary] : telemetry.snapshot()) {
            lhs::JsonObject entry;
            entry.add("count", summary.count)
                .add("min", summary.min)
                .add("max", summary.max)
                .add("avg", summary.sum / static_cast<double>(summary.count))
                .add("last", summary.last);
            metrics.add(name, entry);
        }

        lhs::JsonObject body;
        body.add("server", serverSection).add("metrics", metrics).add("timestamp", unixTimestamp());
        return lhs::HttpResponse::json(body).setHeader("Cache-Control", "no-store");
    });

    server.post("/api/telemetry", [&telemetry](const lhs::HttpRequest& request) {
        // Accept JSON, form-encoded bodies or plain query parameters.
        std::map<std::string, std::string> fields = request.query;
        const std::string contentType = request.header("content-type").value_or("");

        if (!request.body.empty()) {
            if (contentType.rfind("application/json", 0) == 0) {
                auto parsed = FlatJsonReader(request.body).read();
                if (!parsed) {
                    return lhs::HttpResponse::error(400, "Body is not a flat JSON object");
                }
                fields = std::move(*parsed);
            } else if (contentType.rfind("application/x-www-form-urlencoded", 0) == 0) {
                fields = lhs::parseQueryString(request.body);
            } else {
                return lhs::HttpResponse::error(
                    415, "Use application/json or application/x-www-form-urlencoded");
            }
        }

        const auto metricIt = fields.find("metric");
        const auto valueIt = fields.find("value");
        if (metricIt == fields.end() || valueIt == fields.end()) {
            return lhs::HttpResponse::error(400, "Both 'metric' and 'value' fields are required");
        }
        if (!isValidMetricName(metricIt->second)) {
            return lhs::HttpResponse::error(
                422, "Metric name must be 1-64 characters of [A-Za-z0-9_.-]");
        }
        const auto value = parseFiniteDouble(valueIt->second);
        if (!value) {
            return lhs::HttpResponse::error(422, "Value must be a finite number");
        }
        if (!telemetry.record(metricIt->second, *value)) {
            return lhs::HttpResponse::error(409, "Metric limit reached, no new metric names accepted");
        }

        lhs::JsonObject recorded;
        recorded.add("metric", metricIt->second).add("value", *value);
        lhs::JsonObject body;
        body.add("recorded", recorded).add("timestamp", unixTimestamp());
        return lhs::HttpResponse::json(body, 201);
    });

    server.post("/api/echo", [](const lhs::HttpRequest& request) {
        lhs::JsonObject headers;
        for (const auto& [name, value] : request.headers) {
            headers.add(name, value);
        }
        lhs::JsonObject query;
        for (const auto& [name, value] : request.query) {
            query.add(name, value);
        }
        lhs::JsonObject body;
        body.add("method", request.method)
            .add("path", request.path)
            .add("version", request.version)
            .add("remoteAddress", request.remoteAddress)
            .add("query", query)
            .add("headers", headers)
            .add("body", request.body);
        return lhs::HttpResponse::json(body);
    });
}

}  // namespace

int main(int argc, char** argv) {
    CliOptions options;
    try {
        options = parseArguments(argc, argv);
    } catch (const std::invalid_argument& ex) {
        std::cerr << "error: " << ex.what() << "\n\n";
        printUsage(argv[0]);
        return 2;
    }

    if (options.showHelp) {
        printUsage(argv[0]);
        return 0;
    }
    if (options.showVersion) {
        std::cout << kServiceName << " " << kVersion << '\n';
        return 0;
    }

    try {
        lhs::HttpServer server(options.config);
        TelemetryStore telemetry;
        registerRoutes(server, telemetry);

        g_server.store(&server);
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        server.log("Press Ctrl+C to stop");
        server.run();

        g_server.store(nullptr);
    } catch (const std::exception& ex) {
        g_server.store(nullptr);
        std::cerr << "fatal: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
