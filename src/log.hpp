#pragma once

#include <iosfwd>
#include <string_view>

namespace faultline::detail
{

void write_log_line(std::ostream &stream, std::string_view line);

}
