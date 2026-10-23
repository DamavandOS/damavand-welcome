project(
  'damavand-welcome',
  'c',
  version: '1.0.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=release',
  ],
)

gtk_dep = dependency('gtk4', version: '>=4.12')
adw_dep = dependency('libadwaita-1', version: '>=1.4')

executable(
  'dwelcome',
  'src/main.c',
  dependencies: [gtk_dep, adw_dep],
  install: true,
  install_dir: '/vendor/usr/bin',
)
