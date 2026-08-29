#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace faultline
{

struct DirectionPolicy
{
    std::uint32_t latency_ms{0};
    std::uint32_t jitter_ms{0};
    std::uint32_t bandwidth_kbps{0};
};

struct Stage
{
    std::string name;
    std::uint32_t duration_ms{0};
    std::uint32_t idle_timeout_ms{30'000};
    std::uint32_t blackout_after_ms{0};
    std::uint32_t blackout_duration_ms{0};
    double reset_probability{0.0};
    DirectionPolicy upstream;
    DirectionPolicy downstream;
};

struct Scenario
{
    std::string name{"default"};
    std::string experiment_id;
    std::string listen_host{"127.0.0.1"};
    std::uint16_t listen_port{8080};
    std::string upstream_host{"127.0.0.1"};
    std::uint16_t upstream_port{3000};
    std::uint32_t connect_timeout_ms{5'000};
    std::uint32_t max_connections{256};
    std::string control_host{"127.0.0.1"};
    std::uint16_t control_port{9090};
    std::string control_token;
    std::uint64_t seed{1};
    std::uint32_t idle_timeout_ms{30'000};
    std::uint32_t blackout_after_ms{0};
    std::uint32_t blackout_duration_ms{0};
    double reset_probability{0.0};
    DirectionPolicy upstream;
    DirectionPolicy downstream;
    std::vector<Stage> stages;
};

[[nodiscard]] Scenario load_scenario(const std::filesystem::path &path);
void validate(const Scenario &scenario);
[[nodiscard]] std::string describe(const Scenario &scenario);
[[nodiscard]] std::string scenario_json(const Scenario &scenario);

}
