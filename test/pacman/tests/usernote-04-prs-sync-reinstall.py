self.description = "Reinstall sync package, with user note in localdb"

sp = pmpkg("pkg")
self.addpkg2db("sync", sp);
sp = pmpkg("pkg")
sp.xdata = ['user:note=user note']
self.addpkg2db("local", sp);

self.args = "-S --note-extra foo=bar pkg"

self.addrule("PACMAN_RETCODE=0")
self.addrule("PKG_EXIST=pkg")
self.addrule("PKG_XDATA=pkg|user:note=user note")
self.addrule("PKG_XDATA=pkg|user:foo=bar")
