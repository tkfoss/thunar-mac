tse_sources = ['main.c']
tse_dependencies = [glib, gtk, libxfce4ui, libxfce4util]
if host_machine.system() == 'darwin'
  add_languages('objc', native: false, required: true)
  tse_sources += ['tse-macos.m', 'tse-macos.h']
  tse_dependencies += dependency('appleframeworks', modules: ['AppKit'])
endif

executable(
  'thunar-sendto-email',
  tse_sources,
  c_args: [
    '-DG_LOG_DOMAIN="@0@"'.format('thunar-sendto-email'),
  ],
  include_directories: [
    include_directories('..' / '..'),
  ],
  dependencies: tse_dependencies,
  install: true,
  install_dir: helper_path_prefix / 'Thunar',
)

desktop_in = configure_file(
  configuration: configuration_data({
    'HELPERDIR': helper_path_prefix,
  }),
  input: 'thunar-sendto-email.desktop.in.in',
  output: 'thunar-sendto-email.desktop.in',
  install: false,
)
i18n.merge_file(
  input: desktop_in,
  output: 'thunar-sendto-email.desktop',
  po_dir: '..' / '..' / 'po',
  type: 'desktop',
  install: true,
  install_dir: get_option('prefix') / get_option('datadir') / 'Thunar' / 'sendto',
)
