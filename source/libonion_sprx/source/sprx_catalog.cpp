/* Copyright (C) 2026 OnionHEN / LightningMods */

#include <onion/sprx_catalog.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace onion::sprx {
namespace {

constexpr size_t kMaxManifestBytes = 1024 * 1024;
constexpr size_t kMaxEntries = 256;
constexpr size_t kMaxDependencies = 64;

std::string_view trim(std::string_view value) noexcept {
  size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first])))
    ++first;
  size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1])))
    --last;
  return value.substr(first, last - first);
}

bool valid_id(std::string_view id) noexcept {
  if (id.empty() || id.size() >= 32)
    return false;
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
  });
}

bool valid_title_id(std::string_view id) noexcept {
  if (id.size() != 9)
    return false;
  for (const char c : id) {
    if (!std::isalnum(static_cast<unsigned char>(c)))
      return false;
  }
  return true;
}

bool valid_title_prefix(std::string_view prefix) noexcept {
  if (prefix.empty() || prefix.size() >= 9)
    return false;
  for (const char c : prefix) {
    if (!std::isalnum(static_cast<unsigned char>(c)))
      return false;
  }
  return true;
}

bool valid_path(std::string_view path) noexcept {
  if (path.empty() || path.front() != '/')
    return false;
  size_t component = 0;
  while (component < path.size()) {
    const size_t slash = path.find('/', component);
    const size_t end = slash == std::string_view::npos ? path.size() : slash;
    if (path.substr(component, end - component) == "..")
      return false;
    component = slash == std::string_view::npos ? path.size() : slash + 1;
  }
  const size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos)
    return false;
  const std::string_view suffix = path.substr(dot);
  if (suffix.size() != 5 && suffix.size() != 4)
    return false;
  const std::string expected = suffix.size() == 5 ? ".sprx" : ".prx";
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(suffix[i])) != expected[i])
      return false;
  }
  return true;
}

bool parse_bool(std::string_view value, bool *out) noexcept {
  if (!out)
    return false;
  value = trim(value);
  if (value == "true" || value == "1") {
    *out = true;
    return true;
  }
  if (value == "false" || value == "0") {
    *out = false;
    return true;
  }
  return false;
}

bool parse_priority(std::string_view value, int32_t *out) noexcept {
  if (!out)
    return false;
  value = trim(value);
  if (value.empty())
    return false;
  int32_t parsed = 0;
  const char *begin = value.data();
  const char *end = begin + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end)
    return false;
  *out = parsed;
  return true;
}

bool split_csv(std::string_view value, std::vector<std::string> *out,
               bool (*validator)(std::string_view), size_t max_count) {
  if (!out)
    return false;
  out->clear();
  value = trim(value);
  if (value.empty())
    return true;
  size_t start = 0;
  while (start <= value.size()) {
    const size_t comma = value.find(',', start);
    const size_t end = comma == std::string_view::npos ? value.size() : comma;
    const std::string_view item = trim(value.substr(start, end - start));
    if (item.empty() || !validator(item) || out->size() >= max_count)
      return false;
    out->emplace_back(item);
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  return true;
}

void add_issue(std::vector<SprxCatalogIssue> *issues, size_t line,
               std::string_view id, std::string_view message) {
  if (issues)
    issues->push_back({line, std::string(id), std::string(message)});
}

bool has_key(const std::set<std::string> &keys, std::string_view key) {
  return keys.find(std::string(key)) != keys.end();
}

std::string join_csv(const std::vector<std::string> &values) {
  std::string result;
  for (const std::string &value : values) {
    if (!result.empty())
      result.push_back(',');
    result += value;
  }
  return result;
}

bool write_all(int fd, std::string_view text) {
  size_t offset = 0;
  while (offset < text.size()) {
    const ssize_t written = write(fd, text.data() + offset, text.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return false;
    offset += static_cast<size_t>(written);
  }
  return true;
}

} // namespace

bool SprxCatalog::parse(std::string_view text,
                        std::vector<SprxCatalogIssue> *issues) {
  if (issues)
    issues->clear();
  entries_.clear();
  if (text.size() > kMaxManifestBytes) {
    add_issue(issues, 0, {}, "manifest exceeds size limit");
    return false;
  }

  std::vector<SprxManifestEntry> parsed;
  std::set<std::string> ids;
  SprxManifestEntry current;
  std::set<std::string> keys;
  std::string current_id;
  size_t section_line = 0;
  bool section_open = false;
  bool ok = true;

  const auto finish = [&]() {
    if (!section_open)
      return;
    if (!has_key(keys, "path") || current.path.empty()) {
      add_issue(issues, section_line, current_id, "path is required");
      ok = false;
    }
    if (!valid_path(current.path)) {
      add_issue(issues, section_line, current_id,
                "path must be absolute and end in .sprx or .prx");
      ok = false;
    }
    if (current.exact_title_ids.empty() && current.title_id_prefixes.empty()) {
      add_issue(issues, section_line, current_id,
                "at least one title ID or title ID prefix is required");
      ok = false;
    }
    if (current.dependencies.size() > kMaxDependencies) {
      add_issue(issues, section_line, current_id, "too many dependencies");
      ok = false;
    }
    std::set<std::string> dependency_ids;
    for (const std::string &dependency : current.dependencies) {
      if (!dependency_ids.insert(dependency).second) {
        add_issue(issues, section_line, current_id,
                  "duplicate dependency");
        ok = false;
        break;
      }
    }
    if (!ids.insert(current.id).second) {
      add_issue(issues, section_line, current_id, "duplicate plugin ID");
      ok = false;
    }
    parsed.push_back(std::move(current));
    current = {};
    keys.clear();
  };

  size_t line_number = 1;
  size_t cursor = 0;
  while (cursor <= text.size()) {
    const size_t newline = text.find('\n', cursor);
    const size_t end = newline == std::string_view::npos ? text.size() : newline;
    std::string_view line = trim(text.substr(cursor, end - cursor));
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (line.empty() || line.front() == '#' || line.front() == ';') {
      // comment/blank
    } else if (line.front() == '[' && line.back() == ']') {
      finish();
      const std::string_view section = trim(line.substr(1, line.size() - 2));
      constexpr std::string_view prefix = "plugin.";
      if (section.size() <= prefix.size() ||
          section.substr(0, prefix.size()) != prefix ||
          !valid_id(section.substr(prefix.size())) || parsed.size() >= kMaxEntries) {
        add_issue(issues, line_number, {}, "invalid plugin section");
        ok = false;
        section_open = false;
      } else {
        current_id = std::string(section.substr(prefix.size()));
        current.id = current_id;
        section_line = line_number;
        section_open = true;
      }
    } else {
      const size_t equal = line.find('=');
      if (!section_open || equal == std::string_view::npos) {
        add_issue(issues, line_number, current_id, "expected key=value inside a plugin section");
        ok = false;
      } else {
        const std::string key(trim(line.substr(0, equal)));
        const std::string_view value = trim(line.substr(equal + 1));
        if (key.empty() || !keys.insert(key).second) {
          add_issue(issues, line_number, current_id, "duplicate or empty key");
          ok = false;
        } else if (key == "path") {
          current.path = std::string(value);
          if (!valid_path(current.path)) {
            add_issue(issues, line_number, current_id, "path must be absolute and end in .sprx or .prx");
            ok = false;
          }
        } else if (key == "exact_title_ids") {
          if (!split_csv(value, &current.exact_title_ids, valid_title_id, 64)) {
            add_issue(issues, line_number, current_id, "invalid exact_title_ids");
            ok = false;
          }
        } else if (key == "title_id_prefixes") {
          if (!split_csv(value, &current.title_id_prefixes, valid_title_prefix, 64)) {
            add_issue(issues, line_number, current_id, "invalid title_id_prefixes");
            ok = false;
          }
        } else if (key == "enabled") {
          if (!parse_bool(value, &current.enabled)) {
            add_issue(issues, line_number, current_id,
                      "enabled must be true or false");
            ok = false;
          }
        } else if (key == "auto_start") {
          if (!parse_bool(value, &current.auto_start)) {
            add_issue(issues, line_number, current_id, "auto_start must be true or false");
            ok = false;
          }
        } else if (key == "priority") {
          if (!parse_priority(value, &current.priority)) {
            add_issue(issues, line_number, current_id, "priority must be a signed 32-bit integer");
            ok = false;
          }
        } else if (key == "delay_ms") {
          int32_t delay_ms = 0;
          if (!parse_priority(value, &delay_ms) || delay_ms < 0) {
            add_issue(issues, line_number, current_id,
                      "delay_ms must be 0..60000");
            ok = false;
          } else {
            current.delay_ms = delay_ms > 60000 ? 60000 : delay_ms;
          }
        } else if (key == "main_thread") {
          if (!parse_bool(value, &current.main_thread)) {
            add_issue(issues, line_number, current_id,
                      "main_thread must be true or false");
            ok = false;
          }
        } else if (key == "dependencies") {
          if (!split_csv(value, &current.dependencies, valid_id, kMaxDependencies)) {
            add_issue(issues, line_number, current_id, "invalid dependencies");
            ok = false;
          }
        } else {
          /* Unknown keys are tolerated: rejecting the whole manifest over one
             stray key silently emptied the catalog (plugins disappeared and a
             new game selection could not be saved). */
          add_issue(issues, line_number, current_id, "unknown manifest key");
        }
      }
    }
    if (newline == std::string_view::npos)
      break;
    cursor = newline + 1;
    ++line_number;
  }
  finish();

  if (!ok) {
    entries_.clear();
    return false;
  }
  entries_ = std::move(parsed);
  return true;
}

bool SprxCatalog::load_file(std::string_view path,
                            std::vector<SprxCatalogIssue> *issues) {
  if (issues)
    issues->clear();
  const std::string file_path(path);
  FILE *file = std::fopen(file_path.c_str(), "rb");
  if (!file) {
    add_issue(issues, 0, {}, std::string("cannot open manifest: ") + std::strerror(errno));
    entries_.clear();
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    add_issue(issues, 0, {}, "cannot seek manifest");
    entries_.clear();
    return false;
  }
  const long size = std::ftell(file);
  if (size < 0 || static_cast<unsigned long>(size) > kMaxManifestBytes) {
    std::fclose(file);
    add_issue(issues, 0, {}, "manifest exceeds size limit");
    entries_.clear();
    return false;
  }
  std::rewind(file);
  std::string text(static_cast<size_t>(size), '\0');
  const size_t read = std::fread(text.data(), 1, text.size(), file);
  const bool close_ok = std::fclose(file) == 0;
  if (read != text.size() || !close_ok) {
    add_issue(issues, 0, {}, "cannot read manifest");
    entries_.clear();
    return false;
  }
  return parse(text, issues);
}

const SprxManifestEntry *SprxCatalog::find(std::string_view id) const noexcept {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const SprxManifestEntry &entry) {
                                    return entry.id == id;
                                  });
  return found == entries_.end() ? nullptr : &*found;
}

bool SprxCatalog::set_enabled(std::string_view id, bool enabled) noexcept {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const SprxManifestEntry &entry) {
                                    return entry.id == id;
                                  });
  if (found == entries_.end())
    return false;
  found->enabled = enabled;
  return true;
}

bool SprxCatalog::set_delay(std::string_view id, int32_t delay_ms) noexcept {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const SprxManifestEntry &entry) {
                                    return entry.id == id;
                                  });
  if (found == entries_.end())
    return false;
  if (delay_ms < 0)
    delay_ms = 0;
  if (delay_ms > 60000)
    delay_ms = 60000;
  found->delay_ms = delay_ms;
  return true;
}

bool SprxCatalog::set_main_thread(std::string_view id,
                                  bool main_thread) noexcept {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const SprxManifestEntry &entry) {
                                    return entry.id == id;
                                  });
  if (found == entries_.end())
    return false;
  found->main_thread = main_thread;
  return true;
}

bool SprxCatalog::set_title(std::string_view id,
                            std::string_view title_id) noexcept {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const SprxManifestEntry &entry) {
                                    return entry.id == id;
                                  });
  if (found == entries_.end() || !valid_title_id(title_id))
    return false;
  found->exact_title_ids.assign(1, std::string(title_id));
  found->title_id_prefixes.clear();
  return true;
}

bool SprxCatalog::remove(std::string_view id) noexcept {
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const SprxManifestEntry &entry) {
                                    return entry.id == id;
                                  });
  if (found == entries_.end())
    return false;
  entries_.erase(found);
  return true;
}

bool SprxCatalog::add(SprxManifestEntry entry) noexcept {
  try {
    if (!valid_id(entry.id) || !valid_path(entry.path))
      return false;
    if (entries_.size() >= kMaxEntries)
      return false;
    for (const SprxManifestEntry &existing : entries_) {
      if (existing.id == entry.id || existing.path == entry.path)
        return false;
    }
    entries_.push_back(std::move(entry));
    return true;
  } catch (...) {
    return false;
  }
}

std::string SprxCatalog::serialize() const {
  std::string result;
  for (const SprxManifestEntry &entry : entries_) {
    if (!result.empty())
      result.push_back('\n');
    result += "[plugin." + entry.id + "]\n";
    result += "path=" + entry.path + "\n";
    if (!entry.exact_title_ids.empty())
      result += "exact_title_ids=" + join_csv(entry.exact_title_ids) + "\n";
    if (!entry.title_id_prefixes.empty())
      result += "title_id_prefixes=" + join_csv(entry.title_id_prefixes) + "\n";
    result += std::string("enabled=") + (entry.enabled ? "true\n" : "false\n");
    result += std::string("auto_start=") +
              (entry.auto_start ? "true\n" : "false\n");
    result += "priority=" + std::to_string(entry.priority) + "\n";
    if (entry.delay_ms != 0)
      result += "delay_ms=" + std::to_string(entry.delay_ms) + "\n";
    if (entry.main_thread)
      result += "main_thread=true\n";
    if (!entry.dependencies.empty())
      result += "dependencies=" + join_csv(entry.dependencies) + "\n";
  }
  return result;
}

bool SprxCatalog::matches(const SprxManifestEntry &entry,
                          std::string_view title_id) const noexcept {
  if (!valid_title_id(title_id))
    return false;
  if (std::find(entry.exact_title_ids.begin(), entry.exact_title_ids.end(),
                title_id) != entry.exact_title_ids.end())
    return true;
  return std::any_of(entry.title_id_prefixes.begin(), entry.title_id_prefixes.end(),
                     [&](const std::string &prefix) {
                       return title_id.size() >= prefix.size() &&
                              title_id.compare(0, prefix.size(), prefix) == 0;
                     });
}

std::vector<const SprxManifestEntry *>
SprxCatalog::startup_order(std::string_view title_id,
                           std::vector<SprxCatalogIssue> *issues) const {
  if (issues)
    issues->clear();
  std::map<std::string, const SprxManifestEntry *> matched;
  for (const SprxManifestEntry &entry : entries_) {
    if (entry.enabled && matches(entry, title_id))
      matched.emplace(entry.id, &entry);
  }

  std::set<std::string> required;
  std::set<std::string> visiting;
  bool ok = true;
  const auto collect = [&](const auto &self, const std::string &id) -> void {
    if (!ok)
      return;
    if (required.contains(id))
      return;
    if (!visiting.insert(id).second) {
      add_issue(issues, 0, id, "dependency cycle detected");
      ok = false;
      return;
    }
    const auto found = matched.find(id);
    if (found == matched.end()) {
      if (!find(id))
        add_issue(issues, 0, id, "dependency is not declared");
      else
        add_issue(issues, 0, id, "dependency does not match the target Title ID");
      ok = false;
      visiting.erase(id);
      return;
    }
    for (const std::string &dependency : found->second->dependencies)
      self(self, dependency);
    visiting.erase(id);
    required.insert(id);
  };

  for (const auto &[id, entry] : matched) {
    if (entry->auto_start)
      collect(collect, id);
  }
  if (!ok)
    return {};

  std::map<std::string, size_t> indegree;
  std::map<std::string, std::vector<std::string>> dependents;
  for (const std::string &id : required)
    indegree[id] = 0;
  for (const std::string &id : required) {
    const SprxManifestEntry *entry = matched.at(id);
    for (const std::string &dependency : entry->dependencies) {
      if (!required.contains(dependency))
        continue;
      ++indegree[id];
      dependents[dependency].push_back(id);
    }
  }

  std::vector<const SprxManifestEntry *> order;
  while (order.size() < required.size()) {
    const SprxManifestEntry *best = nullptr;
    for (const auto &[id, degree] : indegree) {
      if (degree != 0)
        continue;
      const auto *candidate = matched.at(id);
      if (!best || candidate->priority > best->priority ||
          (candidate->priority == best->priority && candidate->id < best->id))
        best = candidate;
    }
    if (!best) {
      add_issue(issues, 0, {}, "dependency cycle detected");
      return {};
    }
    order.push_back(best);
    indegree.erase(best->id);
    for (const std::string &dependent : dependents[best->id])
      --indegree[dependent];
  }
  return order;
}

bool SprxCatalogStore::load(std::string_view path,
                            std::vector<SprxCatalogIssue> *issues) {
  std::lock_guard<std::mutex> lock(mutex_);
  SprxCatalog loaded;
  if (!loaded.load_file(path, issues)) {
    catalog_ = {};
    path_ = std::string(path);
    loaded_ = false;
    return false;
  }
  catalog_ = std::move(loaded);
  path_ = std::string(path);
  loaded_ = true;
  return true;
}

bool SprxCatalogStore::load_or_create(std::string_view path,
                                      std::vector<SprxCatalogIssue> *issues) {
  std::lock_guard<std::mutex> lock(mutex_);
  path_ = std::string(path);
  struct stat info {};
  if (stat(path_.c_str(), &info) == 0) {
    SprxCatalog loaded;
    if (!loaded.load_file(path_, issues)) {
      catalog_ = {};
      loaded_ = false;
      return false;
    }
    catalog_ = std::move(loaded);
    loaded_ = true;
    return true;
  }
  // A fresh console has no manifest yet. That is the normal first-run state, so
  // start empty and create the file so later reload() calls succeed.
  catalog_ = {};
  loaded_ = true;
  if (issues)
    issues->clear();
  (void)persist_locked();
  return true;
}

SprxOperationResult SprxCatalogStore::add(const SprxManifestEntry &entry) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_)
    return {false, "catalog is not loaded"};
  if (!catalog_.add(entry))
    return {false, "SPRX plugin is invalid or already declared"};
  const SprxOperationResult persisted = persist_locked();
  if (persisted)
    return persisted;
  (void)catalog_.remove(entry.id);
  return persisted;
}

bool SprxCatalogStore::reload(std::vector<SprxCatalogIssue> *issues) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (path_.empty()) {
    if (issues)
      issues->clear();
    loaded_ = false;
    return false;
  }
  SprxCatalog loaded;
  if (!loaded.load_file(path_, issues)) {
    catalog_ = {};
    loaded_ = false;
    return false;
  }
  catalog_ = std::move(loaded);
  loaded_ = true;
  return true;
}

std::vector<SprxInventoryEntry> SprxCatalogStore::inventory() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SprxInventoryEntry> result;
  result.reserve(catalog_.entries().size());
  for (const SprxManifestEntry &entry : catalog_.entries()) {
    result.push_back({entry.id, entry.path, entry.enabled, entry.auto_start,
                      entry.priority, false, false, entry.delay_ms,
                      entry.main_thread,
                      entry.exact_title_ids.empty() ? std::string()
                                                    : entry.exact_title_ids.front()});
  }
  return result;
}

SprxCatalog SprxCatalogStore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return catalog_;
}

std::string SprxCatalogStore::path() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return path_;
}

bool SprxCatalogStore::loaded() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_;
}

SprxOperationResult SprxCatalogStore::persist_locked() {
  if (path_.empty())
    return {false, "catalog path is not configured"};
  const std::string temporary = path_ + ".writing";
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0)
    return {false, std::string("cannot create catalog staging file: ") +
                       std::strerror(errno)};
  const std::string text = catalog_.serialize();
  const bool write_ok = write_all(fd, text);
  const int write_error = write_ok ? 0 : errno;
  const bool sync_ok = write_ok && fsync(fd) == 0;
  const int sync_error = sync_ok ? 0 : errno;
  const int close_result = close(fd);
  const int close_error = close_result == 0 ? 0 : errno;
  if (!write_ok || !sync_ok || close_result != 0) {
    const int error = !write_ok ? write_error
                      : !sync_ok ? sync_error
                                 : close_error;
    (void)unlink(temporary.c_str());
    return {false, std::string("cannot persist catalog: ") +
                       std::strerror(error)};
  }
  if (rename(temporary.c_str(), path_.c_str()) != 0) {
    const int rename_error = errno;
    (void)unlink(temporary.c_str());
    return {false, std::string("cannot persist catalog: ") +
                       std::strerror(rename_error)};
  }
  return {true, {}};
}

SprxOperationResult SprxCatalogStore::set_enabled(std::string_view id,
                                                   bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_)
    return {false, "catalog is not loaded"};
  const SprxManifestEntry *entry = catalog_.find(id);
  if (!entry)
    return {false, "SPRX plugin is not declared"};
  if (entry->enabled == enabled)
    return {true, {}};
  if (!catalog_.set_enabled(id, enabled))
    return {false, "SPRX plugin is not declared"};
  const SprxOperationResult persisted = persist_locked();
  if (persisted)
    return persisted;
  (void)catalog_.set_enabled(id, !enabled);
  return persisted;
}

SprxOperationResult SprxCatalogStore::set_delay(std::string_view id,
                                                int32_t delay_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_)
    return {false, "catalog is not loaded"};
  const SprxManifestEntry *entry = catalog_.find(id);
  if (!entry)
    return {false, "SPRX plugin is not declared"};
  const int32_t previous = entry->delay_ms;
  if (!catalog_.set_delay(id, delay_ms))
    return {false, "SPRX plugin is not declared"};
  const SprxOperationResult persisted = persist_locked();
  if (persisted)
    return persisted;
  (void)catalog_.set_delay(id, previous);
  return persisted;
}

SprxOperationResult SprxCatalogStore::set_main_thread(std::string_view id,
                                                      bool main_thread) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_)
    return {false, "catalog is not loaded"};
  const SprxManifestEntry *entry = catalog_.find(id);
  if (!entry)
    return {false, "SPRX plugin is not declared"};
  const bool previous = entry->main_thread;
  if (!catalog_.set_main_thread(id, main_thread))
    return {false, "SPRX plugin is not declared"};
  const SprxOperationResult persisted = persist_locked();
  if (persisted)
    return persisted;
  (void)catalog_.set_main_thread(id, previous);
  return persisted;
}

SprxOperationResult SprxCatalogStore::set_title(std::string_view id,
                                                std::string_view title_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_)
    return {false, "catalog is not loaded"};
  const SprxManifestEntry *entry = catalog_.find(id);
  if (!entry)
    return {false, "SPRX plugin is not declared"};
  const std::vector<std::string> previous_titles = entry->exact_title_ids;
  const std::vector<std::string> previous_prefixes = entry->title_id_prefixes;
  if (!catalog_.set_title(id, title_id))
    return {false, "Title ID is not valid"};
  const SprxOperationResult persisted = persist_locked();
  if (persisted)
    return persisted;
  if (SprxManifestEntry *restore =
          const_cast<SprxManifestEntry *>(catalog_.find(id))) {
    restore->exact_title_ids = previous_titles;
    restore->title_id_prefixes = previous_prefixes;
  }
  return persisted;
}

SprxOperationResult SprxCatalogStore::remove(std::string_view id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_)
    return {false, "catalog is not loaded"};
  const SprxManifestEntry *entry = catalog_.find(id);
  if (!entry)
    return {false, "SPRX plugin is not declared"};
  SprxCatalog previous = catalog_;
  if (!catalog_.remove(id))
    return {false, "SPRX plugin is not declared"};
  const SprxOperationResult persisted = persist_locked();
  if (persisted)
    return persisted;
  catalog_ = std::move(previous);
  return persisted;
}

} // namespace onion::sprx
