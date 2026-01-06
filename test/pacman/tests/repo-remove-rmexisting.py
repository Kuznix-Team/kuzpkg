import util
import os.path

self.description = "repo-remove --remove removes package files"

p = pmpkg("dummy", "1.0-1")
self.addpkg2db("test", p)
self.addpkg(p)

self.db['test'].dbfile = os.path.join(self.root, util.TMPDIR, "test.db.tar")

self.cmd = ["repo-remove", "test.db.tar", "--remove", "dummy"]

self.addrule("PACMAN_RETCODE=0")
self.addrule("!FILE_EXIST=%s" % os.path.join(util.TMPDIR, p.filename()))
