/*
 * packinstall.c - build, stage, package and optionally install a source tree.
 *
 * Native build-system packaging helper for Kuzpkg.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/utsname.h>

#define REL "1"

static int exists(const char *path) { return access(path, F_OK) == 0; }

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *base_name(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static int run_command(const char *command)
{
    int status;
    fprintf(stderr, "packinstall: %s\n", command);
    status = system(command);
    if (status == -1) {
        fprintf(stderr, "packinstall: cannot execute command: %s\n", strerror(errno));
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        fprintf(stderr, "packinstall: command terminated by signal %d\n", WTERMSIG(status));
    return 128;
}

static char *shell_quote(const char *s)
{
    size_t n = 2;
    char *out, *p;
    const char *q;
    for (q = s; *q; q++) n += (*q == '\'') ? 4 : 1;
    out = malloc(n + 1);
    if (!out) return NULL;
    p = out;
    *p++ = '\'';
    for (q = s; *q; q++) {
        if (*q == '\'') { memcpy(p, "'\\''", 4); p += 4; }
        else *p++ = *q;
    }
    *p++ = '\'';
    *p = '\0';
    return out;
}

static int clean_stage(const char *stage)
{
    char *q = shell_quote(stage), *cmd;
    int ret;
    if (!q) return -1;
    if (!strcmp(stage, "/") || !strcmp(stage, ".") || !strcmp(stage, "..")) {
        fprintf(stderr, "packinstall: refusing unsafe staging directory '%s'\n", stage);
        free(q);
        return -1;
    }
    if (asprintf(&cmd, "rm -rf -- %s && mkdir -p -- %s", q, q) < 0) {
        free(q);
        return -1;
    }
    ret = run_command(cmd);
    free(cmd); free(q);
    return ret;
}

static const char *detect_arch(void)
{
    static struct utsname u;
    if (uname(&u) != 0) return "any";
    if (!strcmp(u.machine, "x86_64")) return "x86_64";
    if (!strcmp(u.machine, "i686") || !strcmp(u.machine, "i386") ||
        !strcmp(u.machine, "i486") || !strcmp(u.machine, "i586")) return "i686";
    return u.machine;
}

static char *package_version(const char *tree)
{
    const char *b = base_name(tree), *p, *dash;
    char *v;
    if (!strncmp(b, "expect", 6) || !strncmp(b, "tcl", 3)) {
        p = b; while (*p && (*p < '0' || *p > '9')) p++;
        return strdup(*p ? p : "0");
    }
    if (!strncmp(b, "unzip", 5)) {
        p = b + 5;
        if (p[0] && p[1] && p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
            asprintf(&v, "%c.%c%s", p[0], p[1], p + 2) >= 0) return v;
    }
    if (!strcmp(b, "docbook-xml")) return strdup("4.5");
    p = strrchr(b, '-');
    dash = strrchr(b, '_');
    if (dash && (!p || dash > p)) p = dash;
    if (p && p[1] && ((p[1] >= '0' && p[1] <= '9') || p[1] == 'v')) p++;
    else { p = b; while (*p && (*p < '0' || *p > '9')) p++; }
    v = strdup(*p ? p : "0");
    if (!v) return NULL;
    for (char *q = v; *q; q++) if (*q == '_') *q = '.';
    return v;
}

static char *package_name(const char *tree)
{
    const char *b = base_name(tree), *p = b;
    char *name;
    while (*p >= '0' && *p <= '9') p++;
    if (p - b >= 2 && p - b <= 4 && *p == '-') b = p + 1;
    name = strdup(b);
    if (!name) return NULL;
    for (size_t i = 1; name[i]; i++) {
        if (name[i] == '-' || name[i] == '_') {
            const char *q = name + i + 1;
            if ((*q >= '0' && *q <= '9') || *q == 'v') { name[i] = '\0'; break; }
        }
    }
    return name;
}

static int build_project(const char *src, const char *stage)
{
    char *qstage = shell_quote(stage), *cmd = NULL;
    int ret = -1;
    if (!qstage) return -1;

#define TRY_BUILD(...) do { \
        if (asprintf(&cmd, __VA_ARGS__) < 0) goto out; \
        ret = run_command(cmd); \
        if (ret == 0) goto out; \
        free(cmd); cmd = NULL; \
    } while (0)

    if (exists("configure"))
        TRY_BUILD("./configure --prefix=/usr && make && DESTDIR=%s make install", qstage);
    if (exists("meson.build"))
        TRY_BUILD("meson setup build --prefix=/usr --buildtype=plain && meson compile -C build && DESTDIR=%s meson install -C build", qstage);
    if (exists("CMakeLists.txt"))
        TRY_BUILD("cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr && cmake --build build && DESTDIR=%s cmake --install build", qstage);
    if (exists("build.ninja"))
        TRY_BUILD("ninja && DESTDIR=%s ninja install", qstage);
    if (exists("Cargo.toml"))
        TRY_BUILD("mkdir -p %s/usr && cargo install --path . --root %s/usr --locked", qstage, qstage);
    if (exists("pyproject.toml") || exists("setup.py") || exists("setup.cfg"))
        TRY_BUILD("python3 -m pip install . --root %s --prefix /usr --no-deps --no-build-isolation", qstage);
    if (exists("Build.PL"))
        TRY_BUILD("perl Build.PL --destdir %s --install_base /usr && ./Build && ./Build install", qstage);
    if (exists("Makefile.PL"))
        TRY_BUILD("perl Makefile.PL PREFIX=/usr && make && DESTDIR=%s make install", qstage);
    if (exists("go.mod"))
        TRY_BUILD("mkdir -p %s/usr/bin && GOBIN=%s/usr/bin go install ./...", qstage, qstage);

    /* RubyGems: build the gem from the project's gemspec, then install it
       directly into the package staging tree without touching the host. */
    if (exists("Gemfile") || exists("Rakefile")) {
        TRY_BUILD("gem install bundler --no-document >/dev/null 2>&1 || true; bundle check >/dev/null 2>&1 || bundle install --local; ruby -e 'require \"rubygems/package\"; puts \"ruby build\"' >/dev/null; mkdir -p %s/usr/bin; gemfile=$(find . -maxdepth 1 -type f -name '*.gemspec' -print -quit); test -n \"$gemfile\"; gem build \"$gemfile\"; gem=$(find . -maxdepth 1 -type f -name '*.gem' -print -quit); gemdir=$(ruby -rrubygems -e 'print Gem.default_dir'); mkdir -p %s$gemdir; gem install --local \"$gem\" --install-dir %s$gemdir --bindir %s/usr/bin --ignore-dependencies --no-document", qstage, qstage, qstage, qstage);
    } else if (exists("*.gemspec")) {
        TRY_BUILD("mkdir -p %s/usr/bin; gemspec=$(find . -maxdepth 1 -type f -name '*.gemspec' -print -quit); test -n \"$gemspec\"; gem build \"$gemspec\"; gem=$(find . -maxdepth 1 -type f -name '*.gem' -print -quit); gemdir=$(ruby -rrubygems -e 'print Gem.default_dir'); mkdir -p %s$gemdir; gem install --local \"$gem\" --install-dir %s$gemdir --bindir %s/usr/bin --ignore-dependencies --no-document", qstage, qstage, qstage, qstage);
    }

    if (exists("Makefile") || exists("makefile") || exists("GNUmakefile"))
        TRY_BUILD("make && DESTDIR=%s make install", qstage);

    fprintf(stderr, "packinstall: no supported build system succeeded in %s\n", src);
    ret = 1;
out:
    free(cmd); free(qstage);
#undef TRY_BUILD
    return ret;
}

static int write_pkgbuild(const char *work, const char *stage, const char *name,
                          const char *version, const char *arch)
{
    char *qstage = shell_quote(stage), *cmd;
    FILE *f;
    if (!qstage) return -1;
    if (asprintf(&cmd, "%s/PKGBUILD", work) < 0) { free(qstage); return -1; }
    f = fopen(cmd, "w");
    if (!f) { fprintf(stderr, "packinstall: cannot write %s: %s\n", cmd, strerror(errno)); free(cmd); free(qstage); return -1; }
    fprintf(f, "pkgname='%s'\npkgver='%s'\npkgrel='%s'\npkgdesc='%s'\narch=('%s')\nsource=()\nsha256sums=()\n\npackage() {\n    cp -a -- %s/. \"$pkgdir\"/\n}\n", name, version, REL, name, arch, qstage);
    fclose(f); free(cmd); free(qstage);
    return 0;
}

static int package_and_install(const char *stage, const char *name, const char *version,
                               const char *arch, int do_install)
{
    char templ[] = "/tmp/packinstall.XXXXXX", *work, *qwork, *cmd = NULL;
    int ret;
    work = mkdtemp(templ);
    if (!work) { fprintf(stderr, "packinstall: mkdtemp: %s\n", strerror(errno)); return 1; }
    if (write_pkgbuild(work, stage, name, version, arch) != 0) return 1;
    qwork = shell_quote(work);
    if (!qwork) return 1;
    if (asprintf(&cmd, "cd %s && makepkg -f --skipinteg --nodeps", qwork) < 0) { free(qwork); return 1; }
    ret = run_command(cmd); free(cmd); free(qwork);
    if (ret != 0) return ret;
    fprintf(stdout, "packinstall: package built in %s\n", work);
    if (do_install) {
        char *q, pkg[PATH_MAX]; FILE *p;
        if (asprintf(&q, "find %s -maxdepth 1 -type f -name '*.pkg.tar.*' -print -quit", work) < 0) return 1;
        p = popen(q, "r"); free(q); if (!p) return 1;
        if (!fgets(pkg, sizeof(pkg), p)) { pclose(p); return 1; }
        pclose(p); pkg[strcspn(pkg, "\n")] = '\0';
        q = shell_quote(pkg); if (!q) return 1;
        if (asprintf(&cmd, "kuzpkg -U --noconfirm %s", q) < 0) { free(q); return 1; }
        ret = run_command(cmd); free(cmd); free(q);
    }
    return ret;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s <source-tree> <staging-dir> [--no-install]\n\nBuild, stage and package a source tree.\n\nDetected systems: Autotools, Make, Ninja, Meson, CMake, Cargo, Python, Perl, RubyGems, Go.\n", argv0);
}

int main(int argc, char **argv)
{
    const char *src, *stage, *arch; char *name = NULL, *version = NULL;
    int do_install = 1, ret; char cwd[PATH_MAX];
    if (argc < 3 || argc > 4) { usage(argv[0]); return 2; }
    src = argv[1]; stage = argv[2];
    if (argc == 4 && !strcmp(argv[3], "--no-install")) do_install = 0;
    else if (argc == 4) { usage(argv[0]); return 2; }
    if (!is_dir(src)) { fprintf(stderr, "packinstall: source tree '%s' does not exist or is not a directory\n", src); return 1; }
    if (!getcwd(cwd, sizeof(cwd))) { fprintf(stderr, "packinstall: getcwd: %s\n", strerror(errno)); return 1; }
    if (chdir(src) != 0) { fprintf(stderr, "packinstall: cannot enter '%s': %s\n", src, strerror(errno)); return 1; }
    name = package_name(src); version = package_version(src); arch = detect_arch();
    if (!name || !version) { fprintf(stderr, "packinstall: out of memory\n"); free(name); free(version); chdir(cwd); return 1; }
    fprintf(stderr, "packinstall: package=%s version=%s arch=%s\n", name, version, arch);
    if (clean_stage(stage) != 0) goto fail;
    ret = build_project(src, stage); if (ret != 0) goto fail;
    { char *q = shell_quote(stage), *c; if (!q || asprintf(&c, "rm -f -- %s/usr/share/info/dir", q) < 0) { free(q); goto fail; } ret = run_command(c); free(q); free(c); if (ret != 0) goto fail; }
    ret = package_and_install(stage, name, version, arch, do_install); if (ret != 0) goto fail;
    free(name); free(version); chdir(cwd); return 0;
fail:
    fprintf(stderr, "packinstall: failed\n"); free(name); free(version); chdir(cwd); return 1;
}
