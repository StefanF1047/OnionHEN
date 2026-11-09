/* OnionHEN SPRX discovery — POSIX adapter. */

#include <onion/sprx_directory_scanner.hpp>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>

namespace onion::sprx {
namespace {

/** A module folder nested deeper than this is treated as a mistake. */
constexpr int kMaxDepth = 4;
constexpr size_t kMaxVisitedDirs = 256;

} // namespace

bool RealSprxDirectoryScanner::canonicalize(std::string_view path,
                                            std::string *out) noexcept {
  if (!out || path.empty())
    return false;
  try {
    std::string owned(path);
    char resolved[PATH_MAX];
    if (realpath(owned.c_str(), resolved) == nullptr)
      return false;
    out->assign(resolved);
    return true;
  } catch (...) {
    return false;
  }
}

bool RealSprxDirectoryScanner::stat_size(std::string_view path,
                                         uint64_t *size) noexcept {
  if (!size || path.empty())
    return false;
  try {
    std::string owned(path);
    struct stat info {};
    if (stat(owned.c_str(), &info) != 0)
      return false;
    if (!S_ISREG(info.st_mode))
      return false;
    *size = static_cast<uint64_t>(info.st_size);
    return true;
  } catch (...) {
    return false;
  }
}

bool RealSprxDirectoryScanner::list(std::string_view dir, bool recursive,
                                    std::vector<std::string> *out,
                                    SprxDiscoveryIssue *issue) noexcept {
  if (!out || dir.empty())
    return false;
  try {
    struct Pending {
      std::string path;
      int depth = 0;
    };
    std::vector<Pending> pending;
    pending.push_back({std::string(dir), 0});
    size_t visited = 0;
    bool opened_any = false;

    while (!pending.empty()) {
      const Pending current = std::move(pending.back());
      pending.pop_back();
      if (++visited > kMaxVisitedDirs)
        break;

      DIR *handle = opendir(current.path.c_str());
      if (handle == nullptr) {
        if (!opened_any && issue) {
          issue->path = current.path;
          issue->message =
              std::string("cannot open directory: ") + std::strerror(errno);
        }
        continue;
      }
      opened_any = true;

      while (struct dirent *entry = readdir(handle)) {
        const char *name = entry->d_name;
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
          continue;

        std::string child = current.path;
        if (child.back() != '/')
          child.push_back('/');
        child.append(name);

        struct stat info {};
        if (stat(child.c_str(), &info) != 0)
          continue;

        if (S_ISDIR(info.st_mode)) {
          if (recursive && current.depth < kMaxDepth)
            pending.push_back({std::move(child), current.depth + 1});
          continue;
        }
        if (!S_ISREG(info.st_mode))
          continue;
        out->push_back(std::move(child));
      }
      closedir(handle);
    }

    return opened_any;
  } catch (...) {
    return false;
  }
}

} // namespace onion::sprx
