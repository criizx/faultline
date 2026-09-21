#include "faultline/config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace faultline
{
namespace
{

std::string trim(std::string value)
{
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
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
            if (const auto escaped = static_cast<unsigned int>(static_cast<unsigned char>(c)); escaped < 0x20U)
            {
                constexpr char hex[] = "0123456789abcdef";
                out << "\\u00" << hex[escaped >> 4U] << hex[escaped & 0x0fU];
            }
            else
            {
                out << c;
            }
        }
    }
    return out.str();
}

std::uint64_t parse_unsigned(const std::string &value, std::string_view key)
{
    std::uint64_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("invalid unsigned value for '" + std::string(key) + "': " + value);
    return result;
}

bool parse_bool(const std::string &value, std::string_view key)
{
    if (value == "true")
        return true;
    if (value == "false")
        return false;
    throw std::runtime_error("invalid boolean value for '" + std::string(key) + "': " + value);
}

bool valid_origin(std::string_view origin)
{
    const auto scheme_size = origin.starts_with("http://")    ? std::size_t{7}
                             : origin.starts_with("https://") ? std::size_t{8}
                                                              : std::size_t{0};
    if (scheme_size == 0 || origin.size() == scheme_size)
        return false;
    const auto authority = origin.substr(scheme_size);
    return authority.find_first_of("/?#@ \t\r\n*") == std::string_view::npos;
}

std::vector<std::string> parse_list(const std::string &value)
{
    std::vector<std::string> result;
    std::string_view remaining = value;
    while (!remaining.empty())
    {
        const auto separator = remaining.find(',');
        auto item = trim(std::string(remaining.substr(0, separator)));
        if (item.empty())
            throw std::runtime_error("control.allowed_origins contains an empty origin");
        result.push_back(std::move(item));
        if (separator == std::string_view::npos)
            break;
        remaining.remove_prefix(separator + 1);
    }
    return result;
}

double parse_probability(const std::string &value, std::string_view key)
{
    std::size_t consumed = 0;
    try
    {
        const auto result = std::stod(value, &consumed);
        if (consumed != value.size())
        {
            throw std::invalid_argument("trailing characters");
        }
        return result;
    }
    catch (const std::exception &)
    {
        throw std::runtime_error("invalid probability for '" + std::string(key) + "': " + value);
    }
}

std::uint16_t parse_port(const std::string &value, std::string_view key)
{
    const auto parsed = parse_unsigned(value, key);
    if (parsed == 0 || parsed > 65'535)
    {
        throw std::runtime_error("port for '" + std::string(key) + "' must be in 1..65535");
    }
    return static_cast<std::uint16_t>(parsed);
}

std::uint32_t parse_u32(const std::string &value, std::string_view key)
{
    const auto parsed = parse_unsigned(value, key);
    if (parsed > UINT32_MAX)
    {
        throw std::runtime_error("value for '" + std::string(key) + "' is too large");
    }
    return static_cast<std::uint32_t>(parsed);
}

}

Scenario load_scenario(const std::filesystem::path &path)
{
    std::ifstream input(path);
    if (!input)
    {
        throw std::runtime_error("cannot open scenario: " + path.string());
    }

    std::unordered_map<std::string, std::string> values;
    std::string section;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        if (line.front() == '[' && line.back() == ']')
        {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos)
        {
            throw std::runtime_error("scenario line " + std::to_string(line_number) + " must contain '='");
        }
        auto key = trim(line.substr(0, equals));
        auto value = trim(line.substr(equals + 1));
        if (key.empty() || value.empty())
        {
            throw std::runtime_error("scenario line " + std::to_string(line_number) + " has an empty key or value");
        }
        if (!section.empty())
        {
            key.insert(0, 1, '.');
            key.insert(0, section);
        }
        if (!values.emplace(key, value).second)
        {
            throw std::runtime_error("duplicate scenario key: " + key);
        }
    }

    Scenario result;
    const auto set_string = [&](std::string_view key, std::string &target) {
        if (const auto it = values.find(std::string(key)); it != values.end())
            target = it->second;
    };
    const auto set_u32 = [&](std::string_view key, std::uint32_t &target) {
        if (const auto it = values.find(std::string(key)); it != values.end())
            target = parse_u32(it->second, key);
    };
    set_string("scenario.name", result.name);
    set_string("scenario.experiment_id", result.experiment_id);
    set_string("proxy.listen_host", result.listen_host);
    set_string("proxy.upstream_host", result.upstream_host);
    set_string("control.host", result.control_host);
    set_string("control.token", result.control_token);
    if (const auto it = values.find("control.allowed_origins"); it != values.end())
        result.control_allowed_origins = parse_list(it->second);
    if (const auto it = values.find("control.allow_insecure_remote"); it != values.end())
        result.allow_insecure_remote_control = parse_bool(it->second, "control.allow_insecure_remote");
    if (const auto it = values.find("proxy.listen_port"); it != values.end())
        result.listen_port = parse_port(it->second, "proxy.listen_port");
    if (const auto it = values.find("proxy.upstream_port"); it != values.end())
        result.upstream_port = parse_port(it->second, "proxy.upstream_port");
    if (const auto it = values.find("control.port"); it != values.end())
        result.control_port = parse_port(it->second, "control.port");
    if (const auto it = values.find("scenario.seed"); it != values.end())
        result.seed = parse_unsigned(it->second, "scenario.seed");
    set_u32("proxy.connect_timeout_ms", result.connect_timeout_ms);
    set_u32("proxy.max_connections", result.max_connections);
    set_u32("proxy.max_queued_bytes", result.max_queued_bytes);
    set_u32("proxy.max_total_queued_bytes", result.max_total_queued_bytes);
    set_u32("faults.idle_timeout_ms", result.idle_timeout_ms);
    set_u32("faults.blackout_after_ms", result.blackout_after_ms);
    set_u32("faults.blackout_duration_ms", result.blackout_duration_ms);
    if (const auto it = values.find("faults.reset_probability"); it != values.end())
        result.reset_probability = parse_probability(it->second, "faults.reset_probability");
    set_u32("upstream.latency_ms", result.upstream.latency_ms);
    set_u32("upstream.jitter_ms", result.upstream.jitter_ms);
    set_u32("upstream.bandwidth_kbps", result.upstream.bandwidth_kbps);
    set_u32("downstream.latency_ms", result.downstream.latency_ms);
    set_u32("downstream.jitter_ms", result.downstream.jitter_ms);
    set_u32("downstream.bandwidth_kbps", result.downstream.bandwidth_kbps);

    std::map<std::size_t, std::unordered_map<std::string, std::string>> stage_values;
    for (const auto &[key, value] : values)
    {
        if (key.rfind("stage.", 0) != 0)
            continue;
        const auto index_end = key.find('.', 6);
        if (index_end == std::string::npos || index_end == 6)
            throw std::runtime_error("invalid stage key: " + key);
        std::size_t index = 0;
        try
        {
            std::size_t consumed = 0;
            index = std::stoull(key.substr(6, index_end - 6), &consumed);
            if (consumed != index_end - 6 || index == 0)
                throw std::invalid_argument("invalid stage index");
        }
        catch (const std::exception &)
        {
            throw std::runtime_error("invalid stage index in key: " + key);
        }
        const auto field = key.substr(index_end + 1);
        if (!stage_values[index].emplace(field, value).second)
            throw std::runtime_error("duplicate stage key: " + key);
    }
    if (!stage_values.empty())
    {
        if (stage_values.size() > 256 || stage_values.begin()->first != 1)
            throw std::runtime_error("stage indexes must start at 1 and contain at most 256 stages");
        std::size_t expected_index = 1;
        for (const auto &[index, fields] : stage_values)
        {
            if (index != expected_index++)
                throw std::runtime_error("stage indexes must be contiguous");
            Stage stage;
            stage.idle_timeout_ms = result.idle_timeout_ms;
            stage.blackout_after_ms = result.blackout_after_ms;
            stage.blackout_duration_ms = result.blackout_duration_ms;
            stage.reset_probability = result.reset_probability;
            stage.upstream = result.upstream;
            stage.downstream = result.downstream;
            const auto set_stage_string = [&](std::string_view key, std::string &target) {
                if (const auto it = fields.find(std::string(key)); it != fields.end())
                    target = it->second;
            };
            const auto set_stage_u32 = [&](std::string_view key, std::uint32_t &target) {
                if (const auto it = fields.find(std::string(key)); it != fields.end())
                    target = parse_u32(it->second, "stage." + std::to_string(index) + "." + std::string(key));
            };
            set_stage_string("name", stage.name);
            set_stage_u32("duration_ms", stage.duration_ms);
            set_stage_u32("idle_timeout_ms", stage.idle_timeout_ms);
            set_stage_u32("blackout_after_ms", stage.blackout_after_ms);
            set_stage_u32("blackout_duration_ms", stage.blackout_duration_ms);
            if (const auto it = fields.find("reset_probability"); it != fields.end())
                stage.reset_probability =
                    parse_probability(it->second, "stage." + std::to_string(index) + ".reset_probability");
            set_stage_u32("upstream.latency_ms", stage.upstream.latency_ms);
            set_stage_u32("upstream.jitter_ms", stage.upstream.jitter_ms);
            set_stage_u32("upstream.bandwidth_kbps", stage.upstream.bandwidth_kbps);
            set_stage_u32("downstream.latency_ms", stage.downstream.latency_ms);
            set_stage_u32("downstream.jitter_ms", stage.downstream.jitter_ms);
            set_stage_u32("downstream.bandwidth_kbps", stage.downstream.bandwidth_kbps);
            static const std::string stage_known[] = {"name",
                                                      "duration_ms",
                                                      "idle_timeout_ms",
                                                      "blackout_after_ms",
                                                      "blackout_duration_ms",
                                                      "reset_probability",
                                                      "upstream.latency_ms",
                                                      "upstream.jitter_ms",
                                                      "upstream.bandwidth_kbps",
                                                      "downstream.latency_ms",
                                                      "downstream.jitter_ms",
                                                      "downstream.bandwidth_kbps"};
            for (const auto &[field, unused] : fields)
            {
                (void)unused;
                if (std::find(std::begin(stage_known), std::end(stage_known), field) == std::end(stage_known))
                    throw std::runtime_error("unknown stage key: stage." + std::to_string(index) + "." + field);
            }
            result.stages.push_back(std::move(stage));
        }
    }

    static const std::string known[] = {"scenario.name",
                                        "scenario.experiment_id",
                                        "scenario.seed",
                                        "proxy.listen_host",
                                        "proxy.listen_port",
                                        "proxy.upstream_host",
                                        "proxy.upstream_port",
                                        "proxy.connect_timeout_ms",
                                        "proxy.max_connections",
                                        "proxy.max_queued_bytes",
                                        "proxy.max_total_queued_bytes",
                                        "control.host",
                                        "control.port",
                                        "control.token",
                                        "control.allowed_origins",
                                        "control.allow_insecure_remote",
                                        "faults.idle_timeout_ms",
                                        "faults.blackout_after_ms",
                                        "faults.blackout_duration_ms",
                                        "faults.reset_probability",
                                        "upstream.latency_ms",
                                        "upstream.jitter_ms",
                                        "upstream.bandwidth_kbps",
                                        "downstream.latency_ms",
                                        "downstream.jitter_ms",
                                        "downstream.bandwidth_kbps"};
    for (const auto &[key, unused] : values)
    {
        (void)unused;
        if (key.rfind("stage.", 0) == 0)
            continue;
        if (std::find(std::begin(known), std::end(known), key) == std::end(known))
        {
            throw std::runtime_error("unknown scenario key: " + key);
        }
    }
    validate(result);
    return result;
}

void validate(const Scenario &scenario)
{
    const auto validate_policy = [](const DirectionPolicy &policy, std::string_view direction) {
        if (policy.latency_ms > 3'600'000 || policy.jitter_ms > 3'600'000)
            throw std::runtime_error(std::string(direction) + " latency and jitter must not exceed 3600000ms");
        if (policy.bandwidth_kbps > 1'000'000'000)
            throw std::runtime_error(std::string(direction) + " bandwidth must not exceed 1000000000kbps");
    };
    if (scenario.name.empty())
        throw std::runtime_error("scenario name must not be empty");
    if (scenario.experiment_id.size() > 128)
        throw std::runtime_error("experiment_id must not exceed 128 characters");
    if (std::any_of(scenario.experiment_id.begin(), scenario.experiment_id.end(), [](unsigned char character) {
            return std::isalnum(character) == 0 && character != '-' && character != '_' && character != '.' &&
                   character != ':';
        }))
        throw std::runtime_error("experiment_id contains unsupported characters");
    if (scenario.listen_host.empty() || scenario.upstream_host.empty())
        throw std::runtime_error("proxy hosts must not be empty");
    if (scenario.control_host.empty())
        throw std::runtime_error("control host must not be empty");
    if (scenario.listen_port == 0 || scenario.upstream_port == 0 || scenario.control_port == 0)
        throw std::runtime_error("ports must not be zero");
    if (scenario.connect_timeout_ms < 100 || scenario.connect_timeout_ms > 300'000)
        throw std::runtime_error("connect_timeout_ms must be in 100..300000");
    if (scenario.max_connections == 0 || scenario.max_connections > 100'000)
        throw std::runtime_error("max_connections must be in 1..100000");
    if (scenario.max_queued_bytes < 16 * 1024 || scenario.max_queued_bytes > 64 * 1024 * 1024)
        throw std::runtime_error("max_queued_bytes must be in 16384..67108864");
    if (scenario.max_total_queued_bytes < scenario.max_queued_bytes ||
        scenario.max_total_queued_bytes > 1024U * 1024U * 1024U)
        throw std::runtime_error("max_total_queued_bytes must be between max_queued_bytes and 1073741824");
    if (scenario.idle_timeout_ms < 100)
        throw std::runtime_error("idle_timeout_ms must be at least 100");
    if (!std::isfinite(scenario.reset_probability) || scenario.reset_probability < 0.0 ||
        scenario.reset_probability > 1.0)
        throw std::runtime_error("reset_probability must be between 0 and 1");
    const bool loopback_control =
        scenario.control_host == "127.0.0.1" || scenario.control_host == "::1" || scenario.control_host == "localhost";
    if (!loopback_control && scenario.control_token.size() < 16)
        throw std::runtime_error("non-loopback control API requires a token with at least 16 characters");
    if (!loopback_control && !scenario.allow_insecure_remote_control)
        throw std::runtime_error("non-loopback control API requires allow_insecure_remote=true or TLS termination");
    for (const auto &origin : scenario.control_allowed_origins)
    {
        if (!valid_origin(origin))
            throw std::runtime_error("control.allowed_origins must contain exact http or https origins");
    }
    if ((scenario.blackout_after_ms == 0) != (scenario.blackout_duration_ms == 0))
    {
        throw std::runtime_error("blackout_after_ms and blackout_duration_ms must either both be zero or both be set");
    }
    validate_policy(scenario.upstream, "upstream");
    validate_policy(scenario.downstream, "downstream");
    if (scenario.stages.size() > 256)
        throw std::runtime_error("scenario must contain at most 256 stages");
    std::uint64_t total_duration = 0;
    for (std::size_t index = 0; index < scenario.stages.size(); ++index)
    {
        const auto &stage = scenario.stages[index];
        if (stage.name.empty())
            throw std::runtime_error("stage name must not be empty");
        if (index + 1 < scenario.stages.size() && stage.duration_ms == 0)
            throw std::runtime_error("only the final stage may have duration_ms=0");
        if (total_duration > UINT64_MAX - stage.duration_ms)
            throw std::runtime_error("stage duration is too large");
        total_duration += stage.duration_ms;
        if (stage.idle_timeout_ms < 100)
            throw std::runtime_error("stage idle_timeout_ms must be at least 100");
        if (!std::isfinite(stage.reset_probability) || stage.reset_probability < 0.0 || stage.reset_probability > 1.0)
            throw std::runtime_error("stage reset_probability must be between 0 and 1");
        if ((stage.blackout_after_ms == 0) != (stage.blackout_duration_ms == 0))
            throw std::runtime_error("stage blackout settings must either both be zero or both be set");
        validate_policy(stage.upstream, "stage upstream");
        validate_policy(stage.downstream, "stage downstream");
    }
}

std::string describe(const Scenario &s)
{
    std::ostringstream out;
    out << "scenario=" << s.name << '\n'
        << "experiment_id=" << (s.experiment_id.empty() ? "-" : s.experiment_id) << '\n'
        << "listen=" << s.listen_host << ':' << s.listen_port << '\n'
        << "upstream=" << s.upstream_host << ':' << s.upstream_port << '\n'
        << "connect_timeout=" << s.connect_timeout_ms << "ms\n"
        << "max_connections=" << s.max_connections << '\n'
        << "max_queued_bytes=" << s.max_queued_bytes << '\n'
        << "max_total_queued_bytes=" << s.max_total_queued_bytes << '\n'
        << "control=" << s.control_host << ':' << s.control_port << '\n'
        << "control_auth=" << (s.control_token.empty() ? "loopback" : "token") << '\n'
        << "control_transport=" << (s.allow_insecure_remote_control ? "insecure-http-opt-in" : "loopback") << '\n'
        << "seed=" << s.seed << '\n'
        << "upstream_policy=latency:" << s.upstream.latency_ms << "ms,jitter:" << s.upstream.jitter_ms
        << "ms,bandwidth:" << s.upstream.bandwidth_kbps << "kbps\n"
        << "downstream_policy=latency:" << s.downstream.latency_ms << "ms,jitter:" << s.downstream.jitter_ms
        << "ms,bandwidth:" << s.downstream.bandwidth_kbps << "kbps\n"
        << "idle_timeout=" << s.idle_timeout_ms << "ms\n"
        << "reset_probability=" << s.reset_probability;
    out << "\nstages=" << s.stages.size();
    if (s.blackout_duration_ms != 0)
    {
        out << "\nblackout=after:" << s.blackout_after_ms << "ms,duration:" << s.blackout_duration_ms << "ms";
    }
    return out.str();
}

std::string scenario_json(const Scenario &s)
{
    std::ostringstream out;
    out << "{\"name\":\"" << json_escape(s.name) << "\",\"experiment_id\":\"" << json_escape(s.experiment_id)
        << "\",\"seed\":" << s.seed << ",\"proxy\":{\"listen\":\"" << json_escape(s.listen_host) << ':' << s.listen_port
        << "\",\"upstream\":\"" << json_escape(s.upstream_host) << ':' << s.upstream_port
        << "\",\"connect_timeout_ms\":" << s.connect_timeout_ms << ",\"max_connections\":" << s.max_connections
        << ",\"max_queued_bytes\":" << s.max_queued_bytes << ",\"max_total_queued_bytes\":" << s.max_total_queued_bytes
        << "},\"control\":{\"listen\":\"" << json_escape(s.control_host) << ':' << s.control_port
        << "\",\"authentication\":\"" << (s.control_token.empty() ? "loopback" : "token") << "\",\"transport\":\""
        << (s.allow_insecure_remote_control ? "insecure-http" : "loopback-http")
        << "\"},\"faults\":{\"idle_timeout_ms\":" << s.idle_timeout_ms
        << ",\"blackout_after_ms\":" << s.blackout_after_ms << ",\"blackout_duration_ms\":" << s.blackout_duration_ms
        << ",\"reset_probability\":" << s.reset_probability
        << "},\"upstream\":{\"latency_ms\":" << s.upstream.latency_ms << ",\"jitter_ms\":" << s.upstream.jitter_ms
        << ",\"bandwidth_kbps\":" << s.upstream.bandwidth_kbps
        << "},\"downstream\":{\"latency_ms\":" << s.downstream.latency_ms << ",\"jitter_ms\":" << s.downstream.jitter_ms
        << ",\"bandwidth_kbps\":" << s.downstream.bandwidth_kbps << "},\"stages\":[";
    for (std::size_t index = 0; index < s.stages.size(); ++index)
    {
        if (index != 0)
            out << ',';
        const auto &stage = s.stages[index];
        out << "{\"name\":\"" << json_escape(stage.name) << "\",\"duration_ms\":" << stage.duration_ms
            << ",\"idle_timeout_ms\":" << stage.idle_timeout_ms << ",\"blackout_after_ms\":" << stage.blackout_after_ms
            << ",\"blackout_duration_ms\":" << stage.blackout_duration_ms
            << ",\"reset_probability\":" << stage.reset_probability
            << ",\"upstream\":{\"latency_ms\":" << stage.upstream.latency_ms
            << ",\"jitter_ms\":" << stage.upstream.jitter_ms << ",\"bandwidth_kbps\":" << stage.upstream.bandwidth_kbps
            << "},\"downstream\":{\"latency_ms\":" << stage.downstream.latency_ms
            << ",\"jitter_ms\":" << stage.downstream.jitter_ms
            << ",\"bandwidth_kbps\":" << stage.downstream.bandwidth_kbps << "}}";
    }
    out << "]}";
    return out.str();
}

}
