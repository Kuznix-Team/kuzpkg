/* packinstall-lfs.c - LFS/MLFS source packager using makepkg.
 *
 * The utility uses makepkg for PKGBUILD metadata and for the final archive,
 * but deliberately replaces build()/prepare()/check() with no-ops.  The
 * package() function is therefore the installation step: files are staged
 * into $pkgdir and makepkg creates a .kuzpkg.tar.zst package.
 *
 * If the PKGBUILD has no package() function, packinstall-lfs can provide a
 * conservative automatic package() for common LFS/BLFS build systems.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define VERSION "0.2"
#define SUFFIX ".kuzpkg.tar.zst"
#define DB_REL "/var/lib/packinstall-lfs"

static const char *root_dir = "/";

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "packinstall-lfs: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap); exit(EXIT_FAILURE);
}

static void usage(void) {
    puts("packinstall-lfs " VERSION " - LFS/MLFS makepkg packager");
    puts("Usage:");
    puts("  packinstall-lfs package [PKGBUILD] [options]");
    puts("  packinstall-lfs create STAGE NAME VERSION [OUTPUT]");
    puts("  packinstall-lfs install PACKAGE [--root DIR]");
    puts("  packinstall-lfs remove NAME VERSION [--root DIR]");
    puts("  packinstall-lfs list [--root DIR]");
    puts("  packinstall-lfs info PACKAGE");
    puts("");
    puts("package options:");
    puts("  --dir DIR       source/PKGBUILD directory");
    puts("  --build-system  auto|make|autotools|meson|ninja|cmake|python|ruby|perl|go|rust|cargo");
    puts("  --buildscript   PKGBUILD filename (default: PKGBUILD)");
    puts("  --output DIR    makepkg package destination (default: source directory)");
    puts("  --skip-checks   do not run check() even if the PKGBUILD has one");
}

static void shell_quote(const char *s, char *out, size_t n) {
    size_t p = 0;
    if (n < 3) die("internal quoting buffer too small");
    out[p++] = '\'';
    for (; *s; ++s) {
        if (*s == '\'') {
            if (p + 4 >= n) die("path is too long");
            memcpy(out + p, "'\\''", 4); p += 4;
        } else {
            if (p + 2 >= n) die("path is too long");
            out[p++] = *s;
        }
    }
    out[p++] = '\''; out[p] = '\0';
}

static int run(const char *cmd) {
    int rc = system(cmd);
    if (rc < 0 || !WIFEXITED(rc)) return -1;
    return WEXITSTATUS(rc);
}

static void ensure_dir(const char *path) {
    char b[PATH_MAX]; size_t l = strlen(path);
    if (!l || l >= sizeof(b)) die("path too long: %s", path);
    memcpy(b, path, l + 1);
    for (char *p = b + 1; *p; ++p) if (*p == '/') {
        *p = 0;
        if (mkdir(b, 0755) && errno != EEXIST)
            die("mkdir %s: %s", b, strerror(errno));
        *p = '/';
    }
    if (mkdir(b, 0755) && errno != EEXIST)
        die("mkdir %s: %s", b, strerror(errno));
}

static const char *arg_value(int argc, char **argv, const char *opt) {
    for (int i = 0; i + 1 < argc; ++i)
        if (!strcmp(argv[i], opt)) return argv[i + 1];
    return NULL;
}

static int has_opt(int argc, char **argv, const char *opt) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], opt)) return 1;
    return 0;
}

static int is_file(const char *p) {
    struct stat s;
    return stat(p, &s) == 0 && S_ISREG(s.st_mode);
}

static void absolute_path(const char *in, char *out, size_t n) {
    char *r = realpath(in, NULL);
    if (!r) die("cannot resolve %s: %s", in, strerror(errno));
    if (strlen(r) >= n) { free(r); die("path too long: %s", in); }
    strcpy(out, r);
    free(r);
}

/* makepkg is deliberately responsible for parsing PKGBUILD syntax. */
static void read_srcinfo(const char *dir, char *name, size_t nn,
                         char *ver, size_t vn, char *rel, size_t rn) {
    char qdir[PATH_MAX * 2], cmd[PATH_MAX * 3];
    shell_quote(dir, qdir, sizeof(qdir));
    snprintf(cmd, sizeof(cmd),
             "cd %s && makepkg --printsrcinfo 2>/dev/null", qdir);
    FILE *fp = popen(cmd, "r");
    if (!fp) die("cannot execute makepkg --printsrcinfo");

    char line[4096];
    name[0] = ver[0] = rel[0] = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *v = strchr(line, '=');
        if (!v) continue;
        *v++ = 0;
        while (*v == ' ' || *v == '\t') ++v;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "pkgname")) snprintf(name, nn, "%s", v);
        else if (!strcmp(line, "pkgver")) snprintf(ver, vn, "%s", v);
        else if (!strcmp(line, "pkgrel")) snprintf(rel, rn, "%s", v);
    }
    int rc = pclose(fp);
    if (rc != 0 || !name[0] || !ver[0])
        die("makepkg could not read pkgname/pkgver from PKGBUILD");
    if (!rel[0]) snprintf(rel, rn, "1");
}

static const char *detect_system(const char *dir) {
    char p[PATH_MAX];
#define F(x) do { \
        if (snprintf(p, sizeof(p), "%s/%s", dir, (x)) >= (int)sizeof(p)) \
            die("source path is too long"); \
        if (is_file(p)) return (x); \
    } while (0)
    /* Prefer rustc bootstrap before generic Cargo detection. */
    F("x.py");
    F("meson.build");
    F("CMakeLists.txt");
    F("configure.ac");
    F("configure.in");
    F("Makefile.am");
    F("pyproject.toml");
    F("setup.py");
    F("extconf.rb");
    F("Makefile.PL");
    F("go.mod");
    F("Cargo.toml");
    F("Makefile");
#undef F
    return NULL;
}

static const char *normalize_system(const char *s, const char *dir) {
    if (!s || !strcmp(s, "auto")) return detect_system(dir);
    if (!strcmp(s, "make")) return "Makefile";
    if (!strcmp(s, "autotools")) return "configure.ac";
    if (!strcmp(s, "meson") || !strcmp(s, "ninja")) return "meson.build";
    if (!strcmp(s, "cmake")) return "CMakeLists.txt";
    if (!strcmp(s, "python")) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/pyproject.toml", dir);
        if (is_file(p)) return "pyproject.toml";
        snprintf(p, sizeof(p), "%s/setup.py", dir);
        if (is_file(p)) return "setup.py";
        return "pyproject.toml";
    }
    if (!strcmp(s, "ruby")) return "extconf.rb";
    if (!strcmp(s, "perl")) return "Makefile.PL";
    if (!strcmp(s, "go")) return "go.mod";
    if (!strcmp(s, "rust") || !strcmp(s, "rustc")) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/x.py", dir);
        return is_file(p) ? "x.py" : "Cargo.toml";
    }
    if (!strcmp(s, "cargo")) return "Cargo.toml";
    return s;
}

static void append_auto_package(FILE *f, const char *system) {
    fprintf(f, "package() {\n");
    fprintf(f, "  cd \"$_packinstall_source\"\n");
    fprintf(f, "  export DESTDIR=\"$pkgdir\"\n");
    fprintf(f, "  export PREFIX=\"/usr\"\n");

    if (!strcmp(system, "meson.build")) {
        fprintf(f, "  command -v meson >/dev/null 2>&1 || { echo 'meson is required' >&2; return 127; }\n");
        fprintf(f, "  command -v ninja >/dev/null 2>&1 || { echo 'ninja is required' >&2; return 127; }\n");
        fprintf(f, "  rm -rf .packinstall-build\n");
        fprintf(f, "  meson setup .packinstall-build --prefix=/usr --buildtype=release\n");
        fprintf(f, "  ninja -C .packinstall-build\n");
        fprintf(f, "  DESTDIR=\"$pkgdir\" ninja -C .packinstall-build install\n");
    } else if (!strcmp(system, "CMakeLists.txt")) {
        fprintf(f, "  rm -rf .packinstall-build\n");
        fprintf(f, "  cmake -S . -B .packinstall-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr\n");
        fprintf(f, "  cmake --build .packinstall-build\n");
        fprintf(f, "  DESTDIR=\"$pkgdir\" cmake --install .packinstall-build\n");
    } else if (!strcmp(system, "configure.ac") || !strcmp(system, "configure.in") || !strcmp(system, "Makefile.am")) {
        fprintf(f, "  if [ ! -x ./configure ]; then command -v autoreconf >/dev/null 2>&1 || { echo 'autoreconf is required' >&2; return 127; }; autoreconf -fi; fi\n");
        fprintf(f, "  ./configure --prefix=/usr\n");
        fprintf(f, "  make\n");
        fprintf(f, "  DESTDIR=\"$pkgdir\" make install\n");
    } else if (!strcmp(system, "setup.py")) {
        fprintf(f, "  python3 setup.py build\n");
        fprintf(f, "  python3 setup.py install --root=\"$pkgdir\" --prefix=/usr --single-version-externally-managed --record=install-record.txt\n");
    } else if (!strcmp(system, "pyproject.toml")) {
        fprintf(f, "  command -v python3 >/dev/null 2>&1 || return 127\n");
        fprintf(f, "  python3 -m pip wheel --no-deps --no-build-isolation --wheel-dir \"$srcdir/.packinstall-wheels\" .\n");
        fprintf(f, "  python3 -m pip install --no-deps --no-index --root \"$pkgdir\" --prefix /usr \"$srcdir/.packinstall-wheels\"/*.whl\n");
    } else if (!strcmp(system, "extconf.rb")) {
        fprintf(f, "  ruby extconf.rb --prefix=/usr\n");
        fprintf(f, "  make\n");
        fprintf(f, "  DESTDIR=\"$pkgdir\" make install\n");
    } else if (!strcmp(system, "Makefile.PL")) {
        fprintf(f, "  perl Makefile.PL PREFIX=/usr\n");
        fprintf(f, "  make\n");
        fprintf(f, "  DESTDIR=\"$pkgdir\" make install\n");
    } else if (!strcmp(system, "go.mod")) {
        fprintf(f, "  mkdir -p \"$pkgdir/usr/bin\"\n");
        fprintf(f, "  GOBIN=\"$pkgdir/usr/bin\" go install ./...\n");
    } else if (!strcmp(system, "x.py")) {
        fprintf(f, "  chmod +x ./x.py\n");
        fprintf(f, "  DESTDIR=\"$pkgdir\" ./x.py install\n");
    } else if (!strcmp(system, "Cargo.toml")) {
        fprintf(f, "  cargo build --release\n");
        fprintf(f, "  mkdir -p \"$pkgdir/usr/bin\"\n");
        fprintf(f, "  for b in target/release/*; do [ -x \"$b\" ] && [ -f \"$b\" ] && install -m755 \"$b\" \"$pkgdir/usr/bin/\" || :; done\n");
    } else {
        fprintf(f, "  make\n");
        fprintf(f, "  DESTDIR=\"$pkgdir\" prefix=/usr make install\n");
    }
    fprintf(f, "}\n");
}

static void package_pkgbuid(const char *dir, const char *pkgbuild,
                            const char *system_override, const char *output,
                            int skip_checks) {
    char name[256], ver[256], rel[256];
    char qdir[PATH_MAX * 2], qout[PATH_MAX * 2], qpkgb[PATH_MAX * 2];
    char absbuild[PATH_MAX], temp[PATH_MAX], wrapper[PATH_MAX], cmd[PATH_MAX * 8];
    const char *system;
    int forced = system_override && strcmp(system_override, "auto") != 0;

    read_srcinfo(dir, name, sizeof(name), ver, sizeof(ver), rel, sizeof(rel));
    system = normalize_system(system_override, dir);
    if (!system) system = "Makefile";

    absolute_path(pkgbuild, absbuild, sizeof(absbuild));
    if (snprintf(temp, sizeof(temp), "%s/.packinstall-lfs.%ld", dir, (long)getpid()) >= (int)sizeof(temp))
        die("temporary path is too long");
    if (mkdir(temp, 0700) != 0) die("cannot create temporary directory %s: %s", temp, strerror(errno));
    if (strlen(temp) + 10 >= sizeof(wrapper)) die("temporary path is too long");
    strcpy(wrapper, temp); strcat(wrapper, "/PKGBUILD");

    FILE *f = fopen(wrapper, "w");
    if (!f) die("cannot create wrapper PKGBUILD: %s", strerror(errno));
    char qorig[PATH_MAX * 2]; shell_quote(absbuild, qorig, sizeof(qorig));

    fprintf(f, "# generated by packinstall-lfs -- do not edit\n");
    fprintf(f, "source %s\n", qorig);
    fprintf(f, "_packinstall_source=%s\n", qorig);
    /* LFS packaging operates on an already prepared source tree.  Do not
     * make makepkg download/extract a second source tree. */
    fprintf(f, "source=()\n");
    fprintf(f, "sha256sums=()\n");
    fprintf(f, "md5sums=()\n");
    fprintf(f, "srcdir=\"$_packinstall_source\"\n");
    fprintf(f, "build() { :; }\n");
    fprintf(f, "prepare() { :; }\n");
    fprintf(f, "check() { :; }\n");
    fprintf(f, "pkgrel=%s\n", rel);

    /* Existing package() is authoritative. --build-system explicitly forces
     * the generated installer, which is useful for ordinary LFS sources. */
    if (forced || !system_override) {
        fprintf(f, "if ! declare -F package >/dev/null 2>&1 || %s; then\n", forced ? "true" : "false");
        append_auto_package(f, system);
        fprintf(f, "fi\n");
    }
    fclose(f);

    shell_quote(temp, qdir, sizeof(qdir));
    shell_quote(output ? output : dir, qout, sizeof(qout));
    shell_quote(wrapper, qpkgb, sizeof(qpkgb));
    snprintf(cmd, sizeof(cmd),
             "mkdir -p %s && cd %s && PKGEXT='%s' PKGDEST=%s "
             "makepkg -f --nodeps --noprepare --noextract -p %s %s",
             qout, qdir, SUFFIX, qout, qpkgb,
             skip_checks ? "--nocheck" : "");

    if (run(cmd) != 0) {
        unlink(wrapper); rmdir(temp);
        die("makepkg failed for %s-%s", name, ver);
    }
    unlink(wrapper); rmdir(temp);
    printf("packaged %s-%s-%s as %s\n", name, ver, rel,
           output ? output : "package directory");
}

/* Legacy stage -> tar.zst creator retained for very early LFS bootstrap use. */
static void create_package(const char *stage, const char *name,
                           const char *version, const char *output) {
    struct stat st;
    if (stat(stage, &st) != 0 || !S_ISDIR(st.st_mode))
        die("stage directory does not exist: %s", stage);
    char qstage[PATH_MAX * 2], qout[PATH_MAX * 2], cmd[PATH_MAX * 4], info[PATH_MAX];
    if (!output) {
        static char generated[PATH_MAX];
        snprintf(generated, sizeof(generated), "%s-%s%s", name, version, SUFFIX);
        output = generated;
    }
    snprintf(info, sizeof(info), "%s/.LFSINFO", stage);
    FILE *fp = fopen(info, "w");
    if (!fp) die("cannot create %s: %s", info, strerror(errno));
    fprintf(fp, "format=packinstall-lfs/%s\nname=%s\nversion=%s\npkgrel=1\narch=native\n",
            VERSION, name, version);
    fclose(fp);
    shell_quote(stage, qstage, sizeof(qstage));
    shell_quote(output, qout, sizeof(qout));
    snprintf(cmd, sizeof(cmd), "tar --zstd -C %s -cf %s .", qstage, qout);
    if (run(cmd) != 0) { unlink(info); die("tar failed"); }
    unlink(info);
    printf("created %s\n", output);
}

static int safe_member(const char *p) {
    if (!p || !*p || p[0] == '/') return 0;
    if (!strcmp(p, ".") || !strcmp(p, "./")) return 1;
    if (!strcmp(p, "..") || !strncmp(p, "../", 3) || strstr(p, "/../")) return 0;
    return 1;
}

static void validate_archive(const char *package) {
    char qp[PATH_MAX * 2], cmd[PATH_MAX * 3];
    shell_quote(package, qp, sizeof(qp));
    snprintf(cmd, sizeof(cmd), "tar --zstd -tf %s", qp);
    FILE *fp = popen(cmd, "r");
    if (!fp) die("cannot inspect %s", package);
    char line[PATH_MAX];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!safe_member(line)) {
            pclose(fp); die("unsafe archive member: %s", line);
        }
    }
    if (pclose(fp) != 0) die("cannot inspect archive: %s", package);
}

static void package_info(const char *package) {
    char qp[PATH_MAX * 2], cmd[PATH_MAX * 3];
    shell_quote(package, qp, sizeof(qp));
    snprintf(cmd, sizeof(cmd), "tar --zstd -xOf %s ./.LFSINFO 2>/dev/null", qp);
    if (run(cmd) != 0) die("%s has no valid .LFSINFO", package);
}

static void install_package(const char *package) {
    validate_archive(package);
    char qp[PATH_MAX * 2], qr[PATH_MAX * 2], cmd[PATH_MAX * 4];
    char tmp[PATH_MAX], db[PATH_MAX], files[PATH_MAX];
    shell_quote(package, qp, sizeof(qp)); shell_quote(root_dir, qr, sizeof(qr));
    snprintf(tmp, sizeof(tmp), "/tmp/packinstall-lfs-info-%ld", (long)getpid());
    char qt[PATH_MAX * 2]; shell_quote(tmp, qt, sizeof(qt));
    snprintf(cmd, sizeof(cmd), "tar --zstd -xOf %s ./.LFSINFO > %s", qp, qt);
    if (run(cmd) != 0) die("cannot read package metadata");

    char name[256] = {0}, ver[256] = {0};
    FILE *fp = fopen(tmp, "r");
    if (!fp) die("cannot read package metadata");
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "name=%255s", name) == 1) continue;
        if (sscanf(line, "version=%255s", ver) == 1) continue;
    }
    fclose(fp); unlink(tmp);
    if (!name[0] || !ver[0]) die("package metadata is incomplete");

    if (snprintf(db, sizeof(db), "%s%s/%s/%s", root_dir, DB_REL, name, ver) >= (int)sizeof(db))
        die("package database path is too long");
    if (snprintf(files, sizeof(files), "%s/files", db) >= (int)sizeof(files))
        die("package database path is too long");
    ensure_dir(db);

    snprintf(cmd, sizeof(cmd),
             "tar --zstd -xpf %s -C %s --exclude=./.LFSINFO "
             "--exclude=./.FILES --no-same-owner --no-same-permissions",
             qp, qr);
    if (run(cmd) != 0) die("failed to install %s", package);

    snprintf(cmd, sizeof(cmd), "tar --zstd -xOf %s ./.FILES > %s 2>/dev/null", qp, qt);
    if (run(cmd) == 0) rename(tmp, files);
    else {
        FILE *m = fopen(files, "w");
        if (m) { fputs("# legacy archive: file list unavailable\n", m); fclose(m); }
    }
    printf("installed %s-%s\n", name, ver);
}

static void remove_package(const char *name, const char *version) {
    char db[PATH_MAX], files[PATH_MAX], path[PATH_MAX];
    if (snprintf(db, sizeof(db), "%s%s/%s/%s", root_dir, DB_REL, name, version) >= (int)sizeof(db))
        die("package database path is too long");
    if (snprintf(files, sizeof(files), "%s/files", db) >= (int)sizeof(files))
        die("package database path is too long");

    FILE *fp = fopen(files, "r");
    if (!fp) die("package %s-%s is not installed", name, version);
    while (fgets(path, sizeof(path), fp)) {
        path[strcspn(path, "\r\n")] = 0;
        if (path[0] == '#' || !safe_member(path)) continue;
        if (strlen(root_dir) + 1 + strlen(path) >= PATH_MAX) continue;
        char full[PATH_MAX]; strcpy(full, root_dir); strcat(full, "/"); strcat(full, path);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) rmdir(full);
        else if (unlink(full) != 0 && errno != ENOENT)
            fprintf(stderr, "packinstall-lfs: cannot remove %s: %s\n", full, strerror(errno));
    }
    fclose(fp); unlink(files); rmdir(db);
    printf("removed %s-%s\n", name, version);
}

static void list_packages(void) {
    char base[PATH_MAX], qb[PATH_MAX * 2], cmd[PATH_MAX * 3];
    snprintf(base, sizeof(base), "%s%s", root_dir, DB_REL);
    shell_quote(base, qb, sizeof(qb));
    snprintf(cmd, sizeof(cmd),
             "find %s -mindepth 2 -maxdepth 2 -type d -printf '%%P\\n' 2>/dev/null", qb);
    run(cmd);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return EXIT_FAILURE; }
    const char *root = arg_value(argc, argv, "--root");
    if (root) root_dir = root;

    if (!strcmp(argv[1], "package")) {
        const char *dir = arg_value(argc, argv, "--dir");
        if (!dir) dir = ".";
        char absdir[PATH_MAX]; absolute_path(dir, absdir, sizeof(absdir));
        const char *pb = arg_value(argc, argv, "--buildscript");
        if (!pb) pb = "PKGBUILD";
        char p[PATH_MAX];
        if (pb[0] == '/') {
            if (strlen(pb) >= sizeof(p)) die("build script path is too long");
            strcpy(p, pb);
        } else {
            if (strlen(absdir) + 1 + strlen(pb) >= sizeof(p)) die("build script path is too long");
            strcpy(p, absdir); strcat(p, "/"); strcat(p, pb);
        }
        if (!is_file(p)) die("build script not found: %s", p);
        const char *out = arg_value(argc, argv, "--output");
        if (!out) out = absdir;
        package_pkgbuid(absdir, p, arg_value(argc, argv, "--build-system"),
                        out, has_opt(argc, argv, "--skip-checks"));
        return EXIT_SUCCESS;
    }
    if (!strcmp(argv[1], "create")) {
        if (argc < 5) { usage(); return EXIT_FAILURE; }
        create_package(argv[2], argv[3], argv[4], argc >= 6 ? argv[5] : NULL);
        return EXIT_SUCCESS;
    }
    if (!strcmp(argv[1], "install")) {
        if (argc < 3) { usage(); return EXIT_FAILURE; }
        install_package(argv[2]); return EXIT_SUCCESS;
    }
    if (!strcmp(argv[1], "remove")) {
        if (argc < 4) { usage(); return EXIT_FAILURE; }
        remove_package(argv[2], argv[3]); return EXIT_SUCCESS;
    }
    if (!strcmp(argv[1], "list")) { list_packages(); return EXIT_SUCCESS; }
    if (!strcmp(argv[1], "info")) {
        if (argc < 3) { usage(); return EXIT_FAILURE; }
        package_info(argv[2]); return EXIT_SUCCESS;
    }
    usage(); return EXIT_FAILURE;
}
