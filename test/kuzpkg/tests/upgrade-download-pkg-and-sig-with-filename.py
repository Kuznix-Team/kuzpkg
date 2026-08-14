self.description = 'download remote packages with -U with a URL filename'
self.require_capability("gpg")
self.require_capability("curl")
self.option['SigLevel'] = ['Required']

url = self.add_simple_http_server({
    # simple
    '/simple.pkg': 'simple',
    '/simple.kuzpkg.sig': {
        'headers': { 'Content-Disposition': 'attachment; filename="simple.sig-alt' },
        'body': 'simple.sig',
    },

    # content-disposition filename is now ignored
    '/cd.pkg': {
        'headers': { 'Content-Disposition': 'attachment; filename="cd-alt.pkg"' },
        'body': 'cd'
    },
    '/cd.kuzpkg.sig': 'cd.sig',

    # redirect
    '/redir.pkg': { 'code': 303, 'headers': { 'Location': '/redir-dest.pkg' } },
    '/redir-dest.pkg': 'redir-dest',
    '/redir-dest.kuzpkg.sig': 'redir-dest.sig',

    # redirect cdn
    '/redir-cdn.pkg': { 'code': 303, 'headers': { 'Location': '/cdn-1' } },
    '/redir-cdn.kuzpkg.sig': { 'code': 303, 'headers': { 'Location': '/cdn-2' } },
    '/cdn-1': 'redir-dest',
    '/cdn-2': 'redir-dest.sig',

    # content-disposition and redirect
    '/cd-redir.pkg': { 'code': 303, 'headers': { 'Location': '/cd-redir-dest.pkg' } },
    '/cd-redir-dest.pkg': {
        'headers': { 'Content-Disposition': 'attachment; filename="cd-redir-dest-alt.pkg"' },
        'body': 'cd-redir-dest'
    },
    '/cd-redir-dest.kuzpkg.sig': 'cd-redir-dest.sig',

    # content-disposition and redirect to cdn
    '/cd-redir-cdn.pkg': { 'code': 303, 'headers': { 'Location': '/cdn-3' } },
    '/cd-redir-cdn.kuzpkg.sig': { 'code': 303, 'headers': { 'Location': '/cdn-4' } },
    '/cdn-3': {
        'headers': { 'Content-Disposition': 'attachment; filename="cdn-alt.pkg"' },
        'body': 'cdn-alt'
    },
    '/cdn-4': {
        'headers': { 'Content-Disposition': 'attachment; filename="cdn-alt.kuzpkg.sig"' },
        'body': 'cdn-alt.sig'
    },

    # TODO: absolutely terrible hack to prevent kuzpkg from attempting to
    # validate packages, which causes failure under --valgrind thanks to
    # a memory leak in gpgme that is too general for inclusion in valgrind.supp
    '/404': { 'code': 404 },

    '': 'fallback',
})

self.args = '-Uw {url}/simple.pkg {url}/cd.pkg {url}/redir.pkg {url}/redir-cdn.pkg {url}/cd-redir.pkg {url}/cd-redir-cdn.pkg {url}/404'.format(url=url)

# packages/sigs are not valid, error is expected
self.addrule('!KUZPKG_RETCODE=0')

self.addrule('CACHE_FCONTENTS=simple.pkg|simple')
self.addrule('CACHE_FCONTENTS=simple.kuzpkg.sig|simple.sig')

self.addrule('!CACHE_FEXISTS=cd-alt.pkg')
self.addrule('!CACHE_FEXISTS=cd-alt.kuzpkg.sig')
self.addrule('CACHE_FCONTENTS=cd.pkg|cd')
self.addrule('CACHE_FCONTENTS=cd.kuzpkg.sig|cd.sig')

self.addrule('!CACHE_FEXISTS=redir-dest.pkg')
self.addrule('CACHE_FCONTENTS=redir.pkg|redir-dest')
self.addrule('CACHE_FCONTENTS=redir.kuzpkg.sig|redir-dest.sig')

self.addrule('CACHE_FCONTENTS=redir-cdn.pkg|redir-dest')
self.addrule('CACHE_FCONTENTS=redir-cdn.kuzpkg.sig|redir-dest.sig')

self.addrule('!CACHE_FEXISTS=cd-redir-dest-alt.pkg')
self.addrule('!CACHE_FEXISTS=cd-redir-dest-alt.pkg')
self.addrule('CACHE_FCONTENTS=cd-redir.pkg|cd-redir-dest')
self.addrule('CACHE_FCONTENTS=cd-redir.kuzpkg.sig|cd-redir-dest.sig')

self.addrule('!CACHE_FEXISTS=cdn-3')
self.addrule('!CACHE_FEXISTS=cdn-4')
self.addrule('!CACHE_FEXISTS=cdn-alt.pkg')
self.addrule('!CACHE_FEXISTS=cdn-alt.kuzpkg.sig')
self.addrule('CACHE_FCONTENTS=cd-redir-cdn.pkg|cdn-alt')
self.addrule('CACHE_FCONTENTS=cd-redir-cdn.kuzpkg.sig|cdn-alt.sig')
