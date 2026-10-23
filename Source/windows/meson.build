# Version and icon resources for the Windows programs. Empty lists on other systems.
gui_resources = []
cli_resources = []

if host_machine.system() == 'windows'
  windows = import('windows')
  version_parts = meson.project_version().split('.')
  icon = get_option('windows_icon')
  if icon == ''
    error('Set -Dwindows_icon=PATH to the .ico file (the Makefile does this)')
  endif

  gui_rc = configure_file(input : 'app.rc.in', output : 'railway-variable-auditor.rc',
    configuration : {
      'ICON' : icon,
      'DESCRIPTION' : 'Railway Variable Auditor',
      'INTERNAL' : 'railway-variable-auditor',
      'VERSION' : meson.project_version(),
      'MAJOR' : version_parts[0], 'MINOR' : version_parts[1], 'PATCH' : version_parts[2],
    })
  cli_rc = configure_file(input : 'app.rc.in', output : 'railway-variable-audit.rc',
    configuration : {
      'ICON' : icon,
      'DESCRIPTION' : 'Railway Variable Audit (command line)',
      'INTERNAL' : 'railway-variable-audit',
      'VERSION' : meson.project_version(),
      'MAJOR' : version_parts[0], 'MINOR' : version_parts[1], 'PATCH' : version_parts[2],
    })
  gui_resources = windows.compile_resources(gui_rc)
  cli_resources = windows.compile_resources(cli_rc)
endif
