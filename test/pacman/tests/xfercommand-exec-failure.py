self.description = "Missing XferCommand reports an exec failure"

self.option["XferCommand"] = [
    "/bin/xfercommand-does-not-exist %u %o"
]
self.option["SigLevel"] = ["Never"]

self.args = "-Uw https://example.invalid/foo.pkg"

self.addrule("!PACMAN_RETCODE=0")
self.addrule(
    "PACMAN_OUTPUT=failed to execute '/bin/xfercommand-does-not-exist'"
)
