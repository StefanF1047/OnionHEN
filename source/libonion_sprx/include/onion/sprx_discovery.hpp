/* OnionHEN SPRX discovery — finds .prx/.sprx modules on disk and feeds the
 * catalog, so no manifest has to be written by hand.
 *
 * The directory scanner is an interface on purpose: host tests exercise all of
 * this logic without touching a PS5.
 */

#pragma once

#include <onion/sprx_catalog.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace onion::sprx {

/** One folder to look for modules in. */
struct DiscoveryRoot {
  std::string path;
  bool recursive = false;
};

/** A module file that exists on disk. */
struct DiscoveredModule {
  std::string path;      // absolute, canonical
  std::string file_name; // final path component, used as the list label
  std::string id;        // stable "auto-" + FNV-1a64(path)
  uint64_t size = 0;
  bool known = false; // already declared by this path in the catalog
};

struct SprxDiscoveryIssue {
  std::string path;
  std::string message;
};

/** Seam for tests: the daemon supplies the POSIX implementation. */
class ISprxDirectoryScanner {
public:
  virtual ~ISprxDirectoryScanner() = default;

  /** Appends candidate paths under `dir`. False on a fatal error. */
  virtual bool list(std::string_view dir, bool recursive,
                    std::vector<std::string> *out,
                    SprxDiscoveryIssue *issue) noexcept = 0;

  /** Canonicalises an absolute path. False when it does not resolve. */
  virtual bool canonicalize(std::string_view path,
                            std::string *out) noexcept = 0;

  /** True for an existing regular file; fills its size. */
  virtual bool stat_size(std::string_view path, uint64_t *size) noexcept = 0;
};

/**
 * Scans roots and turns found modules into catalog entries.
 *
 * Rules, enforced here so the UI can never show an entry that cannot load:
 *   - only .prx / .sprx, compared case-insensitively
 *   - the canonical path must stay inside the root it was found in
 *   - regular file, size in range, de-duplicated across overlapping roots
 *   - a path already declared marks the module known; nothing is re-added
 *   - merging never rewrites an existing entry, so user edits always survive
 */
class SprxDiscovery final {
public:
  explicit SprxDiscovery(ISprxDirectoryScanner &scanner) noexcept;

  /** Deterministic: sorted by path, de-duplicated, catalog-checked. */
  std::vector<DiscoveredModule>
  scan(const std::vector<DiscoveryRoot> &roots, const SprxCatalog &existing,
       std::vector<SprxDiscoveryIssue> *issues = nullptr) const;

  /**
   * Catalog entry for a discovered module: enabled, auto-start, and matching
   * every PS4 (CUSA) title, which is the "load for every game" default.
   */
  static SprxManifestEntry make_entry(const DiscoveredModule &module);

  /** Adds unknown modules to `catalog`. Never rewrites an existing entry. */
  size_t merge(SprxCatalog *catalog,
               const std::vector<DiscoveredModule> &found) const;

  static bool is_module_name(std::string_view name) noexcept;
  static std::string make_id(std::string_view path);

private:
  ISprxDirectoryScanner &scanner_;
};

/** Roots used when the user has not configured any. */
std::vector<DiscoveryRoot> default_roots();

/** Title-ID prefix applied to discovered modules (PS4 titles on PS5). */
inline constexpr std::string_view kDiscoveredTitlePrefix = "CUSA";

/** Default delay for discovered modules: after the splash, once the game runs. */
inline constexpr int32_t kDiscoveredDefaultDelayMs = 1000;

} // namespace onion::sprx
