#ifndef HOST_PATH_H
#define HOST_PATH_H

/* Resolve a Windows-style path to one that actually exists on disk
 * (case-insensitively). The returned pointer refers to an internal cache and is
 * valid until the next call; returns NULL if nothing matches at all. */
const char *host_path_resolve(const char *path);

#endif
