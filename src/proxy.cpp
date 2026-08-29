#include "faultline/proxy.hpp"
#include "log.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace faultline
{
namespace
{

using Clock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;

enum class RunStatus : std::uint8_t
{
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed
};

std::string_view status_name(RunStatus status) noexcept
{
    switch (status)
    {
    case RunStatus::Created:
        return "created";
    case RunStatus::Starting:
        return "starting";
    case RunStatus::Running:
        return "running";
    case RunStatus::Stopping:
        return "stopping";
    case RunStatus::Stopped:
        return "stopped";
    case RunStatus::Failed:
        return "failed";
    }
    return "failed";
}

std::uint64_t unix_milliseconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SystemClock::now().time_since_epoch()).count());
}

std::string make_run_id()
{
    std::random_device source;
    std::mt19937_64 random(source());
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << random() << std::setw(16) << random();
    return out.str();
}

class Socket
{
  public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd)
    {
    }
    ~Socket()
    {
        reset();
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept : fd_(std::exchange(other.fd_, -1))
    {
    }
    Socket &operator=(Socket &&other) noexcept
    {
        if (this != &other)
            reset(std::exchange(other.fd_, -1));
        return *this;
    }
    [[nodiscard]] int get() const noexcept
    {
        return fd_;
    }
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return fd_ >= 0;
    }
    int release() noexcept
    {
        return std::exchange(fd_, -1);
    }
    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

  private:
    int fd_{-1};
};

void set_nonblocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        throw std::runtime_error("failed to set socket nonblocking: " + std::string(std::strerror(errno)));
    }
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

int send_flags() noexcept
{
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

struct ResolvedAddress
{
    sockaddr_storage address{};
    socklen_t size{};
    int family{};
    int socket_type{};
    int protocol{};
};

std::vector<ResolvedAddress> resolve_tcp(const std::string &host, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *raw = nullptr;
    const auto service = std::to_string(port);
    const int status = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &raw);
    if (status != 0)
        throw std::runtime_error("cannot resolve upstream '" + host + "': " + ::gai_strerror(status));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(raw, ::freeaddrinfo);
    std::vector<ResolvedAddress> result;
    for (auto *current = addresses.get(); current != nullptr; current = current->ai_next)
    {
        if (current->ai_addrlen > sizeof(sockaddr_storage))
            continue;
        ResolvedAddress resolved;
        std::memcpy(&resolved.address, current->ai_addr, current->ai_addrlen);
        resolved.size = current->ai_addrlen;
        resolved.family = current->ai_family;
        resolved.socket_type = current->ai_socktype;
        resolved.protocol = current->ai_protocol;
        result.push_back(resolved);
    }
    if (result.empty())
        throw std::runtime_error("upstream '" + host + "' has no usable TCP addresses");
    return result;
}

Socket connect_tcp(const std::vector<ResolvedAddress> &addresses, const Scenario &scenario,
                   const std::atomic_bool &stopping, const std::stop_token &token)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(scenario.connect_timeout_ms);
    for (const auto &address : addresses)
    {
        if (stopping.load() || token.stop_requested())
            return {};
        Socket socket(::socket(address.family, address.socket_type, address.protocol));
        if (!socket)
            continue;
        set_nonblocking(socket.get());
        if (::connect(socket.get(), reinterpret_cast<const sockaddr *>(&address.address), address.size) == 0)
            return socket;
        if (errno != EINPROGRESS)
            continue;
        while (Clock::now() < deadline && !stopping.load() && !token.stop_requested())
        {
            pollfd descriptor{socket.get(), POLLOUT, 0};
            const int status = ::poll(&descriptor, 1, 50);
            if (status < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (status == 0)
                continue;
            int error = 0;
            socklen_t error_size = sizeof(error);
            if (::getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &error_size) == 0 && error == 0)
                return socket;
            break;
        }
    }
    if (stopping.load() || token.stop_requested())
        return {};
    throw std::runtime_error("cannot connect to upstream " + scenario.upstream_host + ':' +
                             std::to_string(scenario.upstream_port));
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
        throw std::runtime_error("cannot resolve listen address: " + std::string(::gai_strerror(status)));
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(raw, ::freeaddrinfo);
    for (auto *current = addresses.get(); current != nullptr; current = current->ai_next)
    {
        Socket socket(::socket(current->ai_family, current->ai_socktype, current->ai_protocol));
        if (!socket)
            continue;
        const int enabled = 1;
        (void)::setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (::bind(socket.get(), current->ai_addr, current->ai_addrlen) == 0 && ::listen(socket.get(), 128) == 0)
        {
            set_nonblocking(socket.get());
            return socket;
        }
    }
    throw std::runtime_error("cannot listen on " + host + ':' + service + ": " + std::strerror(errno));
}

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

struct PendingChunk
{
    std::vector<std::byte> bytes;
    std::size_t offset{};
    Clock::time_point ready_at;
};

struct PipeState
{
    int source{-1};
    int destination{-1};
    DirectionPolicy policy;
    std::deque<PendingChunk> queue;
    std::size_t queued_bytes{};
    bool input_closed{};
    bool output_shutdown{};
    double tokens{};
    Clock::time_point token_updated{Clock::now()};
};

struct SessionWorker
{
    std::shared_ptr<std::atomic_bool> finished;
    std::jthread thread;
};

struct RuntimeSnapshot
{
    DirectionPolicy upstream;
    DirectionPolicy downstream;
    std::uint32_t idle_timeout_ms;
    std::uint32_t blackout_after_ms;
    std::uint32_t blackout_duration_ms;
    double reset_probability;
    std::size_t stage_index{};
    std::string stage_name;
    Clock::time_point stage_started;
};

constexpr std::size_t k_read_buffer_size = std::size_t{16} * 1024;
constexpr std::size_t k_max_queued_bytes = std::size_t{4} * 1024 * 1024;

}

MetricsSnapshot Metrics::snapshot() const noexcept
{
    return {accepted_connections_.load(), active_connections_.load(),    completed_connections_.load(),
            reset_connections_.load(),    timed_out_connections_.load(), upstream_bytes_.load(),
            downstream_bytes_.load(),     delayed_chunks_.load(),        throttled_writes_.load(),
            policy_updates_.load(),       stage_transitions_.load()};
}

std::string Metrics::json() const
{
    const auto s = snapshot();
    std::ostringstream out;
    out << "{\"accepted_connections\":" << s.accepted_connections << ",\"active_connections\":" << s.active_connections
        << ",\"completed_connections\":" << s.completed_connections << ",\"reset_connections\":" << s.reset_connections
        << ",\"timed_out_connections\":" << s.timed_out_connections << ",\"upstream_bytes\":" << s.upstream_bytes
        << ",\"downstream_bytes\":" << s.downstream_bytes << ",\"delayed_chunks\":" << s.delayed_chunks
        << ",\"throttled_writes\":" << s.throttled_writes << ",\"policy_updates\":" << s.policy_updates
        << ",\"stage_transitions\":" << s.stage_transitions << '}';
    return out.str();
}

struct ProxyServer::Impl
{
    Scenario scenario;
    std::ostream &logs;
    std::shared_ptr<Metrics> metrics;
    mutable std::mutex scenario_mutex;
    std::atomic_bool stopping{false};
    Socket listener;
    std::vector<ResolvedAddress> upstream_addresses;
    std::vector<SessionWorker> sessions;
    mutable std::mutex lifecycle_mutex;
    std::string run_id{make_run_id()};
    std::atomic<RunStatus> lifecycle_status{RunStatus::Created};
    std::uint64_t started_at_unix_ms{};
    Clock::time_point started_at;

    Impl(Scenario scenario_value, std::ostream &output, std::shared_ptr<Metrics> metrics_value)
        : scenario(std::move(scenario_value)), logs(output), metrics(std::move(metrics_value))
    {
    }

    [[nodiscard]] Scenario scenario_snapshot() const
    {
        std::lock_guard lock(scenario_mutex);
        return scenario;
    }

    [[nodiscard]] LifecycleSnapshot lifecycle_snapshot() const
    {
        const auto current_scenario = scenario_snapshot();
        std::lock_guard lock(lifecycle_mutex);
        const auto uptime =
            started_at_unix_ms == 0
                ? 0
                : static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count());
        return {current_scenario.experiment_id,
                run_id,
                current_scenario.name,
                std::string(status_name(lifecycle_status.load())),
                started_at_unix_ms,
                uptime,
                current_scenario.stages.size()};
    }

    void set_status(RunStatus value) noexcept
    {
        lifecycle_status.store(value);
    }

    [[nodiscard]] RuntimeSnapshot runtime_snapshot(const Scenario &connection_scenario,
                                                   Clock::time_point connection_started, Clock::time_point now) const
    {
        Scenario current;
        std::lock_guard lock(scenario_mutex);
        current = scenario;
        if (current.stages.empty())
            return {current.upstream,
                    current.downstream,
                    current.idle_timeout_ms,
                    current.blackout_after_ms,
                    current.blackout_duration_ms,
                    current.reset_probability,
                    0,
                    connection_scenario.name,
                    connection_started};
        auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - connection_started).count());
        auto stage_started = connection_started;
        std::size_t stage_index = 0;
        for (; stage_index + 1 < current.stages.size(); ++stage_index)
        {
            const auto duration = current.stages[stage_index].duration_ms;
            if (elapsed < duration)
                break;
            elapsed -= duration;
            stage_started += std::chrono::milliseconds(duration);
        }
        const auto &stage = current.stages[stage_index];
        return {stage.upstream,
                stage.downstream,
                stage.idle_timeout_ms,
                stage.blackout_after_ms,
                stage.blackout_duration_ms,
                stage.reset_probability,
                stage_index,
                stage.name,
                stage_started};
    }

    void update_policy(TrafficDirection direction, DirectionPolicy policy)
    {
        std::lock_guard lock(scenario_mutex);
        auto updated = scenario;
        if (direction == TrafficDirection::Upstream)
        {
            updated.upstream = policy;
            for (auto &stage : updated.stages)
                stage.upstream = policy;
        }
        else
        {
            updated.downstream = policy;
            for (auto &stage : updated.stages)
                stage.downstream = policy;
        }
        validate(updated);
        scenario = std::move(updated);
        metrics->policy_updates_.fetch_add(1);
    }

    void log(std::string_view event, std::uint64_t connection_id, std::string_view detail_text = {})
    {
        std::string experiment_id;
        {
            std::lock_guard lock(scenario_mutex);
            experiment_id = scenario.experiment_id;
        }
        std::ostringstream line;
        line << "{\"event\":\"" << json_escape(event) << "\",\"experiment_id\":\"" << json_escape(experiment_id)
             << "\",\"connection_id\":" << connection_id;
        if (!detail_text.empty())
            line << ",\"detail\":\"" << json_escape(detail_text) << '"';
        line << '}';
        detail::write_log_line(logs, line.str());
    }

    [[nodiscard]] bool blackout_active(const RuntimeSnapshot &runtime, Clock::time_point now) const
    {
        if (runtime.blackout_duration_ms == 0)
            return false;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - runtime.stage_started).count();
        return elapsed >= runtime.blackout_after_ms &&
               elapsed < static_cast<std::int64_t>(runtime.blackout_after_ms) + runtime.blackout_duration_ms;
    }

    void enqueue(PipeState &pipe, std::span<const std::byte> bytes, Clock::time_point now,
                 std::mt19937_64 &random) const
    {
        std::int64_t jitter = 0;
        if (pipe.policy.jitter_ms != 0)
        {
            const auto bound = static_cast<std::int64_t>(pipe.policy.jitter_ms);
            jitter = std::uniform_int_distribution<std::int64_t>(-bound, bound)(random);
        }
        const auto delay = std::max<std::int64_t>(0, static_cast<std::int64_t>(pipe.policy.latency_ms) + jitter);
        PendingChunk chunk;
        chunk.bytes.assign(bytes.begin(), bytes.end());
        chunk.ready_at = now + std::chrono::milliseconds(delay);
        pipe.queued_bytes += chunk.bytes.size();
        pipe.queue.push_back(std::move(chunk));
        if (delay > 0)
            metrics->delayed_chunks_.fetch_add(1);
    }

    void read_pipe(PipeState &pipe, Clock::time_point now, std::mt19937_64 &random,
                   Clock::time_point &last_activity) const
    {
        std::array<std::byte, k_read_buffer_size> buffer{};
        const auto received = ::recv(pipe.source, buffer.data(), buffer.size(), 0);
        if (received > 0)
        {
            const auto count = static_cast<std::size_t>(received);
            enqueue(pipe, std::span(buffer.data(), count), now, random);
            last_activity = now;
        }
        else if (received == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
        {
            pipe.input_closed = true;
        }
    }

    void replenish_tokens(PipeState &pipe, Clock::time_point now)
    {
        if (pipe.policy.bandwidth_kbps == 0)
            return;
        const double bytes_per_second = static_cast<double>(pipe.policy.bandwidth_kbps) * 1000.0 / 8.0;
        const double elapsed = std::chrono::duration<double>(now - pipe.token_updated).count();
        const double burst = std::max(4096.0, bytes_per_second * 0.1);
        pipe.tokens = std::min(burst, pipe.tokens + elapsed * bytes_per_second);
        pipe.token_updated = now;
    }

    bool can_write(PipeState &pipe, Clock::time_point now)
    {
        if (pipe.policy.bandwidth_kbps == 0)
            return true;
        replenish_tokens(pipe, now);
        if (pipe.tokens >= 1.0)
            return true;
        metrics->throttled_writes_.fetch_add(1);
        return false;
    }

    std::size_t bandwidth_allowance(PipeState &pipe, Clock::time_point now, std::size_t requested)
    {
        if (pipe.policy.bandwidth_kbps == 0)
            return requested;
        replenish_tokens(pipe, now);
        const auto allowed =
            static_cast<std::size_t>(std::max(0.0, std::min(pipe.tokens, static_cast<double>(requested))));
        return allowed;
    }

    void write_pipe(PipeState &pipe, Clock::time_point now, bool is_upstream, Clock::time_point &last_activity)
    {
        if (pipe.queue.empty() || pipe.queue.front().ready_at > now)
            return;
        auto &chunk = pipe.queue.front();
        const auto remaining = chunk.bytes.size() - chunk.offset;
        const auto allowed = bandwidth_allowance(pipe, now, remaining);
        if (allowed == 0)
            return;
        const auto sent = ::send(pipe.destination, chunk.bytes.data() + chunk.offset, allowed, send_flags());
        if (sent > 0)
        {
            const auto count = static_cast<std::size_t>(sent);
            chunk.offset += count;
            pipe.queued_bytes -= count;
            pipe.tokens = std::max(0.0, pipe.tokens - static_cast<double>(count));
            if (is_upstream)
                metrics->upstream_bytes_.fetch_add(count);
            else
                metrics->downstream_bytes_.fetch_add(count);
            last_activity = now;
            if (chunk.offset == chunk.bytes.size())
                pipe.queue.pop_front();
        }
        else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            pipe.input_closed = true;
            pipe.queue.clear();
            pipe.queued_bytes = 0;
        }
    }

    void maybe_shutdown(PipeState &pipe)
    {
        if (pipe.input_closed && pipe.queue.empty() && !pipe.output_shutdown)
        {
            (void)::shutdown(pipe.destination, SHUT_WR);
            pipe.output_shutdown = true;
        }
    }

    void force_reset(int fd) noexcept
    {
        linger immediate{1, 0};
        (void)::setsockopt(fd, SOL_SOCKET, SO_LINGER, &immediate, sizeof(immediate));
    }

    void run_session(Socket client, std::uint64_t connection_id, const std::stop_token &token)
    {
        metrics->active_connections_.fetch_add(1);
        struct ActiveGuard
        {
            Metrics &metrics;
            ~ActiveGuard()
            {
                metrics.active_connections_.fetch_sub(1);
                metrics.completed_connections_.fetch_add(1);
            }
        } guard{*metrics};

        try
        {
            const auto connection_scenario = scenario_snapshot();
            std::mt19937_64 random(connection_scenario.seed ^ (connection_id * 0x9e3779b97f4a7c15ULL));
            const auto initial_reset_probability = connection_scenario.stages.empty()
                                                       ? connection_scenario.reset_probability
                                                       : connection_scenario.stages.front().reset_probability;
            if (std::bernoulli_distribution(initial_reset_probability)(random))
            {
                force_reset(client.get());
                metrics->reset_connections_.fetch_add(1);
                log("connection_reset", connection_id, "scenario probability matched");
                return;
            }
            Socket upstream = connect_tcp(upstream_addresses, connection_scenario, stopping, token);
            if (!upstream)
                return;
            set_nonblocking(client.get());

            const auto started = Clock::now();
            const auto initial_runtime = runtime_snapshot(connection_scenario, started, started);
            PipeState to_upstream;
            to_upstream.source = client.get();
            to_upstream.destination = upstream.get();
            to_upstream.policy = initial_runtime.upstream;
            PipeState to_downstream;
            to_downstream.source = upstream.get();
            to_downstream.destination = client.get();
            to_downstream.policy = initial_runtime.downstream;
            auto last_activity = started;
            std::size_t last_stage_index = initial_runtime.stage_index;
            log("connection_open", connection_id, initial_runtime.stage_name);

            while (!stopping.load() && !token.stop_requested())
            {
                const auto now = Clock::now();
                const auto runtime = runtime_snapshot(connection_scenario, started, now);
                if (runtime.stage_index != last_stage_index)
                {
                    last_stage_index = runtime.stage_index;
                    metrics->stage_transitions_.fetch_add(1);
                    log("stage_transition", connection_id, runtime.stage_name);
                    if (std::bernoulli_distribution(runtime.reset_probability)(random))
                    {
                        force_reset(client.get());
                        metrics->reset_connections_.fetch_add(1);
                        log("connection_reset", connection_id, "stage probability matched");
                        return;
                    }
                }
                to_upstream.policy = runtime.upstream;
                to_downstream.policy = runtime.downstream;
                const bool blackout = blackout_active(runtime, now);
                const bool delayed = (!to_upstream.queue.empty() && to_upstream.queue.front().ready_at > now) ||
                                     (!to_downstream.queue.empty() && to_downstream.queue.front().ready_at > now);
                if (blackout || delayed)
                    last_activity = now;
                if (now - last_activity >= std::chrono::milliseconds(runtime.idle_timeout_ms))
                {
                    metrics->timed_out_connections_.fetch_add(1);
                    log("connection_timeout", connection_id);
                    break;
                }
                if (to_upstream.output_shutdown && to_downstream.output_shutdown)
                    break;

                std::array<pollfd, 2> fds{{{client.get(), 0, 0}, {upstream.get(), 0, 0}}};
                if (!to_upstream.input_closed && to_upstream.queued_bytes < k_max_queued_bytes)
                    fds[0].events |= POLLIN;
                if (!to_downstream.input_closed && to_downstream.queued_bytes < k_max_queued_bytes)
                    fds[1].events |= POLLIN;
                if (!blackout && !to_downstream.queue.empty() && to_downstream.queue.front().ready_at <= now &&
                    can_write(to_downstream, now))
                    fds[0].events |= POLLOUT;
                if (!blackout && !to_upstream.queue.empty() && to_upstream.queue.front().ready_at <= now &&
                    can_write(to_upstream, now))
                    fds[1].events |= POLLOUT;

                const int status = ::poll(fds.data(), fds.size(), 10);
                if (status < 0 && errno != EINTR)
                    throw std::runtime_error("session poll failed: " + std::string(std::strerror(errno)));
                const auto after_poll = Clock::now();
                const auto current_runtime = runtime_snapshot(connection_scenario, started, after_poll);
                to_upstream.policy = current_runtime.upstream;
                to_downstream.policy = current_runtime.downstream;
                if (!to_upstream.input_closed && (fds[0].revents & (POLLIN | POLLHUP)) != 0)
                    read_pipe(to_upstream, after_poll, random, last_activity);
                if (!to_downstream.input_closed && (fds[1].revents & (POLLIN | POLLHUP)) != 0)
                    read_pipe(to_downstream, after_poll, random, last_activity);
                if (!blackout_active(current_runtime, after_poll))
                {
                    if ((fds[1].revents & POLLOUT) != 0)
                        write_pipe(to_upstream, after_poll, true, last_activity);
                    if ((fds[0].revents & POLLOUT) != 0)
                        write_pipe(to_downstream, after_poll, false, last_activity);
                }
                if ((fds[0].revents & (POLLERR | POLLNVAL)) != 0)
                    to_upstream.input_closed = true;
                if ((fds[1].revents & (POLLERR | POLLNVAL)) != 0)
                    to_downstream.input_closed = true;
                maybe_shutdown(to_upstream);
                maybe_shutdown(to_downstream);
            }
            log("connection_close", connection_id);
        }
        catch (const std::exception &error)
        {
            log("connection_error", connection_id, error.what());
        }
    }

    void run(const std::stop_token &token)
    {
        {
            std::lock_guard lock(lifecycle_mutex);
            lifecycle_status.store(RunStatus::Starting);
            started_at_unix_ms = unix_milliseconds();
            started_at = Clock::now();
        }
        const auto startup_scenario = scenario_snapshot();
        try
        {
            upstream_addresses = resolve_tcp(startup_scenario.upstream_host, startup_scenario.upstream_port);
            listener = listen_tcp(startup_scenario.listen_host, startup_scenario.listen_port);
        }
        catch (...)
        {
            set_status(RunStatus::Failed);
            throw;
        }
        set_status(RunStatus::Running);
        log("proxy_started", 0, startup_scenario.name);
        std::uint64_t next_id = 1;
        while (!stopping.load() && !token.stop_requested())
        {
            pollfd fd{listener.get(), POLLIN, 0};
            const int status = ::poll(&fd, 1, 100);
            if (status < 0)
            {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("accept poll failed: " + std::string(std::strerror(errno)));
            }
            if (status == 0 || (fd.revents & POLLIN) == 0)
                continue;
            for (;;)
            {
                const int accepted = ::accept(listener.get(), nullptr, nullptr);
                if (accepted < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                        break;
                    throw std::runtime_error("accept failed: " + std::string(std::strerror(errno)));
                }
                std::erase_if(sessions, [](const SessionWorker &session) { return session.finished->load(); });
                if (sessions.size() >= startup_scenario.max_connections)
                {
                    force_reset(accepted);
                    ::close(accepted);
                    log("connection_rejected", 0, "connection limit reached");
                    continue;
                }
                metrics->accepted_connections_.fetch_add(1);
                const auto id = next_id++;
                auto finished = std::make_shared<std::atomic_bool>(false);
                sessions.push_back(
                    SessionWorker{finished, std::jthread([this, client = Socket(accepted), id,
                                                          finished](const std::stop_token &session_token) mutable {
                                      run_session(std::move(client), id, session_token);
                                      finished->store(true);
                                  })});
            }
        }
        listener.reset();
        for (auto &session : sessions)
            session.thread.request_stop();
        sessions.clear();
        set_status(RunStatus::Stopped);
        log("proxy_stopped", 0, metrics->json());
    }
};

ProxyServer::ProxyServer(Scenario scenario, std::ostream &log_stream) : metrics_(std::make_shared<Metrics>())
{
    validate(scenario);
    impl_ = std::make_shared<Impl>(std::move(scenario), log_stream, metrics_);
}

ProxyServer::~ProxyServer()
{
    request_stop();
    impl_.reset();
}

void ProxyServer::run(const std::stop_token &stop_token)
{
    const auto impl = impl_;
    impl->run(stop_token);
}
void ProxyServer::request_stop() noexcept
{
    if (const auto impl = impl_)
    {
        if (!impl->stopping.exchange(true))
            impl->set_status(RunStatus::Stopping);
    }
}

void ProxyServer::update_policy(TrafficDirection direction, DirectionPolicy policy)
{
    const auto impl = impl_;
    impl->update_policy(direction, policy);
}

Scenario ProxyServer::scenario_snapshot() const
{
    const auto impl = impl_;
    return impl->scenario_snapshot();
}

LifecycleSnapshot ProxyServer::lifecycle_snapshot() const
{
    const auto impl = impl_;
    return impl->lifecycle_snapshot();
}

std::string ProxyServer::lifecycle_json() const
{
    const auto snapshot = lifecycle_snapshot();
    std::ostringstream out;
    out << "{\"experiment_id\":\"" << json_escape(snapshot.experiment_id) << "\",\"run_id\":\""
        << json_escape(snapshot.run_id) << "\",\"scenario_name\":\"" << json_escape(snapshot.scenario_name)
        << "\",\"status\":\"" << json_escape(snapshot.status)
        << "\",\"started_at_unix_ms\":" << snapshot.started_at_unix_ms << ",\"uptime_ms\":" << snapshot.uptime_ms
        << ",\"stage_count\":" << snapshot.stage_count << '}';
    return out.str();
}

}
