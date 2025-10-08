self.description = "Change localdb notes"

sp = pmpkg("pkg")
sp.xdata = ["user:note=user note", 'user:foo=bar']
self.addpkg2db("local", sp);

self.args = "-D --delete-note note --delete-note foo --note-extra bar=foo pkg"

self.addrule("PACMAN_RETCODE=0")
self.addrule("PKG_EXIST=pkg")
self.addrule("!PKG_XDATA=pkg|user:note=user note")
self.addrule("!PKG_XDATA=pkg|user:foo=bar")
self.addrule("PKG_XDATA=pkg|user:bar=foo")
