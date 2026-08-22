testpkg_sources = files('testpkg.c')
vercmp_sources = files('vercmp.c')
packinstall_sources = files('packinstall.c')
packinstall_lfs_sources = files('packinstall-lfs.c')

if get_option('packinstall-lfs')
  executable(
    'packinstall-lfs',
    packinstall_lfs_sources,
    install : true,
  )
endif
