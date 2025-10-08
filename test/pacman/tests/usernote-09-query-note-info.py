self.description = "Print user notes in pkg info query"

sp = pmpkg("pkg")
sp.xdata = ['user:foo=bar']
self.addpkg2db("local", sp);

self.args = "-Qi pkg"

self.addrule("PACMAN_RETCODE=0")
self.addrule("PACMAN_OUTPUT=foo=bar")

