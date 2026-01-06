import util
import os.path

self.description = "repo-add --remove removes existing package files"

pold = pmpkg("dummy", "1.0-1")
self.addpkg2db("test", pold)
self.addpkg(pold)

pnew = pmpkg("dummy", "2.0-1")
self.addpkg(pnew)

self.db['test'].dbfile = os.path.join(self.root, util.TMPDIR, "test.db.tar")

self.cmd = ["repo-add", "test.db.tar", "--remove", pnew.filename()]

self.addrule("PACMAN_RETCODE=0")
self.addrule("!FILE_EXIST=%s" % os.path.join(util.TMPDIR, pold.filename()))
self.addrule("FILE_EXIST=%s" % os.path.join(util.TMPDIR, pnew.filename()))
