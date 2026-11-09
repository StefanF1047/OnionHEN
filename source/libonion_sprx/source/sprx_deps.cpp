/* OnionHEN SPRX dependencies — DT_NEEDED reader. */

#include <onion/sprx_deps.hpp>

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <string_view>
#include <utility>

namespace onion::sprx {
namespace {

/* PT_DYNLIBDATA is a Sony addition; the SDK's <elf.h> may not name it. */
constexpr Elf64_Word kPtDynlibData = 0x61000000u;

/* Sony stores the dynlib string table at this offset inside PT_DYNLIBDATA. */
constexpr long kDynlibStringBase = 0x18;

constexpr size_t kMaxDependencies = 64;
constexpr size_t kMaxNameLength = 64;

bool read_at(FILE *file, long offset, void *out, size_t size) {
  if (offset < 0)
    return false;
  return std::fseek(file, offset, SEEK_SET) == 0 &&
         std::fread(out, 1, size, file) == size;
}

/** Resolves one DT_NEEDED string, trying the known dynlib string bases. */
bool dependency_name_at(FILE *file, long dynlib_offset, uint64_t name_offset,
                        std::string *out) {
  const long bases[] = {dynlib_offset + kDynlibStringBase, dynlib_offset, 0};
  for (const long base : bases) {
    char buffer[kMaxNameLength] = {};
    if (base < 0 || name_offset > static_cast<uint64_t>(LONG_MAX))
      continue;
    if (!read_at(file, base + static_cast<long>(name_offset), buffer,
                 sizeof(buffer)))
      continue;
    const size_t length = strnlen(buffer, sizeof(buffer));
    if (!looks_like_module_name(std::string_view(buffer, length)))
      continue;
    out->assign(buffer, length);
    return true;
  }
  return false;
}

} // namespace

bool looks_like_module_name(std::string_view name) noexcept {
  if (name.size() < 5 || name.size() >= kMaxNameLength)
    return false;
  for (const char c : name) {
    if (c < 0x20 || c > 0x7e)
      return false;
  }
  const std::string_view tail = name.substr(name.size() - 4);
  if (tail == ".prx")
    return true;
  return name.size() >= 5 &&
         name.substr(name.size() - 5) == ".sprx";
}

std::vector<std::string> dependency_search_dirs() {
  return {"/system/common/lib/", "/system/priv/lib/"};
}

std::vector<std::string> read_declared_dependencies(std::string_view path) {
  std::vector<std::string> declared;
  FILE *file = std::fopen(std::string(path).c_str(), "rb");
  if (file == nullptr)
    return declared;

  Elf64_Ehdr header = {};
  if (!read_at(file, 0, &header, sizeof(header)) ||
      std::memcmp(header.e_ident, "\x7f" "ELF", 4) != 0 ||
      header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 ||
      header.e_phnum > 64) {
    std::fclose(file);
    return declared;
  }

  long dynamic_offset = -1;
  long dynlib_offset = -1;
  size_t dynamic_size = 0;
  for (int index = 0; index < header.e_phnum; ++index) {
    Elf64_Phdr phdr = {};
    const long offset =
        static_cast<long>(header.e_phoff) + index * static_cast<long>(sizeof(phdr));
    if (!read_at(file, offset, &phdr, sizeof(phdr))) {
      std::fclose(file);
      return {};
    }
    if (phdr.p_type == PT_DYNAMIC) {
      dynamic_offset = static_cast<long>(phdr.p_offset);
      dynamic_size = phdr.p_filesz;
    } else if (phdr.p_type == kPtDynlibData) {
      dynlib_offset = static_cast<long>(phdr.p_offset);
    }
  }

  if (dynamic_offset >= 0 && dynlib_offset >= 0) {
    for (size_t offset = 0; offset + sizeof(Elf64_Dyn) <= dynamic_size;
         offset += sizeof(Elf64_Dyn)) {
      Elf64_Dyn entry = {};
      if (!read_at(file, dynamic_offset + static_cast<long>(offset), &entry,
                   sizeof(entry)))
        break;
      if (entry.d_tag == DT_NULL)
        break;
      if (entry.d_tag != DT_NEEDED)
        continue;

      std::string name;
      if (!dependency_name_at(file, dynlib_offset, entry.d_un.d_val, &name))
        continue;
      bool seen = false;
      for (const std::string &existing : declared) {
        if (existing == name) {
          seen = true;
          break;
        }
      }
      if (seen)
        continue;
      if (declared.size() >= kMaxDependencies)
        break;
      declared.push_back(std::move(name));
    }
  }

  std::fclose(file);
  return declared;
}

} // namespace onion::sprx
