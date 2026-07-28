self.description = "XferCommand returning 127 is not an exec failure"

self.option["XferCommand"] = ["/bin/sh -c 'exit 127'"]
self.option["SigLevel"] = ["Never"]

self.args = "-Uw https://example.invalid/foo.pkg"

self.addrule("!PACMAN_RETCODE=0")
self.addrule("!PACMAN_OUTPUT=failed to execute")
