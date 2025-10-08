self.description = "Upgrade sync package, with user note in localdb"

sp = pmpkg("pkg", "1.1-1")
self.addpkg2db("sync", sp);
sp = pmpkg("pkg", "1.0-1")
sp.xdata = ['user:note=user note']
self.addpkg2db("local", sp);

self.args = "-Su --note-extra foo=bar pkg"

self.addrule("PACMAN_RETCODE=0")
self.addrule("PKG_EXIST=pkg")
self.addrule("PKG_XDATA=pkg|user:note=user note")
self.addrule("PKG_XDATA=pkg|user:foo=bar")
