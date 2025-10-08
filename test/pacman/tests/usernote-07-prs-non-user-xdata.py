self.description = "Upgrade from file, with non-user xdata changed"

lp = pmpkg("pkg")
lp.xdata=['user:note=user note', 'pkgtype=pkg']
self.addpkg2db("local", lp)

p = pmpkg("pkg", "1.0-2")
p.xdata=['newfield=test']
self.addpkg(p)

self.args = "-U --note-extra foo=bar %s" % p.filename()

self.addrule("PACMAN_RETCODE=0")
self.addrule("PKG_VERSION=pkg|1.0-2")
self.addrule("PKG_XDATA=pkg|user:note=user note")
self.addrule("PKG_XDATA=pkg|user:foo=bar")
self.addrule("PKG_XDATA=pkg|newfield=test")
self.addrule("!PKG_XDATA=pkg|pkgtype=pkg")
