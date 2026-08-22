/*
 * packinstall-lfs.c - LFS/MLFS source packager
 *
 * Version: 0.1
 *
 * Modern usage:
 *
 *   packinstall-lfs create
 *   packinstall-lfs create .
 *   packinstall-lfs create /sources/kuzpkg
 *
 * If PKGBUILD exists:
 *   - makepkg --printsrcinfo is used for metadata
 *   - pkgname/pkgver/pkgrel are detected
 *
 * If PKGBUILD does not exist:
 *   - build system is detected automatically
 *   - project name/version are detected when possible
 *   - a temporary PKGBUILD is generated
 *
 * Package format:
 *
 *   .kuzpkg.tar.zst
 *
 * Package metadata:
 *
 *   PKGINFO
 *
 * Legacy raw-stage mode:
 *
 *   packinstall-lfs create-legacy STAGE NAME VERSION [OUTPUT]
 *
 * Supported build systems:
 *
 *   make
 *   autotools
 *   meson
 *   ninja
 *   cmake
 *   python
 *   python-setup
 *   python-pyproject
 *   python-cfg
 *   ruby
 *   perl
 *   go
 *   rust
 *   rustc
 *   cargo
 *   waf
 *   scons
 *
 * ABI profiles:
 *
 *   native
 *   lib32
 *   libx32
 *   libo32
 *   libm32
 *   ppc32
 *   ppc64
 *   ppc64le
 *
 * Copyright (C) 2026 Kuznix
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define VERSION "0.1"
#define SUFFIX ".kuzpkg.tar.zst"
#define PKGINFO_NAME "PKGINFO"
#define DB_REL "/var/lib/packinstall-lfs"

#define BUILDER_USER "builder"
#define BUILDER_GROUP "builder"
#define BUILDER_HOME "/home/builder"
#define BUILDER_SHELL "/bin/bash"

static const char *root_dir = "/";

/* ------------------------------------------------------------------------- */
/* ABI                                                                       */
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

/* ------------------------------------------------------------------------- */
/* Errors                                                                    */
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
	puts("  packinstall-lfs create [DIR] [options]");
	puts("  packinstall-lfs package [DIR] [options]");
	puts("");
	puts("Package operations:");
	puts("  packinstall-lfs install PACKAGE [--root DIR]");
	puts("  packinstall-lfs remove NAME VERSION [--abi ABI] [--root DIR]");
	puts("  packinstall-lfs list [--root DIR]");
	puts("  packinstall-lfs info PACKAGE");
	puts("");
	puts("Legacy:");
	puts("  packinstall-lfs create-legacy STAGE NAME VERSION [OUTPUT]");
	puts("");
	puts("Options:");
	puts("  --dir DIR");
	puts("  --buildscript FILE");
	puts("  --build-system auto|make|autotools|meson|ninja|cmake|");
	puts("                 python|python-setup|python-pyproject|python-cfg|");
	puts("                 ruby|perl|go|rust|rustc|cargo|waf|scons");
	puts("  --abi native|lib32|libx32|libo32|libm32|ppc32|ppc64|ppc64le");
	puts("  --output DIR");
	puts("  --no-build");
	puts("  --skip-checks");
	puts("");
	puts("Examples:");
	puts("  packinstall-lfs create");
	puts("  packinstall-lfs create .");
	puts("  packinstall-lfs create /sources/kuzpkg");
	puts("  packinstall-lfs create /sources/kuzpkg --no-build");
	puts("  packinstall-lfs create /sources/foo --abi lib32");
	puts("  packinstall-lfs create /sources/foo --build-system meson");
	puts("  packinstall-lfs create-legacy /tmp/pkgroot foo 1.0");
}

/* ------------------------------------------------------------------------- */
/* Basic helpers                                                             */
/* ------------------------------------------------------------------------- */

static int command_exists(const char *path)
{
	return access(path, X_OK) == 0;
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

		++s;
	}

	out[p++] = '\'';
	out[p] = '\0';
}

static int run_command(const char *cmd)
{
	int rc = system(cmd);

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

		if (mkdir(buffer, 0755) != 0 &&
		    errno != EEXIST)
			die("mkdir %s: %s",
			    buffer,
			    strerror(errno));

		*p = '/';
	}

	if (mkdir(buffer, 0755) != 0 &&
	    errno != EEXIST)
		die("mkdir %s: %s",
		    buffer,
		    strerror(errno));
}

static const char *arg_value(int argc,
			     char **argv,
			     const char *opt)
{
	int i;

	for (i = 1; i + 1 < argc; ++i) {
		if (!strcmp(argv[i], opt))
			return argv[i + 1];
	}

	return NULL;
}

static int has_opt(int argc,
		   char **argv,
		   const char *opt)
{
	int i;

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], opt))
			return 1;
	}

	return 0;
}

static void absolute_path(const char *input,
			  char *output,
			  size_t size)
{
	char *resolved;

	resolved = realpath(input, NULL);

	if (!resolved)
		die("cannot resolve %s: %s",
		    input,
		    strerror(errno));

	if (strlen(resolved) >= size) {
		free(resolved);
		die("path too long: %s",
		    input);
	}

	strcpy(output, resolved);
	free(resolved);
}

static const char *basename_const(const char *path)
{
	const char *p;

	p = strrchr(path, '/');

	if (!p)
		return path;

	if (*(p + 1))
		return p + 1;

	return path;
}

/*
 * Normalize a version enough for makepkg/pkgver.
 *
 * Meson commonly uses:
 *
 *   0.1.0-alpha
 *
 * Arch-style package versions cannot use '-' as a package-version separator
 * in the same way, so normalize:
 *
 *   0.1.0-alpha -> 0.1.0_alpha
 */
static void normalize_version(const char *input,
			      char *output,
			      size_t output_size)
{
	size_t i;
	size_t p = 0;

	for (i = 0; input[i] && p + 1 < output_size; ++i) {
		char c = input[i];

		if (c == '-')
			c = '_';

		output[p++] = c;
	}

	output[p] = '\0';
}

/* ------------------------------------------------------------------------- */
/* CLI/source layout                                                         */
/* ------------------------------------------------------------------------- */

static void resolve_source_dir(int argc,
			       char **argv,
			       char *output,
			       size_t output_size)
{
	const char *dir;

	dir = arg_value(argc, argv, "--dir");

	if (dir) {
		absolute_path(dir, output, output_size);
		return;
	}

	/*
	 * Convenient form:
	 *
	 *   packinstall-lfs create /sources/kuzpkg
	 */
	if (argc >= 3 &&
	    argv[2][0] != '-' &&
	    strcmp(argv[2], "--help") &&
	    strcmp(argv[2], "--version")) {
		absolute_path(argv[2], output, output_size);
		return;
	}

	absolute_path(".", output, output_size);
}

static void resolve_pkgbuild(const char *dir,
			     const char *requested,
			     char *output,
			     size_t output_size)
{
	if (requested) {
		if (requested[0] == '/') {
			if (strlen(requested) >= output_size)
				die("PKGBUILD path is too long");

			strcpy(output, requested);
			return;
		}

		if (snprintf(output,
			     output_size,
			     "%s/%s",
			     dir,
			     requested) >= (int)output_size)
			die("PKGBUILD path is too long");

		return;
	}

	if (snprintf(output,
		     output_size,
		     "%s/PKGBUILD",
		     dir) >= (int)output_size)
		die("PKGBUILD path is too long");
}

/* ------------------------------------------------------------------------- */
/* ABI                                                                       */
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
		"export LIBDIR='%s'\n",
		abi->libdir);

	fprintf(fp,
		"export libdir='%s'\n",
		abi->libdir);

	fprintf(fp,
		"export INSTALL_LIBDIR='%s'\n",
		abi->libdir);

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
			"export MIPS_ABI=o32\n");
	}

	if (abi->abi == ABI_LIBM32) {
		fprintf(fp,
			"export MIPS_ABI=n32\n");
	}
}

/* ------------------------------------------------------------------------- */
/* Build system detection                                                    */
/* ------------------------------------------------------------------------- */

static int has_named_file(const char *dir,
			  const char *name)
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
	 * Rust compiler sources have Cargo.toml too, so x.py wins.
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
	if (!system || !strcmp(system, "auto"))
		return detect_build_system(dir);

	if (!strcmp(system, "make"))
		return "Makefile";

	if (!strcmp(system, "autotools"))
		return "configure.ac";

	if (!strcmp(system, "meson") ||
	    !strcmp(system, "ninja"))
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
		if (has_named_file(dir, "x.py"))
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
/* Metadata detection without PKGBUILD                                       */
/* ------------------------------------------------------------------------- */

static int read_meson_project(const char *dir,
			      char *name,
			      size_t name_size,
			      char *version,
			      size_t version_size)
{
	char path[PATH_MAX];
	FILE *fp;
	char line[4096];

	if (snprintf(path,
		     sizeof(path),
		     "%s/meson.build",
		     dir) >= (int)sizeof(path))
		return -1;

	fp = fopen(path, "r");

	if (!fp)
		return -1;

	name[0] = '\0';
	version[0] = '\0';

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		char *p;

		/*
		 * project('kuzpkg',
		 *         'c',
		 *         version : '0.1.0-alpha')
		 */
		if (!name[0]) {
			p = strstr(line, "project(");

			if (p) {
				char *q;
				char quote;
				char *end;

				q = strchr(p, '\'');

				if (!q)
					q = strchr(p, '"');

				if (q) {
					quote = *q++;
					end = strchr(q, quote);

					if (end) {
						size_t len =
							(size_t)(end - q);

						if (len >= name_size)
							len = name_size - 1;

						memcpy(name, q, len);
						name[len] = '\0';
					}
				}
			}
		}

		if (!version[0]) {
			p = strstr(line, "version");

			if (p) {
				char *q;
				char quote;
				char *end;

				q = strchr(p, '\'');

				if (!q)
					q = strchr(p, '"');

				if (q) {
					quote = *q++;
					end = strchr(q, quote);

					if (end) {
						size_t len =
							(size_t)(end - q);

						if (len >= version_size)
							len = version_size - 1;

						memcpy(version, q, len);
						version[len] = '\0';
					}
				}
			}
		}

		if (name[0] && version[0])
			break;
	}

	fclose(fp);

	if (!name[0] || !version[0])
		return -1;

	return 0;
}

static int read_cmake_project(const char *dir,
			      char *name,
			      size_t name_size,
			      char *version,
			      size_t version_size)
{
	char path[PATH_MAX];
	FILE *fp;
	char line[4096];

	if (snprintf(path,
		     sizeof(path),
		     "%s/CMakeLists.txt",
		     dir) >= (int)sizeof(path))
		return -1;

	fp = fopen(path, "r");

	if (!fp)
		return -1;

	name[0] = '\0';
	version[0] = '\0';

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		char *p;

		p = strstr(line, "project(");

		if (p && !name[0]) {
			char *q;
			char quote;
			char *end;

			q = strchr(p, '(');

			if (q)
				++q;

			while (q && (*q == ' ' ||
				     *q == '\t'))
				++q;

			if (q) {
				quote = (*q == '\'' || *q == '"') ?
					*q++ :
					0;

				if (quote)
					end = strchr(q, quote);
				else {
					end = q;

					while (*end &&
					       *end != ' ' &&
					       *end != '\t' &&
					       *end != ')')
						++end;
				}

				if (end) {
					size_t len =
						(size_t)(end - q);

					if (len >= name_size)
						len = name_size - 1;

					memcpy(name, q, len);
					name[len] = '\0';
				}
			}
		}

		p = strstr(line, "VERSION");

		if (p && !version[0]) {
			char *q = strchr(p, '=');

			if (q) {
				++q;

				while (*q == ' ' ||
				       *q == '\t' ||
				       *q == '"' ||
				       *q == '\'')
					++q;

				{
					char *end = q;

					while (*end &&
					       *end != '"' &&
					       *end != '\'' &&
					       *end != ')' &&
					       *end != '\n' &&
					       *end != '\r')
						++end;

					if (end > q) {
						size_t len =
							(size_t)(end - q);

						if (len >= version_size)
							len = version_size - 1;

						memcpy(version,
						       q,
						       len);

						version[len] = '\0';
					}
				}
			}
		}
	}

	fclose(fp);

	if (!name[0] || !version[0])
		return -1;

	return 0;
}

static int read_python_project(const char *dir,
			       char *name,
			       size_t name_size,
			       char *version,
			       size_t version_size)
{
	char path[PATH_MAX];
	FILE *fp;
	char line[4096];

	if (snprintf(path,
		     sizeof(path),
		     "%s/pyproject.toml",
		     dir) >= (int)sizeof(path))
		return -1;

	fp = fopen(path, "r");

	if (!fp)
		return -1;

	name[0] = '\0';
	version[0] = '\0';

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		char *eq;

		if (!strstr(line, "name"))
			goto check_version;

		eq = strchr(line, '=');

		if (eq &&
		    !name[0]) {
			char *q;

			q = strchr(eq + 1, '"');

			if (!q)
				q = strchr(eq + 1, '\'');

			if (q) {
				char quote = *q++;
				char *end = strchr(q, quote);

				if (end) {
					size_t len =
						(size_t)(end - q);

					if (len >= name_size)
						len = name_size - 1;

					memcpy(name, q, len);
					name[len] = '\0';
				}
			}
		}

check_version:
		if (!strstr(line, "version"))
			continue;

		eq = strchr(line, '=');

		if (eq &&
		    !version[0]) {
			char *q;

			q = strchr(eq + 1, '"');

			if (!q)
				q = strchr(eq + 1, '\'');

			if (q) {
				char quote = *q++;
				char *end = strchr(q, quote);

				if (end) {
					size_t len =
						(size_t)(end - q);

					if (len >= version_size)
						len = version_size - 1;

					memcpy(version, q, len);
					version[len] = '\0';
				}
			}
		}
	}

	fclose(fp);

	if (!name[0] || !version[0])
		return -1;

	return 0;
}

static void detect_metadata(const char *dir,
			    const char *system,
			    char *name,
			    size_t name_size,
			    char *version,
			    size_t version_size,
			    char *release,
			    size_t release_size)
{
	char normalized_version[256];

	name[0] = '\0';
	version[0] = '\0';

	snprintf(release,
		 release_size,
		 "1");

	if (!strcmp(system, "meson.build")) {
		if (read_meson_project(dir,
				       name,
				       name_size,
				       version,
				       version_size) == 0) {
			normalize_version(version,
					  normalized_version,
					  sizeof(normalized_version));

			snprintf(version,
				 version_size,
				 "%s",
				 normalized_version);

			return;
		}
	}

	if (!strcmp(system, "CMakeLists.txt")) {
		if (read_cmake_project(dir,
				       name,
				       name_size,
				       version,
				       version_size) == 0) {
			normalize_version(version,
					  normalized_version,
					  sizeof(normalized_version));

			snprintf(version,
				 version_size,
				 "%s",
				 normalized_version);

			return;
		}
	}

	if (!strcmp(system, "pyproject.toml")) {
		if (read_python_project(dir,
					name,
					name_size,
					version,
					version_size) == 0) {
			normalize_version(version,
					  normalized_version,
					  sizeof(normalized_version));

			snprintf(version,
				 version_size,
				 "%s",
				 normalized_version);

			return;
		}
	}

	/*
	 * Generic fallback.
	 */
	snprintf(name,
		 name_size,
		 "%s",
		 basename_const(dir));

	snprintf(version,
		 version_size,
		 "0.0.0");
}

/* ------------------------------------------------------------------------- */
/* PKGBUILD metadata                                                         */
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

	fp = popen(cmd,
		   "r");

	if (!fp)
		die("cannot execute makepkg --printsrcinfo");

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		char *eq;
		char *value;

		eq = strchr(line, '=');

		if (!eq)
			continue;

		*eq++ = '\0';

		value = eq;

		while (*value == ' ' ||
		       *value == '\t')
			++value;

		value[strcspn(value,
			      "\r\n")] = '\0';

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

	if (rc != 0 ||
	    !name[0] ||
	    !version[0]) {
		die("makepkg could not read pkgname/pkgver from PKGBUILD");
	}

	if (!release[0])
		snprintf(release,
			 release_size,
			 "1");
}

/* ------------------------------------------------------------------------- */
/* Generate build()                                                          */
/* ------------------------------------------------------------------------- */

static void append_generated_build(FILE *fp,
				   const char *system,
				   int no_build)
{
	if (no_build) {
		fprintf(fp,
			"build() {\n"
			"  :\n"
			"}\n");
		return;
	}

	fprintf(fp,
		"build() {\n"
		"  cd \"$_packinstall_source\"\n");

	if (!strcmp(system, "meson.build")) {
		fprintf(fp,
			"  command -v meson >/dev/null 2>&1 || return 127\n"
			"  command -v ninja >/dev/null 2>&1 || return 127\n"
			"  rm -rf .packinstall-build\n"
			"  meson setup .packinstall-build "
			"--prefix=/usr "
			"--libdir=\"${LIBDIR#/usr/}\" "
			"--buildtype=release\n"
			"  ninja -C .packinstall-build\n");

	} else if (!strcmp(system, "CMakeLists.txt")) {
		fprintf(fp,
			"  command -v cmake >/dev/null 2>&1 || return 127\n"
			"  rm -rf .packinstall-build\n"
			"  cmake -S . "
			"-B .packinstall-build "
			"-DCMAKE_BUILD_TYPE=Release "
			"-DCMAKE_INSTALL_PREFIX=/usr "
			"-DCMAKE_INSTALL_LIBDIR=\"${LIBDIR#/usr/}\"\n"
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
			"  fi\n"
			"  ./configure "
			"--prefix=/usr "
			"--libdir=\"$LIBDIR\"\n"
			"  make\n");

	} else if (!strcmp(system, "Makefile")) {
		fprintf(fp,
			"  command -v make >/dev/null 2>&1 || return 127\n"
			"  make "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\"\n");

	} else if (!strcmp(system, "waf")) {
		fprintf(fp,
			"  test -x ./waf || chmod +x ./waf\n"
			"  ./waf configure --prefix=/usr\n"
			"  ./waf build\n");

	} else if (!strcmp(system, "wscript")) {
		fprintf(fp,
			"  command -v waf >/dev/null 2>&1 || return 127\n"
			"  waf configure --prefix=/usr\n"
			"  waf build\n");

	} else if (!strcmp(system, "SConstruct") ||
		   !strcmp(system, "sconstruct")) {
		fprintf(fp,
			"  command -v scons >/dev/null 2>&1 || return 127\n"
			"  scons\n");

	} else if (!strcmp(system, "setup.py")) {
		fprintf(fp,
			"  command -v python3 >/dev/null 2>&1 || return 127\n"
			"  python3 setup.py build\n");

	} else if (!strcmp(system, "setup.cfg")) {
		fprintf(fp,
			"  command -v python3 >/dev/null 2>&1 || return 127\n"
			"  python3 -m build --wheel --no-isolation\n");

	} else if (!strcmp(system, "pyproject.toml")) {
		fprintf(fp,
			"  command -v python3 >/dev/null 2>&1 || return 127\n"
			"  rm -rf .packinstall-wheels\n"
			"  mkdir -p .packinstall-wheels\n"
			"  python3 -m pip wheel "
			"--no-deps "
			"--no-build-isolation "
			"--wheel-dir .packinstall-wheels .\n");

	} else if (!strcmp(system, "extconf.rb")) {
		fprintf(fp,
			"  command -v ruby >/dev/null 2>&1 || return 127\n"
			"  ruby extconf.rb --prefix=/usr\n"
			"  make\n");

	} else if (!strcmp(system, "Makefile.PL")) {
		fprintf(fp,
			"  command -v perl >/dev/null 2>&1 || return 127\n"
			"  perl Makefile.PL PREFIX=/usr\n"
			"  make\n");

	} else if (!strcmp(system, "go.mod")) {
		fprintf(fp,
			"  command -v go >/dev/null 2>&1 || return 127\n"
			"  rm -rf .packinstall-go\n"
			"  mkdir -p .packinstall-go/bin\n"
			"  go build "
			"-buildvcs=false "
			"-o .packinstall-go/bin/ ./...\n");

	} else if (!strcmp(system, "Cargo.toml")) {
		fprintf(fp,
			"  command -v cargo >/dev/null 2>&1 || return 127\n"
			"  cargo build --release\n");

	} else if (!strcmp(system, "x.py")) {
		fprintf(fp,
			"  test -x ./x.py || chmod +x ./x.py\n"
			"  ./x.py build\n");

	} else {
		fprintf(fp,
			"  make "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\"\n");
	}

	fprintf(fp,
		"}\n");
}

/* ------------------------------------------------------------------------- */
/* Generate package()                                                        */
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
		"  export LIBDIR='%s'\n"
		"  export libdir='%s'\n",
		abi->libdir,
		abi->libdir);

	if (!strcmp(system, "meson.build")) {
		fprintf(fp,
			"  test -d .packinstall-build || return 1\n"
			"  DESTDIR=\"$pkgdir\" "
			"ninja -C .packinstall-build install\n");

	} else if (!strcmp(system, "CMakeLists.txt")) {
		fprintf(fp,
			"  test -d .packinstall-build || return 1\n"
			"  DESTDIR=\"$pkgdir\" "
			"cmake --install .packinstall-build\n");

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
			"  test -x ./waf || chmod +x ./waf\n"
			"  DESTDIR=\"$pkgdir\" ./waf install\n");

	} else if (!strcmp(system, "wscript")) {
		fprintf(fp,
			"  command -v waf >/dev/null 2>&1 || return 127\n"
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
			"  mkdir -p \"$pkgdir/usr/bin\"\n"
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
			"  test \"$found\" = 1 || return 1\n");

	} else if (!strcmp(system, "x.py")) {
		fprintf(fp,
			"  test -d build || return 1\n"
			"  DESTDIR=\"$pkgdir\" ./x.py install\n");

	} else {
		fprintf(fp,
			"  DESTDIR=\"$pkgdir\" "
			"PREFIX=/usr "
			"prefix=/usr "
			"LIBDIR=\"$LIBDIR\" "
			"libdir=\"$LIBDIR\" "
			"make install\n");
	}

	fprintf(fp,
		"}\n");
}

/* ------------------------------------------------------------------------- */
/* PKGINFO generation                                                        */
/* ------------------------------------------------------------------------- */

static void append_pkginfo_function(FILE *fp,
				    const char *name,
				    const char *version,
				    const char *release,
				    const struct abi_profile *abi)
{
	fprintf(fp,
		"packinstall_lfs_pkginfo() {\n"
		"  mkdir -p \"$pkgdir\"\n"
		"  {\n"
		"    printf 'format=packinstall-lfs/%s\\n'\n"
		"    printf 'pkgname=%s\\n'\n"
		"    printf 'pkgver=%s\\n'\n"
		"    printf 'pkgrel=%s\\n'\n"
		"    printf 'abi=%s\\n'\n"
		"    printf 'libdir=%s\\n'\n"
		"    printf 'machine=%s\\n'\n"
		"  } > \"$pkgdir/%s\"\n"
		"}\n",
		VERSION,
		name,
		version,
		release,
		abi->name,
		abi->libdir,
		abi->machine,
		PKGINFO_NAME);
}

/* ------------------------------------------------------------------------- */
/* Save original PKGBUILD functions                                         */
/* ------------------------------------------------------------------------- */

static void save_original_function(FILE *fp,
				   const char *function)
{
	fprintf(fp,
		"if declare -F %s >/dev/null 2>&1; then\n"
		"  eval \"$(declare -f %s | "
		"sed '1s/^%s /_packinstall_original_%s /')\"\n"
		"fi\n",
		function,
		function,
		function,
		function);
}

/* ------------------------------------------------------------------------- */
/* Generate wrapper PKGBUILD                                                 */
/* ------------------------------------------------------------------------- */

static void create_generated_pkgbuid(
	const char *work_source,
	const char *real_pkgbuild,
	const char *temp,
	const char *system,
	const struct abi_profile *abi,
	const char *name,
	const char *version,
	const char *release,
	int have_original_pkgbuild,
	int skip_checks,
	int no_build)
{
	char path[PATH_MAX];
	char qreal[PATH_MAX * 2];
	FILE *fp;

	if (snprintf(path,
		     sizeof(path),
		     "%s/PKGBUILD",
		     temp) >= (int)sizeof(path))
		die("temporary PKGBUILD path too long");

	fp = fopen(path, "w");

	if (!fp)
		die("cannot create generated PKGBUILD: %s",
		    strerror(errno));

	fprintf(fp,
		"# generated by packinstall-lfs %s\n",
		VERSION);

	/*
	 * Existing PKGBUILD:
	 *
	 * source it so dependencies/custom functions/variables remain visible.
	 */
	if (have_original_pkgbuild) {
		shell_quote(real_pkgbuild,
			    qreal,
			    sizeof(qreal));

		fprintf(fp,
			"source %s\n",
			qreal);

		save_original_function(fp,
				       "prepare");

		save_original_function(fp,
				       "build");

		save_original_function(fp,
				       "check");

		save_original_function(fp,
				       "package");
	}

	fprintf(fp,
		"_packinstall_source='%s'\n"
		"_packinstall_pkgname='%s'\n"
		"_packinstall_pkgver='%s'\n"
		"_packinstall_pkgrel='%s'\n"
		"_packinstall_abi='%s'\n"
		"_packinstall_libdir='%s'\n"
		"_packinstall_basename=$(basename "
		"\"$_packinstall_source\")\n",
		work_source,
		name,
		version,
		release,
		abi->name,
		abi->libdir);

	/*
	 * We don't want makepkg to fetch/extract another source tree.
	 */
	fprintf(fp,
		"source=()\n"
		"sha256sums=()\n"
		"sha512sums=()\n"
		"b2sums=()\n"
		"md5sums=()\n");

	if (abi->abi == ABI_NATIVE) {
		fprintf(fp,
			"pkgname='%s'\n",
			name);
	} else {
		fprintf(fp,
			"pkgname='%s-%s'\n",
			name,
			abi->name);
	}

	fprintf(fp,
		"pkgver='%s'\n"
		"pkgrel='%s'\n",
		version,
		release);

	if (abi->abi == ABI_NATIVE)
		fprintf(fp, "arch=('auto')\n");
	else
		fprintf(fp,
			"arch=('%s')\n",
			abi->name);

	append_abi_environment(fp,
			       abi);

	/*
	 * prepare()
	 */
	fprintf(fp,
		"prepare() {\n");

	if (have_original_pkgbuild) {
		fprintf(fp,
			"  if declare -F "
			"_packinstall_original_prepare "
			">/dev/null 2>&1; then\n"
			"    _packinstall_original_prepare\n"
			"  fi\n");
	}

	fprintf(fp,
		"  :\n"
		"}\n");

	/*
	 * build()
	 */
	if (no_build) {
		fprintf(fp,
			"build() {\n"
			"  :\n"
			"}\n");
	} else if (have_original_pkgbuild) {
		fprintf(fp,
			"build() {\n"
			"  if declare -F "
			"_packinstall_original_build "
			">/dev/null 2>&1; then\n"
			"    _packinstall_original_build\n"
			"  else\n");

		/*
		 * Do not generate another build if the user's PKGBUILD already
		 * has one.
		 */
		append_generated_build(fp,
				       system,
				       0);

		fprintf(fp,
			"  fi\n"
			"}\n");
	} else {
		append_generated_build(fp,
				       system,
				       0);
	}

	/*
	 * check()
	 */
	fprintf(fp,
		"check() {\n");

	if (have_original_pkgbuild &&
	    !skip_checks &&
	    !no_build) {
		fprintf(fp,
			"  if declare -F "
			"_packinstall_original_check "
			">/dev/null 2>&1; then\n"
			"    _packinstall_original_check\n"
			"  fi\n");
	} else {
		fprintf(fp,
			"  :\n");
	}

	fprintf(fp,
		"}\n");

	/*
	 * package()
	 */
	if (have_original_pkgbuild) {
		fprintf(fp,
			"package() {\n"
			"  if declare -F "
			"_packinstall_original_package "
			">/dev/null 2>&1; then\n"
			"    _packinstall_original_package\n"
			"  else\n");

		append_generated_package(fp,
				       system,
				       abi);

		fprintf(fp,
			"  fi\n"
			"  packinstall_lfs_pkginfo\n"
			"}\n");
	} else {
		append_generated_package(fp,
					system,
					abi);

		/*
		 * Rename generated implementation and wrap it.
		 */
		fprintf(fp,
			"eval \"$(declare -f package | "
			"sed '1s/^package /_packinstall_generated_package /')\"\n"
			"package() {\n"
			"  _packinstall_generated_package\n"
			"  packinstall_lfs_pkginfo\n"
			"}\n");
	}

	append_pkginfo_function(fp,
				name,
				version,
				release,
				abi);

	fclose(fp);
}

/* ------------------------------------------------------------------------- */
/* Builder account                                                           */
/* ------------------------------------------------------------------------- */

static int passwd_uid_in_use(uid_t uid)
{
	struct passwd *pw;

	setpwent();

	while ((pw = getpwent()) != NULL) {
		if (pw->pw_uid == uid) {
			endpwent();
			return 1;
		}
	}

	endpwent();

	return 0;
}

static int group_gid_in_use(gid_t gid)
{
	struct group *gr;

	setgrent();

	while ((gr = getgrent()) != NULL) {
		if (gr->gr_gid == gid) {
			endgrent();
			return 1;
		}
	}

	endgrent();

	return 0;
}

static uid_t choose_builder_uid(void)
{
	uid_t uid;

	for (uid = 900; uid < 65000; ++uid) {
		if (!passwd_uid_in_use(uid))
			return uid;
	}

	die("cannot find unused UID for builder");
	return 0;
}

static gid_t choose_builder_gid(void)
{
	gid_t gid;

	for (gid = 900; gid < 65000; ++gid) {
		if (!group_gid_in_use(gid))
			return gid;
	}

	die("cannot find unused GID for builder");
	return 0;
}

static void append_line_file(const char *path,
			     const char *line,
			     mode_t mode)
{
	int fd;
	const char *p;
	size_t remaining;

	fd = open(path,
		  O_WRONLY | O_APPEND | O_CREAT,
		  mode);

	if (fd < 0)
		die("cannot open %s: %s",
		    path,
		    strerror(errno));

	p = line;
	remaining = strlen(line);

	while (remaining) {
		ssize_t n =
			write(fd,
			      p,
			      remaining);

		if (n < 0) {
			if (errno == EINTR)
				continue;

			close(fd);

			die("cannot write %s: %s",
			    path,
			    strerror(errno));
		}

		p += n;
		remaining -= (size_t)n;
	}

	close(fd);
}

static void create_builder_from_files(void)
{
	uid_t uid;
	gid_t gid;

	char group_line[256];
	char passwd_line[512];

	struct group *existing_group;

	if (geteuid() != 0)
		die("builder creation requires root");

	if (getpwnam(BUILDER_USER))
		return;

	uid = choose_builder_uid();

	existing_group = getgrnam(BUILDER_GROUP);

	if (existing_group)
		gid = existing_group->gr_gid;
	else
		gid = choose_builder_gid();

	if (gid == 0)
		die("refusing GID 0 for builder");

	if (!existing_group) {
		snprintf(group_line,
			 sizeof(group_line),
			 "%s:x:%lu:\n",
			 BUILDER_GROUP,
			 (unsigned long)gid);

		append_line_file("/etc/group",
				 group_line,
				 0644);
	}

	{
		const char *shell =
			is_file(BUILDER_SHELL) ?
			BUILDER_SHELL :
			"/bin/sh";

		snprintf(passwd_line,
			 sizeof(passwd_line),
			 "%s:x:%lu:%lu:LFS package builder:%s:%s\n",
			 BUILDER_USER,
			 (unsigned long)uid,
			 (unsigned long)gid,
			 BUILDER_HOME,
			 shell);

		append_line_file("/etc/passwd",
				 passwd_line,
				 0644);
	}

	endpwent();
	endgrent();

	if (!getpwnam(BUILDER_USER))
		die("failed to create builder account");

	ensure_dir(BUILDER_HOME);

	if (chown(BUILDER_HOME,
		  uid,
		  gid) != 0)
		die("cannot chown %s: %s",
		    BUILDER_HOME,
		    strerror(errno));

	if (chmod(BUILDER_HOME,
		  0700) != 0)
		die("cannot chmod %s: %s",
		    BUILDER_HOME,
		    strerror(errno));

	printf("created builder using /etc/passwd and /etc/group "
	       "(uid=%lu gid=%lu)\n",
	       (unsigned long)uid,
	       (unsigned long)gid);
}

static void ensure_builder(void)
{
	struct passwd *pw;
	char cmd[PATH_MAX * 3];

	if (geteuid() != 0)
		return;

	pw = getpwnam(BUILDER_USER);

	if (pw) {
		if (pw->pw_uid == 0)
			die("builder account has UID 0");

		return;
	}

	if (command_exists("/usr/sbin/useradd")) {
		snprintf(cmd,
			 sizeof(cmd),
			 "/usr/sbin/useradd "
			 "--system "
			 "--create-home "
			 "--user-group "
			 "--home-dir %s "
			 "--shell %s "
			 "%s",
			 BUILDER_HOME,
			 BUILDER_SHELL,
			 BUILDER_USER);

		if (run_command(cmd) == 0) {
			endpwent();
			endgrent();

			pw = getpwnam(BUILDER_USER);

			if (pw &&
			    pw->pw_uid != 0) {
				ensure_dir(BUILDER_HOME);
				return;
			}
		}
	}

	if (command_exists("/usr/bin/useradd")) {
		snprintf(cmd,
			 sizeof(cmd),
			 "/usr/bin/useradd "
			 "--system "
			 "--create-home "
			 "--user-group "
			 "--home-dir %s "
			 "--shell %s "
			 "%s",
			 BUILDER_HOME,
			 BUILDER_SHELL,
			 BUILDER_USER);

		if (run_command(cmd) == 0) {
			endpwent();
			endgrent();

			pw = getpwnam(BUILDER_USER);

			if (pw &&
			    pw->pw_uid != 0) {
				ensure_dir(BUILDER_HOME);
				return;
			}
		}
	}

	/*
	 * No useradd:
	 *
	 * append builder entries directly to /etc/group and /etc/passwd.
	 */
	create_builder_from_files();
}

static uid_t builder_uid(void)
{
	struct passwd *pw = getpwnam(BUILDER_USER);

	if (!pw)
		die("builder user does not exist");

	if (pw->pw_uid == 0)
		die("builder UID is 0");

	return pw->pw_uid;
}

static gid_t builder_gid(void)
{
	struct passwd *pw = getpwnam(BUILDER_USER);

	if (!pw)
		die("builder user does not exist");

	if (pw->pw_gid == 0)
		die("builder GID is 0");

	return pw->pw_gid;
}

/* ------------------------------------------------------------------------- */
/* Source tree copy                                                          */
/* ------------------------------------------------------------------------- */

static void copy_source_tree(const char *source,
			     char *destination,
			     size_t destination_size)
{
	char templ[PATH_MAX];
	char qsource[PATH_MAX * 2];
	char qdestination[PATH_MAX * 2];
	char cmd[PATH_MAX * 8];
	const char *base;

	snprintf(templ,
		 sizeof(templ),
		 "/tmp/packinstall-lfs-builder-XXXXXX");

	if (!mkdtemp(templ))
		die("cannot create builder temporary directory: %s",
		    strerror(errno));

	base = basename_const(source);

	if (snprintf(destination,
		     destination_size,
		     "%s/%s",
		     templ,
		     base) >= (int)destination_size)
		die("builder source path too long");

	shell_quote(source,
		    qsource,
		    sizeof(qsource));

	shell_quote(destination,
		    qdestination,
		    sizeof(qdestination));

	snprintf(cmd,
		 sizeof(cmd),
		 "cp -a %s %s",
		 qsource,
		 qdestination);

	if (run_command(cmd) != 0)
		die("failed to copy source tree");

	snprintf(cmd,
		 sizeof(cmd),
		 "chown -R %lu:%lu %s",
		 (unsigned long)builder_uid(),
		 (unsigned long)builder_gid(),
		 qdestination);

	if (run_command(cmd) != 0)
		die("failed to chown source tree");

	snprintf(cmd,
		 sizeof(cmd),
		 "chmod 700 %s",
		 qdestination);

	if (run_command(cmd) != 0)
		die("failed to chmod builder source tree");
}

static void remove_tree(const char *path)
{
	char qpath[PATH_MAX * 2];
	char cmd[PATH_MAX * 3];

	shell_quote(path,
		    qpath,
		    sizeof(qpath));

	snprintf(cmd,
		 sizeof(cmd),
		 "rm -rf -- %s",
		 qpath);

	(void)run_command(cmd);
}

/* ------------------------------------------------------------------------- */
/* makepkg execution                                                         */
/* ------------------------------------------------------------------------- */

static void execute_makepkg(const char *workdir,
			    const char *wrapper,
			    const char *output,
			    int skip_checks,
			    int no_build)
{
	char qworkdir[PATH_MAX * 2];
	char qwrapper[PATH_MAX * 2];
	char qoutput[PATH_MAX * 2];
	char command[PATH_MAX * 8];

	shell_quote(workdir,
		    qworkdir,
		    sizeof(qworkdir));

	shell_quote(wrapper,
		    qwrapper,
		    sizeof(qwrapper));

	shell_quote(output,
		    qoutput,
		    sizeof(qoutput));

	snprintf(command,
		 sizeof(command),
		 "cd %s && "
		 "PKGEXT='%s' "
		 "PKGDEST=%s "
		 "makepkg "
		 "-f "
		 "--nodeps "
		 "--noextract "
		 "-p %s "
		 "%s",
		 qworkdir,
		 SUFFIX,
		 qoutput,
		 qwrapper,
		 (skip_checks || no_build) ?
		 "--nocheck" :
		 "");

	/*
	 * Already non-root.
	 */
	if (geteuid() != 0) {
		if (run_command(command) != 0)
			die("makepkg failed");

		return;
	}

	/*
	 * Root -> builder.
	 */
	{
		struct passwd *pw = getpwnam(BUILDER_USER);

		if (!pw)
			die("builder user is missing");

		/*
		 * runuser
		 */
		if (command_exists("/usr/sbin/runuser")) {
			char qcommand[PATH_MAX * 9];
			char qhome[PATH_MAX * 2];
			char run_cmd[PATH_MAX * 12];

			shell_quote(command,
				    qcommand,
				    sizeof(qcommand));

			shell_quote(pw->pw_dir,
				    qhome,
				    sizeof(qhome));

			snprintf(run_cmd,
				 sizeof(run_cmd),
				 "runuser -u %s -- "
				 "env "
				 "HOME=%s "
				 "USER=%s "
				 "LOGNAME=%s "
				 "PATH=/usr/bin:/usr/sbin:/bin:/sbin "
				 "bash -c %s",
				 BUILDER_USER,
				 qhome,
				 BUILDER_USER,
				 BUILDER_USER,
				 qcommand);

			if (run_command(run_cmd) != 0)
				die("makepkg failed as builder");

			return;
		}

		/*
		 * su
		 */
		if (command_exists("/bin/su")) {
			char qcommand[PATH_MAX * 9];
			char run_cmd[PATH_MAX * 10];

			shell_quote(command,
				    qcommand,
				    sizeof(qcommand));

			snprintf(run_cmd,
				 sizeof(run_cmd),
				 "su -s /bin/bash %s -c %s",
				 BUILDER_USER,
				 qcommand);

			if (run_command(run_cmd) != 0)
				die("makepkg failed as builder");

			return;
		}

		if (command_exists("/usr/bin/su")) {
			char qcommand[PATH_MAX * 9];
			char run_cmd[PATH_MAX * 10];

			shell_quote(command,
				    qcommand,
				    sizeof(qcommand));

			snprintf(run_cmd,
				 sizeof(run_cmd),
				 "/usr/bin/su -s /bin/bash "
				 "%s -c %s",
				 BUILDER_USER,
				 qcommand);

			if (run_command(run_cmd) != 0)
				die("makepkg failed as builder");

			return;
		}

		/*
		 * Minimal LFS fallback:
		 * fork, initgroups, setgid, setuid, exec.
		 */
		{
			pid_t pid;
			int status;

			pid = fork();

			if (pid < 0)
				die("fork failed: %s",
				    strerror(errno));

			if (pid == 0) {
				if (initgroups(BUILDER_USER,
					       pw->pw_gid) != 0)
					_exit(126);

				if (setgid(pw->pw_gid) != 0)
					_exit(126);

				if (setuid(pw->pw_uid) != 0)
					_exit(126);

				execl("/bin/bash",
				      "bash",
				      "-c",
				      command,
				      (char *)NULL);

				_exit(127);
			}

			if (waitpid(pid,
				     &status,
				     0) < 0)
				die("waitpid failed: %s",
				    strerror(errno));

			if (!WIFEXITED(status) ||
			    WEXITSTATUS(status) != 0)
				die("makepkg failed as builder");
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Modern create                                                             */
/* ------------------------------------------------------------------------- */

static void modern_create(const char *source_dir,
			  const char *pkgbuild_path,
			  const char *system_override,
			  const struct abi_profile *abi,
			  const char *output,
			  int skip_checks,
			  int no_build)
{
	char name[256];
	char version[256];
	char release[256];

	char workdir[PATH_MAX];
	char temp[PATH_MAX];
	char wrapper[PATH_MAX];
	char output_dir[PATH_MAX];

	char system_buffer[128];
	const char *system;

	int have_pkgbuid;
	int root_build;

	memset(name, 0, sizeof(name));
	memset(version, 0, sizeof(version));
	memset(release, 0, sizeof(release));

	have_pkgbuid = is_file(pkgbuild_path);

	if (have_pkgbuid) {
		read_srcinfo(source_dir,
			     name,
			     sizeof(name),
			     version,
			     sizeof(version),
			     release,
			     sizeof(release));

		system = normalize_build_system(system_override,
						source_dir);
	} else {
		system = normalize_build_system(system_override,
						source_dir);

		if (!system)
			die("no PKGBUILD and no supported build system found");

		detect_metadata(source_dir,
				system,
				name,
				sizeof(name),
				version,
				sizeof(version),
				release,
				sizeof(release));
	}

	if (!name[0])
		die("could not determine package name");

	if (!version[0])
		die("could not determine package version");

	if (!release[0])
		strcpy(release, "1");

	snprintf(system_buffer,
		 sizeof(system_buffer),
		 "%s",
		 system);

	system = system_buffer;

	root_build = geteuid() == 0;

	/*
	 * Root packaging gets a private builder-owned source tree.
	 */
	if (root_build) {
		copy_source_tree(source_dir,
				 workdir,
				 sizeof(workdir));
	} else {
		snprintf(workdir,
			 sizeof(workdir),
			 "%s",
			 source_dir);
	}

	snprintf(temp,
		 sizeof(temp),
		 "%s/.packinstall-lfs.%ld",
		 workdir,
		 (long)getpid());

	if (mkdir(temp, 0700) != 0)
		die("cannot create package temporary directory: %s",
		    strerror(errno));

	if (root_build) {
		if (chown(temp,
			  builder_uid(),
			  builder_gid()) != 0)
			die("cannot chown package temporary directory");
	}

	snprintf(wrapper,
		 sizeof(wrapper),
		 "%s/PKGBUILD",
		 temp);

	/*
	 * Output.
	 */
	if (output) {
		absolute_path(output,
			      output_dir,
			      sizeof(output_dir));

		if (!is_directory(output_dir))
			ensure_dir(output_dir);
	} else if (root_build) {
		snprintf(output_dir,
			 sizeof(output_dir),
			 "%s/.packinstall-output-%ld",
			 workdir,
			 (long)getpid());

		if (mkdir(output_dir,
			  0755) != 0)
			die("cannot create package output directory");

		if (chown(output_dir,
			  builder_uid(),
			  builder_gid()) != 0)
			die("cannot chown package output directory");
	} else {
		snprintf(output_dir,
			 sizeof(output_dir),
			 "%s",
			 source_dir);
	}

	if (root_build) {
		if (chown(output_dir,
			  builder_uid(),
			  builder_gid()) != 0)
			die("cannot chown output directory");
	}

	/*
	 * If the original source has PKGBUILD, point the temporary PKGBUILD
	 * to its copied version when root is used.
	 */
	{
		char copied_pkgbuild[PATH_MAX];
		const char *real_pkgbuild = pkgbuild_path;

		if (root_build) {
			snprintf(copied_pkgbuild,
				 sizeof(copied_pkgbuild),
				 "%s/%s",
				 workdir,
				 basename_const(pkgbuild_path));

			if (!is_file(copied_pkgbuild))
				real_pkgbuild = "";
			else
				real_pkgbuild = copied_pkgbuild;
		}

		/*
		 * For source directories without PKGBUILD, create_generated_pkgbuid
		 * does not source anything.
		 */
		create_generated_pkgbuid(
			workdir,
			real_pkgbuild,
			temp,
			system,
			abi,
			name,
			version,
			release,
			have_pkgbuid,
			skip_checks,
			no_build);
	}

	execute_makepkg(
		temp,
		wrapper,
		output_dir,
		skip_checks,
		no_build);

	unlink(wrapper);
	rmdir(temp);

	/*
	 * Root/no-output:
	 *
	 * copy package back to the original source directory.
	 */
	if (root_build && !output) {
		char qout[PATH_MAX * 2];
		char qsource[PATH_MAX * 2];
		char cmd[PATH_MAX * 6];

		shell_quote(output_dir,
			    qout,
			    sizeof(qout));

		shell_quote(source_dir,
			    qsource,
			    sizeof(qsource));

		snprintf(cmd,
			 sizeof(cmd),
			 "cp -a %s/*.kuzpkg.tar.zst %s/ 2>/dev/null",
			 qout,
			 qsource);

		if (run_command(cmd) != 0)
			warnx("could not copy generated package back to %s",
			      source_dir);

		remove_tree(output_dir);
		remove_tree(workdir);
	}

	printf("packaged %s-%s-%s [%s] using %s\n",
	       name,
	       version,
	       release,
	       abi->name,
	       system);
}

/* ------------------------------------------------------------------------- */
/* Legacy create                                                             */
/* ------------------------------------------------------------------------- */

static void create_legacy(const char *stage,
			  const char *name,
			  const char *version,
			  const char *output)
{
	struct stat st;

	char qstage[PATH_MAX * 2];
	char qoutput[PATH_MAX * 2];
	char cmd[PATH_MAX * 5];
	char pkginfo[PATH_MAX];

	if (stat(stage,
		 &st) != 0 ||
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

	snprintf(pkginfo,
		 sizeof(pkginfo),
		 "%s/%s",
		 stage,
		 PKGINFO_NAME);

	{
		FILE *fp = fopen(pkginfo,
				 "w");

		if (!fp)
			die("cannot create %s: %s",
			    pkginfo,
			    strerror(errno));

		fprintf(fp,
			"format=packinstall-lfs/%s\n"
			"pkgname=%s\n"
			"pkgver=%s\n"
			"pkgrel=1\n"
			"abi=native\n"
			"libdir=/usr/lib\n"
			"machine=native\n",
			VERSION,
			name,
			version);

		fclose(fp);
	}

	shell_quote(stage,
		    qstage,
		    sizeof(qstage));

	shell_quote(output,
		    qoutput,
		    sizeof(qoutput));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -C %s -cf %s .",
		 qstage,
		 qoutput);

	if (run_command(cmd) != 0) {
		unlink(pkginfo);
		die("tar failed");
	}

	unlink(pkginfo);

	printf("created legacy package %s\n",
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
		 "tar --zstd -xOf %s ./%s 2>/dev/null",
		 qpackage,
		 PKGINFO_NAME);

	if (run_command(cmd) != 0)
		die("%s has no PKGINFO",
		    package);
}

/* ------------------------------------------------------------------------- */
/* install                                                                   */
/* ------------------------------------------------------------------------- */

static void install_package(const char *package)
{
	char qpackage[PATH_MAX * 2];
	char qroot[PATH_MAX * 2];
	char qtmp[PATH_MAX * 2];

	char cmd[PATH_MAX * 5];
	char tmp[PATH_MAX];

	char name[256] = {0};
	char version[256] = {0};
	char release[256] = {0};
	char abi[128] = {0};

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
		 "/tmp/packinstall-lfs-pkginfo-%ld",
		 (long)getpid());

	shell_quote(tmp,
		    qtmp,
		    sizeof(qtmp));

	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -xOf %s ./%s > %s 2>/dev/null",
		 qpackage,
		 PKGINFO_NAME,
		 qtmp);

	if (run_command(cmd) != 0)
		die("package does not contain PKGINFO");

	fp = fopen(tmp,
		   "r");

	if (!fp)
		die("cannot read PKGINFO");

	while (fgets(line,
		     sizeof(line),
		     fp)) {
		if (sscanf(line,
			   "pkgname=%255s",
			   name) == 1)
			continue;

		if (sscanf(line,
			   "pkgver=%255s",
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

	if (!name[0] ||
	    !version[0])
		die("PKGINFO is missing pkgname/pkgver");

	if (!release[0])
		strcpy(release, "1");

	if (!abi[0])
		strcpy(abi, "native");

	if (snprintf(db,
		     sizeof(db),
		     "%s%s/%s/%s/%s",
		     root_dir,
		     DB_REL,
		     name,
		     version,
		     abi) >= (int)sizeof(db))
		die("package database path too long");

	if (snprintf(files,
		     sizeof(files),
		     "%s/files",
		     db) >= (int)sizeof(files))
		die("package database path too long");

	ensure_dir(db);

	/*
	 * PKGINFO is package metadata, not a target-system file.
	 */
	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -xpf %s "
		 "-C %s "
		 "--exclude=./%s "
		 "--no-same-owner "
		 "--no-same-permissions",
		 qpackage,
		 qroot,
		 PKGINFO_NAME);

	if (run_command(cmd) != 0)
		die("failed to install %s",
		    package);

	/*
	 * Create an installation manifest.
	 */
	snprintf(cmd,
		 sizeof(cmd),
		 "tar --zstd -tf %s "
		 "| sed '/^%s$/d' > %s",
		 qpackage,
		 PKGINFO_NAME,
		 qtmp);

	if (run_command(cmd) == 0) {
		if (rename(tmp,
			   files) != 0)
			warnx("cannot save package manifest: %s",
			      strerror(errno));
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
		die("package database path too long");

	if (snprintf(files,
		     sizeof(files),
		     "%s/files",
		     db) >= (int)sizeof(files))
		die("package database path too long");

	fp = fopen(files,
		   "r");

	if (!fp)
		die("package %s-%s [%s] is not installed",
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

		snprintf(full,
			 sizeof(full),
			 "%s/%s",
			 root_dir,
			 path);

		if (lstat(full,
			  &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode))
			(void)rmdir(full);
		else if (unlink(full) != 0 &&
			 errno != ENOENT) {
			fprintf(stderr,
				"packinstall-lfs: cannot remove %s: %s\n",
				full,
				strerror(errno));
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
		die("package database path too long");

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
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	const char *command;
	const char *root;

	if (argc < 2) {
		usage();
		return EXIT_FAILURE;
	}

	command = argv[1];

	if (!strcmp(command, "--version") ||
	    !strcmp(command, "-V")) {
		printf("packinstall-lfs %s\n",
		       VERSION);
		return EXIT_SUCCESS;
	}

	if (!strcmp(command, "--help") ||
	    !strcmp(command, "-h")) {
		usage();
		return EXIT_SUCCESS;
	}

	root = arg_value(argc,
			 argv,
			 "--root");

	if (root)
		root_dir = root;

	/*
	 * Modern create/package path.
	 */
	if (!strcmp(command, "create") ||
	    !strcmp(command, "package")) {
		const char *source_dir;
		const char *requested_pkgbuild;
		const char *system_override;
		const char *abi_name;
		const char *output;

		const struct abi_profile *abi;

		char pkgbuild[PATH_MAX];

		resolve_source_dir(argc,
				   argv,
				   pkgbuild,
				   sizeof(pkgbuild));

		source_dir =
			pkgbuild;

		resolve_pkgbuild(
			source_dir,
			arg_value(argc,
				  argv,
				  "--buildscript"),
			pkgbuild,
			sizeof(pkgbuild));

		requested_pkgbuild =
			arg_value(argc,
				  argv,
				  "--buildscript");

		(void)requested_pkgbuild;

		/*
		 * A PKGBUILD is optional now.
		 *
		 * No error if it doesn't exist.
		 */
		if (!is_file(pkgbuild)) {
			system_override =
				arg_value(argc,
					  argv,
					  "--build-system");

			if (system_override &&
			    strcmp(system_override, "auto") != 0) {
				if (!normalize_build_system(
					system_override,
					source_dir)) {
					die("unsupported build system: %s",
					    system_override);
				}
			}
		}

		abi_name =
			arg_value(argc,
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

		system_override =
			arg_value(argc,
				  argv,
				  "--build-system");

		output =
			arg_value(argc,
				  argv,
				  "--output");

		/*
		 * If the source is not a directory, this is an invocation error.
		 */
		if (!is_directory(source_dir))
			die("source directory does not exist: %s",
			    source_dir);

		/*
		 * The source argument may have been:
		 *
		 *   create .
		 *   create /sources/kuzpkg
		 *
		 * but if --dir was used we need that actual directory.
		 */
		{
			const char *explicit_dir =
				arg_value(argc,
					  argv,
					  "--dir");

			if (explicit_dir) {
				absolute_path(
					explicit_dir,
					pkgbuild,
					sizeof(pkgbuild));

				source_dir = pkgbuild;

				resolve_pkgbuild(
					source_dir,
					arg_value(argc,
						  argv,
						  "--buildscript"),
					pkgbuild,
					sizeof(pkgbuild));
			}
		}

		if (geteuid() == 0)
			ensure_builder();

		modern_create(
			source_dir,
			pkgbuild,
			system_override,
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

	/*
	 * Legacy is explicit and NOT the default.
	 */
	if (!strcmp(command,
		    "create-legacy")) {
		if (argc < 5) {
			fprintf(stderr,
				"packinstall-lfs: create-legacy requires:\n"
				"  packinstall-lfs create-legacy "
				"STAGE NAME VERSION [OUTPUT]\n");

			return EXIT_FAILURE;
		}

		create_legacy(
			argv[2],
			argv[3],
			argv[4],
			argc >= 6 ? argv[5] : NULL);

		return EXIT_SUCCESS;
	}

	if (!strcmp(command,
		    "install")) {
		if (argc < 3) {
			usage();
			return EXIT_FAILURE;
		}

		install_package(argv[2]);

		return EXIT_SUCCESS;
	}

	if (!strcmp(command,
		    "remove")) {
		const char *abi;

		if (argc < 4) {
			usage();
			return EXIT_FAILURE;
		}

		abi =
			arg_value(argc,
				  argv,
				  "--abi");

		remove_package(
			argv[2],
			argv[3],
			abi);

		return EXIT_SUCCESS;
	}

	if (!strcmp(command,
		    "list")) {
		list_packages();
		return EXIT_SUCCESS;
	}

	if (!strcmp(command,
		    "info")) {
		if (argc < 3) {
			usage();
			return EXIT_FAILURE;
		}

		package_info(argv[2]);

		return EXIT_SUCCESS;
	}

	fprintf(stderr,
		"packinstall-lfs: unknown command: %s\n\n",
		command);

	usage();

	return EXIT_FAILURE;
}
