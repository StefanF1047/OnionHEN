/* OnionHEN SPRX discovery — POSIX adapter for ISprxDirectoryScanner. */

#pragma once

#include <onion/sprx_discovery.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace onion::sprx {

/**
 * Real filesystem scanner used by the daemon. Its own translation unit so host
 * tests can link discovery without pulling in opendir/realpath.
 */
class RealSprxDirectoryScanner final : public ISprxDirectoryScanner {
public:
  bool list(std::string_view dir, bool recursive,
            std::vector<std::string> *out,
            SprxDiscoveryIssue *issue) noexcept override;

  bool canonicalize(std::string_view path, std::string *out) noexcept override;

  bool stat_size(std::string_view path, uint64_t *size) noexcept override;
};

} // namespace onion::sprx
