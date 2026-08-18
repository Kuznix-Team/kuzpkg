# Favor runtime performance for the package manager and its command-line tools.
# Keep this portable: do not use -march=native so binaries remain distributable.
add_project_arguments('-O3', language : 'c')

kuzpkg_sources = files('''
  check.h check.c
  conf.h conf.c
  database.c
  deptest.c
  files.c
  package.h package.c
  kuzpkg.h kuzpkg.c
  query.c
  remove.c
  sighandler.h sighandler.c
  sync.c
  callback.h callback.c
  upgrade.c
  util.h util.c
'''.split())

kuzpkg_conf_sources = files('''
  kuzpkg-conf.c
  util.h util.c
  callback.h callback.c
  conf.h conf.c
'''.split())
