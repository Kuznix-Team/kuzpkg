self.description = "Refuse to extract archive entry with leading '/'"

p = pmpkg("evilpkg", "1.0-1")
p.allow_unsafe_paths = True
p.files = ["/etc/evil"]
self.addpkg(p)

self.args = "-U %s" % p.filename()

self.addrule("PACMAN_RETCODE=1")
self.addrule("!FILE_EXIST=etc/evil")
