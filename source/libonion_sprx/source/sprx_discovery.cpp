/* OnionHEN SPRX discovery — pure logic, no syscalls. */

#include <onion/sprx_discovery.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace onion::sprx {
namespace {

constexpr uint64_t kMaxModuleBytes = 512ull * 1024 * 1024;
constexpr size_t kMaxListedPerRoot = 4096;

bool starts_with_absolute(std::string_view path) noexcept {
  return !path.empty() && path.front() == '/';
}

/** `path` must be `root` itself or live under `root/`. */
bool path_is_under(std::string_view path, std::string_view root) noexcept {
  if (!starts_with_absolute(path) || !starts_with_absolute(root))
    return false;
  if (path == root)
    return true;
  if (path.size() <= root.size() || path.compare(0, root.size(), root) != 0)
    return false;
  // "/data" must not accept "/database/x".
  return root == "/" || path[root.size()] == '/';
}

std::string file_name_of(std::string_view path) {
  const size_t slash = path.find_last_of('/');
  return std::string(slash == std::string_view::npos ? path
                                                    : path.substr(slash + 1));
}

bool catalog_declares_path(const SprxCatalog &catalog, std::string_view path,
                           ISprxDirectoryScanner &scanner) {
  for (const SprxManifestEntry &entry : catalog.entries()) {
    if (entry.path == path)
      return true;
    std::string canonical;
    if (scanner.canonicalize(entry.path, &canonical) && canonical == path)
      return true;
  }
  return false;
}

bool equals_ci(std::string_view value, std::string_view expected) noexcept {
  if (value.size() != expected.size())
    return false;
  for (size_t index = 0; index < value.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(value[index])) != expected[index])
      return false;
  }
  return true;
}

} // namespace

bool SprxDiscovery::is_module_name(std::string_view name) noexcept {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size())
    return false;
  const std::string_view extension = name.substr(dot + 1);
  return equals_ci(extension, "prx") || equals_ci(extension, "sprx");
}

std::string SprxDiscovery::make_id(std::string_view path) {
  uint64_t hash = 1469598103934665603ull;
  for (const char raw : path) {
    hash ^= static_cast<unsigned char>(raw);
    hash *= 1099511628211ull;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  char buffer[17];
  for (int index = 15; index >= 0; --index) {
    buffer[index] = kHex[hash & 0xf];
    hash >>= 4;
  }
  buffer[16] = '\0';
  return std::string("auto-") + buffer;
}

SprxDiscovery::SprxDiscovery(ISprxDirectoryScanner &scanner) noexcept
    : scanner_(scanner) {}

SprxManifestEntry SprxDiscovery::make_entry(const DiscoveredModule &module) {
  SprxManifestEntry entry;
  entry.id = module.id;
  entry.path = module.path;
  entry.title_id_prefixes.emplace_back(kDiscoveredTitlePrefix);
  entry.enabled = true;    // the list toggle, i.e. ploader's "inject"
  entry.auto_start = true; // load for every CUSA title
  entry.priority = 0;
  // Injecting during the splash screen resolves imports too early and the game
  // dies with PRX_NOT_RESOLVED_FUNCTION, so discovered modules wait until the
  // game's own initialisation has run. Tune per plugin in catalog.ini.
  entry.delay_ms = kDiscoveredDefaultDelayMs;
  return entry;
}

std::vector<DiscoveredModule>
SprxDiscovery::scan(const std::vector<DiscoveryRoot> &roots,
                    const SprxCatalog &existing,
                    std::vector<SprxDiscoveryIssue> *issues) const {
  std::vector<DiscoveredModule> found;
  if (issues)
    issues->clear();

  std::set<std::string> seen;
  for (const DiscoveryRoot &root : roots) {
    if (!starts_with_absolute(root.path)) {
      if (issues)
        issues->push_back({root.path, "scan root is not an absolute path"});
      continue;
    }

    std::string canonical_root;
    if (!scanner_.canonicalize(root.path, &canonical_root)) {
      if (issues)
        issues->push_back({root.path, "scan root cannot be resolved"});
      continue;
    }

    std::vector<std::string> listed;
    SprxDiscoveryIssue list_issue;
    if (!scanner_.list(canonical_root, root.recursive, &listed, &list_issue)) {
      if (issues) {
        issues->push_back(list_issue.path.empty()
                              ? SprxDiscoveryIssue{canonical_root,
                                                   list_issue.message}
                              : list_issue);
      }
      continue;
    }
    if (listed.size() > kMaxListedPerRoot)
      listed.resize(kMaxListedPerRoot);

    for (const std::string &candidate : listed) {
      if (!is_module_name(candidate))
        continue;

      std::string path;
      if (!scanner_.canonicalize(candidate, &path)) {
        if (issues)
          issues->push_back({candidate, "module path cannot be resolved"});
        continue;
      }
      if (!path_is_under(path, canonical_root)) {
        if (issues)
          issues->push_back({path, "module resolves outside its scan root"});
        continue;
      }

      uint64_t size = 0;
      if (!scanner_.stat_size(path, &size)) {
        if (issues)
          issues->push_back({path, "module is not a readable regular file"});
        continue;
      }
      if (size == 0 || size > kMaxModuleBytes) {
        if (issues)
          issues->push_back({path, "module size is out of range"});
        continue;
      }
      if (!seen.insert(path).second)
        continue;

      DiscoveredModule module;
      module.path = path;
      module.file_name = file_name_of(path);
      module.id = make_id(path);
      module.size = size;
      module.known = catalog_declares_path(existing, path, scanner_);
      found.push_back(std::move(module));
    }
  }

  std::sort(found.begin(), found.end(),
            [](const DiscoveredModule &left, const DiscoveredModule &right) {
              return left.path < right.path;
            });
  return found;
}

size_t
SprxDiscovery::merge(SprxCatalog *catalog,
                     const std::vector<DiscoveredModule> &found) const {
  if (!catalog)
    return 0;

  size_t added = 0;
  for (const DiscoveredModule &module : found) {
    if (module.known)
      continue;
    if (catalog_declares_path(*catalog, module.path, scanner_))
      continue;
    if (catalog->add(make_entry(module)))
      ++added;
  }
  return added;
}

std::vector<DiscoveryRoot> default_roots() {
  std::vector<DiscoveryRoot> roots;
  roots.push_back({"/data/plugins", false});
  roots.push_back({"/data/OnionHEN/sprx", false});
  return roots;
}

} // namespace onion::sprx
