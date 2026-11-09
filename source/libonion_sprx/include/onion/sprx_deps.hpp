/* OnionHEN SPRX dependencies — reads DT_NEEDED from a .prx/.sprx on the host.
 *
 * A module cannot be loaded until every library it declares is already mapped
 * in the target. Retail games load those lazily, so a plugin injected at a
 * specific moment (splash -> black screen) usually finds one or two missing and
 * sceKernelLoadStartModule returns ENOEXEC (0x80020008). Knowing the list lets
 * the manager load the missing ones itself first.
 */

#pragma once

#include <string>
#include <vector>

namespace onion::sprx {

/** Directories searched when preloading a declared dependency. */
std::vector<std::string> dependency_search_dirs();

/**
 * Parses the ELF dynamic table for DT_NEEDED entries.
 *
 * Returns an empty vector when the file is not an ELF, has no dynamic data, or
 * the names cannot be resolved; callers treat that as "no known dependencies".
 */
std::vector<std::string> read_declared_dependencies(std::string_view path);

/** True for a name that looks like a module (printable, .prx / .sprx). */
bool looks_like_module_name(std::string_view name) noexcept;

} // namespace onion::sprx
