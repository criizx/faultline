#include "faultline/config.hpp"
#include "faultline/proxy.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Profile
{
    std::string_view name;
    std::vector<std::uint32_t> latency_targets_ms;
    std::size_t latency_samples;
    std::vector<std::uint32_t> bandwidth_targets_kbps;
    std::size_t bandwidth_payload_bytes;
    std::size_t overhead_payload_bytes;
    std::size_t overhead_iterations;
    std::size_t churn_connections;
};

void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

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

int free_port()
{
    Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
    require(static_cast<bool>(socket), "cannot create port probe socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(socket.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0,
            "cannot bind port probe socket");
    socklen_t size = sizeof(address);
    require(::getsockname(socket.get(), reinterpret_cast<sockaddr *>(&address), &size) == 0,
            "cannot inspect port probe socket");
    return ntohs(address.sin_port);
}

class EchoServer
{
  public:
    explicit EchoServer(int port) : port_(port), thread_([this](const std::stop_token &token) { run(token); })
    {
        for (int attempt = 0; attempt < 200 && !ready_.load(); ++attempt)
            std::this_thread::sleep_for(5ms);
        require(ready_.load(), "echo server did not start");
    }

    ~EchoServer()
    {
        thread_.request_stop();
        const int client = client_.load();
        if (client >= 0)
            ::shutdown(client, SHUT_RDWR);
        thread_.join();
        const int listener = listener_.load();
        if (listener >= 0)
            ::close(listener);
    }

  private:
    void run(const std::stop_token &token)
    {
        Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
        if (!listener)
            return;
        listener_ = listener.get();
        const int enabled = 1;
        (void)::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port_));
        if (::bind(listener.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(listener.get(), 16) != 0)
            return;
        ready_ = true;
        while (!token.stop_requested())
        {
            pollfd descriptor{listener.get(), POLLIN, 0};
            const int status = ::poll(&descriptor, 1, 50);
            if (status <= 0 || (descriptor.revents & POLLIN) == 0)
                continue;
            Socket client(::accept(listener.get(), nullptr, nullptr));
            if (!client)
                continue;
            client_ = client.get();
            echo(token);
            client_ = -1;
        }
        listener_ = -1;
    }

    void echo(const std::stop_token &token)
    {
        const int client = client_.load();
        std::vector<char> buffer(std::size_t{64} * 1024);
        while (!token.stop_requested())
        {
            const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
            if (received <= 0)
                return;
            std::size_t offset = 0;
            const auto size = static_cast<std::size_t>(received);
            while (offset < size)
            {
                const auto sent = ::send(client, buffer.data() + offset, size - offset, 0);
                if (sent <= 0)
                    return;
                offset += static_cast<std::size_t>(sent);
            }
        }
    }

    int port_;
    std::atomic_int listener_{-1};
    std::atomic_int client_{-1};
    std::atomic_bool ready_{false};
    std::jthread thread_;
};

Socket connect_local(int port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
        require(static_cast<bool>(socket), "cannot create client socket");
        timeval timeout{15, 0};
        (void)::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (::connect(socket.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
            return socket;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("cannot connect to local endpoint");
}

void send_all(int descriptor, std::string_view payload)
{
    std::size_t offset = 0;
    while (offset < payload.size())
    {
        const auto sent = ::send(descriptor, payload.data() + offset, payload.size() - offset, 0);
        require(sent > 0, "socket send failed");
        offset += static_cast<std::size_t>(sent);
    }
}

void receive_exact(int descriptor, std::size_t size)
{
    std::vector<char> buffer(std::size_t{64} * 1024);
    std::size_t received_total = 0;
    while (received_total < size)
    {
        const auto remaining = std::min(buffer.size(), size - received_total);
        const auto received = ::recv(descriptor, buffer.data(), remaining, 0);
        require(received > 0, "socket receive failed");
        received_total += static_cast<std::size_t>(received);
    }
}

struct ProxyConfiguration
{
    int proxy_port;
    int upstream_port;
    faultline::DirectionPolicy upstream;
    faultline::DirectionPolicy downstream;
};

faultline::Scenario scenario_for(const ProxyConfiguration &configuration)
{
    faultline::Scenario scenario;
    scenario.name = "benchmark";
    scenario.experiment_id = "benchmark-local";
    scenario.listen_port = static_cast<std::uint16_t>(configuration.proxy_port);
    scenario.upstream_port = static_cast<std::uint16_t>(configuration.upstream_port);
    scenario.control_port = static_cast<std::uint16_t>(free_port());
    scenario.connect_timeout_ms = 2'000;
    scenario.max_connections = 512;
    scenario.upstream = configuration.upstream;
    scenario.downstream = configuration.downstream;
    return scenario;
}

class RunningProxy
{
  public:
    explicit RunningProxy(ProxyConfiguration configuration)
        : echo_(configuration.upstream_port), proxy_(scenario_for(configuration), logs_),
          thread_([this](const std::stop_token &token) { proxy_.run(token); })
    {
        Socket probe = connect_local(configuration.proxy_port);
    }

    ~RunningProxy()
    {
        proxy_.request_stop();
        thread_.request_stop();
        thread_.join();
    }

  private:
    EchoServer echo_;
    std::ostringstream logs_;
    faultline::ProxyServer proxy_;
    std::jthread thread_;
};

double milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

double seconds(Clock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}

double process_cpu_seconds()
{
    rusage usage{};
    require(::getrusage(RUSAGE_SELF, &usage) == 0, "cannot read process CPU usage");
    const double user = static_cast<double>(usage.ru_utime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec) / 1e6;
    const double system =
        static_cast<double>(usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_stime.tv_usec) / 1e6;
    return user + system;
}

double percentile(std::vector<double> values, double quantile)
{
    std::ranges::sort(values);
    const auto raw_index = std::ceil(quantile * static_cast<double>(values.size()));
    const auto index = static_cast<std::size_t>(std::max(1.0, raw_index)) - 1;
    return values[index];
}

std::string platform_name()
{
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string compiler_name()
{
#if defined(__apple_build_version__)
    return "AppleClang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__clang__)
    return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
    return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#else
    return "unknown";
#endif
}

void emit_metadata(const Profile &profile)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::cout << "{\"schema_version\":1,\"type\":\"metadata\",\"profile\":\"" << profile.name
              << "\",\"faultline_version\":\"" << FAULTLINE_VERSION << "\",\"build_type\":\"" << FAULTLINE_BUILD_TYPE
              << "\",\"platform\":\"" << platform_name() << "\",\"architecture\":\"" << FAULTLINE_SYSTEM_PROCESSOR
              << "\",\"compiler\":\"" << compiler_name() << "\",\"started_at_unix_ms\":" << timestamp << "}\n";
}

std::vector<double> latency_samples(int proxy_port, std::size_t sample_count)
{
    Socket client = connect_local(proxy_port);
    send_all(client.get(), "w");
    receive_exact(client.get(), 1);
    std::vector<double> samples;
    samples.reserve(sample_count);
    for (std::size_t sample = 0; sample < sample_count; ++sample)
    {
        const auto start = Clock::now();
        send_all(client.get(), "x");
        receive_exact(client.get(), 1);
        samples.push_back(milliseconds(Clock::now() - start));
    }
    return samples;
}

double mean_of(const std::vector<double> &values)
{
    return std::ranges::fold_left(values, 0.0, std::plus<>()) / static_cast<double>(values.size());
}

void benchmark_latency(const Profile &profile)
{
    double baseline_mean = 0.0;
    {
        const int upstream_port = free_port();
        const int proxy_port = free_port();
        RunningProxy proxy({proxy_port, upstream_port, {}, {}});
        baseline_mean = mean_of(latency_samples(proxy_port, profile.latency_samples));
    }
    for (const auto target : profile.latency_targets_ms)
    {
        const int upstream_port = free_port();
        const int proxy_port = free_port();
        faultline::DirectionPolicy upstream;
        faultline::DirectionPolicy downstream;
        upstream.latency_ms = target / 2;
        downstream.latency_ms = target - upstream.latency_ms;
        RunningProxy proxy({proxy_port, upstream_port, upstream, downstream});
        const auto samples = latency_samples(proxy_port, profile.latency_samples);
        const double mean = mean_of(samples);
        const double measured_injected = mean - baseline_mean;
        std::cout << std::fixed << std::setprecision(3)
                  << "{\"schema_version\":1,\"type\":\"result\",\"benchmark\":\"latency_accuracy\",\"profile\":\""
                  << profile.name << "\",\"scenario\":\"benchmark\",\"target_injected_ms\":" << target
                  << ",\"samples\":" << samples.size() << ",\"baseline_mean_rtt_ms\":" << baseline_mean
                  << ",\"mean_rtt_ms\":" << mean << ",\"measured_injected_ms\":" << measured_injected
                  << ",\"p50_rtt_ms\":" << percentile(samples, 0.50) << ",\"p95_rtt_ms\":" << percentile(samples, 0.95)
                  << ",\"injected_error_ms\":" << measured_injected - static_cast<double>(target) << "}\n";
    }
}

void benchmark_bandwidth(const Profile &profile)
{
    const std::string payload(profile.bandwidth_payload_bytes, 'b');
    for (const auto target : profile.bandwidth_targets_kbps)
    {
        const int upstream_port = free_port();
        const int proxy_port = free_port();
        faultline::DirectionPolicy upstream;
        upstream.bandwidth_kbps = target;
        RunningProxy proxy({proxy_port, upstream_port, upstream, {}});
        Socket client = connect_local(proxy_port);
        const auto start = Clock::now();
        send_all(client.get(), payload);
        receive_exact(client.get(), payload.size());
        const double elapsed = seconds(Clock::now() - start);
        const double measured = static_cast<double>(payload.size()) * 8.0 / elapsed / 1000.0;
        std::cout << std::fixed << std::setprecision(3)
                  << "{\"schema_version\":1,\"type\":\"result\",\"benchmark\":\"bandwidth_accuracy\",\"profile\":\""
                  << profile.name << "\",\"scenario\":\"benchmark\",\"target_kbps\":" << target
                  << ",\"payload_bytes\":" << payload.size() << ",\"elapsed_ms\":" << elapsed * 1000.0
                  << ",\"measured_kbps\":" << measured << ",\"error_percent\":"
                  << (measured - static_cast<double>(target)) / static_cast<double>(target) * 100.0 << "}\n";
    }
}

struct TransferMeasurement
{
    double elapsed_seconds;
    double cpu_seconds;
};

TransferMeasurement measure_transfer(int port, std::string_view payload)
{
    Socket client = connect_local(port);
    const double cpu_start = process_cpu_seconds();
    const auto start = Clock::now();
    std::exception_ptr send_error;
    std::jthread sender([&] {
        try
        {
            send_all(client.get(), payload);
        }
        catch (...)
        {
            send_error = std::current_exception();
        }
    });
    receive_exact(client.get(), payload.size());
    sender.join();
    if (send_error)
        std::rethrow_exception(send_error);
    return {seconds(Clock::now() - start), process_cpu_seconds() - cpu_start};
}

void benchmark_overhead(const Profile &profile)
{
    const std::string payload(profile.overhead_payload_bytes, 'o');
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    RunningProxy proxy({proxy_port, upstream_port, {}, {}});
    std::vector<double> direct_elapsed;
    std::vector<double> proxy_elapsed;
    std::vector<double> direct_cpu;
    std::vector<double> proxy_cpu;
    direct_elapsed.reserve(profile.overhead_iterations);
    proxy_elapsed.reserve(profile.overhead_iterations);
    direct_cpu.reserve(profile.overhead_iterations);
    proxy_cpu.reserve(profile.overhead_iterations);
    for (std::size_t iteration = 0; iteration < profile.overhead_iterations; ++iteration)
    {
        TransferMeasurement direct{};
        TransferMeasurement proxied{};
        if (iteration % 2 == 0)
        {
            direct = measure_transfer(upstream_port, payload);
            proxied = measure_transfer(proxy_port, payload);
        }
        else
        {
            proxied = measure_transfer(proxy_port, payload);
            direct = measure_transfer(upstream_port, payload);
        }
        direct_elapsed.push_back(direct.elapsed_seconds);
        proxy_elapsed.push_back(proxied.elapsed_seconds);
        direct_cpu.push_back(direct.cpu_seconds);
        proxy_cpu.push_back(proxied.cpu_seconds);
    }
    const double direct_seconds = percentile(direct_elapsed, 0.50);
    const double proxy_seconds = percentile(proxy_elapsed, 0.50);
    const double direct_cpu_seconds = percentile(direct_cpu, 0.50);
    const double proxy_cpu_seconds = percentile(proxy_cpu, 0.50);
    const double direct_mbps = static_cast<double>(payload.size()) * 8.0 / direct_seconds / 1'000'000.0;
    const double proxy_mbps = static_cast<double>(payload.size()) * 8.0 / proxy_seconds / 1'000'000.0;
    std::cout << std::fixed << std::setprecision(3)
              << "{\"schema_version\":1,\"type\":\"result\",\"benchmark\":\"proxy_overhead\",\"profile\":\""
              << profile.name << "\",\"scenario\":\"benchmark\",\"payload_bytes\":" << payload.size()
              << ",\"iterations\":" << profile.overhead_iterations << ",\"direct_mbps\":" << direct_mbps
              << ",\"proxy_mbps\":" << proxy_mbps
              << ",\"throughput_delta_percent\":" << (proxy_mbps / direct_mbps - 1.0) * 100.0
              << ",\"direct_cpu_ms\":" << direct_cpu_seconds * 1000.0
              << ",\"proxy_cpu_ms\":" << proxy_cpu_seconds * 1000.0
              << ",\"proxy_cpu_percent\":" << proxy_cpu_seconds / proxy_seconds * 100.0 << "}\n";
}

void benchmark_churn(const Profile &profile)
{
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    RunningProxy proxy({proxy_port, upstream_port, {}, {}});
    const auto start = Clock::now();
    for (std::size_t connection = 0; connection < profile.churn_connections; ++connection)
    {
        Socket client = connect_local(proxy_port);
        send_all(client.get(), "c");
        receive_exact(client.get(), 1);
    }
    const double elapsed = seconds(Clock::now() - start);
    std::cout << std::fixed << std::setprecision(3)
              << "{\"schema_version\":1,\"type\":\"result\",\"benchmark\":\"connection_churn\",\"profile\":\""
              << profile.name << "\",\"scenario\":\"benchmark\",\"connections\":" << profile.churn_connections
              << ",\"elapsed_ms\":" << elapsed * 1000.0
              << ",\"connections_per_second\":" << static_cast<double>(profile.churn_connections) / elapsed << "}\n";
}

Profile profile_from(std::string_view name)
{
    if (name == "smoke")
        return {"smoke", {10, 50}, 5, {2'048}, std::size_t{128} * 1024, std::size_t{1024} * 1024, 3, 50};
    if (name == "standard")
        return {"standard", {10, 50, 100}, 20, {512, 2'048}, std::size_t{512} * 1024, std::size_t{8} * 1024 * 1024,
                7,          5'000};
    throw std::invalid_argument("profile must be smoke or standard");
}

}

int main(int argc, char **argv)
{
    try
    {
        std::signal(SIGPIPE, SIG_IGN);
        if (argc > 2)
            throw std::invalid_argument("usage: faultline_benchmarks [smoke|standard]");
        const Profile profile = profile_from(argc == 2 ? argv[1] : "standard");
        emit_metadata(profile);
        benchmark_latency(profile);
        benchmark_bandwidth(profile);
        benchmark_overhead(profile);
        benchmark_churn(profile);
        return EXIT_SUCCESS;
    }
    catch (const std::invalid_argument &error)
    {
        std::cerr << error.what() << '\n';
        return 2;
    }
    catch (const std::exception &error)
    {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
