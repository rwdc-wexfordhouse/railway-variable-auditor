gtk_dep = dependency('gtk4', version : '>=4.10')

app_sources = ['app.c']
app_args = []

app_deps = [rv_core_dep, gtk_dep]
# Remote control: a demo mode on invented data, and a screenshot driver, for the
# documentation. Off by default.
if get_option('automation')
  app_sources += 'demo.c'
  app_args += '-DRVC_AUTOMATION=1'
endif

executable('railway-variable-auditor', [app_sources, gui_resources],
  win_subsystem : 'windows',
  dependencies : app_deps,
  c_args : app_args,
  install : true)
