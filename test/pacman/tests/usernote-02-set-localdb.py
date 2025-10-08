self.description = "Set note in localdb"

sp = pmpkg("pkg")
self.addpkg2db("local", sp);

self.args = "-D --note 'user note' --note-extra foo=bar pkg"

self.addrule("PACMAN_RETCODE=0")
self.addrule("PKG_EXIST=pkg")
self.addrule("PKG_XDATA=pkg|user:note=user note")
self.addrule("PKG_XDATA=pkg|user:foo=bar")
