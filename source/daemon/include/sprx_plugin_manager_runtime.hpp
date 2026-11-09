#pragma once

#include <cstdint>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include <onion/sprx_catalog.hpp>

namespace onion::daemon::sprx_plugins {

inline constexpr const char *kCatalogPath =
    "/data/OnionHEN/sprx/catalog.ini";

void start();
void reconcile();
std::vector<onion::sprx::SprxInventoryEntry> inventory();
onion::sprx::SprxOperationResult set_enabled(std::string_view id,
                                              bool enabled);
/** Persists a new per-plugin delay and rewrites ploader.toml. */
onion::sprx::SprxOperationResult set_delay(std::string_view id,
                                            int32_t delay_ms);
/** Persists the main-thread flag for one module. */
onion::sprx::SprxOperationResult set_main_thread(std::string_view id,
                                                  bool main_thread);
/** Loader-level settings: timeout_s, expose_data, scan paths. */
bool set_loader_settings(int timeout_s, bool expose_data,
                         const std::string &scan_paths);
/** JSON snapshot of the loader settings for the Toolbox. */
std::string loader_settings_json();
/** JSON snapshot of the installed PS4 titles. */
std::string games_json();
/** Binds one module to a single title. */
onion::sprx::SprxOperationResult set_title(std::string_view id,
                                            std::string_view title_id);
/** Rescans the plugin folders and rewrites ploader.toml. Returns entries added. */
size_t rescan();
/** Rewrites /data/plugins/ploader.toml from the current catalog. */
void sync_ploader_config();
/** Removes only the manifest entry; it never deletes the module file. */
onion::sprx::SprxOperationResult remove(std::string_view id);
void on_big_app_started(pid_t pid, uint32_t app_id,
                        std::string_view title_id);
void on_big_app_exited(pid_t pid, uint32_t app_id,
                       std::string_view title_id);
void stop();

} // namespace onion::daemon::sprx_plugins
