#pragma once

#include <onion/sprx_catalog.hpp>
#include <onion/sprx_deps.hpp>
#include <onion/sprx_discovery.hpp>
#include <onion/sprx_loader.hpp>

#include <string>
#include <string_view>
#include <mutex>
#include <vector>

namespace onion::sprx {

struct SprxTarget {
  pid_t pid = -1;
  std::string title_id;
};

class ISprxTargetProvider {
public:
  virtual ~ISprxTargetProvider() = default;
  virtual bool current(SprxTarget *out) noexcept = 0;
};

struct SprxPluginResult {
  std::string id;
  LoadResult load;
  bool skipped_dependency = false;
};

struct SprxStartupReport {
  bool catalog_loaded = false;
  std::vector<SprxCatalogIssue> issues;
  std::vector<SprxPluginResult> results;
};

/** Coordinates catalog selection and loading for one current game target. */
class SprxPluginManager final {
public:
  SprxPluginManager(IRemoteSprxRuntime &runtime,
                    ITargetAccessPolicy &access_policy,
                    ISprxTargetProvider &target_provider) noexcept;

  bool load_catalog(std::string_view path,
                    std::vector<SprxCatalogIssue> *issues = nullptr);

  /**
   * Loads (or creates) the catalog at `catalog_path`, then adds every module
   * found under `roots`. Returns the number of entries added.
   */
  size_t discover(std::string_view catalog_path,
                  const std::vector<DiscoveryRoot> &roots);
  SprxStartupReport start();
  SprxStartupReport reconcile();
  std::vector<SprxInventoryEntry> inventory() const;
  SprxOperationResult set_enabled(std::string_view id, bool enabled);
  /** Persists a new per-plugin delay (0..60000 ms). */
  SprxOperationResult set_delay(std::string_view id, int32_t delay_ms);
  /** Persists the main-thread flag for one plugin. */
  SprxOperationResult set_main_thread(std::string_view id, bool main_thread);
  /** Persists the game (Title ID) a plugin belongs to. */
  SprxOperationResult set_title(std::string_view id, std::string_view title_id);
  SprxOperationResult remove(std::string_view id);
  void stop() noexcept;

  SprxCatalog catalog() const { return catalog_store_.snapshot(); }

private:
  SprxStartupReport start_locked();
  /** Loads any declared dependency the target has not mapped yet. */
  void preload_missing_dependencies(SprxLoader &loader, const SprxTarget &target,
                                    const std::string &plugin_path);

  IRemoteSprxRuntime &runtime_;
  ITargetAccessPolicy &access_policy_;
  ISprxTargetProvider &target_provider_;
  SprxCatalogStore catalog_store_;
  mutable std::mutex mutex_;
};

} // namespace onion::sprx
