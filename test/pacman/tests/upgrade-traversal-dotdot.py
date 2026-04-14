self.description = "Refuse to extract archive entry with '..' component escaping root"

p = pmpkg("evilpkg", "1.0-1")
p.files = ["foo/../../etc/evil"]
self.addpkg(p)

self.args = "-U %s" % p.filename()

self.addrule("PACMAN_RETCODE=1")
self.addrule("!FILE_EXIST=etc/evil")
