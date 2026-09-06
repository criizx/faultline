#pragma once

#include "faultline/config.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <stop_token>

namespace faultline
{

struct MetricsSnapshot
{
    std::uint64_t accepted_connections{};
    std::uint64_t active_connections{};
    std::uint64_t completed_connections{};
    std::uint64_t reset_connections{};
    std::uint64_t stage_reset_connections{};
    std::uint64_t timed_out_connections{};
    std::uint64_t upstream_bytes{};
    std::uint64_t downstream_bytes{};
    std::uint64_t delayed_chunks{};
    std::uint64_t throttled_writes{};
    std::uint64_t policy_updates{};
    std::uint64_t stage_transitions{};
    std::uint64_t blackout_entries{};
};

struct LifecycleSnapshot
{
    std::string experiment_id;
    std::string run_id;
    std::string scenario_name;
    std::string status;
    std::uint64_t started_at_unix_ms{};
    std::uint64_t uptime_ms{};
    std::size_t stage_count{};
};

struct ActiveConnectionSnapshot
{
    std::uint64_t connection_id{};
    std::string state;
    std::size_t stage_index{};
    std::string stage_name;
    std::uint64_t stage_elapsed_ms{};
    std::uint64_t stage_started_at_unix_ms{};
};

enum class TrafficDirection
{
    Upstream,
    Downstream
};

class Metrics
{
  public:
    [[nodiscard]] MetricsSnapshot snapshot() const noexcept;
    [[nodiscard]] std::string json() const;

  private:
    friend class ProxyServer;
    std::atomic_uint64_t accepted_connections_{};
    std::atomic_uint64_t active_connections_{};
    std::atomic_uint64_t completed_connections_{};
    std::atomic_uint64_t reset_connections_{};
    std::atomic_uint64_t stage_reset_connections_{};
    std::atomic_uint64_t timed_out_connections_{};
    std::atomic_uint64_t upstream_bytes_{};
    std::atomic_uint64_t downstream_bytes_{};
    std::atomic_uint64_t delayed_chunks_{};
    std::atomic_uint64_t throttled_writes_{};
    std::atomic_uint64_t policy_updates_{};
    std::atomic_uint64_t stage_transitions_{};
    std::atomic_uint64_t blackout_entries_{};
};

class ProxyServer
{
  public:
    explicit ProxyServer(Scenario scenario, std::ostream &log_stream);
    ~ProxyServer();

    ProxyServer(const ProxyServer &) = delete;
    ProxyServer &operator=(const ProxyServer &) = delete;

    void run(const std::stop_token &stop_token = {});
    void request_stop() noexcept;
    void request_shutdown();
    void update_policy(TrafficDirection direction, DirectionPolicy policy);
    [[nodiscard]] Scenario scenario_snapshot() const;
    [[nodiscard]] LifecycleSnapshot lifecycle_snapshot() const;
    [[nodiscard]] std::string lifecycle_json() const;
    [[nodiscard]] std::string connections_json() const;
    [[nodiscard]] std::string events_json(std::uint64_t after_sequence, std::size_t limit) const;
    [[nodiscard]] const Metrics &metrics() const noexcept
    {
        return *metrics_;
    }

  private:
    struct Impl;
    std::shared_ptr<Metrics> metrics_;
    std::shared_ptr<Impl> impl_;
};

}
