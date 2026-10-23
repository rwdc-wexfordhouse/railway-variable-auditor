glib_dep = dependency('glib-2.0', version : '>=2.70')
json_dep = dependency('json-glib-1.0')
gio_dep = dependency('gio-2.0')

sdk_inc = include_directories('.')

rw_sdk = static_library('railway',
  ['railway.c', 'railway-cli.c'],
  include_directories : sdk_inc,
  dependencies : [glib_dep, gio_dep, json_dep])

rw_sdk_dep = declare_dependency(
  link_with : rw_sdk,
  include_directories : sdk_inc,
  dependencies : [glib_dep, gio_dep, json_dep])

if get_option('tests')
  test_railway = executable('test-railway',
    'tests/test-railway.c',
    dependencies : rw_sdk_dep)

  test('railway-sdk', test_railway)
endif
