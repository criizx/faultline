#include "faultline/config.hpp"
#include "faultline/control_server.hpp"
#include "faultline/proxy.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::filesystem::path temporary_scenario(std::string_view body)
{
    const auto path =
        std::filesystem::temp_directory_path() / ("faultline-test-" + std::to_string(::getpid()) + ".conf");
    std::ofstream output(path);
    output << body;
    return path;
}

int free_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "cannot create port probe socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "cannot bind port probe");
    socklen_t size = sizeof(address);
    require(::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) == 0, "cannot inspect port probe");
    const int port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

class EchoServer
{
  public:
    explicit EchoServer(int port) : port_(port), thread_([this](const std::stop_token &token) { run(token); })
    {
        for (int i = 0; i < 100 && !ready_; ++i)
            std::this_thread::sleep_for(5ms);
        require(ready_, "echo server did not start");
    }

    ~EchoServer()
    {
        thread_.request_stop();
        if (client_ >= 0)
            ::shutdown(client_, SHUT_RDWR);
        thread_.join();
        if (listener_ >= 0)
            ::close(listener_);
    }

  private:
    void run(const std::stop_token &token)
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0)
            return;
        const int enabled = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(port_));
        if (::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 4) != 0)
            return;
        ready_ = true;
        while (!token.stop_requested())
        {
            pollfd descriptor{listener_, POLLIN, 0};
            const int poll_status = ::poll(&descriptor, 1, 50);
            if (poll_status <= 0 || (descriptor.revents & POLLIN) == 0)
                continue;
            client_ = ::accept(listener_, nullptr, nullptr);
            if (client_ < 0)
                break;
            char buffer[4096];
            while (!token.stop_requested())
            {
                const auto count = ::recv(client_, buffer, sizeof(buffer), 0);
                if (count <= 0)
                    break;
                std::size_t offset = 0;
                while (offset < static_cast<std::size_t>(count))
                {
                    const auto sent = ::send(client_, buffer + offset, static_cast<std::size_t>(count) - offset, 0);
                    if (sent <= 0)
                        break;
                    offset += static_cast<std::size_t>(sent);
                }
            }
            ::close(client_);
            client_ = -1;
        }
    }

    int port_;
    int listener_{-1};
    std::atomic_int client_{-1};
    std::atomic_bool ready_{false};
    std::jthread thread_;
};

int connect_local(int port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    for (int i = 0; i < 100; ++i)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        require(fd >= 0, "cannot create client socket");
        if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
            return fd;
        ::close(fd);
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("cannot connect to proxy");
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

std::string receive_exact(int descriptor, std::string result)
{
    std::size_t offset = 0;
    while (offset < result.size())
    {
        const auto received = ::recv(descriptor, result.data() + offset, result.size() - offset, 0);
        require(received > 0, "socket receive failed");
        offset += static_cast<std::size_t>(received);
    }
    return result;
}

std::string http_request(int port, std::string_view request)
{
    const int client = connect_local(port);
    timeval timeout{2, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    require(::send(client, request.data(), request.size(), 0) == static_cast<ssize_t>(request.size()),
            "HTTP request send failed");
    std::string response;
    char buffer[4096];
    while (true)
    {
        const auto received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break;
        response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(client);
    return response;
}

void test_config()
{
    const auto path = temporary_scenario(R"([scenario]
name=integration
experiment_id=test-integration
seed=42
[proxy]
listen_host=127.0.0.1
listen_port=18080
upstream_host=localhost
upstream_port=19090
connect_timeout_ms=1200
max_connections=32
[control]
host=0.0.0.0
port=18081
token=integration-token-123
[faults]
idle_timeout_ms=5000
reset_probability=0.25
[upstream]
latency_ms=20
jitter_ms=3
bandwidth_kbps=512
[downstream]
latency_ms=30
)");
    const auto scenario = faultline::load_scenario(path);
    std::filesystem::remove(path);
    require(scenario.name == "integration", "scenario name mismatch");
    require(scenario.experiment_id == "test-integration", "experiment id mismatch");
    require(scenario.seed == 42, "scenario seed mismatch");
    require(scenario.upstream.latency_ms == 20, "upstream latency mismatch");
    require(scenario.downstream.latency_ms == 30, "downstream latency mismatch");
    require(scenario.control_host == "0.0.0.0", "control host mismatch");
    require(scenario.control_port == 18081, "control port mismatch");
    require(scenario.control_token == "integration-token-123", "control token mismatch");
    require(scenario.connect_timeout_ms == 1200, "connect timeout mismatch");
    require(scenario.max_connections == 32, "connection limit mismatch");
    require(scenario.reset_probability == 0.25, "reset probability mismatch");
}

void test_invalid_config()
{
    const auto path = temporary_scenario("[faults]\nreset_probability=1.1\n");
    bool rejected = false;
    try
    {
        (void)faultline::load_scenario(path);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    std::filesystem::remove(path);
    require(rejected, "invalid probability was accepted");

    const auto nan_path = temporary_scenario("[faults]\nreset_probability=nan\n");
    rejected = false;
    try
    {
        (void)faultline::load_scenario(nan_path);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    std::filesystem::remove(nan_path);
    require(rejected, "NaN probability was accepted");

    const auto exposed_path = temporary_scenario("[control]\nhost=0.0.0.0\n");
    rejected = false;
    try
    {
        (void)faultline::load_scenario(exposed_path);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    std::filesystem::remove(exposed_path);
    require(rejected, "unauthenticated non-loopback control API was accepted");

    faultline::Scenario invalid;
    invalid.control_port = 0;
    std::ostringstream logs;
    rejected = false;
    try
    {
        faultline::ProxyServer proxy(invalid, logs);
    }
    catch (const std::exception &)
    {
        rejected = true;
    }
    require(rejected, "programmatic scenario bypassed validation");
}

void test_staged_config()
{
    const auto path = temporary_scenario(R"([scenario]
name=staged
[stage.1]
name=degraded
duration_ms=120
upstream.latency_ms=90
downstream.latency_ms=110
[stage.2]
name=recovered
duration_ms=0
upstream.latency_ms=0
downstream.latency_ms=0
)");
    const auto scenario = faultline::load_scenario(path);
    std::filesystem::remove(path);
    require(scenario.stages.size() == 2, "stage count mismatch");
    require(scenario.stages[0].name == "degraded", "first stage name mismatch");
    require(scenario.stages[0].duration_ms == 120, "first stage duration mismatch");
    require(scenario.stages[1].name == "recovered", "final stage name mismatch");
    require(scenario.stages[1].duration_ms == 0, "final stage duration mismatch");
    require(scenario.stages[0].upstream.latency_ms == 90, "stage policy mismatch");
}

void test_proxy_round_trip_and_latency()
{
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    EchoServer echo(upstream_port);
    faultline::Scenario scenario;
    scenario.name = "round-trip";
    scenario.listen_port = static_cast<std::uint16_t>(proxy_port);
    scenario.upstream_port = static_cast<std::uint16_t>(upstream_port);
    scenario.upstream.latency_ms = 35;
    scenario.downstream.latency_ms = 35;
    scenario.idle_timeout_ms = 2'000;
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    std::jthread proxy_thread([&proxy](const std::stop_token &token) { proxy.run(token); });
    const int client = connect_local(proxy_port);
    timeval timeout{3, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const std::string message = "faultline-round-trip";
    const auto started = std::chrono::steady_clock::now();
    require(::send(client, message.data(), message.size(), 0) == static_cast<ssize_t>(message.size()),
            "client send failed");
    std::string response(message.size(), '\0');
    std::size_t offset = 0;
    while (offset < response.size())
    {
        const auto count = ::recv(client, response.data() + offset, response.size() - offset, 0);
        require(count > 0, "client receive failed");
        offset += static_cast<std::size_t>(count);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ::close(client);
    proxy.request_stop();
    proxy_thread.request_stop();
    require(response == message, "proxy corrupted payload");
    require(elapsed >= 60ms, "configured round-trip latency was not applied");
    const auto metrics = proxy.metrics().snapshot();
    require(metrics.accepted_connections == 1, "accepted connection metric mismatch");
    require(metrics.upstream_bytes == message.size(), "upstream byte metric mismatch");
    require(metrics.downstream_bytes == message.size(), "downstream byte metric mismatch");
    require(metrics.delayed_chunks >= 2, "delayed chunk metric mismatch");
}

void test_staged_runtime()
{
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    EchoServer echo(upstream_port);
    faultline::Scenario scenario;
    scenario.name = "staged-runtime";
    scenario.experiment_id = "test-staged-runtime";
    scenario.listen_port = static_cast<std::uint16_t>(proxy_port);
    scenario.upstream_port = static_cast<std::uint16_t>(upstream_port);
    scenario.idle_timeout_ms = 2'000;
    faultline::Stage degraded;
    degraded.name = "degraded";
    degraded.duration_ms = 120;
    degraded.upstream.latency_ms = 80;
    degraded.downstream.latency_ms = 80;
    faultline::Stage recovered;
    recovered.name = "recovered";
    recovered.duration_ms = 0;
    scenario.stages = {degraded, recovered};
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    std::jthread proxy_thread([&proxy](const std::stop_token &token) { proxy.run(token); });
    const int client = connect_local(proxy_port);
    timeval timeout{3, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const auto first_started = std::chrono::steady_clock::now();
    send_all(client, "a");
    require(receive_exact(client, std::string(1, '\0')) == "a", "staged first exchange failed");
    const auto first_elapsed = std::chrono::steady_clock::now() - first_started;
    require(first_elapsed >= 120ms, "initial stage fault was not applied");
    std::this_thread::sleep_for(180ms);
    const auto second_started = std::chrono::steady_clock::now();
    send_all(client, "b");
    require(receive_exact(client, std::string(1, '\0')) == "b", "staged recovered exchange failed");
    const auto second_elapsed = std::chrono::steady_clock::now() - second_started;
    ::close(client);
    proxy.request_stop();
    proxy_thread.request_stop();
    proxy_thread.join();
    require(second_elapsed < 100ms, "final stage policy was not activated");
    require(proxy.metrics().snapshot().stage_transitions > 0, "stage transition was not recorded");
    require(logs.str().find("stage_transition") != std::string::npos, "stage transition was not logged");
    require(logs.str().find("test-staged-runtime") != std::string::npos, "experiment id was not logged");
}

void test_forced_reset()
{
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    EchoServer echo(upstream_port);
    faultline::Scenario scenario;
    scenario.name = "forced-reset";
    scenario.listen_port = static_cast<std::uint16_t>(proxy_port);
    scenario.upstream_port = static_cast<std::uint16_t>(upstream_port);
    scenario.reset_probability = 1.0;
    scenario.idle_timeout_ms = 2'000;
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    std::jthread proxy_thread([&proxy](const std::stop_token &token) { proxy.run(token); });
    const int client = connect_local(proxy_port);
    timeval timeout{2, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const char payload = 'x';
    (void)::send(client, &payload, 1, 0);
    char response{};
    const auto received = ::recv(client, &response, 1, 0);
    ::close(client);
    proxy.request_stop();
    proxy_thread.request_stop();
    require(received <= 0, "forced reset left the connection usable");
    for (int i = 0; i < 100 && proxy.metrics().snapshot().reset_connections == 0; ++i)
    {
        std::this_thread::sleep_for(5ms);
    }
    require(proxy.metrics().snapshot().reset_connections == 1, "reset metric mismatch");
}

void test_bandwidth_limit()
{
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    EchoServer echo(upstream_port);
    faultline::Scenario scenario;
    scenario.name = "bandwidth-limit";
    scenario.listen_port = static_cast<std::uint16_t>(proxy_port);
    scenario.upstream_port = static_cast<std::uint16_t>(upstream_port);
    scenario.upstream.bandwidth_kbps = 64;
    scenario.idle_timeout_ms = 3'000;
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    std::jthread proxy_thread([&proxy](const std::stop_token &token) { proxy.run(token); });
    const int client = connect_local(proxy_port);
    timeval timeout{3, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const std::string payload(std::size_t{8} * 1024, 'f');
    const auto started = std::chrono::steady_clock::now();
    require(::send(client, payload.data(), payload.size(), 0) == static_cast<ssize_t>(payload.size()),
            "bandwidth test send failed");
    std::string response(payload.size(), '\0');
    std::size_t offset = 0;
    while (offset < response.size())
    {
        const auto count = ::recv(client, response.data() + offset, response.size() - offset, 0);
        require(count > 0, "bandwidth test receive failed");
        offset += static_cast<std::size_t>(count);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ::close(client);
    proxy.request_stop();
    proxy_thread.request_stop();
    require(response == payload, "bandwidth limiter corrupted payload");
    require(elapsed >= 850ms, "bandwidth limit was not applied");
    require(proxy.metrics().snapshot().throttled_writes > 0, "throttling metric mismatch");
}

void test_large_half_close()
{
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    EchoServer echo(upstream_port);
    faultline::Scenario scenario;
    scenario.name = "large-half-close";
    scenario.listen_port = static_cast<std::uint16_t>(proxy_port);
    scenario.upstream_port = static_cast<std::uint16_t>(upstream_port);
    scenario.idle_timeout_ms = 5'000;
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    std::jthread proxy_thread([&proxy](const std::stop_token &token) { proxy.run(token); });
    const int client = connect_local(proxy_port);
    timeval timeout{5, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string payload(std::size_t{256} * 1024, '\0');
    for (std::size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<char>('a' + index % 26);
    send_all(client, payload);
    require(::shutdown(client, SHUT_WR) == 0, "client half-close failed");
    const auto response = receive_exact(client, std::string(payload.size(), '\0'));
    char trailing{};
    require(::recv(client, &trailing, 1, 0) == 0, "proxy did not propagate half-close");
    ::close(client);
    proxy.request_stop();
    proxy_thread.request_stop();
    require(response == payload, "large half-closed payload was truncated");
}

void test_fault_waits_do_not_trigger_idle_timeout()
{
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    EchoServer echo(upstream_port);
    faultline::Scenario scenario;
    scenario.name = "fault-wait";
    scenario.listen_port = static_cast<std::uint16_t>(proxy_port);
    scenario.upstream_port = static_cast<std::uint16_t>(upstream_port);
    scenario.upstream.latency_ms = 150;
    scenario.downstream.latency_ms = 150;
    scenario.blackout_after_ms = 1;
    scenario.blackout_duration_ms = 250;
    scenario.idle_timeout_ms = 100;
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    std::jthread proxy_thread([&proxy](const std::stop_token &token) { proxy.run(token); });
    const int client = connect_local(proxy_port);
    timeval timeout{3, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const auto started = std::chrono::steady_clock::now();
    send_all(client, "z");
    const auto response = receive_exact(client, std::string(1, '\0'));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ::close(client);
    proxy.request_stop();
    proxy_thread.request_stop();
    require(response == "z", "fault wait dropped payload");
    require(elapsed >= 350ms, "blackout or latency was not applied");
    require(proxy.metrics().snapshot().timed_out_connections == 0, "injected delay triggered idle timeout");
}

void test_connection_limit_and_worker_reaping()
{
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    EchoServer echo(upstream_port);
    faultline::Scenario scenario;
    scenario.name = "connection-limit";
    scenario.listen_port = static_cast<std::uint16_t>(proxy_port);
    scenario.upstream_port = static_cast<std::uint16_t>(upstream_port);
    scenario.max_connections = 1;
    scenario.idle_timeout_ms = 2'000;
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    std::jthread proxy_thread([&proxy](const std::stop_token &token) { proxy.run(token); });
    const int first = connect_local(proxy_port);
    send_all(first, "a");
    require(receive_exact(first, std::string(1, '\0')) == "a", "first limited connection failed");

    const int rejected = connect_local(proxy_port);
    timeval timeout{1, 0};
    ::setsockopt(rejected, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char value{};
    require(::recv(rejected, &value, 1, 0) <= 0, "connection limit was not enforced");
    ::close(rejected);
    ::close(first);

    for (int index = 0; index < 100 && proxy.metrics().snapshot().active_connections != 0; ++index)
        std::this_thread::sleep_for(5ms);
    require(proxy.metrics().snapshot().active_connections == 0, "completed worker remained active");

    const int next = connect_local(proxy_port);
    send_all(next, "c");
    require(receive_exact(next, std::string(1, '\0')) == "c", "reaped worker did not release connection slot");
    ::close(next);
    proxy.request_stop();
    proxy_thread.request_stop();
    require(proxy.metrics().snapshot().accepted_connections == 2, "connection admission metric mismatch");
}

void test_connect_shutdown_is_bounded()
{
    const int proxy_port = free_port();
    faultline::Scenario scenario;
    scenario.name = "connect-shutdown";
    scenario.listen_port = static_cast<std::uint16_t>(proxy_port);
    scenario.upstream_host = "192.0.2.1";
    scenario.upstream_port = 65'000;
    scenario.connect_timeout_ms = 30'000;
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    std::jthread proxy_thread([&proxy](const std::stop_token &token) { proxy.run(token); });
    const int client = connect_local(proxy_port);
    for (int index = 0; index < 100 && proxy.metrics().snapshot().active_connections == 0; ++index)
        std::this_thread::sleep_for(5ms);
    const auto started = std::chrono::steady_clock::now();
    proxy.request_stop();
    proxy_thread.request_stop();
    proxy_thread.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ::close(client);
    require(elapsed < 500ms, "shutdown waited for upstream connect timeout");
}

void test_control_api()
{
    faultline::Scenario scenario;
    scenario.name = "control-api";
    scenario.control_port = static_cast<std::uint16_t>(free_port());
    scenario.control_token = "control-test-token";
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    faultline::ControlServer control(proxy, logs);
    std::jthread control_thread([&control](const std::stop_token &token) { control.run(token); });

    const auto health = http_request(scenario.control_port, "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(health.find("200 OK") != std::string::npos, "health endpoint failed: " + health);
    require(health.find("{\"status\":\"ok\"}") != std::string::npos, "health response mismatch");

    const auto unauthenticated_state =
        http_request(scenario.control_port, "GET /v1/state HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(unauthenticated_state.find("401 Unauthorized") != std::string::npos,
            "state endpoint bypassed authentication");

    const auto state = http_request(scenario.control_port, "GET /v1/state HTTP/1.1\r\nHost: localhost\r\n"
                                                           "Authorization: Bearer control-test-token\r\n\r\n");
    require(state.find("200 OK") != std::string::npos, "state endpoint failed: " + state + " logs: " + logs.str());
    require(state.find("\"name\":\"control-api\"") != std::string::npos, "state scenario missing");
    require(state.find("\"accepted_connections\":0") != std::string::npos, "state metrics missing");
    require(state.find("\"lifecycle\":{") != std::string::npos, "state lifecycle missing");
    require(state.find("\"run_id\":\"") != std::string::npos, "state run id missing");

    const auto preflight =
        http_request(scenario.control_port,
                     "OPTIONS /v1/state HTTP/1.1\r\nHost: localhost\r\nAccess-Control-Request-Method: GET\r\n\r\n");
    require(preflight.find("204 No Content") != std::string::npos, "control preflight failed");
    require(preflight.find("Access-Control-Allow-Origin: *") != std::string::npos, "control CORS header missing");

    const auto lifecycle = http_request(scenario.control_port, "GET /v1/lifecycle HTTP/1.1\r\nHost: localhost\r\n"
                                                               "Authorization: Bearer control-test-token\r\n\r\n");
    require(lifecycle.find("200 OK") != std::string::npos, "lifecycle endpoint failed");
    require(lifecycle.find("\"status\":\"created\"") != std::string::npos, "lifecycle status mismatch");

    const auto rejected_update = http_request(
        scenario.control_port,
        "PUT /v1/policies/upstream?latency_ms=77 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    require(rejected_update.find("401 Unauthorized") != std::string::npos, "policy authentication was not enforced");
    require(proxy.scenario_snapshot().upstream.latency_ms == 0, "unconfirmed policy update was applied");

    const auto updated = http_request(
        scenario.control_port, "PUT /v1/policies/upstream?latency_ms=77&jitter_ms=9&bandwidth_kbps=321 HTTP/1.1\r\n"
                               "Host: localhost\r\nauthorization: Bearer control-test-token\r\n"
                               "x-faultline-confirm: update\r\nContent-Length: 0\r\n\r\n");
    require(updated.find("200 OK") != std::string::npos, "policy update failed");
    const auto current = proxy.scenario_snapshot();
    require(current.upstream.latency_ms == 77, "runtime latency update mismatch");
    require(current.upstream.jitter_ms == 9, "runtime jitter update mismatch");
    require(current.upstream.bandwidth_kbps == 321, "runtime bandwidth update mismatch");
    require(proxy.metrics().snapshot().policy_updates == 1, "policy update metric mismatch");

    const auto invalid_update =
        http_request(scenario.control_port, "PUT /v1/policies/upstream?latency_ms=10&latency_ms=20 HTTP/1.1\r\n"
                                            "Host: localhost\r\nAuthorization: Bearer control-test-token\r\n"
                                            "X-Faultline-Confirm: update\r\nContent-Length: 0\r\n\r\n");
    require(invalid_update.find("400 Bad Request") != std::string::npos, "duplicate policy key was accepted");
    require(proxy.scenario_snapshot().upstream.latency_ms == 77, "invalid update changed the runtime policy");
    require(proxy.metrics().snapshot().policy_updates == 1, "invalid update changed the policy metric");

    const auto forbidden =
        http_request(scenario.control_port, "POST /v1/shutdown HTTP/1.1\r\nHost: localhost\r\n"
                                            "Authorization: Bearer control-test-token\r\nContent-Length: 0\r\n\r\n");
    require(forbidden.find("403 Forbidden") != std::string::npos, "shutdown confirmation was not enforced");

    const int slow = connect_local(scenario.control_port);
    send_all(slow, "GET /healthz HTTP/1.1\r\n");
    const auto parallel_started = std::chrono::steady_clock::now();
    const auto parallel_health =
        http_request(scenario.control_port, "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const auto parallel_elapsed = std::chrono::steady_clock::now() - parallel_started;
    require(parallel_health.find("200 OK") != std::string::npos, "slow client blocked control API");
    require(parallel_elapsed < 500ms, "control API handled clients serially");
    ::close(slow);

    const int abandoned = connect_local(scenario.control_port);
    linger immediate{1, 0};
    ::setsockopt(abandoned, SOL_SOCKET, SO_LINGER, &immediate, sizeof(immediate));
    send_all(abandoned, "GET /v1/state HTTP/1.1\r\nHost: localhost\r\n\r\n");
    ::close(abandoned);
    std::this_thread::sleep_for(20ms);
    const auto after_abandon = http_request(scenario.control_port, "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(after_abandon.find("200 OK") != std::string::npos, "abandoned client killed control API");

    const auto escaped = http_request(scenario.control_port, "GET /bad\"target HTTP/1.1\r\nHost: localhost\r\n\r\n");
    require(escaped.find("404 Not Found") != std::string::npos, "unexpected target response");

    const auto accepted =
        http_request(scenario.control_port, "POST /v1/shutdown HTTP/1.1\r\nHost: localhost\r\n"
                                            "Authorization: Bearer control-test-token\r\n"
                                            "X-Faultline-Confirm: shutdown\r\nContent-Length: 0\r\n\r\n");
    require(accepted.find("202 Accepted") != std::string::npos, "confirmed shutdown failed");
    control.request_stop();
    control_thread.request_stop();
    control_thread.join();
    require(logs.str().find("bad\\\"target") != std::string::npos, "control log did not escape request target");
    require(state.find("control-test-token") == std::string::npos, "control token leaked through state API");
}

void test_live_policy_update_on_open_connection()
{
    const int upstream_port = free_port();
    const int proxy_port = free_port();
    EchoServer echo(upstream_port);
    faultline::Scenario scenario;
    scenario.name = "live-policy";
    scenario.listen_port = static_cast<std::uint16_t>(proxy_port);
    scenario.upstream_port = static_cast<std::uint16_t>(upstream_port);
    scenario.idle_timeout_ms = 3'000;
    std::ostringstream logs;
    faultline::ProxyServer proxy(scenario, logs);
    std::jthread proxy_thread([&proxy](const std::stop_token &token) { proxy.run(token); });
    const int client = connect_local(proxy_port);
    timeval timeout{3, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    const char first = 'a';
    require(::send(client, &first, 1, 0) == 1, "initial live-policy send failed");
    char first_response{};
    require(::recv(client, &first_response, 1, 0) == 1 && first_response == first,
            "initial live-policy exchange failed");

    proxy.update_policy(faultline::TrafficDirection::Upstream, {50, 0, 0});
    proxy.update_policy(faultline::TrafficDirection::Downstream, {50, 0, 0});
    const char second = 'b';
    const auto started = std::chrono::steady_clock::now();
    require(::send(client, &second, 1, 0) == 1, "updated live-policy send failed");
    char second_response{};
    require(::recv(client, &second_response, 1, 0) == 1 && second_response == second,
            "updated live-policy exchange failed");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ::close(client);
    proxy.request_stop();
    proxy_thread.request_stop();
    require(elapsed >= 85ms, "updated policy did not affect the open connection");
}

}

int main(int argc, char **argv)
{
    try
    {
        using Test = std::pair<std::string_view, void (*)()>;
        const std::array tests{
            Test{"config", test_config},
            Test{"invalid_config", test_invalid_config},
            Test{"staged_config", test_staged_config},
            Test{"round_trip_and_latency", test_proxy_round_trip_and_latency},
            Test{"staged_runtime", test_staged_runtime},
            Test{"forced_reset", test_forced_reset},
            Test{"bandwidth_limit", test_bandwidth_limit},
            Test{"large_half_close", test_large_half_close},
            Test{"fault_wait", test_fault_waits_do_not_trigger_idle_timeout},
            Test{"connection_limit", test_connection_limit_and_worker_reaping},
            Test{"connect_shutdown", test_connect_shutdown_is_bounded},
            Test{"control_api", test_control_api},
            Test{"live_policy", test_live_policy_update_on_open_connection},
        };
        bool matched = argc == 1;
        for (const auto &[name, test] : tests)
        {
            if (argc == 1 || name == argv[1])
            {
                std::cerr << "running " << name << '\n';
                test();
                matched = true;
            }
        }
        require(matched, "unknown test name");
        std::cout << "all tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "test failure: " << error.what() << '\n';
        return 1;
    }
}
