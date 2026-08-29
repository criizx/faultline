#include "faultline/control_server.hpp"
#include "log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace faultline
{
namespace
{

class Socket
{
  public:
    Socket() = default;
    explicit Socket(int descriptor) : descriptor_(descriptor)
    {
    }
    ~Socket()
    {
        reset();
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept : descriptor_(std::exchange(other.descriptor_, -1))
    {
    }
    Socket &operator=(Socket &&other) noexcept
    {
        if (this != &other)
            reset(std::exchange(other.descriptor_, -1));
        return *this;
    }
    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return descriptor_ >= 0;
    }
    void reset(int descriptor = -1) noexcept
    {
        if (descriptor_ >= 0)
            ::close(descriptor_);
        descriptor_ = descriptor;
    }

  private:
    int descriptor_{-1};
};

void set_nonblocking(int descriptor)
{
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("failed to set control socket nonblocking: " + std::string(std::strerror(errno)));
}

void set_blocking(int descriptor)
{
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) < 0)
        throw std::runtime_error("failed to set control client blocking: " + std::string(std::strerror(errno)));
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    (void)::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

Socket listen_tcp(const std::string &host, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo *raw = nullptr;
    const auto service = std::to_string(port);
    const char *node = (host == "0.0.0.0" || host == "::" || host == "*") ? nullptr : host.c_str();
    const int status = ::getaddrinfo(node, service.c_str(), &hints, &raw);
    if (status != 0)
        throw std::runtime_error("cannot resolve control address: " + std::string(::gai_strerror(status)));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(raw, ::freeaddrinfo);
    for (auto *current = addresses.get(); current != nullptr; current = current->ai_next)
    {
        Socket socket(::socket(current->ai_family, current->ai_socktype, current->ai_protocol));
        if (!socket)
            continue;
        const int enabled = 1;
        (void)::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (::bind(socket.get(), current->ai_addr, current->ai_addrlen) == 0 && ::listen(socket.get(), 32) == 0)
        {
            set_nonblocking(socket.get());
            return socket;
        }
    }
    throw std::runtime_error("cannot listen on control address " + host + ':' + service + ": " + std::strerror(errno));
}

int send_flags() noexcept
{
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void send_all(int descriptor, std::string_view response)
{
    std::size_t offset = 0;
    while (offset < response.size())
    {
        const auto sent = ::send(descriptor, response.data() + offset, response.size() - offset, send_flags());
        if (sent > 0)
        {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        break;
    }
}

std::string make_response(int status, std::string_view reason, std::string_view body)
{
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ' << reason
             << "\r\nContent-Type: application/json\r\nContent-Length: " << body.size()
             << "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *"
                "\r\nAccess-Control-Allow-Headers: Authorization, Content-Type, X-Faultline-Confirm"
                "\r\nAccess-Control-Allow-Methods: GET, PUT, POST, OPTIONS\r\nConnection: close\r\n\r\n"
             << body;
    return response.str();
}

struct ParsedRequest
{
    std::string method;
    std::string target;
    std::unordered_map<std::string, std::string> headers;
};

struct ControlWorker
{
    std::shared_ptr<std::atomic_bool> finished;
    std::jthread thread;
};

struct ControlLog
{
    std::string_view event;
    std::string_view target;
};

struct PolicyUpdate
{
    TrafficDirection direction;
    DirectionPolicy policy;
};

std::string json_escape(std::string_view value)
{
    std::ostringstream out;
    for (const char c : value)
    {
        switch (c)
        {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (const auto escaped = static_cast<unsigned char>(c); escaped < 0x20)
            {
                constexpr char hex[] = "0123456789abcdef";
                out << "\\u00" << hex[escaped >> 4] << hex[escaped & 0x0f];
            }
            else
            {
                out << c;
            }
        }
    }
    return out.str();
}

std::string trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
        value.remove_suffix(1);
    return std::string(value);
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool constant_time_equal(std::string_view left, std::string_view right)
{
    std::size_t difference = left.size() ^ right.size();
    const auto count = std::max(left.size(), right.size());
    for (std::size_t index = 0; index < count; ++index)
    {
        const unsigned char left_value = index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
        const unsigned char right_value = index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
        difference |= left_value ^ right_value;
    }
    return difference == 0;
}

std::uint32_t parse_u32(std::string_view value)
{
    std::uint32_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("invalid policy value");
    return result;
}

PolicyUpdate parse_policy_update(std::string_view target, const Scenario &scenario)
{
    const auto query_start = target.find('?');
    if (query_start == std::string_view::npos || query_start + 1 == target.size())
        throw std::runtime_error("policy query is required");
    const auto path = target.substr(0, query_start);
    PolicyUpdate update;
    if (path == "/v1/policies/upstream")
    {
        update.direction = TrafficDirection::Upstream;
        update.policy = scenario.upstream;
    }
    else if (path == "/v1/policies/downstream")
    {
        update.direction = TrafficDirection::Downstream;
        update.policy = scenario.downstream;
    }
    else
    {
        throw std::runtime_error("unknown policy direction");
    }

    bool latency_seen = false;
    bool jitter_seen = false;
    bool bandwidth_seen = false;
    auto query = target.substr(query_start + 1);
    while (!query.empty())
    {
        const auto separator = query.find('&');
        const auto pair = query.substr(0, separator);
        const auto equals = pair.find('=');
        if (equals == std::string_view::npos || equals == 0 || equals + 1 == pair.size())
            throw std::runtime_error("invalid policy query");
        const auto key = pair.substr(0, equals);
        const auto value = parse_u32(pair.substr(equals + 1));
        if (key == "latency_ms" && !latency_seen)
        {
            update.policy.latency_ms = value;
            latency_seen = true;
        }
        else if (key == "jitter_ms" && !jitter_seen)
        {
            update.policy.jitter_ms = value;
            jitter_seen = true;
        }
        else if (key == "bandwidth_kbps" && !bandwidth_seen)
        {
            update.policy.bandwidth_kbps = value;
            bandwidth_seen = true;
        }
        else
        {
            throw std::runtime_error("unknown or duplicate policy key");
        }
        if (separator == std::string_view::npos)
            break;
        query.remove_prefix(separator + 1);
    }
    return update;
}

ParsedRequest parse_request(std::string_view request)
{
    const auto line_end = request.find("\r\n");
    if (line_end == std::string_view::npos)
        throw std::runtime_error("invalid HTTP request line");
    std::istringstream line(std::string(request.substr(0, line_end)));
    std::string method;
    std::string target;
    std::string version;
    line >> method >> target >> version;
    if (method.empty() || target.empty() || version.rfind("HTTP/", 0) != 0)
        throw std::runtime_error("invalid HTTP request line");
    ParsedRequest parsed{std::move(method), std::move(target), {}};
    auto remaining = request.substr(line_end + 2);
    while (!remaining.empty())
    {
        const auto end = remaining.find("\r\n");
        if (end == std::string_view::npos)
            throw std::runtime_error("incomplete HTTP headers");
        const auto header = remaining.substr(0, end);
        remaining.remove_prefix(end + 2);
        if (header.empty())
            break;
        const auto colon = header.find(':');
        if (colon == std::string_view::npos || colon == 0)
            throw std::runtime_error("invalid HTTP header");
        auto name = lowercase(trim(header.substr(0, colon)));
        auto value = trim(header.substr(colon + 1));
        if (!parsed.headers.emplace(std::move(name), std::move(value)).second)
            throw std::runtime_error("duplicate HTTP header");
    }
    return parsed;
}

}

struct ControlServer::Impl
{
    ProxyServer &proxy;
    std::ostream &logs;
    std::atomic_bool stopping{false};
    Socket listener;
    std::vector<ControlWorker> workers;

    Impl(ProxyServer &proxy_server, std::ostream &log_stream) : proxy(proxy_server), logs(log_stream)
    {
    }

    void log(ControlLog record)
    {
        const auto experiment_id = proxy.scenario_snapshot().experiment_id;
        std::ostringstream line;
        line << "{\"event\":\"" << json_escape(record.event) << "\",\"experiment_id\":\"" << json_escape(experiment_id)
             << '"';
        if (!record.target.empty())
            line << ",\"target\":\"" << json_escape(record.target) << '"';
        line << '}';
        detail::write_log_line(logs, line.str());
    }

    bool authorized(const ParsedRequest &request) const
    {
        const auto token = proxy.scenario_snapshot().control_token;
        if (token.empty())
            return true;
        const auto header = request.headers.find("authorization");
        return header != request.headers.end() && constant_time_equal(header->second, "Bearer " + token);
    }

    bool confirmed(const ParsedRequest &request, std::string_view action) const
    {
        const auto header = request.headers.find("x-faultline-confirm");
        return header != request.headers.end() && header->second == action;
    }

    std::string route(const ParsedRequest &request)
    {
        if (request.method == "OPTIONS")
            return make_response(204, "No Content", {});
        if (request.target.rfind("/v1/", 0) == 0 && !authorized(request))
            return make_response(401, "Unauthorized", "{\"error\":\"authentication_required\"}");
        if (request.method == "POST" && request.target == "/v1/shutdown")
        {
            if (!confirmed(request, "shutdown"))
                return make_response(403, "Forbidden", "{\"error\":\"confirmation_required\"}");
            proxy.request_stop();
            return make_response(202, "Accepted", "{\"status\":\"stopping\"}");
        }
        if (request.method == "PUT" && request.target.rfind("/v1/policies/", 0) == 0)
        {
            if (!confirmed(request, "update"))
                return make_response(403, "Forbidden", "{\"error\":\"confirmation_required\"}");
            const auto update = parse_policy_update(request.target, proxy.scenario_snapshot());
            proxy.update_policy(update.direction, update.policy);
            return make_response(200, "OK", scenario_json(proxy.scenario_snapshot()));
        }
        if (request.method != "GET")
            return make_response(405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}");
        if (request.target == "/healthz")
            return make_response(200, "OK", "{\"status\":\"ok\"}");
        if (request.target == "/v1/metrics")
            return make_response(200, "OK", proxy.metrics().json());
        if (request.target == "/v1/scenario")
            return make_response(200, "OK", scenario_json(proxy.scenario_snapshot()));
        if (request.target == "/v1/lifecycle")
            return make_response(200, "OK", proxy.lifecycle_json());
        if (request.target == "/v1/state")
            return make_response(200, "OK",
                                 "{\"scenario\":" + scenario_json(proxy.scenario_snapshot()) + ",\"metrics\":" +
                                     proxy.metrics().json() + ",\"lifecycle\":" + proxy.lifecycle_json() + '}');
        return make_response(404, "Not Found", "{\"error\":\"not_found\"}");
    }

    void handle(Socket client)
    {
        try
        {
            set_blocking(client.get());
            timeval timeout{1, 0};
            (void)::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            (void)::setsockopt(client.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            std::array<char, 8192> buffer{};
            std::string request;
            while (request.size() < buffer.size())
            {
                const auto received = ::recv(client.get(), buffer.data(), buffer.size() - request.size(), 0);
                if (received <= 0)
                    break;
                request.append(buffer.data(), static_cast<std::size_t>(received));
                if (request.find("\r\n\r\n") != std::string::npos)
                    break;
            }
            if (request.find("\r\n\r\n") == std::string::npos)
                throw std::runtime_error("incomplete HTTP request");
            const auto parsed = parse_request(request);
            send_all(client.get(), route(parsed));
            log({"control_request", parsed.target});
        }
        catch (const std::exception &error)
        {
            log({"control_bad_request", error.what()});
            send_all(client.get(), make_response(400, "Bad Request", "{\"error\":\"bad_request\"}"));
        }
    }

    void run(const std::stop_token &stop_token)
    {
        const auto startup_scenario = proxy.scenario_snapshot();
        listener = listen_tcp(startup_scenario.control_host, startup_scenario.control_port);
        log({"control_started", {}});
        while (!stopping.load() && !stop_token.stop_requested())
        {
            pollfd descriptor{listener.get(), POLLIN, 0};
            const int status = ::poll(&descriptor, 1, 100);
            if (status < 0)
            {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("control poll failed: " + std::string(std::strerror(errno)));
            }
            if (status == 0 || (descriptor.revents & POLLIN) == 0)
                continue;
            for (;;)
            {
                Socket client(::accept(listener.get(), nullptr, nullptr));
                if (!client)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                        break;
                    throw std::runtime_error("control accept failed: " + std::string(std::strerror(errno)));
                }
                std::erase_if(workers, [](const ControlWorker &worker) { return worker.finished->load(); });
                if (workers.size() >= 32)
                {
                    set_blocking(client.get());
                    send_all(client.get(), make_response(503, "Service Unavailable", "{\"error\":\"busy\"}"));
                    continue;
                }
                auto finished = std::make_shared<std::atomic_bool>(false);
                workers.push_back(
                    ControlWorker{finished, std::jthread([this, client = std::move(client), finished]() mutable {
                                      handle(std::move(client));
                                      finished->store(true);
                                  })});
            }
        }
        listener.reset();
        for (auto &worker : workers)
            worker.thread.request_stop();
        workers.clear();
        log({"control_stopped", {}});
    }
};

ControlServer::ControlServer(ProxyServer &proxy, std::ostream &log_stream)
    : impl_(std::make_shared<Impl>(proxy, log_stream))
{
}

ControlServer::~ControlServer()
{
    request_stop();
    impl_.reset();
}

void ControlServer::run(const std::stop_token &stop_token)
{
    const auto impl = impl_;
    impl->run(stop_token);
}

void ControlServer::request_stop() noexcept
{
    if (const auto impl = impl_)
        impl->stopping.store(true);
}

}
