#include "log.hpp"

#include <mutex>
#include <ostream>

namespace faultline::detail
{

void write_log_line(std::ostream &stream, std::string_view line)
{
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    stream << line << '\n';
    stream.flush();
}

}
