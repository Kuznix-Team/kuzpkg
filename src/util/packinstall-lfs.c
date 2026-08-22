/*
 * packinstall-lfs.c - LFS/MLFS source packager using makepkg.
 *
 * Features:
 *   - Uses makepkg --printsrcinfo for pkgname/pkgver/pkgrel detection
 *   - pkgrel defaults to 1
 *   - Uses makepkg for final .kuzpkg.tar.zst packages
 *   - Existing PKGBUILD lifecycle functions
 *   - --no-build
 *   - --skip-checks
 *   - Generic Makefile support
 *   - Autotools
 *   - Meson/Ninja
 *   - CMake
 *   - Python setup.py/setup.cfg/pyproject.toml
 *   - Ruby extconf.rb
 *   - Perl Makefile.PL
 *   - Go
 *   - Cargo
 *   - Rust compiler ./x.py
 *   - Waf
 *   - SCons
 *
 * Multilib / ABI:
 *   - native
 *   - lib32
 *   - libx32
 *   - libo32
 *   - libm32
 *   - ppc32
 *   - ppc64
 *   - ppc64le
 *
 * Example:
 *   packinstall-lfs package --dir .
 *   packinstall-lfs package --dir . --abi lib32
 *   packinstall-lfs package --dir . --abi libx32
 *   packinstall-lfs package --dir . --abi libo32
 *   packinstall-lfs package --dir . --abi libm32
 *   packinstall-lfs package --dir . --abi ppc32
 *   packinstall-lfs package --dir . --abi ppc64
 *   packinstall-lfs package --dir . --abi ppc64le
 *
 * Copyright (C) 2026 Kuznix
 * SPDX-License-Identifier: GPL-2.0-or-later
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
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define VERSION "0.4"
#define SUFFIX ".kuzpkg.tar.zst"
#define DB_REL "/var/lib/packinstall-lfs"

/* ------------------------------------------------------------------------- */
/* ABI profiles                                                              */
/* ------------------------------------------------------------------------- */

enum package_abi {
	ABI_NATIVE = 0,
	ABI_LIB32,
	ABI_LIBX32,
	ABI_LIBO32,
	ABI_LIBM32,
	ABI_PPC32,
	ABI_PPC64,
	ABI_PPC64LE
};

struct abi_profile {
	enum package_abi abi;
	const char *name;
	const char *libdir;
	const char *flag;
	const char *machine;
};

static const struct abi_profile abi_profiles[] = {
	{
		ABI_NATIVE,
		"native",
		"/usr/lib",
		"",
		"native"
	},

	{
		ABI_LIB32,
		"lib32",
		"/usr/lib32",
		"-m32",
		"32-bit"
	},

	{
		ABI_LIBX32,
		"libx32",
		"/usr/libx32",
		"-mx32",
		"x32"
	},

	{
		ABI_LIBO32,
		"libo32",
		"/usr/libo32",
		"-mabi=32",
		"mips-o32"
	},

	{
		ABI_LIBM32,
		"libm32",
		"/usr/libm32",
		"-mabi=n32",
		"mips-n32"
	},

	{
		ABI_PPC32,
		"ppc32",
		"/usr/lib32",
		"-m32",
		"powerpc32"
	},

	{
		ABI_PPC64,
		"ppc64",
		"/usr/lib64",
		"-m64",
		"powerpc64"
	},

	{
		ABI_PPC64LE,
		"ppc64le",
		"/usr/lib64",
		"-m64",
		"powerpc64le"
	}
};

#define ABI_PROFILE_COUNT \
	(sizeof(abi_profiles) / sizeof(abi_profiles[0]))

static const char *root_dir = "/";

/* ------------------------------------------------------------------------- */
/* Error handling                                                            */
/* ------------------------------------------------------------------------- */

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	fprintf(stderr, "packinstall-lfs: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);

	va_end(ap);

	exit(EXIT_FAILURE);
}

static void warnx(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	fprintf(stderr, "packinstall-lfs: warning: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);

	va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* Usage                                                                     */
/* ------------------------------------------------------------------------- */

static void usage(void)
{
	puts("packinstall-lfs " VERSION " - LFS/MLFS makepkg packager");
	puts("");
	puts("Usage:");
	puts("  packinstall-lfs package [options]");
	puts("  packinstall-lfs create STAGE NAME VERSION [OUTPUT]");
	puts("  packinstall-lfs install PACKAGE [--root DIR]");
	puts("  packinstall-lfs remove NAME VERSION [--root DIR]");
	puts("  packinstall-lfs list [--root DIR]");
	puts("  packinstall-lfs info PACKAGE");
	puts("");
	puts("package options:");
	puts("  --dir DIR");
	puts("      source / PKGBUILD directory");
	puts("");
	puts("  --buildscript FILE");
	puts("      PKGBUILD filename (default: PKGBUILD)");
	puts("");
	puts("  --build-system SYSTEM");
	puts("      auto|make|autotools|meson|ninja|cmake|");
	puts("      python|python-setup|python-pyproject|python-cfg|");
	puts("      ruby|perl|go|rust|rustc|cargo|waf|scons");
	puts("");
	puts("  --abi ABI");
	puts("      native|lib32|libx32|libo32|libm32|");
	puts("      ppc32|ppc64|ppc64le");
	puts("");
	puts("  --output DIR");
	puts("      makepkg package destination");
	puts("");
	puts("  --no-build");
	puts("      skip the build phase");
	puts("");
	puts("  --skip-checks");
	puts("      skip check()");
	puts("");
	puts("Examples:");
	puts("  packinstall-lfs package --dir .");
	puts("  packinstall-lfs package --dir . --abi lib32");
	puts("  packinstall-lfs package --dir . --abi libx32");
	puts("  packinstall-lfs package --dir . --abi libo32");
	puts("  packinstall-lfs package --dir . --abi libm32");
	puts("  packinstall-lfs package --dir . --abi ppc32");
	puts("  packinstall-lfs package --dir . --abi ppc64");
	puts("  packinstall-lfs package --dir . --abi ppc64le");
	puts("  packinstall-lfs package --dir . --no-build");
}

/* ------------------------------------------------------------------------- */
/* Generic helpers                                                           */
/* ------------------------------------------------------------------------- */

static void shell_quote(const char *s, char *out, size_t n)
{
	size_t p = 0;

	if (n < 3)
		die("internal quoting buffer too small");

	out[p++] = '\'';

	while (*s) {
		if (*s == '\'') {
			if (p + 4 >= n)
				die("quoted string is too long");

			memcpy(out + p, "'\\''", 4);
			p += 4;
		} else {
			if (p + 2 >= n)
				die("quoted string is too long");

			out[p++] = *s;
		}

		s++;
	}

	out[p++] = '\'';
	out[p] = '\0';
}

static int run_command(const char *cmd)
{
	int rc;

	rc = system(cmd);

	if (rc < 0)
		return -1;

	if (!WIFEXITED(rc))
		return -1;

	return WEXITSTATUS(rc);
}

static void ensure_dir(const char *path)
{
	char buffer[PATH_MAX];
	size_t len;
	char *p;

	len = strlen(path);

	if (!len || len >= sizeof(buffer))
		die("path too long: %s", path);

	memcpy(buffer, path, len + 1);

	for (p = buffer + 1; *p; ++p) {
		if (*p != '/')
			continue;

		*p = '\0';

		if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
			die("mkdir %s: %s", buffer, strerror(errno));

		*p = '/';
	}

	if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
		die("mkdir %s: %s", buffer, strerror(errno));
}

static const char *arg_value(int argc, char **argv, const char *opt)
{
	int i;

	for (i = 1; i + 1 < argc; ++i) {
		if (!strcmp(argv[i], opt))
			return argv[i + 1];
	}

	return NULL;
}

static int has_opt(int argc, char **argv, const char *opt)
{
	int i;

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], opt))
			return 1;
	}

	return 0;
}

static int is_file(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return 0;

	return S_ISREG(st.st_mode);
}

static int is_directory(const char *path)
{
	struct stat st;

	if (stat(path, &st) != 0)
		return 0;

	return S_ISDIR(st.st_mode);
}

static void absolute_path(const char *input, char *output, size_t size)
{
	char *resolved;

	resolved = realpath(input, NULL);

	if (!resolved)
		die("cannot resolve %s: %s",
		    input,
		    strerror(errno));

	if (strlen(resolved) >= size) {
		free(resolved);
		die("path too long: %s", input);
	}

	strcpy(output, resolved);

	free(resolved);
}

/* ------------------------------------------------------------------------- */
/* ABI helpers                                                               */
/* ------------------------------------------------------------------------- */

static const struct abi_profile *find_abi(const char *name)
{
	size_t i;

	if (!name || !strcmp(name, "native"))
		return &abi_profiles[ABI_NATIVE];

	for (i = 0; i < ABI_PROFILE_COUNT; ++i) {
		if (!strcmp(name, abi_profiles[i].name))
			return &abi_profiles[i];
	}

	return NULL;
}

static const struct abi_profile *detect_default_abi(void)
{
	struct utsname uts;

	if (uname(&uts) != 0)
		return &abi_profiles[ABI_NATIVE];

	if (!strcmp(uts.machine, "ppc64"))
		return &abi_profiles[ABI_PPC64];

	if (!strcmp(uts.machine, "ppc64le"))
		return &abi_profiles[ABI_PPC64LE];

	return &abi_profiles[ABI_NATIVE];
}

static int abi_is_native(const struct abi_profile *abi)
{
	return abi->abi == ABI_NATIVE;
}

static void makepkg_arch_for_abi(const struct abi_profile *abi,
				 char *output,
				 size_t size)
{
	if (abi_is_native(abi)) {
		snprintf(output,
			 size,
			 "%s",
			 "auto");
		return;
	}

	snprintf(output,
		 size,
		 "%s",
		 abi->name);
}

/* ------------------------------------------------------------------------- */
/* makepkg metadata                                                          */
/* ------------------------------------------------------------------------- */

static void read_srcinfo(const char *dir,
			 char *name,
			 size_t name_size,
			 char *version,
			 size_t version_size,
			 char *release,
			 size_t release_size)
{
	char qdir[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];
	FILE *fp;
	char line[4096];
	int rc;

	name[0] = '\0';
	version[0] = '\0';
	release[0] = '\0';

	shell_quote(dir,
		    qdir,
		    sizeof(qdir));

	snprintf(cmd,
		 sizeof(cmd),
		 "cd %s && makepkg --printsrcinfo 2>/dev/null",
		 qdir);

	fp = popen(cmd, "r");

	if (!fp)
		die("cannot execute makepkg --printsrcinfo");

	while (fgets(line, sizeof(line), fp)) {
		char *eq;
		char *value;

		eq = strchr(line, '=');

		if (!eq)
			continue;

		*eq++ = '\0';

		value = eq;

		while (*value == ' ' || *value == '\t')
			++value;

		value[strcspn(value, "\r\n")] = '\0';

		if (!strcmp(line, "pkgname")) {
			snprintf(name,
				 name_size,
				 "%s",
				 value);
		} else if (!strcmp(line, "pkgver")) {
			snprintf(version,
				 version_size,
				 "%s",
				 value);
		} else if (!strcmp(line, "pkgrel")) {
			snprintf(release,
				 release_size,
				 "%s",
				 value);
		}
	}

	rc = pclose(fp);

	if (rc != 0 || !name[0] || !version[0])
		die("makepkg could not read pkgname/pkgver from PKGBUILD");

	if (!release[0])
		snprintf(release,
			 release_size,
			 "1");
}

/* ------------------------------------------------------------------------- */
/* Build system detection                                                    */
/* ------------------------------------------------------------------------- */

static int has_named_file(const char *dir, const char *name)
{
	char path[PATH_MAX];

	if (snprintf(path,
		     sizeof(path),
		     "%s/%s",
		     dir,
		     name) >= (int)sizeof(path))
		die("source path is too long");

	return is_file(path);
}

static const char *detect_build_system(const char *dir)
{
	/*
	 * Rust compiler sources also contain Cargo.toml, so x.py comes first.
	 */

	if (has_named_file(dir, "x.py"))
		return "x.py";

	if (has_named_file(dir, "meson.build"))
		return "meson.build";

	if (has_named_file(dir, "CMakeLists.txt"))
		return "CMakeLists.txt";

	if (has_named_file(dir, "configure.ac"))
		return "configure.ac";

	if (has_named_file(dir, "configure.in"))
		return "configure.in";

	if (has_named_file(dir, "autogen.sh"))
		return "autogen.sh";

	if (has_named_file(dir, "bootstrap"))
		return "bootstrap";

	if (has_named_file(dir, "Makefile.am"))
		return "Makefile.am";

	if (has_named_file(dir, "waf"))
		return "waf";

	if (has_named_file(dir, "wscript"))
		return "wscript";

	if (has_named_file(dir, "SConstruct"))
		return "SConstruct";

	if (has_named_file(dir, "sconstruct"))
		return "sconstruct";

	if (has_named_file(dir, "pyproject.toml"))
		return "pyproject.toml";

	if (has_named_file(dir, "setup.py"))
		return "setup.py";

	if (has_named_file(dir, "setup.cfg"))
		return "setup.cfg";

	if (has_named_file(dir, "extconf.rb"))
		return "extconf.rb";

	if (has_named_file(dir, "Makefile.PL"))
		return "Makefile.PL";

	if (has_named_file(dir, "go.mod"))
		return "go.mod";

	if (has_named_file(dir, "Cargo.toml"))
		return "Cargo.toml";

	if (has_named_file(dir, "Makefile"))
		return "Makefile";

	return NULL;
}

static const char *normalize_build_system(const char *system,
					  const char *dir)
{
	char path[PATH_MAX];

	if (!system || !strcmp(system, "auto"))
		return detect_build_system(dir);

	if (!strcmp(system, "make"))
		return "Makefile";

	if (!strcmp(system, "autotools"))
		return "configure.ac";

	if (!strcmp(system, "meson"))
		return "meson.build";

	if (!strcmp(system, "ninja"))
		return "meson.build";

	if (!strcmp(system, "cmake"))
		return "CMakeLists.txt";

	if (!strcmp(system, "python")) {
		if (has_named_file(dir, "pyproject.toml"))
			return "pyproject.toml";

		if (has_named_file(dir, "setup.py"))
			return "setup.py";

		if (has_named_file(dir, "setup.cfg"))
			return "setup.cfg";

		return "pyproject.toml";
	}

	if (!strcmp(system, "python-setup"))
		return "setup.py";

	if (!strcmp(system, "python-pyproject"))
		return "pyproject.toml";

	if (!strcmp(system, "python-cfg"))
		return "setup.cfg";

	if (!strcmp(system, "ruby"))
		return "extconf.rb";

	if (!strcmp(system, "perl"))
		return "Makefile.PL";

	if (!strcmp(system, "go"))
		return "go.mod";

	if (!strcmp(system, "rust") ||
	    !strcmp(system, "rustc")) {
		snprintf(path,
			 sizeof(path),
			 "%s/x.py",
			 dir);

		if (is_file(path))
			return "x.py";

		return "Cargo.toml";
	}

	if (!strcmp(system, "cargo"))
		return "Cargo.toml";

	if (!strcmp(system, "waf"))
		return "waf";

	if (!strcmp(system, "scons"))
		return "SConstruct";

	return system;
}

/* ------------------------------------------------------------------------- */
/* ABI environment                                                           */
/* ------------------------------------------------------------------------- */

static void append_abi_environment(FILE *fp,
				   const struct abi_profile *abi)
{
	fprintf(fp,
		"_packinstall_abi='%s'\n",
		abi->name);

	fprintf(fp,
		"_packinstall_libdir='%s'\n",
		abi->libdir);

	fprintf(fp,
		"_packinstall_arch='%s'\n",
		abi->machine);

	fprintf(fp,
		"export LIBDIR=\"$_packinstall_libdir\"\n");

	fprintf(fp,
		"export libdir=\"$_packinstall_libdir\"\n");

	fprintf(fp,
		"export INSTALL_LIBDIR=\"$_packinstall_libdir\"\n");

	if (abi->flag[0]) {
		fprintf(fp,
			"export CFLAGS=\"${CFLAGS:-} %s\"\n",
			abi->flag);

		fprintf(fp,
			"export CXXFLAGS=\"${CXXFLAGS:-} %s\"\n",
			abi->flag);

		fprintf(fp,
			"export CPPFLAGS=\"${CPPFLAGS:-} %s\"\n",
			abi->flag);

		fprintf(fp,
			"export LDFLAGS=\"${LDFLAGS:-} -L%s\"\n",
			abi->libdir);
	}

	if (abi->abi == ABI_LIBO32) {
		fprintf(fp,
			"export MIPS_ABI='o32'\n");
	} else if (abi->abi == ABI_LIBM32) {
		fprintf(fp,
			"export MIPS_ABI='n32'\n");
	}
}

/* ------------------------------------------------------------------------- */
/* Generated build()                                                         */
/* ------------------------------------------------------------------------- */

static void append_generated_build(FILE *fp,
				   const char *system,
				   const struct abi_profile *abi,
				   int no_build)
{
	if (no_build) {
		fprintf(fp,
			"build() {\n"
			"  :\n"
			"}\n");
		return;
	}

	fprintf(fp, "build() {\n");
	fprintf(fp, "  cd \"$_packinstall_source\"\n");

	if (!strcmp(system, "meson.build")) {
		fprintf(fp,
			"  command -v meson >/dev/null 2>&1 || "
			"{ echo 'meson is required' >&2; return 127; }\n");

		fprintf(fp,
			"  command -v ninja >/dev/null 2>&1 || "
			"{ echo 'ninja is required' >&2; return 127; }\n");

		fprintf(fp,
			"  rm -rf .packinstall-build\n");

		fprintf(fp,
			"  meson setup .packinstall-build "
			"--prefix=/usr "
			"--libdir=\"${LIBDIR#/usr/}\" "
			"--buildtype=release\n");

		fprintf(fp,
			"  ninja -C .packinstall-build\n");

	} else if (!strcmp(system, "CMakeLists.txt")) {
		fprintf(fp,
			"  command -v cmake >/dev/null 2>&1 || "
			"{ echo 'cmake is required' >&2; return 127; }\n");

		fprintf(fp,
			"  rm -rf .packinstall-build\n");

		fprintf(fp,
			"  cmake -S . "
			"-B .packinstall-build "
			"-DCMAKE_BUILD_TYPE=Release "
			"-DCMAKE_INSTALL_PREFIX=/usr "
			"-DCMAKE_INSTALL_LIBDIR=\"${LIBDIR#/usr/}\"\n");

		fprintf(fp,
			"  cmake --build .packinstall-build\n");

	} else if (!strcmp(system, "configure.ac") ||
		   !strcmp(system, "configure.in") ||
		   !strcmp(system, "autogen.sh") ||
		   !strcmp(system, "bootstrap") ||
		   !strcmp(system, "Makefile.am")) {
		fprintf(fp,
			"  if [ ! -x ./configure ]; then\n"
			"    if [ -x ./autogen.sh ]; then\n"
			"      ./autogen.sh\n"
			"    elif [ -x ./bootstrap ]; then\n"
			"      ./bootstrap\n"
			"    else\n"
			"      command -v autoreconf >/dev/null 2>&1 || return 127\n"
			"      autoreconf -fi\n"
			"    fi\n"
			"  fi\n");

		fprintf(fp,
			"  ./configure "
			"--prefix=/usr "
			"--libdir=\"$LIBDIR\"\n");

		fprintf(fp,
			"  make\n");

	} else if (!strcmp(system, "Makefile")) {
		fprintf(fp,
			"  command -v make >/dev/null 2>&1 || return 127\n");

		/*
		 * Do not assume every Makefile supports CFLAGS.
		 * Supplying the variables through the environment is harmless for
		 * projects that don't use them.
		 */
		fprintf(fp,
			"  make "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\"\n");

	} else if (!strcmp(system, "waf")) {
		fprintf(fp,
			"  test -x ./waf || chmod +x ./waf\n");

		fprintf(fp,
			"  ./waf configure "
			"--prefix=/usr\n");

		fprintf(fp,
			"  ./waf build\n");

	} else if (!strcmp(system, "wscript")) {
		fprintf(fp,
			"  command -v waf >/dev/null 2>&1 || "
			"{ echo 'waf is required' >&2; return 127; }\n");

		fprintf(fp,
			"  waf configure --prefix=/usr\n");

		fprintf(fp,
			"  waf build\n");

	} else if (!strcmp(system, "SConstruct") ||
		   !strcmp(system, "sconstruct")) {
		fprintf(fp,
			"  command -v scons >/dev/null 2>&1 || "
			"{ echo 'scons is required' >&2; return 127; }\n");

		fprintf(fp,
			"  scons\n");

	} else if (!strcmp(system, "setup.py")) {
		fprintf(fp,
			"  command -v python3 >/dev/null 2>&1 || return 127\n");

		fprintf(fp,
			"  python3 setup.py build\n");

	} else if (!strcmp(system, "setup.cfg")) {
		fprintf(fp,
			"  command -v python3 >/dev/null 2>&1 || return 127\n");

		fprintf(fp,
			"  python3 -m build --wheel --no-isolation\n");

	} else if (!strcmp(system, "pyproject.toml")) {
		fprintf(fp,
			"  command -v python3 >/dev/null 2>&1 || return 127\n");

		fprintf(fp,
			"  rm -rf .packinstall-wheels\n");

		fprintf(fp,
			"  mkdir -p .packinstall-wheels\n");

		fprintf(fp,
			"  python3 -m pip wheel "
			"--no-deps "
			"--no-build-isolation "
			"--wheel-dir .packinstall-wheels "
			".\n");

	} else if (!strcmp(system, "extconf.rb")) {
		fprintf(fp,
			"  command -v ruby >/dev/null 2>&1 || return 127\n");

		fprintf(fp,
			"  ruby extconf.rb --prefix=/usr\n");

		fprintf(fp,
			"  make\n");

	} else if (!strcmp(system, "Makefile.PL")) {
		fprintf(fp,
			"  command -v perl >/dev/null 2>&1 || return 127\n");

		fprintf(fp,
			"  perl Makefile.PL PREFIX=/usr\n");

		fprintf(fp,
			"  make\n");

	} else if (!strcmp(system, "go.mod")) {
		fprintf(fp,
			"  command -v go >/dev/null 2>&1 || return 127\n");

		fprintf(fp,
			"  rm -rf .packinstall-go\n");

		fprintf(fp,
			"  mkdir -p .packinstall-go/bin\n");

		fprintf(fp,
			"  go build "
			"-buildvcs=false "
			"-o .packinstall-go/bin/ "
			"./...\n");

	} else if (!strcmp(system, "Cargo.toml")) {
		fprintf(fp,
			"  command -v cargo >/dev/null 2>&1 || return 127\n");

		fprintf(fp,
			"  cargo build --release\n");

	} else if (!strcmp(system, "x.py")) {
		fprintf(fp,
			"  test -x ./x.py || chmod +x ./x.py\n");

		fprintf(fp,
			"  ./x.py build\n");

	} else {
		warnx("unknown build system '%s'; using generic make",
		      system);

		fprintf(fp,
			"  make "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\"\n");
	}

	/*
	 * Keep the compiler ABI flag visible in generated build scripts.
	 * The environment was already exported by append_abi_environment().
	 */
	(void)abi;

	fprintf(fp,
		"}\n");
}

/* ------------------------------------------------------------------------- */
/* Generated package()                                                       */
/* ------------------------------------------------------------------------- */

static void append_generated_package(FILE *fp,
				     const char *system,
				     const struct abi_profile *abi)
{
	fprintf(fp,
		"package() {\n"
		"  cd \"$_packinstall_source\"\n"
		"  export DESTDIR=\"$pkgdir\"\n"
		"  export PREFIX=/usr\n"
		"  export prefix=/usr\n"
		"  export LIBDIR=\"%s\"\n"
		"  export libdir=\"%s\"\n",
		abi->libdir,
		abi->libdir);

	if (!strcmp(system, "meson.build")) {
		fprintf(fp,
			"  test -d .packinstall-build || {\n"
			"    echo 'Meson build directory not found; use --no-build only with an existing build' >&2\n"
			"    return 1\n"
			"  }\n");

		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" ninja -C .packinstall-build install\n");

	} else if (!strcmp(system, "CMakeLists.txt")) {
		fprintf(fp,
			"  test -d .packinstall-build || {\n"
			"    echo 'CMake build directory not found; use --no-build only with an existing build' >&2\n"
			"    return 1\n"
			"  }\n");

		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" cmake --install .packinstall-build\n");

	} else if (!strcmp(system, "configure.ac") ||
		   !strcmp(system, "configure.in") ||
		   !strcmp(system, "autogen.sh") ||
		   !strcmp(system, "bootstrap") ||
		   !strcmp(system, "Makefile.am")) {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\" "
			"make install\n");

	} else if (!strcmp(system, "Makefile")) {
		/*
		 * Generic Makefile support.
		 *
		 * The dry-run test avoids blindly calling a non-existent install
		 * target. This works well with projects such as neofetch and many
		 * traditional Makefile-based utilities.
		 */
		fprintf(fp,
			"  if make -n install >/dev/null 2>&1; then\n"
			"    DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\" "
			"make install\n"
			"  else\n"
			"    echo 'Makefile has no install target' >&2\n"
			"    return 1\n"
			"  fi\n");

	} else if (!strcmp(system, "waf")) {
		fprintf(fp,
			"  test -x ./waf || chmod +x ./waf\n");

		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"./waf install\n");

	} else if (!strcmp(system, "wscript")) {
		fprintf(fp,
			"  command -v waf >/dev/null 2>&1 || return 127\n");

		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" waf install\n");

	} else if (!strcmp(system, "SConstruct") ||
		   !strcmp(system, "sconstruct")) {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"scons install "
			"prefix=/usr "
			"libdir=\"$LIBDIR\"\n");

	} else if (!strcmp(system, "setup.py")) {
		fprintf(fp,
			"  python3 setup.py install "
			"--root=\"$pkgdir\" "
			"--prefix=/usr "
			"--single-version-externally-managed "
			"--record=\"$srcdir/install-record.txt\"\n");

	} else if (!strcmp(system, "setup.cfg")) {
		fprintf(fp,
			"  for whl in dist/*.whl; do\n"
			"    test -f \"$whl\" || continue\n"
			"    python3 -m pip install "
			"--no-deps "
			"--no-index "
			"--root \"$pkgdir\" "
			"--prefix /usr "
			"\"$whl\"\n"
			"  done\n");

	} else if (!strcmp(system, "pyproject.toml")) {
		fprintf(fp,
			"  for whl in .packinstall-wheels/*.whl; do\n"
			"    test -f \"$whl\" || continue\n"
			"    python3 -m pip install "
			"--no-deps "
			"--no-index "
			"--root \"$pkgdir\" "
			"--prefix /usr "
			"\"$whl\"\n"
			"  done\n");

	} else if (!strcmp(system, "extconf.rb")) {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"make install\n");

	} else if (!strcmp(system, "Makefile.PL")) {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"make install\n");

	} else if (!strcmp(system, "go.mod")) {
		fprintf(fp,
			"  mkdir -p \"$pkgdir/usr/bin\"\n");

		fprintf(fp,
			"  for bin in .packinstall-go/bin/*; do\n"
			"    test -f \"$bin\" || continue\n"
			"    install -m755 \"$bin\" \"$pkgdir/usr/bin/\"\n"
			"  done\n");

	} else if (!strcmp(system, "Cargo.toml")) {
		fprintf(fp,
			"  mkdir -p \"$pkgdir/usr/bin\"\n"
			"  found=0\n"
			"  for bin in target/release/*; do\n"
			"    test -f \"$bin\" || continue\n"
			"    test -x \"$bin\" || continue\n"
			"    case \"$bin\" in\n"
			"      *.so|*.a|*.rlib|*.rmeta|*.d) continue ;;\n"
			"    esac\n"
			"    install -m755 \"$bin\" \"$pkgdir/usr/bin/\"\n"
			"    found=1\n"
			"  done\n"
			"  test \"$found\" = 1 || {\n"
			"    echo 'no Cargo release executable found' >&2\n"
			"    return 1\n"
			"  }\n");

	} else if (!strcmp(system, "x.py")) {
		fprintf(fp,
			"  test -d build || {\n"
			"    echo 'Rust build directory not found' >&2\n"
			"    return 1\n"
			"  }\n");

		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"./x.py install\n");

	} else {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\" "
			"make install\n");
	}

	fprintf(fp, "}\n");
}

/* ------------------------------------------------------------------------- */
/* Metadata/package name                                                      */
/* ------------------------------------------------------------------------- */

static void append_abi_metadata(FILE *fp,
				const char *name,
				const char *version,
				const char *release,
				const struct abi_profile *abi)
{
	fprintf(fp,
		"_packinstall_original_pkgname='%s'\n",
		name);

	fprintf(fp,
		"_packinstall_original_pkgver='%s'\n",
		version);

	fprintf(fp,
		"_packinstall_original_pkgrel='%s'\n",
		release);

	fprintf(fp,
		"_packinstall_abi='%s'\n",
		abi->name);

	fprintf(fp,
		"_packinstall_libdir='%s'\n",
		abi->libdir);
}

/*
 * Non-native ABI packages receive an ABI suffix in pkgname.
 *
 * native:
 *   zlib-1.3.1-1-<arch>.kuzpkg.tar.zst
 *
 * lib32:
 *   zlib-lib32-1.3.1-1-<arch>.kuzpkg.tar.zst
 *
 * libx32:
 *   zlib-libx32-1.3.1-1-<arch>.kuzpkg.tar.zst
 */
static void append_pkgname_override(FILE *fp,
				    const char *name,
				    const struct abi_profile *abi)
{
	if (abi_is_native(abi)) {
		fprintf(fp,
			"pkgname='%s'\n",
			name);
	} else {
		fprintf(fp,
			"pkgname='%s-%s'\n",
			name,
			abi->name);
	}
}

/* ------------------------------------------------------------------------- */
/* Existing PKGBUILD functions                                                */
/* ------------------------------------------------------------------------- */

static void save_original_function(FILE *fp,
				   const char *function)
{
	fprintf(fp,
		"if declare -F %s >/dev/null 2>&1; then\n"
		"  eval \"$(declare -f %s | sed '1s/^%s /_packinstall_original_%s /')\"\n"
		"fi\n",
		function,
		function,
		function,
		function);
}

static void append_prepare_function(FILE *fp,
				    int no_build)
{
	fprintf(fp, "prepare() {\n");

	fprintf(fp,
		"  if [ ! -e \"$srcdir/$_packinstall_basename\" ]; then\n"
		"    ln -s \"$_packinstall_source\" "
		"\"$srcdir/$_packinstall_basename\"\n"
		"  fi\n");

	fprintf(fp,
		"  if [ ! -e \"$srcdir/$_packinstall_pkgname_original\" ]; then\n"
		"    ln -s \"$_packinstall_source\" "
		"\"$srcdir/$_packinstall_pkgname_original\"\n"
		"  fi\n");

	fprintf(fp,
		"  if [ ! -e \"$srcdir/$_packinstall_pkgname_original-$_packinstall_pkgver\" ]; then\n"
		"    ln -s \"$_packinstall_source\" "
		"\"$srcdir/$_packinstall_pkgname_original-$_packinstall_pkgver\"\n"
		"  fi\n");

	if (!no_build) {
		fprintf(fp,
			"  if declare -F _packinstall_original_prepare >/dev/null 2>&1; then\n"
			"    _packinstall_original_prepare\n"
			"  fi\n");
	}

	fprintf(fp, "}\n");
}

static void append_check_function(FILE *fp,
				  int skip_checks,
				  int no_build)
{
	fprintf(fp, "check() {\n");

	if (!skip_checks && !no_build) {
		fprintf(fp,
			"  if declare -F _packinstall_original_check >/dev/null 2>&1; then\n"
			"    _packinstall_original_check\n"
			"  fi\n");
	} else {
		fprintf(fp, "  :\n");
	}

	fprintf(fp, "}\n");
}

/* ------------------------------------------------------------------------- */
/* package command                                                            */
/* ------------------------------------------------------------------------- */

static void package_pkgbuid(const char *dir,
			    const char *pkgbuild,
			    const char *system_override,
			    const struct abi_profile *abi,
			    const char *output,
			    int skip_checks,
			    int no_build)
{
	char name[256];
	char version[256];
	char release[256];

	char absbuild[PATH_MAX];
	char temp[PATH_MAX];
	char wrapper[PATH_MAX];

	char qtemp[PATH_MAX * 2];
	char qoutput[PATH_MAX * 2];
	char qorig[PATH_MAX * 2];

	char cmd[PATH_MAX * 10];

	const char *system;
	int forced_system;

	FILE *fp;

	read_srcinfo(dir,
		     name,
		     sizeof(name),
		     version,
		     sizeof(version),
		     release,
		     sizeof(release));

	system = normalize_build_system(system_override,
					dir);

	if (!system)
		die("could not detect a build system in %s",
		    dir);

	forced_system =
		system_override &&
		strcmp(system_override, "auto") != 0;

	absolute_path(pkgbuild,
		      absbuild,
		      sizeof(absbuild));

	if (snprintf(temp,
		     sizeof(temp),
		     "%s/.packinstall-lfs.%ld",
		     dir,
		     (long)getpid()) >= (int)sizeof(temp))
		die("temporary path is too long");

	if (mkdir(temp, 0700) != 0)
		die("cannot create temporary directory %s: %s",
		    temp,
		    strerror(errno));

	if (strlen(temp) +
	    strlen("/PKGBUILD") + 1 >= sizeof(wrapper))
		die("temporary PKGBUILD path is too long");

	strcpy(wrapper, temp);
	strcat(wrapper, "/PKGBUILD");

	fp = fopen(wrapper, "w");

	if (!fp) {
		rmdir(temp);
		die("cannot create generated PKGBUILD: %s",
		    strerror(errno));
	}

	shell_quote(absbuild,
		    qorig,
		    sizeof(qorig));

	/*
	 * Source the original PKGBUILD first.
	 */
	fprintf(fp,
		"# generated by packinstall-lfs %s\n",
		VERSION);

	fprintf(fp,
		"source %s\n",
		qorig);

	fprintf(fp,
		"_packinstall_source='%s'\n",
		dir);

	fprintf(fp,
		"_packinstall_pkgname_original='%s'\n",
		name);

	fprintf(fp,
		"_packinstall_pkgver='%s'\n",
		version);

	fprintf(fp,
		"_packinstall_pkgrel='%s'\n",
		release);

	fprintf(fp,
		"_packinstall_basename=$(basename "
		"\"$_packinstall_source\")\n");

	append_abi_metadata(fp,
			    name,
			    version,
			    release,
			    abi);

	append_abi_environment(fp,
				abi);

	/*
	 * Save user's functions before replacing them.
	 */
	save_original_function(fp, "prepare");
	save_original_function(fp, "build");
	save_original_function(fp, "check");
	save_original_function(fp, "package");

	/*
	 * The source tree is already available locally.
	 */
	fprintf(fp,
		"source=()\n"
		"sha256sums=()\n"
		"sha512sums=()\n"
		"b2sums=()\n"
		"md5sums=()\n");

	/*
	 * Preserve release detected by makepkg.
	 */
	fprintf(fp,
		"pkgrel='%s'\n",
		release);

	/*
	 * ABI-specific package names let multiple ABI packages coexist.
	 */
	append_pkgname_override(fp,
				name,
				abi);

	/*
	 * For native packages use the normal package architecture.
	 * For ABI-specific packages use the ABI name as makepkg arch.
	 */
	if (abi_is_native(abi)) {
		fprintf(fp,
			"arch=('auto')\n");
	} else {
		fprintf(fp,
			"arch=('%s')\n",
			abi->name);
	}

	append_prepare_function(fp,
				no_build);

	/*
	 * Build.
	 */
	if (no_build) {
		fprintf(fp,
			"build() {\n"
			"  :\n"
			"}\n");
	} else if (forced_system) {
		append_generated_build(fp,
				       system,
				       abi,
				       0);
	} else {
		fprintf(fp,
			"if declare -F _packinstall_original_build "
			">/dev/null 2>&1; then\n"
			"  build() {\n"
			"    _packinstall_original_build\n"
			"  }\n"
			"else\n");

		append_generated_build(fp,
				       system,
				       abi,
				       0);

		fprintf(fp,
			"fi\n");
	}

	append_check_function(fp,
			      skip_checks,
			      no_build);

	/*
	 * Explicit --build-system means the generated package() wins.
	 * Otherwise preserve the user's package() if it exists.
	 */
	if (forced_system) {
		append_generated_package(fp,
					 system,
					 abi);
	} else {
		fprintf(fp,
			"if declare -F _packinstall_original_package "
			">/dev/null 2>&1; then\n"
			"  package() {\n"
			"    _packinstall_original_package\n"
			"  }\n"
			"else\n");

		append_generated_package(fp,
					 system,
					 abi);

		fprintf(fp,
			"fi\n");
	}

	/*
	 * Package metadata.
	 */
	fprintf(fp,
		"packinstall_lfs_abi() {\n"
		"  mkdir -p \"$pkgdir/etc/packinstall-lfs\"\n"
		"  printf 'format=packinstall-lfs/%s\\n' > "
		"\"$pkgdir/etc/packinstall-lfs/package-info\"\n"
		"  printf 'pkgname=%s\\n' >> "
		"\"$pkgdir/etc/packinstall-lfs/package-info\"\n"
		"  printf 'pkgver=%s\\n' >> "
		"\"$pkgdir/etc/packinstall-lfs/package-info\"\n"
		"  printf 'pkgrel=%s\\n' >> "
		"\"$pkgdir/etc/packinstall-lfs/package-info\"\n"
		"  printf 'abi=%s\\n' >> "
		"\"$pkgdir/etc/packinstall-lfs/package-info\"\n"
		"  printf 'libdir=%s\\n' >> "
		"\"$pkgdir/etc/packinstall-lfs/package-info\"\n"
		"  printf 'machine=%s\\n' >> "
		"\"$pkgdir/etc/packinstall-lfs/package-info\"\n"
		"}\n",
		VERSION,
		abi->name);

	fclose(fp);

	shell_quote(temp,
		    qtemp,
		    sizeof(qtemp));

	if (output)
		shell_quote(output,
			    qoutput,
			    sizeof(qoutput));
	else
		shell_quote(dir,
			    qoutput,
			    sizeof(qoutput));

	/*
	 * PKGEXT is overridden so makepkg generates Kuznix packages.
	 *
	 * --noextract is important: the LFS source tree already exists.
	 */
	snprintf(cmd,
		 sizeof(cmd),
		 "mkdir -p %s && "
		 "cd %s && "
		 "PKGEXT='%s' "
		 "PKGDEST=%s "
		 "makepkg "
		 "-f "
		 "--nodeps "
		 "--noextract "
		 "-p %s "
		 "%s",
		 qoutput,
		 qtemp,
		 SUFFIX,
		 qoutput,
		 wrapper,
		 (skip_checks || no_build) ? "--nocheck" : "");

	if (run_command(cmd) != 0) {
		unlink(wrapper);
		rmdir(temp);

		die("makepkg failed for %s-%s-%s-%s",
		    name,
		    version,
		    release,
		    abi->name);
	}

	unlink(wrapper);
	rmdir(temp);

	printf("packaged %s-%s-%s-%s using %s\n",
	       name,
	       version,
	       release,
	       abi->name,
	       system);
}

/* ------------------------------------------------------------------------- */
/* Legacy create                                                             */
/* ------------------------------------------------------------------------- */

static void create_package(const char *stage,
			   const char *name,
			   const char *version,
			   const char *output)
{
	struct stat st;

	char qstage[PATH_MAX * 2];
	char qout[PATH_MAX * 2];
	char cmd[PATH_MAX * 4];
	char info[PATH_MAX];

	if (stat(stage, &st) != 0 ||
	    !S_ISDIR(st.st_mode))
		die("stage directory does not exist: %s",
		    stage);

	if (!output) {
		static char generated[PATH_MAX];

		snprintf(generated,
			 sizeof(generated),
			 "%s-%s-1%s",
			 name,
			 version,
			 SUFFIX);

		output = generated;
	}

	snprintf(info,
		 sizeof(info),
		 "%s/.LFSINFO",
		 stage);

	{
		FILE *fp;

		fp = fopen(info, "w");

		if (!fp)
			die("cannot create %s: %s",
			    info,
			    strerror(errno));

		fprintf(fp,
			"format=packinstall-lfs/%s\n"
			"name=%s\n"
			"version=%s\n"
			"pkgrel=1\n"
			"abi=native\n"
			"libdir=/usr/lib\n"
			"arch=native\n",
			VERSION,
			name,
			version);

		fclose(fp);
	}

	shell_quote(stage,
		    qstage,
		    sizeof(qstage));

	shell_quote(output,
		    qout,
		    sizeof(qout));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -C %s -cf %s .",
		 qstage,
		 qout);

	if (run_command(cmd) != 0) {
		unlink(info);
		die("tar failed");
	}

	unlink(info);

	printf("created %s\n",
	       output);
}

/* ------------------------------------------------------------------------- */
/* Archive validation                                                        */
/* ------------------------------------------------------------------------- */

static int safe_member(const char *path)
{
	if (!path || !*path)
		return 0;

	if (path[0] == '/')
		return 0;

	if (!strcmp(path, ".") ||
	    !strcmp(path, "./"))
		return 1;

	if (!strcmp(path, ".."))
		return 0;

	if (!strncmp(path, "../", 3))
		return 0;

	if (strstr(path, "/../"))
		return 0;

	if (strstr(path, "/./"))
		return 0;

	return 1;
}

static void validate_archive(const char *package)
{
	char qpackage[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];
	FILE *fp;
	char line[PATH_MAX];

	shell_quote(package,
		    qpackage,
		    sizeof(qpackage));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -tf %s",
		 qpackage);

	fp = popen(cmd, "r");

	if (!fp)
		die("cannot inspect %s",
		    package);

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		line[strcspn(line,
			     "\r\n")] = '\0';

		if (!safe_member(line)) {
			pclose(fp);

			die("unsafe archive member: %s",
			    line);
		}
	}

	if (pclose(fp) != 0)
		die("cannot inspect archive: %s",
		    package);
}

/* ------------------------------------------------------------------------- */
/* info                                                                      */
/* ------------------------------------------------------------------------- */

static void package_info(const char *package)
{
	char qpackage[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];

	shell_quote(package,
		    qpackage,
		    sizeof(qpackage));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -xOf %s "
		 "./.LFSINFO 2>/dev/null",
		 qpackage);

	if (run_command(cmd) != 0) {
		/*
		 * makepkg-generated ABI packages store their metadata here too.
		 */
		snprintf(cmd,
			 sizeof(cmd),
			 "tar --zstd -xOf %s "
			 "./etc/packinstall-lfs/package-info 2>/dev/null",
			 qpackage);

		if (run_command(cmd) != 0)
			die("%s has no readable package metadata",
			    package);
	}
}

/* ------------------------------------------------------------------------- */
/* install                                                                    */
/* ------------------------------------------------------------------------- */

static void install_package(const char *package)
{
	char qpackage[PATH_MAX * 2];
	char qroot[PATH_MAX * 2];
	char qtmp[PATH_MAX * 2];

	char cmd[PATH_MAX * 5];
	char tmp[PATH_MAX];

	char name[256] = { 0 };
	char version[256] = { 0 };
	char release[256] = { 0 };
	char abi[128] = { 0 };

	char db[PATH_MAX];
	char files[PATH_MAX];

	FILE *fp;
	char line[1024];

	validate_archive(package);

	shell_quote(package,
		    qpackage,
		    sizeof(qpackage));

	shell_quote(root_dir,
		    qroot,
		    sizeof(qroot));

	snprintf(tmp,
		 sizeof(tmp),
		 "/tmp/packinstall-lfs-info-%ld",
		 (long)getpid());

	shell_quote(tmp,
		    qtmp,
		    sizeof(qtmp));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -xOf %s "
		 "./etc/packinstall-lfs/package-info > %s 2>/dev/null",
		 qpackage,
		 qtmp);

	if (run_command(cmd) != 0) {
		snprintf(cmd,
			 sizeof(cmd),
			 "tar --zstd -xOf %s "
			 "./.LFSINFO > %s 2>/dev/null",
			 qpackage,
			 qtmp);

		if (run_command(cmd) != 0)
			die("cannot read package metadata");
	}

	fp = fopen(tmp, "r");

	if (!fp)
		die("cannot read package metadata");

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		if (sscanf(line,
			   "pkgname=%255s",
			   name) == 1)
			continue;

		if (sscanf(line,
			   "name=%255s",
			   name) == 1)
			continue;

		if (sscanf(line,
			   "pkgver=%255s",
			   version) == 1)
			continue;

		if (sscanf(line,
			   "version=%255s",
			   version) == 1)
			continue;

		if (sscanf(line,
			   "pkgrel=%255s",
			   release) == 1)
			continue;

		if (sscanf(line,
			   "abi=%127s",
			   abi) == 1)
			continue;
	}

	fclose(fp);
	unlink(tmp);

	if (!name[0] || !version[0])
		die("package metadata is incomplete");

	if (!abi[0])
		snprintf(abi,
			 sizeof(abi),
			 "native");

	if (!release[0])
		snprintf(release,
			 sizeof(release),
			 "1");

	if (snprintf(db,
		     sizeof(db),
		     "%s%s/%s/%s/%s",
		     root_dir,
		     DB_REL,
		     name,
		     version,
		     abi) >= (int)sizeof(db))
		die("package database path is too long");

	if (snprintf(files,
		     sizeof(files),
		     "%s/files",
		     db) >= (int)sizeof(files))
		die("package database path is too long");

	ensure_dir(db);

	/*
	 * Never install package-manager metadata into /
	 * from an ABI package.
	 */
	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -xpf %s "
		 "-C %s "
		 "--exclude=./etc/packinstall-lfs "
		 "--exclude=./.LFSINFO "
		 "--exclude=./.FILES "
		 "--no-same-owner "
		 "--no-same-permissions",
		 qpackage,
		 qroot);

	if (run_command(cmd) != 0)
		die("failed to install %s",
		    package);

	/*
	 * Record package files when available.
	 */
	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -xOf %s "
		 "./.FILES > %s 2>/dev/null",
		 qpackage,
		 qtmp);

	if (run_command(cmd) == 0) {
		if (rename(tmp, files) != 0)
			warnx("cannot store file manifest: %s",
			      strerror(errno));
	} else {
		fp = fopen(files, "w");

		if (fp) {
			fprintf(fp,
				"# package %s-%s-%s-%s\n",
				name,
				version,
				release,
				abi);

			fclose(fp);
		}
	}

	printf("installed %s-%s-%s [%s]\n",
	       name,
	       version,
	       release,
	       abi);
}

/* ------------------------------------------------------------------------- */
/* remove                                                                    */
/* ------------------------------------------------------------------------- */

static void remove_package(const char *name,
			   const char *version,
			   const char *abi_name)
{
	char db[PATH_MAX];
	char files[PATH_MAX];
	char path[PATH_MAX];

	FILE *fp;

	if (!abi_name)
		abi_name = "native";

	if (snprintf(db,
		     sizeof(db),
		     "%s%s/%s/%s/%s",
		     root_dir,
		     DB_REL,
		     name,
		     version,
		     abi_name) >= (int)sizeof(db))
		die("package database path is too long");

	if (snprintf(files,
		     sizeof(files),
		     "%s/files",
		     db) >= (int)sizeof(files))
		die("package database path is too long");

	fp = fopen(files, "r");

	if (!fp)
		die("package %s-%s-%s is not installed",
		    name,
		    version,
		    abi_name);

	while (fgets(path,
		     sizeof(path),
		     fp)) {
		char full[PATH_MAX];
		struct stat st;

		path[strcspn(path,
			     "\r\n")] = '\0';

		if (path[0] == '#')
			continue;

		if (!safe_member(path))
			continue;

		if (strlen(root_dir) +
		    1 +
		    strlen(path) >= PATH_MAX)
			continue;

		strcpy(full,
		       root_dir);

		strcat(full,
		       "/");

		strcat(full,
		       path);

		if (lstat(full,
			  &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			(void)rmdir(full);
		} else {
			if (unlink(full) != 0 &&
			    errno != ENOENT) {
				fprintf(stderr,
					"packinstall-lfs: cannot remove %s: %s\n",
					full,
					strerror(errno));
			}
		}
	}

	fclose(fp);

	unlink(files);
	rmdir(db);

	printf("removed %s-%s [%s]\n",
	       name,
	       version,
	       abi_name);
}

/* ------------------------------------------------------------------------- */
/* list                                                                      */
/* ------------------------------------------------------------------------- */

static void list_packages(void)
{
	char base[PATH_MAX];
	char qbase[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];

	if (snprintf(base,
		     sizeof(base),
		     "%s%s",
		     root_dir,
		     DB_REL) >= (int)sizeof(base))
		die("package database path is too long");

	shell_quote(base,
		    qbase,
		    sizeof(qbase));

	snprintf(cmd,
		 sizeof(cmd),
		 "find %s "
		 "-mindepth 3 "
		 "-maxdepth 3 "
		 "-type d "
		 "-printf '%%P\\n' "
		 "2>/dev/null",
		 qbase);

	(void)run_command(cmd);
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *root;

	if (argc < 2) {
		usage();
		return EXIT_FAILURE;
	}

	root = arg_value(argc,
			 argv,
			 "--root");

	if (root)
		root_dir = root;

	if (!strcmp(argv[1], "package")) {
		const char *dir;
		const char *buildscript;
		const char *system;
		const char *abi_name;
		const char *output;

		const struct abi_profile *abi;

		char absdir[PATH_MAX];
		char pkgbuild[PATH_MAX];

		dir = arg_value(argc,
				argv,
				"--dir");

		if (!dir)
			dir = ".";

		if (!is_directory(dir))
			die("source directory does not exist: %s",
			    dir);

		absolute_path(dir,
			      absdir,
			      sizeof(absdir));

		buildscript = arg_value(argc,
					argv,
					"--buildscript");

		if (!buildscript)
			buildscript = "PKGBUILD";

		if (buildscript[0] == '/') {
			if (strlen(buildscript) >= sizeof(pkgbuild))
				die("buildscript path is too long");

			strcpy(pkgbuild,
			       buildscript);
		} else {
			if (strlen(absdir) +
			    1 +
			    strlen(buildscript) >= sizeof(pkgbuild))
				die("buildscript path is too long");

			strcpy(pkgbuild,
			       absdir);

			strcat(pkgbuild,
			       "/");

			strcat(pkgbuild,
			       buildscript);
		}

		if (!is_file(pkgbuild))
			die("build script not found: %s",
			    pkgbuild);

		abi_name = arg_value(argc,
				     argv,
				     "--abi");

		if (abi_name) {
			abi = find_abi(abi_name);

			if (!abi)
				die("unknown ABI profile: %s",
				    abi_name);
		} else {
			abi = detect_default_abi();
		}

		system = arg_value(argc,
				   argv,
				   "--build-system");

		output = arg_value(argc,
				   argv,
				   "--output");

		if (!output)
			output = absdir;

		package_pkgbuid(
			absdir,
			pkgbuild,
			system,
			abi,
			output,
			has_opt(argc,
				argv,
				"--skip-checks"),
			has_opt(argc,
				argv,
				"--no-build"));

		return EXIT_SUCCESS;
	}

	if (!strcmp(argv[1], "create")) {
		if (argc < 5) {
			usage();
			return EXIT_FAILURE;
		}

		create_package(
			argv[2],
			argv[3],
			argv[4],
			argc >= 6 ? argv[5] : NULL);

		return EXIT_SUCCESS;
	}

	if (!strcmp(argv[1], "install")) {
		if (argc < 3) {
			usage();
			return EXIT_FAILURE;
		}

		install_package(argv[2]);

		return EXIT_SUCCESS;
	}

	if (!strcmp(argv[1], "remove")) {
		const char *abi;

		if (argc < 4) {
			usage();
			return EXIT_FAILURE;
		}

		abi = arg_value(argc,
				argv,
				"--abi");

		remove_package(
			argv[2],
			argv[3],
			abi);

		return EXIT_SUCCESS;
	}

	if (!strcmp(argv[1], "list")) {
		list_packages();
		return EXIT_SUCCESS;
	}

	if (!strcmp(argv[1], "info")) {
		if (argc < 3) {
			usage();
			return EXIT_FAILURE;
		}

		package_info(argv[2]);
		return EXIT_SUCCESS;
	}

	usage();
	return EXIT_FAILURE;
}
