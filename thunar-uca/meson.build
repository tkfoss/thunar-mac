thunar_uca_sources = [
  'thunar-uca-model.h',
  'thunar-uca-model.c',
  'thunar-uca-context.h',
  'thunar-uca-context.c',
  'thunar-uca-base-provider.h',
  'thunar-uca-base-provider.c',
]

thunar_uca_dependencies = [
  gio,
  gio_unix,
  gthread,
  gtk,
  libxfce4ui,
  libxfce4util,
]

libthunar_uca = static_library(
  'thunar-uca',
  thunar_uca_sources,
  sources: xfce_revision_h,
  c_args: [
    '-DG_LOG_DOMAIN="@0@"'.format('thunar-uca'),
  ],
  include_directories: [
    include_directories('..'),
  ],
  dependencies: thunar_uca_dependencies,
  link_with: [
    libthunarx,
  ],
  install: false,
)

i18n.merge_file(
  input: 'uca.xml.in',
  output: 'uca.xml',
  data_dirs: '..',
  po_dir: '..' / 'po',
  type: 'xml',
  install: true,
  install_dir: get_option('prefix') / get_option('sysconfdir') / 'xdg' / 'Thunar',
)
