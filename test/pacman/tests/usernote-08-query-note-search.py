self.description = "Search for user note"

sp = pmpkg("searchpkg")
sp.xdata = ['user:foo=bar']
self.addpkg2db("local", sp);

self.args = "-Q --note-search bar"

self.addrule("PACMAN_RETCODE=0")
self.addrule("PACMAN_OUTPUT=searchpkg")
self.addrule("PACMAN_OUTPUT=foo=bar")

