import util
import os.path

self.description = "repo-add --remove does not remove package files if updating existing version"

p = pmpkg("dummy", "1.0-1")
self.addpkg2db("test", p)
self.addpkg(p)

self.db['test'].dbfile = os.path.join(self.root, util.TMPDIR, "test.db.tar")

self.cmd = ["repo-add", "test.db.tar", "--remove", p.filename()]

self.addrule("PACMAN_RETCODE=0")
self.addrule("FILE_EXIST=%s" % os.path.join(util.TMPDIR, p.filename()))
