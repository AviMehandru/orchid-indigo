/* Where everything lives.
 *
 * Every path rule here is a copy of one that already exists in the pipeline,
 * and the copy is deliberate: this process is started by a desktop launcher,
 * not by ytdl, so it inherits nothing. The rules it mirrors:
 *
 *   install root   run_ytdlp.ps1 / ytdl.ps1 platform block
 *   data root      defaults to the install root, per -DataRoot handling
 *   archive root   <dataRoot>/Youtube Videos/Complete Archive
 *   cache dir      archive-viewer.py's default_cache_dir()
 *
 * This is the LINUX app, so the Windows and macOS branches that the Rust
 * version carried are gone rather than being kept as dead code -- there is no
 * MAX_PATH story here, no C:/yt-dlp, and no ~/Library. What replaces them is
 * the XDG basedir spec, which is what a Linux-native app should have been
 * using all along.
 *
 * The one rule that must NEVER be relaxed: nothing derived is written inside
 * the archive tree. postprocess.ps1 writes a checksums.sha256 over every file
 * in a video folder, so a stray file there makes that manifest stop verifying.
 *
 * Every function returning char* returns newly allocated memory; free it with
 * g_free(). Functions that can fail to find something return NULL.
 */

#ifndef YTDL_PATHS_H
#define YTDL_PATHS_H

#include <glib.h>

G_BEGIN_DECLS

/* $HOME, or the passwd entry, or "." -- never NULL. */
char *ytdl_home_dir(void);

/* $YTDLP_INSTALL_ROOT, else ~/yt-dlp. Must agree with the platform block at
 * the top of run_ytdlp.ps1 and ytdl.ps1. */
char *ytdl_install_root(void);

char *ytdl_scripts_dir(void);

/* Note the plural. The INSTALLED config directory is configs/ while the repo
 * directory is config/ -- not a typo. The installed name predates the repo
 * restructure and is baked into run_ytdlp.ps1 and postprocess.ps1. */
char *ytdl_configs_dir(void);

/* Derived and disposable: the metadata index and remuxed playback copies.
 * Deleting it costs one re-index and nothing else.
 *
 * $XDG_CACHE_HOME/ytdl-gtk. A DIFFERENT directory from both
 * archive-viewer.py's ytdlp-archive-viewer and the Tauri app's ytdlp-gui:
 * three readers, three index formats, and a shared directory would mean each
 * treating the others' files as corrupt. */
char *ytdl_cache_dir(void);

/* NOT disposable: settings, the queue and the run history. Losing this loses
 * real user data, which is why it is not under the cache directory.
 * $XDG_CONFIG_HOME/ytdl-gtk. */
char *ytdl_state_dir(void);

/* "~" and "~/..." only. A bare "~user" is deliberately not expanded: the
 * pipeline does not expand it either, and silently resolving it here would
 * reintroduce exactly the two-different-folders bug that made the library
 * index one path while downloads went to another. */
char *ytdl_expand_tilde(const char *path);

/* Accept anything reasonable the user might point at -- a data root, the
 * "Youtube Videos" folder, "Complete Archive" itself, a channel folder, or a
 * reorganised tree -- and find the real Complete Archive directory. Same
 * acceptance set as archive-viewer.py's --root, so a path that works for one
 * works for both. NULL if nothing plausible is there. */
char *ytdl_resolve_archive_root(const char *candidate);

/* The usual install locations, in order. NULL if none of them hold one. */
char *ytdl_autodetect_archive_root(void);

/* The opaque key a video folder is addressed by.
 *
 * The UI never holds a filesystem path: it holds one of these plus an index
 * into the entry's own file list, and the path is resolved from the index.
 * Traversal is off the table because no route accepts a path, not because a
 * filter has to be right. SHA-256 of the '/'-separated relative path,
 * truncated to 8 bytes -- 16 hex characters. */
char *ytdl_key_for(const char *rel);

/* Locate an executable on PATH. Deliberately not a which(1) subprocess, which
 * would be one more thing that can be missing. */
char *ytdl_which(const char *name);

/* pwsh, or NULL. Every stage of this pipeline is a PowerShell 7 script, so a
 * missing pwsh is not a degraded mode -- no download can run at all, and the
 * UI says exactly that rather than failing at spawn time with a confusing
 * OS error. */
char *ytdl_find_pwsh(void);

G_END_DECLS

#endif /* YTDL_PATHS_H */
