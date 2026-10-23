# The old read only scan. Built so that it keeps working, but not installed.
executable('railway-variable-auditor-scan', 'scan.c',
  dependencies : rv_core_dep,
  install : false)

audit_exe = executable('railway-variable-audit', ['audit.c', cli_resources],
  dependencies : rv_core_dep,
  install : true)

if get_option('tests') and host_machine.system() != 'windows'
  test('audit end to end', find_program('sh'),
    args : [files('tests/test-audit.sh'), audit_exe, files('tests/fake-railway.py')],
    suite : 'tools')
endif
