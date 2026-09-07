project('ytdl-gtk', 'c',
  version : '0.1.0',
  license : 'MIT',
  default_options : [
    # gnu11, not c11. GLib's g_autoptr/g_autofree are __attribute__((cleanup))
    # underneath, which strict ISO mode plus -Wpedantic rejects. Those macros
    # are how a C codebase gets scope-based cleanup at all, and hand-rolled
    # goto-out chains in their place would be the actual memory-safety risk.
    'c_std=gnu11',
    # 2 rather than 3: level 3 adds -Wpedantic, which fires throughout the
    # GLib headers rather than on anything written here. The specific warnings
    # worth having are named explicitly below instead.
    'warning_level=2',
    'werror=true',
  ])

# NO RUST, NO WEBVIEW, NO BUNDLED RUNTIME. That is the point of this app.
#
# The dependency list is three system libraries that a GNOME desktop already
# has. gtk4 pulls glib and gio itself; both are named anyway so a build failure
# says which one is missing rather than making you read gtk4's pkg-config file.
#
# json-glib rather than a hand-rolled parser: info.json is yt-dlp's output and
# routinely contains multi-megabyte comment arrays, escaped unicode and deeply
# nested objects. Writing a JSON parser to avoid a GNOME-platform dependency
# that ships on every target distro would be trading a real risk for an
# imaginary one.
gtk    = dependency('gtk4',          version : '>= 4.10')
jsonc  = dependency('json-glib-1.0', version : '>= 1.6')
glib   = dependency('glib-2.0',      version : '>= 2.74')
gio    = dependency('gio-2.0',       version : '>= 2.74')

deps = [gtk, jsonc, glib, gio]

# -Werror with warning_level=3 is deliberate for a C codebase reimplementing
# logic that used to be memory-safe. The compiler is most of what replaces the
# borrow checker here, so its output is not advisory.
add_project_arguments(
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  '-Wcast-align',
  '-Wwrite-strings',
  '-Wno-unused-parameter',   # GTK callbacks take arguments you often ignore
  language : 'c')

core_sources = files(
  'src/paths.c',
  'src/archive.c',
)

core = static_library('ytdlcore', core_sources,
  dependencies : deps,
  include_directories : include_directories('src'))

executable('ytdl-gtk',
  files('src/main.c', 'src/library_view.c'),
  dependencies : deps,
  link_with : core,
  include_directories : include_directories('src'),
  install : true)

# The conformance test the archive layout contract asks every out-of-repo
# consumer to keep: build a fixture tree in the documented shape and assert
# this reader finds it. A layout bump then breaks the build here rather than
# arriving as somebody's empty library.
#
# CLI_VERSION lives at the REPOSITORY root, one level up -- it is shared by
# every app here, not owned by this one. Its path is baked in at compile time
# so the test can assert REQUIRES_ARCHIVE_LAYOUT against
# YTDL_SUPPORTED_ARCHIVE_LAYOUT: two copies of one fact that must not drift.
cli_version = meson.project_source_root() / '..' / 'CLI_VERSION'

test_archive = executable('test-archive',
  files('tests/test_archive.c'),
  c_args : ['-DYTDL_CLI_VERSION_PATH="' + cli_version + '"'],
  dependencies : deps,
  link_with : core,
  include_directories : include_directories('src'))

test('archive conformance', test_archive, protocol : 'tap')
