self.description = "Accept paths with '..' substring that is not a parent-dir component"

p = pmpkg("okpkg", "1.0-1")
p.files = ["usr/share/foo..bar/baz"]
self.addpkg(p)

self.args = "-U %s" % p.filename()

self.addrule("PACMAN_RETCODE=0")
self.addrule("PKG_EXIST=okpkg")
self.addrule("FILE_EXIST=usr/share/foo..bar/baz")
