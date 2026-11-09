#include "sprx_plugin_manager_runtime.hpp"

#include "daemon_ops.hpp"
#include "daemon_jailbreak.hpp"

#include <elfldr_remote.h>

#include <onion/log.h>
#include <onion/proc_query.h>
#include <onion/sprx_plugin_manager.hpp>

#include <ps5/kernel.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

/* Bundled by embedded_payloads.c (.incbin of third_party/ploader/ploader.elf).
 * Declared at global scope: an anonymous-namespace declaration would get
 * internal linkage and never resolve to the symbol the asm block defines. */
extern uint8_t ploader_elf_start[];
extern uint32_t ploader_elf_size;

namespace onion::daemon::sprx_plugins {
namespace {

/** Scan folders are created before discovery so the catalog can be persisted. */
constexpr const char *kSprxDirectory = "/data/OnionHEN/sprx";

/** FreeBSD nmount(2); the SDK's <sys/mount.h> does not declare it. */
extern "C" int nmount(struct iovec *iov, unsigned int niov, int flags);

/**
 * Mirrors /data into the target's own sandbox at <rootdir>/data with nullfs.
 *
 * The game keeps its rootdir and jaildir, so its asset mounts, saves and
 * relative paths are untouched; only /data gains the host view. This is what
 * lets a sandboxed game open a module under /data and lets the plugin read its
 * own files there. Returns true when the mirror is in place.
 */
/** Finds /mnt/sandbox/<title>_xxx when the sandbox is enforced by jaildir. */
std::string find_sandbox_dir(std::string_view title_id) {
  if (title_id.size() != 9)
    return {};
  for (int slot = 0; slot <= 50; ++slot) {
    char candidate[64] = {};
    std::snprintf(candidate, sizeof(candidate), "/mnt/sandbox/%.*s_%03d",
                  static_cast<int>(title_id.size()), title_id.data(), slot);
    struct stat info {};
    if (stat(candidate, &info) == 0 && S_ISDIR(info.st_mode))
      return std::string(candidate);
  }
  return {};
}

/**
 * Mirrors /data into the target's own sandbox at <sandbox>/data with nullfs.
 *
 * Two ways to find the sandbox: a process whose rootdir is already the sandbox
 * (payload apps), or - the normal case for a PS4 title on PS5 - a rootdir of "/"
 * where the sandbox lives in jaildir, so we scan /mnt/sandbox/<TITLE>_xxx.
 * Returns true when the mirror is in place.
 */
bool expose_data_in_sandbox(pid_t pid, std::string_view title_id) {
  std::string base;
  const std::string proc_root = "/proc/" + std::to_string(pid) + "/root";
  char root[PATH_MAX] = {};
  const ssize_t length =
      readlink(proc_root.c_str(), root, sizeof(root) - 1);
  if (length > 0) {
    root[length] = '\0';
    std::string trimmed(root);
    while (trimmed.size() > 1 && trimmed.back() == '/')
      trimmed.pop_back();
    if (trimmed != "/")
      base = trimmed;
  } else {
    LOG_WARN("[sprx-manager] readlink %s: %s", proc_root.c_str(),
             std::strerror(errno));
  }

  if (base.empty()) {
    base = find_sandbox_dir(title_id);
    if (base.empty()) {
      LOG_WARN("[sprx-manager] no sandbox directory for %.*s_xxx",
               static_cast<int>(title_id.size()), title_id.data());
      return false;
    }
  }

  const std::string mount_point = base + "/data";
  if (mkdir(mount_point.c_str(), 0755) != 0 && errno != EEXIST) {
    LOG_WARN("[sprx-manager] mkdir %s: %s", mount_point.c_str(),
             std::strerror(errno));
    return false;
  }

  struct iovec iov[6] = {};
  iov[0].iov_base = const_cast<char *>("fstype");
  iov[0].iov_len = sizeof("fstype");
  iov[1].iov_base = const_cast<char *>("nullfs");
  iov[1].iov_len = sizeof("nullfs");
  iov[2].iov_base = const_cast<char *>("fspath");
  iov[2].iov_len = sizeof("fspath");
  iov[3].iov_base = const_cast<char *>(mount_point.data());
  iov[3].iov_len = mount_point.size() + 1;
  iov[4].iov_base = const_cast<char *>("from");
  iov[4].iov_len = sizeof("from");
  iov[5].iov_base = const_cast<char *>("/data");
  iov[5].iov_len = sizeof("/data");

  if (nmount(iov, 6, 0) == 0) {
    LOG_INFO("[sprx-manager] mirrored /data into %s", mount_point.c_str());
    return true;
  }
  // A repeat launch finds the mirror already mounted; that is success for us.
  const int error = errno;
  LOG_WARN("[sprx-manager] nullfs /data -> %s: %s", mount_point.c_str(),
           std::strerror(error));
  return error == EBUSY;
}

constexpr const char *kPloaderElf = "/data/plugins/ploader.elf";
constexpr const char *kPloaderConfig = "/data/plugins/ploader.toml";
/** Plugin folder: holds ploader.elf, its TOML and the user's modules. */
constexpr const char *kPluginDir = "/data/plugins";
/** Optional escape hatch: touch this file to keep injection inside the daemon. */
constexpr const char *kPloaderDisable = "/data/plugins/.no_ploader";
constexpr const char *kLoaderSettingsPath = "/data/OnionHEN/sprx/loader.ini";

/** Loader-level settings: they belong to [loader] in the generated TOML. */
struct LoaderSettings {
  int timeout_s = 7;
  bool expose_data = true;
  std::vector<std::string> scan_paths;
};

std::vector<std::string> default_scan_paths() {
  return {"/data", "/data/plugins", "/data/OnionHEN/sprx"};
}

std::vector<std::string> split_scan_paths(const std::string &text) {
  std::vector<std::string> paths;
  std::string current;
  for (const char c : text) {
    if (c == ';' || c == ',') {
      if (!current.empty())
        paths.push_back(current);
      current.clear();
    } else if (c != ' ' && c != '\t') {
      current.push_back(c);
    }
  }
  if (!current.empty())
    paths.push_back(current);
  return paths;
}

std::string join_scan_paths(const std::vector<std::string> &paths) {
  std::string joined;
  for (const std::string &path : paths) {
    if (!joined.empty())
      joined.push_back(';');
    joined += path;
  }
  return joined;
}

LoaderSettings load_loader_settings() {
  LoaderSettings settings;
  settings.scan_paths = default_scan_paths();

  FILE *file = std::fopen(kLoaderSettingsPath, "r");
  if (file == nullptr)
    return settings;

  char line[512];
  std::vector<std::string> paths;
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == ' '))
      text.pop_back();
    if (text.rfind("timeout_s=", 0) == 0) {
      const int value = std::atoi(text.c_str() + 10);
      settings.timeout_s = value < 1 ? 1 : (value > 3600 ? 3600 : value);
    } else if (text.rfind("expose_data=", 0) == 0) {
      settings.expose_data = std::atoi(text.c_str() + 12) != 0;
    } else if (text.rfind("scan_paths=", 0) == 0) {
      paths = split_scan_paths(text.substr(11));
    }
  }
  std::fclose(file);

  if (!paths.empty())
    settings.scan_paths = std::move(paths);
  return settings;
}

bool save_loader_settings(const LoaderSettings &settings) {
  (void)mkdir(kSprxDirectory, 0755);
  FILE *file = std::fopen(kLoaderSettingsPath, "w");
  if (file == nullptr) {
    LOG_WARN("[sprx-manager] cannot write %s", kLoaderSettingsPath);
    return false;
  }
  std::fprintf(file, "timeout_s=%d\nexpose_data=%d\nscan_paths=%s\n",
               settings.timeout_s, settings.expose_data ? 1 : 0,
               join_scan_paths(settings.scan_paths).c_str());
  std::fclose(file);
  return true;
}

/** Scan roots from the saved list. Directories are listed, never walked. */
std::vector<onion::sprx::DiscoveryRoot> discovery_roots() {
  const LoaderSettings settings = load_loader_settings();
  std::vector<onion::sprx::DiscoveryRoot> roots;
  for (const std::string &path : settings.scan_paths) {
    onion::sprx::DiscoveryRoot root;
    root.path = path;
    root.recursive = false;
    roots.push_back(std::move(root));
  }
  return roots;
}

struct GameEntry {
  std::string id;
  std::string name;
};

/** Reads titleName out of an app's param.json; empty when unavailable. */
std::string param_title_name(const std::string &title_id) {
  const std::string path = "/user/appmeta/" + title_id + "/param.json";
  FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr)
    return {};
  std::string text;
  char buffer[2048];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    text.append(buffer, read);
  std::fclose(file);

  const size_t key = text.find("\"titleName\"");
  if (key == std::string::npos)
    return {};
  const size_t colon = text.find(':', key);
  if (colon == std::string::npos)
    return {};
  const size_t open = text.find('"', colon + 1);
  if (open == std::string::npos)
    return {};
  const size_t close = text.find('"', open + 1);
  if (close == std::string::npos)
    return {};
  return text.substr(open + 1, close - open - 1);
}

/** Installed PS4 titles: /user/app holds CUSAxxxxx directories. */
std::vector<GameEntry> installed_ps4_games() {
  std::vector<GameEntry> games;
  DIR *dir = opendir("/user/app");
  if (dir == nullptr)
    return games;
  while (struct dirent *entry = readdir(dir)) {
    const std::string name(entry->d_name);
    if (name.size() != 9 || name.compare(0, 4, "CUSA") != 0)
      continue;
    GameEntry game;
    game.id = name;
    game.name = param_title_name(name);
    if (game.name.empty())
      game.name = name;
    games.push_back(std::move(game));
  }
  closedir(dir);
  std::sort(games.begin(), games.end(),
            [](const GameEntry &left, const GameEntry &right) {
              return left.name < right.name;
            });
  return games;
}

std::string json_escape(const std::string &text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\')
      out.push_back('\'');
    else if (c == '\n' || c == '\r' || c == '\t')
      out.push_back(' ');
    else
      out.push_back(c);
  }
  return out;
}

/**
 * True when /data already holds exactly the bundled build. Compares content,
 * not just size, so replacing third_party/ploader/ploader.elf in the tree and
 * rebuilding always wins over a stale copy left on the console.
 */
bool matches_bundled_ploader(const char *path) {
  FILE *file = std::fopen(path, "rb");
  if (file == nullptr)
    return false;
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  const long size = std::ftell(file);
  std::rewind(file);
  if (size < 0 || static_cast<uint32_t>(size) != ploader_elf_size) {
    std::fclose(file);
    return false;
  }

  char buffer[4096];
  size_t offset = 0;
  bool same = true;
  while (offset < ploader_elf_size) {
    size_t chunk = ploader_elf_size - offset;
    if (chunk > sizeof(buffer))
      chunk = sizeof(buffer);
    if (std::fread(buffer, 1, chunk, file) != chunk ||
        std::memcmp(buffer, ploader_elf_start + offset, chunk) != 0) {
      same = false;
      break;
    }
    offset += chunk;
  }
  std::fclose(file);
  return same;
}

/**
 * Guarantees /data holds the bundled ploader build. A different file there is
 * replaced, so the payload in OnionHEN.elf is always what runs.
 */
bool ensure_ploader_on_disk() {
  if (matches_bundled_ploader(kPloaderElf))
    return true;

  (void)mkdir("/data/plugins", 0755);
  (void)mkdir("/data/plugins", 0755);
  FILE *file = std::fopen(kPloaderElf, "wb");
  if (file == nullptr) {
    LOG_WARN("[sprx-manager] cannot create %s: %s", kPloaderElf,
             std::strerror(errno));
    return false;
  }
  const size_t written =
      std::fwrite(ploader_elf_start, 1, ploader_elf_size, file);
  const bool wrote_ok = written == ploader_elf_size && std::ferror(file) == 0;
  std::fclose(file);
  if (!wrote_ok) {
    LOG_ERROR("[sprx-manager] short write (%zu of %u bytes) for %s", written,
              static_cast<unsigned>(ploader_elf_size), kPloaderElf);
    (void)unlink(kPloaderElf);
    return false;
  }
  (void)chmod(kPloaderElf, 0755);
  LOG_INFO("[sprx-manager] extracted bundled ploader (%u bytes) to %s",
           static_cast<unsigned>(ploader_elf_size), kPloaderElf);
  return true;
}

/**
 * Rewrites ploader's TOML from the catalog and the loader settings.
 *
 * `title` scopes the entries: an empty title writes "*" (the preview the
 * Toolbox shows), while a launch passes the real Title ID so a PS4 plugin can
 * never be injected into a PS5 game. The file is written to both
 * /data/plugins/ploader.toml and /data/plugin/ploader.toml so whichever path a
 * given ploader build reads, it finds the same configuration.
 */
bool write_ploader_config(const onion::sprx::SprxCatalog &catalog,
                          std::string_view title = {}) {
  const LoaderSettings settings = load_loader_settings();

  std::string text = "[loader]\n";
  text += "timeout_s = " + std::to_string(settings.timeout_s) + "\n";
  text += std::string("expose_data = ") +
          (settings.expose_data ? "true\n" : "false\n");
  text += "debug_log = true\nconsole_log = false\n\n";

  size_t written_entries = 0;

  for (const onion::sprx::SprxManifestEntry &entry : catalog.entries()) {
    if (!entry.enabled)
      continue;

    // Each plugin names its own game. An explicit Title ID wins; otherwise the
    // CUSA prefix is resolved against the title being launched. With no title
    // (the preview file the Toolbox writes) everything enabled is listed.
    std::string scope;
    if (!entry.exact_title_ids.empty()) {
      scope = entry.exact_title_ids.front();
    } else if (!title.empty()) {
      for (const std::string &prefix : entry.title_id_prefixes) {
        if (title.size() >= prefix.size() &&
            title.compare(0, prefix.size(), prefix) == 0) {
          scope = std::string(title);
          break;
        }
      }
      if (scope.empty())
        continue; // this plugin is not for this game
    } else {
      /* Preview file: only plugins that already name a game are listed, so the
       * file never contains a wildcard title. */
      if (entry.exact_title_ids.empty())
        continue;
      scope = entry.exact_title_ids.front();
    }

    text += "[[plugin]]\n";
    text += "title = \"" + scope + "\"\n";
    text += "path = \"" + entry.path + "\"\n";
    text += "delay_ms = " + std::to_string(entry.delay_ms) + "\n";
    text += "inject = true\n";
    text += std::string("main_thread = ") +
            (entry.main_thread ? "true\n" : "false\n");
    text += "\n";
    ++written_entries;
  }

  /* A preview with nothing to show must not clobber the file: discovery entries
     carry a CUSA prefix rather than an explicit title, so they are only
     resolvable while a game is being launched. */
  if (written_entries == 0 && title.empty()) {
    LOG_INFO("[sprx-manager] %s left unchanged (no plugin resolves yet)",
             kPloaderConfig);
    return false;
  }

  auto write_one = [&text](const char *path) {
    FILE *file = std::fopen(path, "w");
    if (file == nullptr)
      return false;
    const size_t written = std::fwrite(text.data(), 1, text.size(), file);
    const bool ok = written == text.size() && std::ferror(file) == 0;
    std::fclose(file);
    return ok;
  };

  /* The folder is deleted between builds on this console, so create it before
     opening the file: fopen() cannot create a missing directory. */
  (void)mkdir(kPluginDir, 0755);
  const bool primary = write_one(kPloaderConfig);
  return primary;
}

class CurrentGameTarget final : public onion::sprx::ISprxTargetProvider {
public:
  bool current(onion::sprx::SprxTarget *out) noexcept override {
    if (!out)
      return false;
    std::string title_id;
    int app_id = -1;
    const int pid = get_game_pid();
    if (pid <= 1 || !Get_Running_App_TID(title_id, app_id))
      return false;
    out->pid = static_cast<pid_t>(pid);
    out->title_id = std::move(title_id);
    return true;
  }
};

class PreparedTargetAccess final : public onion::sprx::ITargetAccessPolicy {
public:
  onion::sprx::AccessResult prepare(pid_t pid,
                                    std::string_view title_id) noexcept override {
    if (pid <= 1 || !onion_proc_is_alive(pid) || kernel_get_proc(pid) == 0)
      return {onion::sprx::AccessStatus::PrepareFailed};
    // A retail game runs sandboxed as a non-root user, so it cannot be ptraced
    // and cannot reach a module outside its sandbox. Take uid 0 only: the
    // sandbox escape (rootdir/jaildir -> /) stays OFF on purpose, because
    // rewriting a game's rootdir stalls its own asset mounts part-way through
    // loading. /data is exposed separately by mounting it into the sandbox.
    if (kernel_get_ucred_uid(pid) != 0 &&
        !daemon_jailbreak_apply_privileges(pid, /*escape_sandbox=*/false))
      return {onion::sprx::AccessStatus::PrepareFailed};
    if (!expose_data_in_sandbox(pid, title_id)) {
      // Fallback: rewrite rootdir/jaildir so /data resolves from the sandbox.
      // Less clean (the game loses its jail) but it keeps plugins loading when
      // the mirror cannot be created at this point in the launch.
      LOG_WARN("[sprx-manager] /data mirror unavailable for pid=%d, "
               "escaping the sandbox instead",
               static_cast<int>(pid));
      if (!daemon_jailbreak_apply_privileges(pid, /*escape_sandbox=*/true))
        return {onion::sprx::AccessStatus::PrepareFailed};
    }
    return kernel_get_ucred_uid(pid) == 0
               ? onion::sprx::AccessResult{onion::sprx::AccessStatus::Allowed}
               : onion::sprx::AccessResult{onion::sprx::AccessStatus::Denied};
  }

  onion::sprx::AccessResult restore(pid_t pid,
                                     std::string_view) noexcept override {
    return pid > 1 && onion_proc_is_alive(pid)
               ? onion::sprx::AccessResult{onion::sprx::AccessStatus::Allowed}
               : onion::sprx::AccessResult{onion::sprx::AccessStatus::RestoreFailed};
  }
};

struct Runtime {
  std::mutex mutex;
  onion::sprx::PtraceSprxRuntime transport;
  PreparedTargetAccess access;
  CurrentGameTarget target;
  onion::sprx::SprxPluginManager manager;

  Runtime() : manager(transport, access, target) {}
};

Runtime &runtime() {
  static Runtime value;
  return value;
}

void log_report(const char *operation,
                const onion::sprx::SprxStartupReport &report) {
  for (const auto &issue : report.issues)
    LOG_WARN("[sprx-manager] line=%zu plugin=%s: %s", issue.line,
             issue.plugin_id.empty() ? "-" : issue.plugin_id.c_str(),
             issue.message.c_str());
  size_t loaded = 0;
  size_t failed = 0;
  size_t skipped = 0;
  for (const auto &item : report.results) {
    if (item.skipped_dependency)
      ++skipped;
    else if (item.load.succeeded())
      ++loaded;
    else
      ++failed;
  }
  LOG_INFO("[sprx-manager] %s catalog=%d entries=%zu loaded=%zu failed=%zu skipped=%zu",
           operation, report.catalog_loaded ? 1 : 0, report.results.size(),
           loaded, failed, skipped);
}

/**
 * Applies delay / main_thread / title from an existing ploader.toml to the
 * catalog, so the Toolbox shows the values that are actually in the file -
 * including a config written by hand or by an older build.
 */
void import_ploader_config_locked(Runtime &current) {
  const char *candidates[] = {kPloaderConfig};
  FILE *file = nullptr;
  for (const char *candidate : candidates) {
    file = std::fopen(candidate, "r");
    if (file != nullptr)
      break;
  }
  if (file == nullptr)
    return;

  const onion::sprx::SprxCatalog catalog = current.manager.catalog();
  std::string path;
  int delay_ms = -1;
  int main_thread = -1;

  auto apply = [&]() {
    if (path.empty())
      return;
    const onion::sprx::SprxManifestEntry *entry = nullptr;
    for (const onion::sprx::SprxManifestEntry &candidate : catalog.entries()) {
      if (candidate.path == path) {
        entry = &candidate;
        break;
      }
    }
    if (entry != nullptr) {
      if (delay_ms >= 0 && delay_ms != entry->delay_ms)
        (void)current.manager.set_delay(entry->id, delay_ms);
      if (main_thread >= 0 && (main_thread != 0) != entry->main_thread)
        (void)current.manager.set_main_thread(entry->id, main_thread != 0);
      /* `title` is deliberately NOT imported: the catalog is authoritative for
         the game a plugin belongs to, otherwise a stale TOML would revert a
         selection the user just made in the Toolbox. */
    }
    path.clear();
    delay_ms = -1;
    main_thread = -1;
  };

  char line[512];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    std::string text(line);
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
      text.pop_back();
    if (text.rfind("[[plugin]]", 0) == 0) {
      apply();
      continue;
    }
    const size_t equal = text.find('=');
    if (equal == std::string::npos)
      continue;
    std::string key = text.substr(0, equal);
    std::string value = text.substr(equal + 1);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
      key.pop_back();
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '"' || value.front() == '\t'))
      value.erase(0, 1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '"'))
      value.pop_back();

    if (key == "path")
      path = value;
    else if (key == "delay_ms")
      delay_ms = std::atoi(value.c_str());
    else if (key == "main_thread")
      main_thread = std::atoi(value.c_str());
  }
  std::fclose(file);
  apply();
}

void run(const char *operation, bool reload) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  if (reload) {
    // Discovery owns catalog creation: it creates the manifest when it is
    // missing and adds every module found in the scan roots, so a fresh console
    // never needs a hand-written file. The folder must exist for the write to
    // succeed, hence the mkdir.
    (void)mkdir(kSprxDirectory, 0755);
    const size_t discovered =
        current.manager.discover(kCatalogPath, discovery_roots());
    if (discovered)
      LOG_INFO("[sprx-manager] discovered %zu new module(s)", discovered);
    // Reflect values that are already in ploader.toml before regenerating it.
    import_ploader_config_locked(current);
    if (write_ploader_config(current.manager.catalog()))
      LOG_INFO("[sprx-manager] wrote %s", kPloaderConfig);
  }
  const auto report = reload ? current.manager.start() : current.manager.reconcile();
  log_report(operation, report);
}

/**
 * Hands this title to ploader when its ELF is present, and reports whether it
 * took over. ploader owns the injection (proven on this console); OnionHEN owns
 * discovery, the catalog and the UI. Without ploader.elf the built-in loader is
 * used instead, so nothing regresses for people who do not install it.
 */
bool delegate_to_ploader(Runtime &current, std::string_view title_id) {
  // Default: ploader does the injection, launched the moment the game starts,
  // exactly as if it had been run by hand.
  if (access(kPloaderDisable, F_OK) == 0) {
    LOG_WARN("[sprx-manager] %s exists: ploader will NOT be launched",
             kPloaderDisable);
    return false;
  }
  if (!ensure_ploader_on_disk()) {
    LOG_WARN("[sprx-manager] cannot stage %s: ploader not launched",
             kPloaderElf);
    return false;
  }

  const onion::sprx::SprxCatalog catalog = current.manager.catalog();
  const std::vector<const onion::sprx::SprxManifestEntry *> order =
      catalog.startup_order(title_id, nullptr);
  if (order.empty()) {
    LOG_INFO("[sprx-manager] no enabled plugin matches %.*s: ploader not launched",
             static_cast<int>(title_id.size()), title_id.data());
    return false;
  }

  if (!write_ploader_config(catalog, title_id)) {
    LOG_ERROR("[sprx-manager] cannot write %s", kPloaderConfig);
    return false;
  }
  if (!elfldr_remote_onion_available()) {
    LOG_WARN("[sprx-manager] ploader.elf is present but the private elfldr is "
             "unavailable; using the built-in loader");
    return false;
  }

  const pid_t launched =
      elfldr_remote_onion_launch_file_get_pid(kPloaderElf, nullptr);
  if (launched <= 1) {
    LOG_WARN("[sprx-manager] ploader launch failed; using the built-in loader");
    return false;
  }

  LOG_INFO("[sprx-manager] delegated %zu plugin(s) for %.*s to ploader pid=%d",
           order.size(), static_cast<int>(title_id.size()), title_id.data(),
           static_cast<int>(launched));
  return true;
}

bool is_current_target(Runtime &current, pid_t pid, uint32_t app_id,
                       std::string_view title_id) {
  onion::sprx::SprxTarget target;
  if (!current.target.current(&target)) {
    LOG_DEBUG("[sprx-manager] lifecycle target unavailable pid=%d",
              static_cast<int>(pid));
    return false;
  }
  if (target.pid != pid || target.title_id != title_id) {
    LOG_DEBUG("[sprx-manager] stale Big App start pid=%d tid=%.*s current_pid=%d current_tid=%s",
              static_cast<int>(pid), static_cast<int>(title_id.size()),
              title_id.data(), static_cast<int>(target.pid),
              target.title_id.c_str());
    return false;
  }
  (void)app_id;
  return true;
}

} // namespace

void start() { run("startup", true); }
void reconcile() { run("reconcile", false); }

std::vector<onion::sprx::SprxInventoryEntry> inventory() {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  return current.manager.inventory();
}

onion::sprx::SprxOperationResult set_enabled(std::string_view id,
                                             bool enabled) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  const onion::sprx::SprxOperationResult result =
      current.manager.set_enabled(id, enabled);
  if (result.success)
    (void)write_ploader_config(current.manager.catalog());
  return result;
}

onion::sprx::SprxOperationResult set_delay(std::string_view id,
                                           int32_t delay_ms) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  const onion::sprx::SprxOperationResult result =
      current.manager.set_delay(id, delay_ms);
  if (result.success)
    (void)write_ploader_config(current.manager.catalog());
  return result;
}

size_t rescan() {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  (void)mkdir(kSprxDirectory, 0755);
  const size_t added =
      current.manager.discover(kCatalogPath, discovery_roots());
  (void)write_ploader_config(current.manager.catalog());
  return added;
}

std::string games_json() {
  const std::vector<GameEntry> games = installed_ps4_games();
  std::string json = "{\"games\":[";
  bool first = true;
  for (const GameEntry &game : games) {
    if (!first)
      json += ",";
    first = false;
    json += "{\"id\":\"" + json_escape(game.id) + "\",\"name\":\"" +
            json_escape(game.name) + "\"}";
  }
  json += "]}";
  return json;
}

onion::sprx::SprxOperationResult set_title(std::string_view id,
                                           std::string_view title_id) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  const onion::sprx::SprxOperationResult result =
      current.manager.set_title(id, title_id);
  if (result.success)
    (void)write_ploader_config(current.manager.catalog());
  return result;
}

onion::sprx::SprxOperationResult set_main_thread(std::string_view id,
                                                 bool main_thread) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  const onion::sprx::SprxOperationResult result =
      current.manager.set_main_thread(id, main_thread);
  if (result.success)
    (void)write_ploader_config(current.manager.catalog());
  return result;
}

bool set_loader_settings(int timeout_s, bool expose_data,
                         const std::string &scan_paths) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  LoaderSettings settings;
  settings.timeout_s = timeout_s < 1 ? 1 : (timeout_s > 3600 ? 3600 : timeout_s);
  settings.expose_data = expose_data;
  settings.scan_paths = split_scan_paths(scan_paths);
  if (settings.scan_paths.empty())
    settings.scan_paths = default_scan_paths();
  const bool saved = save_loader_settings(settings);
  (void)write_ploader_config(current.manager.catalog());
  return saved;
}

std::string loader_settings_json() {
  const LoaderSettings settings = load_loader_settings();
  std::string json = "{\"timeout_s\":" + std::to_string(settings.timeout_s);
  json += std::string(",\"expose_data\":") +
          (settings.expose_data ? "true" : "false");
  json += ",\"scan_paths\":\"" + join_scan_paths(settings.scan_paths) + "\"}";
  return json;
}

void sync_ploader_config() {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  if (write_ploader_config(current.manager.catalog()))
    LOG_INFO("[sprx-manager] wrote %s", kPloaderConfig);
  else
    LOG_WARN("[sprx-manager] cannot write %s", kPloaderConfig);
}

onion::sprx::SprxOperationResult remove(std::string_view id) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  const onion::sprx::SprxOperationResult result = current.manager.remove(id);
  if (result.success)
    (void)write_ploader_config(current.manager.catalog());
  return result;
}

void on_big_app_started(pid_t pid, uint32_t app_id,
                        std::string_view title_id) {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  if (!is_current_target(current, pid, app_id, title_id)) {
    LOG_INFO("[sprx-manager] start event pid=%d tid=%.*s is not the current "
             "target; nothing to do",
             static_cast<int>(pid), static_cast<int>(title_id.size()),
             title_id.data());
    return;
  }
  LOG_INFO("[sprx-manager] delegating for %.*s pid=%d",
           static_cast<int>(title_id.size()), title_id.data(),
           static_cast<int>(pid));
  if (delegate_to_ploader(current, title_id))
    return;
  log_report("big-app-start", current.manager.reconcile());
}

void on_big_app_exited(pid_t pid, uint32_t app_id,
                       std::string_view title_id) {
  LOG_INFO("[sprx-manager] target exited pid=%d appid=%u tid=%.*s; no daemon-owned SPRX runtime or UI session to release",
           static_cast<int>(pid), app_id, static_cast<int>(title_id.size()),
           title_id.data());
}

void stop() {
  Runtime &current = runtime();
  std::lock_guard<std::mutex> lock(current.mutex);
  current.manager.stop();
  LOG_INFO("[sprx-manager] stopped target-bound modules");
}

} // namespace onion::daemon::sprx_plugins
