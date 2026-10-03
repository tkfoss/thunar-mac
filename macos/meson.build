# macOS-only helpers

thumbnailer_sources = [
  'thunar-macos-thumbnailer.m',
]
thumbnailer_sources += gnome.gdbus_codegen(
  'thunar-macos-thumbnailer-dbus',
  sources: '..' / 'thunar' / 'thunar-thumbnailer-dbus.xml',
  namespace: 'Tmt',
  install_header: false,
)
thumbnailer_sources += gnome.gdbus_codegen(
  'thunar-macos-thumbnail-cache-dbus',
  sources: '..' / 'thunar' / 'thunar-thumbnail-cache-dbus.xml',
  namespace: 'Tmt',
  install_header: false,
)

thumbnailer_dir = helper_path_prefix / 'Thunar'

executable(
  'thunar-macos-thumbnailer',
  thumbnailer_sources,
  objc_args: ['-fobjc-arc', '-Wno-unused-parameter', '-DG_LOG_DOMAIN="thunar-macos-thumbnailer"'],
  dependencies: [
    gio,
    gdk_pixbuf,
    dependency('appleframeworks', modules: ['Foundation', 'QuickLookThumbnailing', 'CoreGraphics']),
  ],
  install: true,
  install_dir: thumbnailer_dir,
)

foreach service_name : ['org.freedesktop.thumbnails.Thumbnailer1', 'org.freedesktop.thumbnails.Cache1']
  configure_file(
    input: 'thumbnailer.service.in',
    output: '@0@.service'.format(service_name),
    configuration: {
      'NAME': service_name,
      'EXEC': thumbnailer_dir / 'thunar-macos-thumbnailer',
    },
    install: true,
    install_dir: get_option('prefix') / get_option('datadir') / 'dbus-1' / 'services',
  )
endforeach

# Thunar.app: launcher, Info.plist and private bus config; the bundle itself
# is assembled from the installed files by make-app-bundle.sh (.py)
macos_data_dir = get_option('prefix') / get_option('datadir') / 'Thunar' / 'macos'

configure_file(
  input: 'dbus-session.conf.in',
  output: 'dbus-session.conf',
  configuration: {
    'SERVICEDIR': get_option('prefix') / get_option('datadir') / 'dbus-1' / 'services',
  },
  install: true,
  install_dir: macos_data_dir,
)

configure_file(
  input: 'Info.plist.in',
  output: 'Info.plist',
  configuration: {
    'VERSION': meson.project_version(),
  },
  install: true,
  install_dir: macos_data_dir,
)

executable(
  'thunar-app-launcher',
  'thunar-app-launcher.c',
  dependencies: dependency('appleframeworks', modules: ['CoreFoundation']),
  install: true,
  install_dir: macos_data_dir,
)

install_data('make-app-bundle.sh', 'make-app-bundle.py', install_dir: macos_data_dir, install_mode: 'rwxr-xr-x')
