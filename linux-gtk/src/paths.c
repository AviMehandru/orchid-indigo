#include "paths.h"

#include <gio/gio.h>
#include <string.h>

/* $HOME is read directly rather than through g_get_home_dir(), which caches
 * on first call and reads the passwd entry when HOME is unset. Reading the
 * variable keeps this testable -- the conformance test points HOME at a
 * fixture tree -- and matches what the pipeline scripts themselves resolve. */
char *
ytdl_home_dir (void)
{
  const char *home = g_getenv ("HOME");
  if (home != NULL && *g_strchug ((char *) home) != '\0')
    return g_strdup (home);

  const char *fallback = g_get_home_dir ();
  return g_strdup (fallback != NULL ? fallback : ".");
}

char *
ytdl_install_root (void)
{
  const char *env = g_getenv ("YTDLP_INSTALL_ROOT");
  if (env != NULL && env[0] != '\0')
    return g_strdup (env);

  g_autofree char *home = ytdl_home_dir ();
  return g_build_filename (home, "yt-dlp", NULL);
}

char *
ytdl_scripts_dir (void)
{
  g_autofree char *root = ytdl_install_root ();
  return g_build_filename (root, "scripts", NULL);
}

char *
ytdl_configs_dir (void)
{
  g_autofree char *root = ytdl_install_root ();
  return g_build_filename (root, "configs", NULL);
}

/* XDG, with the same env-first rule as ytdl_home_dir and for the same reason.
 * g_get_user_cache_dir() would be the idiomatic call but caches identically. */
static char *
xdg_base (const char *var, const char *fallback_leaf)
{
  const char *env = g_getenv (var);
  if (env != NULL && env[0] != '\0')
    return g_strdup (env);

  g_autofree char *home = ytdl_home_dir ();
  return g_build_filename (home, fallback_leaf, NULL);
}

char *
ytdl_cache_dir (void)
{
  g_autofree char *base = xdg_base ("XDG_CACHE_HOME", ".cache");
  return g_build_filename (base, "ytdl-gtk", NULL);
}

char *
ytdl_state_dir (void)
{
  g_autofree char *base = xdg_base ("XDG_CONFIG_HOME", ".config");
  return g_build_filename (base, "ytdl-gtk", NULL);
}

char *
ytdl_expand_tilde (const char *path)
{
  if (path == NULL)
    return NULL;

  if (g_strcmp0 (path, "~") == 0)
    return ytdl_home_dir ();

  if (g_str_has_prefix (path, "~/"))
    {
      g_autofree char *home = ytdl_home_dir ();
      return g_build_filename (home, path + 2, NULL);
    }

  return g_strdup (path);
}

static gboolean
is_dir (const char *path)
{
  return path != NULL && g_file_test (path, G_FILE_TEST_IS_DIR);
}

/* A directory is "a channel folder" if any of its first 60 children is either
 * named "Channel Info" or holds a Final files/ or Video metadata/ subfolder.
 *
 * The cap is not laziness: a user can point this at their home directory, and
 * the answer is decided by the first handful of entries in every real case.
 * Reading an unbounded directory to say "no" is how a folder picker hangs. */
static gboolean
looks_like_channel_dir (const char *path)
{
  g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
  if (dir == NULL)
    return FALSE;

  const char *name;
  int seen = 0;
  while ((name = g_dir_read_name (dir)) != NULL && seen < 60)
    {
      g_autofree char *child = g_build_filename (path, name, NULL);
      if (!is_dir (child))
        continue;
      seen++;

      if (g_strcmp0 (name, "Channel Info") == 0)
        return TRUE;

      g_autofree char *finals = g_build_filename (child, "Final files", NULL);
      g_autofree char *meta = g_build_filename (child, "Video metadata", NULL);
      if (is_dir (finals) || is_dir (meta))
        return TRUE;
    }

  return FALSE;
}

static gboolean
basename_is (const char *path, const char *want)
{
  g_autofree char *base = g_path_get_basename (path);
  return g_strcmp0 (base, want) == 0;
}

char *
ytdl_resolve_archive_root (const char *candidate)
{
  if (candidate == NULL)
    return NULL;

  g_autofree char *expanded = ytdl_expand_tilde (candidate);
  if (!g_file_test (expanded, G_FILE_TEST_EXISTS))
    return NULL;

  /* Canonicalise so the walk-up below terminates and so two spellings of the
   * same directory produce the same answer. g_canonicalize_filename does not
   * touch the filesystem, which is what we want: a symlinked archive root
   * should stay spelled the way the user gave it. */
  g_autofree char *cwd = g_get_current_dir ();
  g_autofree char *p = g_canonicalize_filename (expanded, cwd);

  g_autofree char *nested =
      g_build_filename (p, "Youtube Videos", "Complete Archive", NULL);
  g_autofree char *direct = g_build_filename (p, "Complete Archive", NULL);

  const char *tries[] = { nested, direct, p };
  for (gsize i = 0; i < G_N_ELEMENTS (tries); i++)
    {
      if (is_dir (tries[i]) && basename_is (tries[i], "Complete Archive"))
        return g_strdup (tries[i]);
    }

  /* Pointed at a channel folder, or somewhere below the root: walk up. */
  g_autofree char *cur = g_strdup (p);
  while (cur != NULL && strcmp (cur, "/") != 0 && strcmp (cur, ".") != 0)
    {
      if (is_dir (cur) && basename_is (cur, "Complete Archive"))
        return g_steal_pointer (&cur);

      char *parent = g_path_get_dirname (cur);
      if (g_strcmp0 (parent, cur) == 0)
        {
          g_free (parent);
          break;
        }
      g_free (cur);
      cur = parent;
    }

  /* Last resort: a directory whose children look like channel folders is good
   * enough to index, whatever it happens to be called. */
  if (is_dir (p))
    {
      g_autoptr (GDir) dir = g_dir_open (p, 0, NULL);
      if (dir != NULL)
        {
          const char *name;
          int seen = 0;
          while ((name = g_dir_read_name (dir)) != NULL && seen < 60)
            {
              g_autofree char *child = g_build_filename (p, name, NULL);
              if (!is_dir (child))
                continue;
              seen++;
              if (looks_like_channel_dir (child))
                return g_strdup (p);
            }
        }
      if (looks_like_channel_dir (p))
        return g_strdup (p);
    }

  return NULL;
}

char *
ytdl_autodetect_archive_root (void)
{
  g_autofree char *home = ytdl_home_dir ();
  g_autofree char *install = ytdl_install_root ();
  g_autofree char *home_ytdlp = g_build_filename (home, "yt-dlp", NULL);
  g_autofree char *docs_ytdlp =
      g_build_filename (home, "Documents", "yt-dlp", NULL);

  const char *candidates[] = { install, home_ytdlp, docs_ytdlp, home };
  for (gsize i = 0; i < G_N_ELEMENTS (candidates); i++)
    {
      char *found = ytdl_resolve_archive_root (candidates[i]);
      if (found != NULL)
        return found;
    }

  return NULL;
}

char *
ytdl_key_for (const char *rel)
{
  g_return_val_if_fail (rel != NULL, NULL);

  /* Backslashes are normalised because manifests written on Windows use them
   * in places the contract does not cover, and the same folder must produce
   * the same key whichever platform indexed it. */
  g_autofree char *normalised = g_strdelimit (g_strdup (rel), "\\", '/');

  g_autoptr (GChecksum) sum = g_checksum_new (G_CHECKSUM_SHA256);
  g_checksum_update (sum, (const guchar *) normalised, -1);

  guint8 digest[32];
  gsize len = sizeof digest;
  g_checksum_get_digest (sum, digest, &len);

  GString *out = g_string_sized_new (16);
  for (gsize i = 0; i < 8 && i < len; i++)
    g_string_append_printf (out, "%02x", digest[i]);

  return g_string_free (out, FALSE);
}

char *
ytdl_which (const char *name)
{
  g_return_val_if_fail (name != NULL, NULL);

  /* An explicit path is used as given, so a configured absolute pwsh is not
   * second-guessed against PATH. */
  if (strchr (name, G_DIR_SEPARATOR) != NULL)
    return g_file_test (name, G_FILE_TEST_IS_EXECUTABLE) ? g_strdup (name)
                                                         : NULL;

  const char *path_var = g_getenv ("PATH");
  if (path_var == NULL)
    return NULL;

  g_auto (GStrv) dirs = g_strsplit (path_var, G_SEARCHPATH_SEPARATOR_S, -1);
  for (gsize i = 0; dirs[i] != NULL; i++)
    {
      if (dirs[i][0] == '\0')
        continue;
      g_autofree char *candidate = g_build_filename (dirs[i], name, NULL);
      if (g_file_test (candidate, G_FILE_TEST_IS_EXECUTABLE) &&
          !g_file_test (candidate, G_FILE_TEST_IS_DIR))
        return g_steal_pointer (&candidate);
    }

  return NULL;
}

char *
ytdl_find_pwsh (void)
{
  char *found = ytdl_which ("pwsh");
  if (found != NULL)
    return found;

  /* The places a Linux pwsh lands when it is not on PATH: the Microsoft .deb,
   * a manual tarball install, and a distro package. */
  static const char *const extra[] = {
    "/usr/bin/pwsh",
    "/usr/local/bin/pwsh",
    "/opt/microsoft/powershell/7/pwsh",
    "/snap/bin/pwsh",
  };

  for (gsize i = 0; i < G_N_ELEMENTS (extra); i++)
    {
      if (g_file_test (extra[i], G_FILE_TEST_IS_EXECUTABLE))
        return g_strdup (extra[i]);
    }

  return NULL;
}
