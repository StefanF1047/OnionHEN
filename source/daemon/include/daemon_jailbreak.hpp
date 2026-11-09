/* OnionHEN daemon — shared app-jailbreak entry point.
 *
 * The app-jailbreak listener owns the protocol; this exposes the privilege
 * routine it uses so other daemon modules (e.g. the SPRX plugin runtime) apply
 * the identical transformation instead of duplicating kernel writes.
 */

#pragma once

#include <sys/types.h>

/**
 * Applies full app privileges: uid/ruid/svuid/rgid 0, the HEN authid, full
 * caps, the ptrace attr byte, and (when `escape_sandbox`) rootdir/jaildir
 * pointing at the filesystem root so the target can reach /data.
 *
 * Returns false when any kernel write fails.
 */
bool daemon_jailbreak_apply_privileges(pid_t pid, bool escape_sandbox);
