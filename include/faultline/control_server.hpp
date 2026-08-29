#pragma once

#include "faultline/config.hpp"
#include "faultline/proxy.hpp"

#include <iosfwd>
#include <memory>
#include <stop_token>

namespace faultline
{

class ControlServer
{
  public:
    ControlServer(ProxyServer &proxy, std::ostream &log_stream);
    ~ControlServer();

    ControlServer(const ControlServer &) = delete;
    ControlServer &operator=(const ControlServer &) = delete;

    void run(const std::stop_token &stop_token = {});
    void request_stop() noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}
