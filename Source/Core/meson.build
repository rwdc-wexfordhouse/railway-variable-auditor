cc = meson.get_compiler('c')
core_deps = [rw_sdk_dep, dependency('gmodule-2.0'), cc.find_library('m', required : false)]
if host_machine.system() == 'windows'
  core_deps += cc.find_library('bcrypt')
endif

core_inc = include_directories('.')

rv_core = static_library('rvcore',
  ['rv-util.c', 'rv-random.c', 'rv-generate.c', 'rv-policy.c', 'rv-inventory.c',
   'rv-classify.c', 'rv-scram.c', 'rv-pgurl.c', 'rv-roles.c', 'rv-roles-run.c', 'rv-tunnel.c', 'rv-libpq.c', 'rv-scan.c', 'rv-plan.c', 'rv-run.c', 'rv-undo.c'],
  include_directories : core_inc,
  dependencies : core_deps)

rv_core_dep = declare_dependency(
  link_with : rv_core,
  include_directories : core_inc,
  dependencies : core_deps)

if get_option('tests')
  test_rvcore = executable('test-rvcore', 'tests/test-rvcore.c',
    dependencies : rv_core_dep)
  test('rvcore', test_rvcore)

  # A live check of the real database interface against a throwaway PostgreSQL in a container.
  # It is skipped when Docker is not available. Run it with: meson test --suite live
  test_pg_live = executable('test-pg-live', 'tests/test-pg-live.c',
    dependencies : rv_core_dep)
  test('postgres live', find_program('tests/postgres_live_test.sh'),
    args : [test_pg_live], suite : 'live', timeout : 300)
endif
