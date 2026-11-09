#include <onion/sprx_plugin_manager.hpp>

#include <onion/log.h>
#include <onion/sprx_directory_scanner.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits.h>
#include <mutex>
#include <set>
#include <thread>

namespace onion::sprx {
namespace {

void append_issues(std::vector<SprxCatalogIssue> *dst,
                   const std::vector<SprxCatalogIssue> &src) {
  if (dst)
    dst->insert(dst->end(), src.begin(), src.end());
}

std::string_view basename_of(std::string_view path) noexcept {
  const size_t slash = path.find_last_of('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/** How long past its delay a plugin keeps retrying while the game boots. */
constexpr int64_t kLoadRetryWindowMs = 20000;
constexpr int64_t kLoadRetrySpacingMs = 2000;

/**
 * Statuses that mean "the game is not ready yet" rather than "this module is
 * wrong". A splash screen has no loader and no imports; a few seconds later the
 * same load succeeds. Everything else (bad path, policy denial, cleanup failure)
 * stops immediately.
 */
/**
 * Logs what the module file actually is. A loadable PRX/SPRX must be a plain
 * ELF shared object: magic \x7fELF, e_type ET_DYN (3), e_machine EM_X86_64 (62).
 * A signed SELF has a different magic; a static ET_EXEC or a wrong machine both
 * make sceKernelLoadStartModule return ENOEXEC (0x80020008), which is what we
 * are seeing, so state all three facts instead of guessing.
 */
void log_module_header(const std::string &path) {
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    LOG_WARN("[sprx-manager] cannot open %s", path.c_str());
    return;
  }
  unsigned char header[20] = {};
  const size_t read = std::fread(header, 1, sizeof(header), file);
  std::fclose(file);
  if (read < sizeof(header)) {
    LOG_WARN("[sprx-manager] %s is shorter than an ELF header (%zu bytes)",
             path.c_str(), read);
    return;
  }
  const bool elf = header[0] == 0x7f && header[1] == 'E' && header[2] == 'L' &&
                   header[3] == 'F';
  // ELF64 stores e_type and e_machine little-endian at offsets 16 and 18.
  const unsigned type = header[16] | (header[17] << 8);
  const unsigned machine = header[18] | (header[19] << 8);
  LOG_INFO("[sprx-manager] %s magic=%02x%02x%02x%02x elf=%d e_type=%u "
           "e_machine=%u (want elf=1 e_type=3 e_machine=62)",
           path.c_str(), header[0], header[1], header[2], header[3],
           elf ? 1 : 0, type, machine);
}

bool worth_retrying(LoadStatus status) noexcept {
  switch (status) {
  case LoadStatus::ResolveFailed:
  case LoadStatus::AttachFailed:
  case LoadStatus::TargetNotFound:
  case LoadStatus::AllocationFailed:
  case LoadStatus::Timeout:
  case LoadStatus::RetryExhausted:
  case LoadStatus::RemoteError:
  case LoadStatus::AccessPrepareFailed:
    return true;
  default:
    return false;
  }
}

std::string canonical_module_name(std::string_view path) {
  const std::string input(path);
  char resolved[PATH_MAX] = {};
  if (realpath(input.c_str(), resolved))
    return std::string(basename_of(resolved));
  return std::string(basename_of(path));
}

} // namespace

SprxPluginManager::SprxPluginManager(IRemoteSprxRuntime &runtime,
                                     ITargetAccessPolicy &access_policy,
                                     ISprxTargetProvider &target_provider) noexcept
    : runtime_(runtime), access_policy_(access_policy),
      target_provider_(target_provider) {}

bool SprxPluginManager::load_catalog(
    std::string_view path, std::vector<SprxCatalogIssue> *issues) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (issues)
    issues->clear();
  return catalog_store_.load(path, issues);
}

size_t SprxPluginManager::discover(std::string_view catalog_path,
                                   const std::vector<DiscoveryRoot> &roots) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SprxCatalogIssue> issues;
  const bool loaded = catalog_store_.load_or_create(catalog_path, &issues);
  // The parser is strict: one unknown or duplicated key rejects the whole
  // manifest, so surface every issue instead of failing silently.
  for (const SprxCatalogIssue &issue : issues)
    LOG_WARN("[sprx-manager] catalog line=%zu %s: %s", issue.line,
             issue.plugin_id.c_str(), issue.message.c_str());
  if (!loaded)
    return 0;

  RealSprxDirectoryScanner scanner;
  SprxDiscovery discovery(scanner);
  std::vector<SprxDiscoveryIssue> scan_issues;
  const std::vector<DiscoveredModule> found =
      discovery.scan(roots, catalog_store_.snapshot(), &scan_issues);
  for (const SprxDiscoveryIssue &issue : scan_issues)
    LOG_DEBUG("[sprx-manager] scan %s: %s", issue.path.c_str(),
              issue.message.c_str());

  size_t added = 0;
  for (const DiscoveredModule &module : found) {
    if (module.known)
      continue;
    if (catalog_store_.add(SprxDiscovery::make_entry(module)))
      ++added;
  }
  return added;
}

SprxStartupReport SprxPluginManager::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  return start_locked();
}

SprxStartupReport SprxPluginManager::reconcile() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!catalog_store_.path().empty()) {
    std::vector<SprxCatalogIssue> issues;
    if (!catalog_store_.reload(&issues)) {
      return {false, std::move(issues), {}};
    }
  }
  return start_locked();
}

void SprxPluginManager::preload_missing_dependencies(
    SprxLoader &loader, const SprxTarget &target,
    const std::string &plugin_path) {
  const std::vector<std::string> declared =
      read_declared_dependencies(plugin_path);
  if (declared.empty())
    return;

  const std::vector<std::string> dirs = dependency_search_dirs();
  size_t already_loaded = 0;
  size_t preloaded = 0;
  size_t unavailable = 0;

  for (const std::string &name : declared) {
    ModuleInfo info;
    if (runtime_.find_loaded(target.pid, name, &info)) {
      ++already_loaded;
      continue;
    }

    bool ok = false;
    for (const std::string &dir : dirs) {
      LoadRequest request;
      request.pid = target.pid;
      request.title_id = target.title_id;
      request.path = dir + name;
      request.options.timeout_ms = 20000;
      if (loader.load(request).succeeded()) {
        ok = true;
        break;
      }
    }
    if (ok) {
      ++preloaded;
      LOG_INFO("[sprx-manager] preloaded %s for %s", name.c_str(),
               plugin_path.c_str());
    } else {
      ++unavailable;
      LOG_WARN("[sprx-manager] %s: cannot preload %s", plugin_path.c_str(),
               name.c_str());
    }
  }

  LOG_INFO("[sprx-manager] deps for %s: %zu declared, %zu loaded, %zu preloaded, "
           "%zu unavailable",
           plugin_path.c_str(), declared.size(), already_loaded, preloaded,
           unavailable);
}

SprxStartupReport SprxPluginManager::start_locked() {
  SprxStartupReport report;
  report.catalog_loaded = catalog_store_.loaded();
  if (!report.catalog_loaded)
    return report;

  SprxTarget target;
  if (!target_provider_.current(&target)) {
    LOG_DEBUG("[sprx-manager] no current game target");
    return report;
  }

  // Per-plugin delays are relative to this moment (the game start), not to each
  // other, so a plugin with delay_ms=3000 always waits ~3 s after launch.
  const auto launch_time = std::chrono::steady_clock::now();

  const SprxCatalog catalog = catalog_store_.snapshot();
  std::vector<SprxCatalogIssue> order_issues;
  const auto order = catalog.startup_order(target.title_id, &order_issues);
  append_issues(&report.issues, order_issues);
  if (order.empty())
    return report;

  TitleIdAllowlist allowlist;
  if (!allowlist.add_exact(target.title_id)) {
    report.issues.push_back({0, {}, "invalid current target Title ID"});
    return report;
  }
  SprxLoader loader(runtime_, access_policy_, allowlist);
  std::set<std::string> failed;
  for (const SprxManifestEntry *entry : order) {
    SprxPluginResult item;
    item.id = entry->id;
    for (const std::string &dependency : entry->dependencies) {
      if (failed.contains(dependency)) {
        item.skipped_dependency = true;
        failed.insert(entry->id);
        break;
      }
    }
    if (!item.skipped_dependency) {
      if (entry->delay_ms > 0) {
        const auto due =
            launch_time + std::chrono::milliseconds(entry->delay_ms);
        const auto now = std::chrono::steady_clock::now();
        if (due > now)
          std::this_thread::sleep_for(due - now);
        SprxTarget still_running;
        if (!target_provider_.current(&still_running) ||
            still_running.pid != target.pid) {
          report.issues.push_back({0, entry->id, "target exited before load"});
          break;
        }
      }
      // Retry until the game is ready. delay_ms is the minimum wait; after that
      // we keep trying for kLoadRetryWindowMs so a slow boot still gets the
      // plugin in, instead of giving up 200 ms after the first attempt.
      log_module_header(entry->path);
      // The game loads its libraries lazily, so at a specific injection moment
      // a declared dependency is often still missing and the loader answers
      // ENOEXEC. Load the gaps ourselves before touching the plugin.
      preload_missing_dependencies(loader, target, entry->path);
      const auto deadline =
          launch_time + std::chrono::milliseconds(entry->delay_ms) +
          std::chrono::milliseconds(kLoadRetryWindowMs);
      for (uint32_t attempt = 1;; ++attempt) {
        LoadRequest request;
        request.pid = target.pid;
        request.title_id = target.title_id;
        request.path = entry->path;
        // A large PRX inside a busy game can take well over the 5 s default.
        request.options.timeout_ms = 20000;
        item.load = loader.load(request);
        if (item.load.succeeded() || !worth_retrying(item.load.status) ||
            std::chrono::steady_clock::now() >= deadline)
          break;
        LOG_INFO("[sprx-manager] %s not ready (%s), retry %u in %lld ms",
                 entry->id.c_str(), load_status_name(item.load.status), attempt,
                 static_cast<long long>(kLoadRetrySpacingMs));
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kLoadRetrySpacingMs));
        SprxTarget still_running;
        if (!target_provider_.current(&still_running) ||
            still_running.pid != target.pid) {
          report.issues.push_back({0, entry->id, "target exited during retry"});
          break;
        }
      }
      if (!item.load.succeeded()) {
        LOG_WARN("[sprx-manager] %s failed: %s remote=%#llx pid=%d",
                 entry->id.c_str(), load_status_name(item.load.status),
                 static_cast<unsigned long long>(item.load.remote_result),
                 static_cast<int>(target.pid));
        failed.insert(entry->id);
      }
    }
    report.results.push_back(std::move(item));
  }
  return report;
}

std::vector<SprxInventoryEntry> SprxPluginManager::inventory() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SprxInventoryEntry> entries = catalog_store_.inventory();
  const SprxCatalog catalog = catalog_store_.snapshot();
  SprxTarget target;
  if (!target_provider_.current(&target))
    return entries;
  for (SprxInventoryEntry &item : entries) {
    const SprxManifestEntry *entry = catalog.find(item.id);
    if (!entry || !catalog.matches(*entry, target.title_id))
      continue;
    item.matches_current_target = true;
    ModuleInfo module;
    const std::string module_name = canonical_module_name(item.path);
    item.loaded_for_current_target = !module_name.empty() &&
                                     runtime_.find_loaded(target.pid, module_name,
                                                          &module);
  }
  return entries;
}

SprxOperationResult SprxPluginManager::set_enabled(std::string_view id,
                                                    bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  return catalog_store_.set_enabled(id, enabled);
}

SprxOperationResult SprxPluginManager::set_delay(std::string_view id,
                                                 int32_t delay_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  return catalog_store_.set_delay(id, delay_ms);
}

SprxOperationResult SprxPluginManager::set_main_thread(std::string_view id,
                                                       bool main_thread) {
  std::lock_guard<std::mutex> lock(mutex_);
  return catalog_store_.set_main_thread(id, main_thread);
}

SprxOperationResult SprxPluginManager::set_title(std::string_view id,
                                                 std::string_view title_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return catalog_store_.set_title(id, title_id);
}

SprxOperationResult SprxPluginManager::remove(std::string_view id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const SprxCatalog catalog = catalog_store_.snapshot();
  const SprxManifestEntry *entry = catalog.find(id);
  if (!entry)
    return {false, "SPRX plugin is not declared"};
  SprxTarget target;
  if (target_provider_.current(&target) &&
      catalog.matches(*entry, target.title_id)) {
    ModuleInfo module;
    const std::string module_name = canonical_module_name(entry->path);
    if (!module_name.empty() && runtime_.find_loaded(target.pid, module_name,
                                                     &module)) {
      return {false, "SPRX plugin is loaded in the current game"};
    }
  }
  return catalog_store_.remove(id);
}

void SprxPluginManager::stop() noexcept {
  /* SPRX modules belong to the target process; unload is intentionally absent. */
}

} // namespace onion::sprx
