plugin_sources = [
  'thunar-uca-plugin.c',
]

shared_module(
  'thunar-uca',
  plugin_sources,
  name_prefix: '',
  name_suffix: thunarx_module_suffix,
  gnu_symbol_visibility: 'hidden',
  c_args: [
    '-DG_LOG_DOMAIN="@0@"'.format('thunar-uca'),
  ],
  include_directories: [
    include_directories('..'),
    include_directories('..' / '..'),
  ],
  dependencies: [
    glib,
    gtk,
    libxfce4ui,
    libxfce4kbd,
    libxfce4util,
    libintl,
  ],
  link_with: [
    libthunarx,
    libthunar_uca,
  ],
  install: true,
  install_dir: get_option('prefix') / get_option('libdir') / 'thunarx-@0@'.format(thunarx_version_api),
)
