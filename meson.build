project('railway-variable-auditor', 'c',
  version : '0.1.0',
  license : 'GPL-3.0-only',
  default_options : ['c_std=c11', 'warning_level=2'])

subdir('Source/windows')
subdir('Source/SDK')
subdir('Source/Core')
subdir('Source/Tools')
subdir('Source/App')

# Man pages, the website, the menu entry and the icon are for Linux. The Windows installer
# takes the website straight from the source tree.
if host_machine.system() != 'windows'
  install_man(
    'Documentation/Man Pages/railway-variable-auditor.1',
    'Documentation/Man Pages/railway-variable-audit.1',
    'Documentation/Man Pages/railway-variable-auditor-csv.5',
    'Documentation/Man Pages/railway.3',
    'Documentation/Man Pages/rvcore.3')

  install_subdir('Documentation/Website',
    install_dir : get_option('datadir') / 'doc' / 'railway-variable-auditor')

  # The menu entry and icon, so the window appears in the desktop's application list.
  install_data('Packaging/za.co.wexfordhouse.RailwayVariableAuditor.desktop',
    install_dir : get_option('datadir') / 'applications')
  install_data('Documentation/Website/assets/img/favicon.svg',
    install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
    rename : 'za.co.wexfordhouse.RailwayVariableAuditor.svg')
endif
