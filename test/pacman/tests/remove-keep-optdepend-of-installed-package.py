self.description = "Do not remove packages which is an optdepend of another package"

p1 = pmpkg("a")
p1.depends = ["b"]
self.addpkg2db("local", p1)

p2 = pmpkg("b")
self.addpkg2db("local", p2)


p3 = pmpkg("c")
p3.optdepends = ["b: for foobar"]
self.addpkg2db("local", p3)

self.args = "-Rs %s" % p1.name

self.addrule("PACMAN_RETCODE=0")
self.addrule("!PKG_EXIST=%s" % p1.name)
self.addrule("PKG_EXIST=%s" % p2.name)
self.addrule("PKG_EXIST=%s" % p3.name)
self.addrule("!PACMAN_OUTPUT=%s optionally requires %s" % (p3.name, p2.name))
