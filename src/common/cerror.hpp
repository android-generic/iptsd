/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef IPTSD_COMMON_CERROR_HPP
#define IPTSD_COMMON_CERROR_HPP

#include <cerrno>
#include <string>
#include <system_error>

namespace iptsd::common {

inline auto cerror(const std::string &msg) -> std::system_error
{
	return std::system_error {std::error_code {errno, std::system_category()}, msg};
}

} /* namespace iptsd::common */

#endif /* IPTSD_COMMON_CERROR_HPP */
