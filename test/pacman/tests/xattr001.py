self.description = "ExtractXattr allowlist filters extracted extended attributes"

# Only restore xattrs whose name matches the allowlist: a literal name and a
# shell-style glob.  Everything else in the payload must be dropped.
self.option['ExtractXattr'] = ['user.keep', 'user.glob.*']

p = pmpkg("dummy")
p.files = ["usr/bin/dummy"]
p.xattrs = {
    "usr/bin/dummy": {
        "user.keep": "yes",          # matches the literal pattern
        "user.glob.one": "1",        # matches the glob pattern
        "user.drop": "no",           # matches nothing -> dropped
    },
}
self.addpkg(p)

self.args = "-U %s" % p.filename()

self.addrule("PACMAN_RETCODE=0")
self.addrule("FILE_EXIST=usr/bin/dummy")
# allowlisted attributes survive with their values intact
self.addrule("FILE_XATTR=usr/bin/dummy|user.keep=yes")
self.addrule("FILE_XATTR=usr/bin/dummy|user.glob.one=1")
# the non-matching attribute is gone
self.addrule("!FILE_XATTR=usr/bin/dummy|user.drop")
