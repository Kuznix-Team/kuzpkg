/*
 * packinstall.c - build, stage, package and optionally install a source tree.
 *
 * Supported build systems:
 *   Autotools, Make, Ninja, Meson, CMake, Cargo, Python,
 *   Perl, Go, Ruby/RubyGems and Bundler.
 *
 * Target support:
 *   Linux, historical Linux, NetBSD, FreeBSD, OpenBSD,
 *   DragonFly BSD, Solaris, Darwin and generic targets.
 *
 * Multilib support:
 *   lib32, lib64, libx32,
 *   MIPS o32/n32/n64,
 *   MIPS soft/hard-float,
 *   PowerPC 32/64,
 *   ARM 32/64.
 *
 * Wildcard target support:
 *   *-*-elf
 *   *-*-eabi
 *   riscv64-*-elf
 *   arm-*-eabi
 *   *-*-linux-*
 *   *-*-netbsd*
 *   *-*-freebsd*
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
#include <sys/utsname.h>
#include <unistd.h>

#define REL "1"

/* -------------------------------------------------------------------------
 * Target OS
 * ------------------------------------------------------------------------- */

enum target_os {
	TARGET_OS_UNKNOWN = 0,
	TARGET_OS_LINUX,
	TARGET_OS_NETBSD,
	TARGET_OS_FREEBSD,
	TARGET_OS_OPENBSD,
	TARGET_OS_DRAGONFLY,
	TARGET_OS_SOLARIS,
	TARGET_OS_AIX,
	TARGET_OS_HPUX,
	TARGET_OS_DARWIN,
	TARGET_OS_NONE,
	TARGET_OS_OTHER
};

static const char *target_os_name(enum target_os os);

/* -------------------------------------------------------------------------
 * Architecture database
 * ------------------------------------------------------------------------- */

struct architecture {
	const char *name;
	const char *uname_name;
	const char *triplet;
	const char *family;
	enum target_os os;
	int supported;
	int historical;
};

/*
 * These are package/target identities. Historical architectures are retained
 * so that package metadata can represent old systems even when they are not
 * usable on the current host.
 */
static const struct architecture architectures[] = {
	/* Linux x86 */
	{
		"x86_64", "x86_64", "x86_64-unknown-linux-gnu",
		"x86", TARGET_OS_LINUX, 1, 0
	},
	{
		"i686", "i686", "i686-unknown-linux-gnu",
		"x86", TARGET_OS_LINUX, 1, 0
	},
	{
		"i586", "i586", "i586-unknown-linux-gnu",
		"x86", TARGET_OS_LINUX, 1, 0
	},
	{
		"i486", "i486", "i486-unknown-linux-gnu",
		"x86", TARGET_OS_LINUX, 1, 0
	},
	{
		"i386", "i386", "i386-unknown-linux-gnu",
		"x86", TARGET_OS_LINUX, 1, 0
	},
	{
		"x32", "x86_64", "x86_64-unknown-linux-gnux32",
		"x86", TARGET_OS_LINUX, 1, 0
	},

	/* Linux ARM */
	{
		"aarch64", "aarch64", "aarch64-unknown-linux-gnu",
		"arm", TARGET_OS_LINUX, 1, 0
	},
	{
		"aarch64_be", "aarch64_be", "aarch64_be-unknown-linux-gnu",
		"arm", TARGET_OS_LINUX, 1, 0
	},
	{
		"arm", "arm", "arm-unknown-linux-gnueabi",
		"arm", TARGET_OS_LINUX, 1, 0
	},
	{
		"armel", "arm", "arm-unknown-linux-gnueabi",
		"arm", TARGET_OS_LINUX, 1, 0
	},
	{
		"armhf", "arm", "arm-unknown-linux-gnueabihf",
		"arm", TARGET_OS_LINUX, 1, 0
	},
	{
		"armv6", "arm", "arm-unknown-linux-gnueabi",
		"arm", TARGET_OS_LINUX, 1, 0
	},
	{
		"armv7", "arm", "arm-unknown-linux-gnueabihf",
		"arm", TARGET_OS_LINUX, 1, 0
	},
	{
		"armeb", "armeb", "armeb-unknown-linux-gnueabi",
		"arm", TARGET_OS_LINUX, 1, 0
	},

	/* Linux MIPS */
	{
		"mips", "mips", "mips-unknown-linux-gnu",
		"mips", TARGET_OS_LINUX, 1, 0
	},
	{
		"mipsel", "mipsel", "mipsel-unknown-linux-gnu",
		"mips", TARGET_OS_LINUX, 1, 0
	},
	{
		"mips64", "mips64", "mips64-unknown-linux-gnuabi64",
		"mips", TARGET_OS_LINUX, 1, 0
	},
	{
		"mips64el", "mips64el", "mips64el-unknown-linux-gnuabi64",
		"mips", TARGET_OS_LINUX, 1, 0
	},
	{
		"mips64n32", "mips64", "mips64-unknown-linux-gnuabin32",
		"mips", TARGET_OS_LINUX, 1, 0
	},
	{
		"mips64n32el", "mips64el",
		"mips64el-unknown-linux-gnuabin32",
		"mips", TARGET_OS_LINUX, 1, 0
	},

	/* Linux PowerPC */
	{
		"powerpc", "ppc", "powerpc-unknown-linux-gnu",
		"powerpc", TARGET_OS_LINUX, 1, 0
	},
	{
		"powerpcle", "ppcle", "powerpcle-unknown-linux-gnu",
		"powerpc", TARGET_OS_LINUX, 1, 0
	},
	{
		"powerpc64", "ppc64", "powerpc64-unknown-linux-gnu",
		"powerpc", TARGET_OS_LINUX, 1, 0
	},
	{
		"powerpc64le", "ppc64le",
		"powerpc64le-unknown-linux-gnu",
		"powerpc", TARGET_OS_LINUX, 1, 0
	},

	/* Linux RISC-V */
	{
		"riscv32", "riscv32", "riscv32-unknown-linux-gnu",
		"riscv", TARGET_OS_LINUX, 1, 0
	},
	{
		"riscv32be", "riscv32be",
		"riscv32be-unknown-linux-gnu",
		"riscv", TARGET_OS_LINUX, 1, 0
	},
	{
		"riscv64", "riscv64", "riscv64-unknown-linux-gnu",
		"riscv", TARGET_OS_LINUX, 1, 0
	},
	{
		"riscv64be", "riscv64be",
		"riscv64be-unknown-linux-gnu",
		"riscv", TARGET_OS_LINUX, 1, 0
	},

	/* Linux IBM / SPARC */
	{
		"s390", "s390", "s390-ibm-linux-gnu",
		"s390", TARGET_OS_LINUX, 1, 0
	},
	{
		"s390x", "s390x", "s390x-ibm-linux-gnu",
		"s390", TARGET_OS_LINUX, 1, 0
	},
	{
		"sparc", "sparc", "sparc-unknown-linux-gnu",
		"sparc", TARGET_OS_LINUX, 1, 0
	},
	{
		"sparc64", "sparc64", "sparc64-unknown-linux-gnu",
		"sparc", TARGET_OS_LINUX, 1, 0
	},

	/* Linux other modern */
	{
		"alpha", "alpha", "alpha-unknown-linux-gnu",
		"alpha", TARGET_OS_LINUX, 1, 0
	},
	{
		"arc", "arc", "arc-unknown-linux-gnu",
		"arc", TARGET_OS_LINUX, 1, 0
	},
	{
		"arceb", "arceb", "arceb-unknown-linux-gnu",
		"arc", TARGET_OS_LINUX, 1, 0
	},
	{
		"csky", "csky", "csky-unknown-linux-gnuabiv2",
		"csky", TARGET_OS_LINUX, 1, 0
	},
	{
		"hexagon", "hexagon", "hexagon-unknown-linux-musl",
		"hexagon", TARGET_OS_LINUX, 1, 0
	},
	{
		"loongarch64", "loongarch64",
		"loongarch64-unknown-linux-gnu",
		"loongarch", TARGET_OS_LINUX, 1, 0
	},
	{
		"m68k", "m68k", "m68k-unknown-linux-gnu",
		"m68k", TARGET_OS_LINUX, 1, 0
	},
	{
		"microblaze", "microblaze",
		"microblaze-unknown-linux-gnu",
		"microblaze", TARGET_OS_LINUX, 1, 0
	},
	{
		"microblazeel", "microblazeel",
		"microblazeel-unknown-linux-gnu",
		"microblaze", TARGET_OS_LINUX, 1, 0
	},
	{
		"nios2", "nios2", "nios2-unknown-linux-gnu",
		"nios2", TARGET_OS_LINUX, 1, 0
	},
	{
		"openrisc", "or1k", "or1k-unknown-linux-gnu",
		"openrisc", TARGET_OS_LINUX, 1, 0
	},
	{
		"or1k", "or1k", "or1k-unknown-linux-gnu",
		"openrisc", TARGET_OS_LINUX, 1, 0
	},
	{
		"parisc", "hppa", "hppa-unknown-linux-gnu",
		"parisc", TARGET_OS_LINUX, 1, 0
	},
	{
		"parisc64", "hppa64", "hppa64-unknown-linux-gnu",
		"parisc", TARGET_OS_LINUX, 1, 0
	},
	{
		"sh", "sh", "sh-unknown-linux-gnu",
		"sh", TARGET_OS_LINUX, 1, 0
	},
	{
		"sh4", "sh4", "sh4-unknown-linux-gnu",
		"sh", TARGET_OS_LINUX, 1, 0
	},
	{
		"sh4eb", "sh4eb", "sh4eb-unknown-linux-gnu",
		"sh", TARGET_OS_LINUX, 1, 0
	},
	{
		"xtensa", "xtensa", "xtensa-unknown-linux-gnu",
		"xtensa", TARGET_OS_LINUX, 1, 0
	},
	{
		"xtensaeb", "xtensa", "xtensaeb-unknown-linux-gnu",
		"xtensa", TARGET_OS_LINUX, 1, 0
	},

	/* Historical Linux */
	{
		"ia64", "ia64", "ia64-unknown-linux-gnu",
		"ia64", TARGET_OS_LINUX, 0, 1
	},
	{
		"cris", "cris", "cris-unknown-linux-gnu",
		"cris", TARGET_OS_LINUX, 0, 1
	},
	{
		"frv", "frv", "frv-unknown-linux-gnu",
		"frv", TARGET_OS_LINUX, 0, 1
	},
	{
		"h8300", "h8300", "h8300-unknown-linux-gnu",
		"h8300", TARGET_OS_LINUX, 0, 1
	},
	{
		"m32r", "m32r", "m32r-unknown-linux-gnu",
		"m32r", TARGET_OS_LINUX, 0, 1
	},
	{
		"mn10300", "mn10300", "mn10300-unknown-linux-gnu",
		"mn10300", TARGET_OS_LINUX, 0, 1
	},
	{
		"metag", "metag", "metag-unknown-linux-gnu",
		"metag", TARGET_OS_LINUX, 0, 1
	},
	{
		"blackfin", "bfin", "bfin-unknown-linux-gnu",
		"blackfin", TARGET_OS_LINUX, 0, 1
	},
	{
		"tile", "tile", "tile-unknown-linux-gnu",
		"tile", TARGET_OS_LINUX, 0, 1
	},
	{
		"avr32", "avr32", "avr32-unknown-linux-gnu",
		"avr32", TARGET_OS_LINUX, 0, 1
	},
	{
		"c6x", "tic6x", "tic6x-unknown-linux-gnu",
		"c6x", TARGET_OS_LINUX, 0, 1
	},

	/* NetBSD */
	{
		"netbsd-amd64", "x86_64",
		"x86_64-unknown-netbsd",
		"x86", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-i386", "i386",
		"i386-unknown-netbsd",
		"x86", TARGET_OS_NETBSD, 0, 1
	},
	{
		"netbsd-alpha", "alpha",
		"alpha-unknown-netbsd",
		"alpha", TARGET_OS_NETBSD, 0, 1
	},
	{
		"netbsd-arm", "arm",
		"arm-unknown-netbsd",
		"arm", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-earm", "arm",
		"earm-unknown-netbsd",
		"arm", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-hppa", "hppa",
		"hppa-unknown-netbsd",
		"parisc", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-m68k", "m68k",
		"m68k-unknown-netbsd",
		"m68k", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-mipseb", "mipseb",
		"mipseb-unknown-netbsd",
		"mips", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-mipsel", "mipsel",
		"mipsel-unknown-netbsd",
		"mips", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-powerpc", "powerpc",
		"powerpc-unknown-netbsd",
		"powerpc", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-sh3eb", "sh3eb",
		"sh3eb-unknown-netbsd",
		"sh", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-sh3el", "sh3el",
		"sh3el-unknown-netbsd",
		"sh", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-sparc", "sparc",
		"sparc-unknown-netbsd",
		"sparc", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-sparc64", "sparc64",
		"sparc64-unknown-netbsd",
		"sparc", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-vax", "vax",
		"vax-unknown-netbsd",
		"vax", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-evbarm", "arm",
		"arm-unknown-netbsd",
		"arm", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-evbmips", "mips",
		"mips-unknown-netbsd",
		"mips", TARGET_OS_NETBSD, 1, 0
	},
	{
		"netbsd-evbppc", "powerpc",
		"powerpc-unknown-netbsd",
		"powerpc", TARGET_OS_NETBSD, 1, 0
	},

	/* FreeBSD */
	{
		"freebsd-amd64", "amd64",
		"x86_64-unknown-freebsd",
		"x86", TARGET_OS_FREEBSD, 1, 0
	},
	{
		"freebsd-aarch64", "aarch64",
		"aarch64-unknown-freebsd",
		"arm", TARGET_OS_FREEBSD, 1, 0
	},
	{
		"freebsd-armv7", "armv7",
		"armv7-unknown-freebsd",
		"arm", TARGET_OS_FREEBSD, 1, 0
	},
	{
		"freebsd-armv6", "armv6",
		"armv6-unknown-freebsd",
		"arm", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-i386", "i386",
		"i386-unknown-freebsd",
		"x86", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-powerpc", "powerpc",
		"powerpc-unknown-freebsd",
		"powerpc", TARGET_OS_FREEBSD, 1, 0
	},
	{
		"freebsd-powerpc64", "powerpc64",
		"powerpc64-unknown-freebsd",
		"powerpc", TARGET_OS_FREEBSD, 1, 0
	},
	{
		"freebsd-powerpc64le", "powerpc64le",
		"powerpc64le-unknown-freebsd",
		"powerpc", TARGET_OS_FREEBSD, 1, 0
	},
	{
		"freebsd-powerpcspe", "powerpcspe",
		"powerpcspe-unknown-freebsd",
		"powerpc", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-riscv64", "riscv64",
		"riscv64-unknown-freebsd",
		"riscv", TARGET_OS_FREEBSD, 1, 0
	},
	{
		"freebsd-mips", "mips",
		"mips-unknown-freebsd",
		"mips", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-mipsel", "mipsel",
		"mipsel-unknown-freebsd",
		"mips", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-mips64", "mips64",
		"mips64-unknown-freebsd",
		"mips", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-mips64el", "mips64el",
		"mips64el-unknown-freebsd",
		"mips", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-mipsn32", "mipsn32",
		"mipsn32-unknown-freebsd",
		"mips", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-sparc64", "sparc64",
		"sparc64-unknown-freebsd",
		"sparc", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-alpha", "alpha",
		"alpha-unknown-freebsd",
		"alpha", TARGET_OS_FREEBSD, 0, 1
	},
	{
		"freebsd-ia64", "ia64",
		"ia64-unknown-freebsd",
		"ia64", TARGET_OS_FREEBSD, 0, 1
	},

	/* OpenBSD */
	{
		"openbsd-amd64", "amd64",
		"x86_64-unknown-openbsd",
		"x86", TARGET_OS_OPENBSD, 1, 0
	},
	{
		"openbsd-i386", "i386",
		"i386-unknown-openbsd",
		"x86", TARGET_OS_OPENBSD, 1, 0
	},
	{
		"openbsd-aarch64", "arm64",
		"aarch64-unknown-openbsd",
		"arm", TARGET_OS_OPENBSD, 1, 0
	},
	{
		"openbsd-armv7", "armv7",
		"armv7-unknown-openbsd",
		"arm", TARGET_OS_OPENBSD, 1, 0
	},
	{
		"openbsd-mips64", "mips64",
		"mips64-unknown-openbsd",
		"mips", TARGET_OS_OPENBSD, 0, 1
	},
	{
		"openbsd-powerpc64", "powerpc64",
		"powerpc64-unknown-openbsd",
		"powerpc", TARGET_OS_OPENBSD, 0, 1
	},

	/* DragonFly */
	{
		"dragonfly-amd64", "x86_64",
		"x86_64-unknown-dragonfly",
		"x86", TARGET_OS_DRAGONFLY, 1, 0
	},

	/* Solaris */
	{
		"solaris-amd64", "i86pc",
		"x86_64-pc-solaris2",
		"x86", TARGET_OS_SOLARIS, 1, 0
	},
	{
		"solaris-sparc64", "sparc64",
		"sparc64-sun-solaris2",
		"sparc", TARGET_OS_SOLARIS, 1, 0
	},

	/* Darwin */
	{
		"darwin-arm64", "arm64",
		"aarch64-apple-darwin",
		"arm", TARGET_OS_DARWIN, 1, 0
	},
	{
		"darwin-x86_64", "x86_64",
		"x86_64-apple-darwin",
		"x86", TARGET_OS_DARWIN, 1, 0
	},

	{
		NULL,
		NULL,
		NULL,
		NULL,
		TARGET_OS_UNKNOWN,
		0,
		0
	}
};

/* -------------------------------------------------------------------------
 * Multilib
 * ------------------------------------------------------------------------- */

enum multilib_kind {
	MULTILIB_NONE = 0,
	MULTILIB_LIB32,
	MULTILIB_LIB64,
	MULTILIB_LIBX32,
	MULTILIB_MIPS_O32,
	MULTILIB_MIPS_N32,
	MULTILIB_MIPS_N64,
	MULTILIB_MIPS_SOFTFLOAT,
	MULTILIB_MIPS_HARDFLOAT,
	MULTILIB_PPC32,
	MULTILIB_PPC64,
	MULTILIB_PPC32_ABI,
	MULTILIB_PPC64_ABI,
	MULTILIB_ARM32,
	MULTILIB_ARM64,
	MULTILIB_OTHER
};

struct multilib_variant {
	const char *name;
	const char *directory;
	const char *flags;
	const char *abi;
	enum multilib_kind kind;
};

static const struct multilib_variant multilib_variants[] = {
	{
		"lib32", "lib32", "-m32", "i386",
		MULTILIB_LIB32
	},
	{
		"lib64", "lib64", "-m64", "x86_64",
		MULTILIB_LIB64
	},
	{
		"libx32", "libx32", "-mx32", "x32",
		MULTILIB_LIBX32
	},

	{
		"mips-o32", "mips32", "-mabi=32", "o32",
		MULTILIB_MIPS_O32
	},
	{
		"mips-n32", "mipsn32", "-mabi=n32", "n32",
		MULTILIB_MIPS_N32
	},
	{
		"mips-n64", "mips64", "-mabi=64", "n64",
		MULTILIB_MIPS_N64
	},
	{
		"mips-softfloat", "mips-softfloat",
		"-msoft-float", "softfloat",
		MULTILIB_MIPS_SOFTFLOAT
	},
	{
		"mips-hardfloat", "mips-hardfloat",
		"-mhard-float", "hardfloat",
		MULTILIB_MIPS_HARDFLOAT
	},

	{
		"ppc32", "lib32", "-m32", "32",
		MULTILIB_PPC32
	},
	{
		"ppc64", "lib64", "-m64", "64",
		MULTILIB_PPC64
	},
	{
		"ppc32-abi", "ppc32-abi", "-m32",
		"ppc32", MULTILIB_PPC32_ABI
	},
	{
		"ppc64-abi", "ppc64-abi", "-m64",
		"ppc64", MULTILIB_PPC64_ABI
	},

	{
		"arm32", "arm", "-marm", "32",
		MULTILIB_ARM32
	},
	{
		"arm64", "aarch64", "", "64",
		MULTILIB_ARM64
	},

	{
		NULL, NULL, NULL, NULL,
		MULTILIB_NONE
	}
};

/* -------------------------------------------------------------------------
 * Target spec
 * ------------------------------------------------------------------------- */

struct target_spec {
	char *triplet;
	char *cpu;
	char *vendor;
	char *system;
	char *abi;

	enum target_os os;

	int elf;
	int bare_metal;
	int wildcard;
	int known;
};

/* -------------------------------------------------------------------------
 * Build options
 * ------------------------------------------------------------------------- */

struct build_options {
	const char *arch_name;
	const char *target_pattern;
	const char *build_triplet;
	const char *host_triplet;

	const struct multilib_variant *multilib;

	int do_install;
	int all_multilib;
	int list_architectures;
	int list_multilib;
};

/* =========================================================================
 * Helper implementations
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Wildcard matcher
 * ------------------------------------------------------------------------- */

static int wildcard_match(
	const char *pattern,
	const char *string)
{
	const char *star = NULL;
	const char *mark = NULL;

	if(!pattern || !string)
		return 0;

	while(*string) {

		if(*pattern == '*') {
			star = pattern++;
			mark = string;
			continue;
		}

		if(*pattern == '?' ||
		   *pattern == *string) {

			pattern++;
			string++;

			continue;
		}

		if(star) {

			pattern = star + 1;
			string = ++mark;

			continue;
		}

		return 0;
	}

	while(*pattern == '*')
		pattern++;

	return *pattern == '\0';
}

/* -------------------------------------------------------------------------
 * Shell execution
 * ------------------------------------------------------------------------- */

static char *shell_quote(const char *s)
{
	size_t n = 2;
	char *out;
	char *p;
	const char *q;

	if(!s)
		return NULL;

	for(q = s; *q; q++)
		n += (*q == '\'') ? 4 : 1;

	out = malloc(n + 1);

	if(!out)
		return NULL;

	p = out;

	*p++ = '\'';

	for(q = s; *q; q++) {

		if(*q == '\'') {

			memcpy(p, "'\\''", 4);
			p += 4;

		} else {

			*p++ = *q;
		}
	}

	*p++ = '\'';
	*p = '\0';

	return out;
}

static int run_command(const char *command)
{
	int status;

	if(!command)
		return -1;

	fprintf(
		stderr,
		"packinstall: %s\n",
		command);

	status = system(command);

	if(status == -1) {

		fprintf(
			stderr,
			"packinstall: cannot execute command: %s\n",
			strerror(errno));

		return -1;
	}

	if(WIFEXITED(status))
		return WEXITSTATUS(status);

	if(WIFSIGNALED(status)) {

		fprintf(
			stderr,
			"packinstall: command terminated by signal %d\n",
			WTERMSIG(status));

		return 128;
	}

	return 128;
}

/* -------------------------------------------------------------------------
 * Filesystem helpers
 * ------------------------------------------------------------------------- */

static int exists(const char *path)
{
	return path && access(path, F_OK) == 0;
}

static int is_dir(const char *path)
{
	struct stat st;

	if(!path)
		return 0;

	return stat(path, &st) == 0 &&
	       S_ISDIR(st.st_mode);
}

static const char *base_name(const char *path)
{
	const char *p;

	if(!path)
		return "";

	p = strrchr(path, '/');

	return p ? p + 1 : path;
}

/* -------------------------------------------------------------------------
 * Staging cleanup
 * ------------------------------------------------------------------------- */

static int clean_stage(const char *stage)
{
	char *q;
	char *cmd;
	int ret;

	if(!stage || !*stage)
		return -1;

	if(strcmp(stage, "/") == 0 ||
	   strcmp(stage, ".") == 0 ||
	   strcmp(stage, "..") == 0) {

		fprintf(
			stderr,
			"packinstall: refusing to clean unsafe "
			"staging directory '%s'\n",
			stage);

		return -1;
	}

	q = shell_quote(stage);

	if(!q)
		return -1;

	if(asprintf(
		&cmd,
		"rm -rf -- %s && mkdir -p -- %s",
		q,
		q) < 0) {

		free(q);

		return -1;
	}

	ret = run_command(cmd);

	free(q);
	free(cmd);

	return ret;
}

/* -------------------------------------------------------------------------
 * OS helpers
 * ------------------------------------------------------------------------- */

static const char *target_os_name(
	enum target_os os)
{
	switch(os) {
		case TARGET_OS_LINUX:
			return "linux";

		case TARGET_OS_NETBSD:
			return "netbsd";

		case TARGET_OS_FREEBSD:
			return "freebsd";

		case TARGET_OS_OPENBSD:
			return "openbsd";

		case TARGET_OS_DRAGONFLY:
			return "dragonfly";

		case TARGET_OS_SOLARIS:
			return "solaris";

		case TARGET_OS_AIX:
			return "aix";

		case TARGET_OS_HPUX:
			return "hpux";

		case TARGET_OS_DARWIN:
			return "darwin";

		case TARGET_OS_NONE:
			return "none";

		case TARGET_OS_OTHER:
			return "other";

		default:
			return "unknown";
	}
}

/* -------------------------------------------------------------------------
 * Architecture lookup
 * ------------------------------------------------------------------------- */

static const struct architecture *find_architecture(
	const char *name)
{
	size_t i;

	if(!name)
		return NULL;

	for(i = 0; architectures[i].name; i++) {

		if(strcmp(
			architectures[i].name,
			name) == 0)
			return &architectures[i];
	}

	return NULL;
}

static const struct architecture *find_architecture_triplet(
	const char *triplet)
{
	size_t i;

	if(!triplet)
		return NULL;

	for(i = 0; architectures[i].name; i++) {

		if(strcmp(
			architectures[i].triplet,
			triplet) == 0)
			return &architectures[i];
	}

	return NULL;
}

static const struct architecture *
detect_host_architecture(void)
{
	struct utsname u;
	size_t i;

	if(uname(&u) != 0)
		return NULL;

	for(i = 0; architectures[i].name; i++) {

		if(strcmp(
			architectures[i].uname_name,
			u.machine) != 0)
			continue;

		if(architectures[i].historical)
			continue;

		return &architectures[i];
	}

	return NULL;
}

/* -------------------------------------------------------------------------
 * Target detection
 * ------------------------------------------------------------------------- */

static enum target_os detect_target_os(
	const char *target)
{
	if(!target)
		return TARGET_OS_UNKNOWN;

	if(wildcard_match(
		"*-*-linux-*",
		target) ||
	   wildcard_match(
		"*-*-linux",
		target))
		return TARGET_OS_LINUX;

	if(wildcard_match(
		"*-*-netbsd*",
		target))
		return TARGET_OS_NETBSD;

	if(wildcard_match(
		"*-*-freebsd*",
		target))
		return TARGET_OS_FREEBSD;

	if(wildcard_match(
		"*-*-openbsd*",
		target))
		return TARGET_OS_OPENBSD;

	if(wildcard_match(
		"*-*-dragonfly*",
		target))
		return TARGET_OS_DRAGONFLY;

	if(wildcard_match(
		"*-*-solaris*",
		target))
		return TARGET_OS_SOLARIS;

	if(wildcard_match(
		"*-*-darwin*",
		target))
		return TARGET_OS_DARWIN;

	if(wildcard_match(
		"*-*-none*",
		target) ||
	   wildcard_match(
		"*-*-elf*",
		target) ||
	   wildcard_match(
		"*-*-eabi*",
		target))
		return TARGET_OS_NONE;

	return TARGET_OS_OTHER;
}

static int target_is_elf(
	const char *target)
{
	if(!target)
		return 0;

	if(wildcard_match(
		"*-*-elf",
		target) ||
	   wildcard_match(
		"*-*-elf32",
		target) ||
	   wildcard_match(
		"*-*-elf64",
		target) ||
	   wildcard_match(
		"*-*-eabi",
		target) ||
	   wildcard_match(
		"*-*-eabihf",
		target) ||
	   wildcard_match(
		"*-*-eabisim",
		target))
		return 1;

	if(strstr(target, "-elf") != NULL)
		return 1;

	if(strstr(target, "-eabi") != NULL)
		return 1;

	return 0;
}

static int target_is_bare_metal(
	const char *target)
{
	if(!target)
		return 0;

	if(detect_target_os(target) ==
	   TARGET_OS_NONE)
		return 1;

	if(target_is_elf(target))
		return 1;

	if(wildcard_match(
		"*-*-none",
		target))
		return 1;

	return 0;
}

/* -------------------------------------------------------------------------
 * Target parsing
 * ------------------------------------------------------------------------- */

static int split_target(
	const char *triplet,
	char **cpu,
	char **vendor,
	char **system)
{
	char *copy;
	char *p;
	char *q;

	if(!triplet ||
	   !cpu ||
	   !vendor ||
	   !system)
		return -1;

	*cpu = NULL;
	*vendor = NULL;
	*system = NULL;

	copy = strdup(triplet);

	if(!copy)
		return -1;

	p = strchr(copy, '-');

	if(!p) {
		free(copy);
		return -1;
	}

	*p++ = '\0';

	q = strchr(p, '-');

	if(!q) {
		free(copy);
		return -1;
	}

	*q++ = '\0';

	*cpu = strdup(copy);
	*vendor = strdup(p);
	*system = strdup(q);

	free(copy);

	if(!*cpu ||
	   !*vendor ||
	   !*system) {

		free(*cpu);
		free(*vendor);
		free(*system);

		*cpu = NULL;
		*vendor = NULL;
		*system = NULL;

		return -1;
	}

	return 0;
}

static void free_target(
	struct target_spec *t)
{
	if(!t)
		return;

	free(t->triplet);
	free(t->cpu);
	free(t->vendor);
	free(t->system);
	free(t->abi);

	memset(t, 0, sizeof(*t));
}

static int parse_target(
	const char *triplet,
	struct target_spec *out)
{
	struct target_spec t;

	if(!triplet || !out)
		return -1;

	memset(&t, 0, sizeof(t));

	t.triplet = strdup(triplet);

	if(!t.triplet)
		return -1;

	t.wildcard =
		strchr(triplet, '*') != NULL ||
		strchr(triplet, '?') != NULL;

	t.known =
		find_architecture_triplet(
			triplet) != NULL;

	if(split_target(
		triplet,
		&t.cpu,
		&t.vendor,
		&t.system) != 0) {

		free_target(&t);

		return -1;
	}

	t.os =
		detect_target_os(triplet);

	t.elf =
		target_is_elf(triplet);

	t.bare_metal =
		target_is_bare_metal(triplet);

	t.abi =
		strdup(t.system);

	if(!t.abi) {

		free_target(&t);

		return -1;
	}

	*out = t;

	return 0;
}

static char *resolve_target_pattern(
	const char *pattern)
{
	size_t i;

	if(!pattern)
		return NULL;

	if(!strchr(pattern, '*') &&
	   !strchr(pattern, '?'))
		return strdup(pattern);

	for(i = 0; architectures[i].name; i++) {

		if(wildcard_match(
			pattern,
			architectures[i].triplet))
			return strdup(
				architectures[i].triplet);
	}

	for(i = 0; architectures[i].name; i++) {

		if(wildcard_match(
			pattern,
			architectures[i].name))
			return strdup(
				architectures[i].triplet);
	}

	return NULL;
}

/* -------------------------------------------------------------------------
 * Multilib
 * ------------------------------------------------------------------------- */

static const struct multilib_variant *
find_multilib(const char *name)
{
	size_t i;

	if(!name)
		return NULL;

	for(i = 0; multilib_variants[i].name; i++) {

		if(strcmp(
			multilib_variants[i].name,
			name) == 0)
			return &multilib_variants[i];
	}

	return NULL;
}

/* -------------------------------------------------------------------------
 * Toolchain
 * ------------------------------------------------------------------------- */

static int command_exists(
	const char *command)
{
	char *q;
	char *cmd;
	int ret;

	q = shell_quote(command);

	if(!q)
		return 0;

	if(asprintf(
		&cmd,
		"command -v %s >/dev/null 2>&1",
		q) < 0) {

		free(q);

		return 0;
	}

	ret = system(cmd);

	free(q);
	free(cmd);

	return ret == 0;
}

static int configure_cross_environment(
	const char *target,
	const char *multilib_flags)
{
	char cc[PATH_MAX];
	char cxx[PATH_MAX];
	char ar[PATH_MAX];
	char as[PATH_MAX];
	char ld[PATH_MAX];
	char ranlib[PATH_MAX];
	char strip[PATH_MAX];

	if(target && *target) {

		snprintf(
			cc,
			sizeof(cc),
			"%s-gcc",
			target);

		snprintf(
			cxx,
			sizeof(cxx),
			"%s-g++",
			target);

		snprintf(
			ar,
			sizeof(ar),
			"%s-ar",
			target);

		snprintf(
			as,
			sizeof(as),
			"%s-as",
			target);

		snprintf(
			ld,
			sizeof(ld),
			"%s-ld",
			target);

		snprintf(
			ranlib,
			sizeof(ranlib),
			"%s-ranlib",
			target);

		snprintf(
			strip,
			sizeof(strip),
			"%s-strip",
			target);

		if(command_exists(cc)) {

			if(setenv("CC", cc, 1) != 0)
				return -1;

			if(setenv("CXX", cxx, 1) != 0)
				return -1;

			if(setenv("AR", ar, 1) != 0)
				return -1;

			if(setenv("AS", as, 1) != 0)
				return -1;

			if(setenv("LD", ld, 1) != 0)
				return -1;

			if(setenv("RANLIB", ranlib, 1) != 0)
				return -1;

			if(setenv("STRIP", strip, 1) != 0)
				return -1;
		}
	}

	if(multilib_flags &&
	   *multilib_flags) {

		const char *old_cflags;
		const char *old_cxxflags;
		const char *old_ldflags;

		char *cflags = NULL;
		char *cxxflags = NULL;
		char *ldflags = NULL;

		old_cflags = getenv("CFLAGS");
		old_cxxflags = getenv("CXXFLAGS");
		old_ldflags = getenv("LDFLAGS");

		if(asprintf(
			&cflags,
			"%s%s%s",
			old_cflags ?
				old_cflags : "",
			old_cflags &&
			*old_cflags ?
				" " : "",
			multilib_flags) < 0)
			return -1;

		if(asprintf(
			&cxxflags,
			"%s%s%s",
			old_cxxflags ?
				old_cxxflags : "",
			old_cxxflags &&
			*old_cxxflags ?
				" " : "",
			multilib_flags) < 0) {

			free(cflags);

			return -1;
		}

		if(asprintf(
			&ldflags,
			"%s%s%s",
			old_ldflags ?
				old_ldflags : "",
			old_ldflags &&
			*old_ldflags ?
				" " : "",
			multilib_flags) < 0) {

			free(cflags);
			free(cxxflags);

			return -1;
		}

		if(setenv(
			"CFLAGS",
			cflags,
			1) != 0 ||
		   setenv(
			"CXXFLAGS",
			cxxflags,
			1) != 0 ||
		   setenv(
			"LDFLAGS",
			ldflags,
			1) != 0) {

			free(cflags);
			free(cxxflags);
			free(ldflags);

			return -1;
		}

		free(cflags);
		free(cxxflags);
		free(ldflags);
	}

	return 0;
}

/* -------------------------------------------------------------------------
 * Package name/version
 * ------------------------------------------------------------------------- */

static char *package_version(
	const char *tree)
{
	const char *b;
	const char *p;
	const char *dash;
	char *v;

	b = base_name(tree);

	if(strncmp(
		b,
		"expect",
		6) == 0 ||
	   strncmp(
		b,
		"tcl",
		3) == 0) {

		p = b;

		while(*p &&
		      (*p < '0' ||
		       *p > '9'))
			p++;

		return strdup(
			*p ? p : "0");
	}

	if(strncmp(
		b,
		"unzip",
		5) == 0) {

		p = b + 5;

		if(p[0] &&
		   p[1] &&
		   p[0] >= '0' &&
		   p[0] <= '9' &&
		   p[1] >= '0' &&
		   p[1] <= '9') {

			if(asprintf(
				&v,
				"%c.%c%s",
				p[0],
				p[1],
				p + 2) >= 0)
				return v;
		}
	}

	if(strcmp(
		b,
		"docbook-xml") == 0)
		return strdup("4.5");

	p = strrchr(b, '-');
	dash = strrchr(b, '_');

	if(dash &&
	   (!p || dash > p))
		p = dash;

	if(p &&
	   p[1] &&
	   ((p[1] >= '0' &&
	     p[1] <= '9') ||
	    p[1] == 'v')) {

		p++;

	} else {

		p = b;

		while(*p &&
		      (*p < '0' ||
		       *p > '9'))
			p++;
	}

	v = strdup(
		*p ? p : "0");

	if(!v)
		return NULL;

	for(char *q = v; *q; q++) {

		if(*q == '_')
			*q = '.';
	}

	return v;
}

static char *package_name(
	const char *tree)
{
	const char *b;
	const char *p;
	char *name;

	b = base_name(tree);
	p = b;

	while(*p >= '0' &&
	      *p <= '9')
		p++;

	if(p - b >= 2 &&
	   p - b <= 4 &&
	   *p == '-')
		b = p + 1;

	name = strdup(b);

	if(!name)
		return NULL;

	for(size_t i = 1;
	    name[i];
	    i++) {

		if(name[i] == '-' ||
		   name[i] == '_') {

			const char *q =
				name + i + 1;

			if((*q >= '0' &&
			    *q <= '9') ||
			   *q == 'v') {

				name[i] = '\0';

				break;
			}
		}
	}

	return name;
}

/* -------------------------------------------------------------------------
 * Ruby
 * ------------------------------------------------------------------------- */

static char *find_gemspec(void)
{
	FILE *p;
	char line[PATH_MAX];

	p = popen(
		"find . -maxdepth 1 -type f "
		"-name '*.gemspec' -print -quit",
		"r");

	if(!p)
		return NULL;

	if(!fgets(
		line,
		sizeof(line),
		p)) {

		pclose(p);

		return NULL;
	}

	pclose(p);

	line[
		strcspn(
			line,
			"\n")
	] = '\0';

	if(!line[0])
		return NULL;

	if(line[0] == '.' &&
	   line[1] == '/')
		return strdup(line + 2);

	return strdup(line);
}

static char *find_gem_package(void)
{
	FILE *p;
	char line[PATH_MAX];

	p = popen(
		"find . -maxdepth 1 -type f "
		"-name '*.gem' -print -quit",
		"r");

	if(!p)
		return NULL;

	if(!fgets(
		line,
		sizeof(line),
		p)) {

		pclose(p);

		return NULL;
	}

	pclose(p);

	line[
		strcspn(
			line,
			"\n")
	] = '\0';

	if(!line[0])
		return NULL;

	if(line[0] == '.' &&
	   line[1] == '/')
		return strdup(line + 2);

	return strdup(line);
}

static int build_ruby_gem(
	const char *stage,
	const struct multilib_variant *multilib)
{
	char *qstage = NULL;
	char *gemspec = NULL;
	char *qgemspec = NULL;
	char *gem = NULL;
	char *qgem = NULL;
	char *cmd = NULL;
	int ret = 1;

	(void)multilib;

	qstage =
		shell_quote(stage);

	if(!qstage)
		goto out;

	gemspec =
		find_gemspec();

	if(gemspec) {

		qgemspec =
			shell_quote(
				gemspec);

		if(!qgemspec)
			goto out;

		if(asprintf(
			&cmd,
			"gem build %s",
			qgemspec) < 0)
			goto out;

		ret =
			run_command(cmd);

		free(cmd);
		cmd = NULL;

		if(ret != 0)
			goto out;
	}

	gem =
		find_gem_package();

	if(!gem) {

		fprintf(
			stderr,
			"packinstall: no Ruby .gem package found\n");

		ret = 1;

		goto out;
	}

	qgem =
		shell_quote(gem);

	if(!qgem)
		goto out;

	if(asprintf(
		&cmd,
		"mkdir -p -- %s/usr/lib/ruby/gems "
		"&& mkdir -p -- %s/usr/bin "
		"&& gem install --local %s "
		"--install-dir %s/usr/lib/ruby/gems "
		"--bindir %s/usr/bin "
		"--ignore-dependencies "
		"--no-document "
		"--force",
		qstage,
		qstage,
		qgem,
		qstage,
		qstage) < 0)
		goto out;

	ret =
		run_command(cmd);

out:
	free(cmd);
	free(qstage);
	free(gemspec);
	free(qgemspec);
	free(gem);
	free(qgem);

	return ret;
}

/* -------------------------------------------------------------------------
 * Build project
 * ------------------------------------------------------------------------- */

static int build_project(
	const char *src,
	const char *stage,
	const struct build_options *opts,
	const struct target_spec *target)
{
	char *qstage;
	char *cmd = NULL;
	int ret = -1;

	qstage =
		shell_quote(stage);

	if(!qstage)
		return -1;

	if(configure_cross_environment(
		target ?
			target->triplet :
			NULL,
		opts->multilib ?
			opts->multilib->flags :
			NULL) != 0)
		goto out;

	/* Autotools */
	if(exists("configure")) {

		if(target &&
		   opts->build_triplet &&
		   opts->host_triplet) {

			if(asprintf(
				&cmd,
				"./configure "
				"--build='%s' "
				"--host='%s' "
				"--target='%s' "
				"--prefix=/usr "
				"&& make "
				"&& DESTDIR=%s "
				"make install",
				opts->build_triplet,
				opts->host_triplet,
				target->triplet,
				qstage) < 0)
				goto out;

		} else {

			if(asprintf(
				&cmd,
				"./configure "
				"--prefix=/usr "
				"&& make "
				"&& DESTDIR=%s "
				"make install",
				qstage) < 0)
				goto out;
		}

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Meson */
	if(exists("meson.build")) {

		if(asprintf(
			&cmd,
			"meson setup build "
			"--prefix=/usr "
			"--buildtype=plain "
			"&& meson compile -C build "
			"&& DESTDIR=%s "
			"meson install -C build",
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* CMake */
	if(exists("CMakeLists.txt")) {

		if(asprintf(
			&cmd,
			"cmake -S . -B build "
			"-DCMAKE_BUILD_TYPE=Release "
			"-DCMAKE_INSTALL_PREFIX=/usr "
			"&& cmake --build build "
			"&& DESTDIR=%s "
			"cmake --install build",
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Ninja */
	if(exists("build.ninja")) {

		if(asprintf(
			&cmd,
			"ninja "
			"&& DESTDIR=%s "
			"ninja install",
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Cargo */
	if(exists("Cargo.toml")) {

		if(asprintf(
			&cmd,
			"mkdir -p %s/usr "
			"&& cargo install --path . "
			"--root %s/usr --locked",
			qstage,
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Ruby/RubyGems */
	{
		char *gemspec =
			find_gemspec();

		if(gemspec) {

			free(gemspec);

			ret =
				build_ruby_gem(
					stage,
					opts->multilib);

			if(ret == 0)
				goto out;
		}
	}

	/* Bundler */
	if(exists("Gemfile")) {

		if(asprintf(
			&cmd,
			"mkdir -p "
			"%s/usr/lib/ruby/vendor_ruby "
			"&& bundle config set --local path "
			"%s/usr/lib/ruby/vendor_ruby "
			"&& bundle config set --local "
			"without 'development test' "
			"&& bundle install "
			"--jobs 1 --retry 3",
			qstage,
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Rake */
	if(exists("Rakefile")) {

		if(asprintf(
			&cmd,
			"rake "
			"&& DESTDIR=%s "
			"rake install",
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Python */
	if(exists("pyproject.toml") ||
	   exists("setup.py") ||
	   exists("setup.cfg")) {

		if(asprintf(
			&cmd,
			"python3 -m pip install . "
			"--root %s "
			"--prefix /usr "
			"--no-deps "
			"--no-build-isolation",
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Perl Module::Build */
	if(exists("Build.PL")) {

		if(asprintf(
			&cmd,
			"perl Build.PL "
			"--destdir %s "
			"--install_base /usr "
			"&& ./Build "
			"&& ./Build install",
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Perl MakeMaker */
	if(exists("Makefile.PL")) {

		if(asprintf(
			&cmd,
			"perl Makefile.PL PREFIX=/usr "
			"&& make "
			"&& DESTDIR=%s "
			"make install",
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Go */
	if(exists("go.mod")) {

		if(asprintf(
			&cmd,
			"mkdir -p %s/usr/bin "
			"&& GOBIN=%s/usr/bin "
			"go install ./...",
			qstage,
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	/* Generic Make */
	if(exists("Makefile") ||
	   exists("makefile") ||
	   exists("GNUmakefile")) {

		if(asprintf(
			&cmd,
			"make "
			"&& DESTDIR=%s "
			"make install",
			qstage) < 0)
			goto out;

		ret =
			run_command(cmd);

		if(ret == 0)
			goto out;

		free(cmd);
		cmd = NULL;
	}

	fprintf(
		stderr,
		"packinstall: no supported build system "
		"succeeded in %s\n",
		src);

	ret = 1;

out:
	free(cmd);
	free(qstage);

	return ret;
}

/* -------------------------------------------------------------------------
 * Normalization
 * ------------------------------------------------------------------------- */

static int remove_info_dir(
	const char *stage)
{
	char *q;
	char *cmd;
	int ret;

	q =
		shell_quote(stage);

	if(!q)
		return -1;

	if(asprintf(
		&cmd,
		"rm -f -- %s/usr/share/info/dir",
		q) < 0) {

		free(q);

		return -1;
	}

	ret =
		run_command(cmd);

	free(q);
	free(cmd);

	return ret;
}

static int normalize_multilib_layout(
	const char *stage,
	const struct multilib_variant *multilib)
{
	char *qstage;
	char *cmd;
	int ret;

	if(!multilib)
		return 0;

	qstage =
		shell_quote(stage);

	if(!qstage)
		return -1;

	if(asprintf(
		&cmd,
		"mkdir -p -- %s/usr/share/kuzpkg "
		"&& printf 'multilib=%s\\n"
		"abi=%s\\n"
		"flags=%s\\n' "
		"> %s/usr/share/kuzpkg/multilib",
		qstage,
		multilib->name,
		multilib->abi,
		multilib->flags ?
			multilib->flags : "",
		qstage) < 0) {

		free(qstage);

		return -1;
	}

	ret =
		run_command(cmd);

	free(qstage);
	free(cmd);

	return ret;
}

/* -------------------------------------------------------------------------
 * PKGBUILD generation
 * ------------------------------------------------------------------------- */

static int write_pkgbuild(
	const char *work,
	const char *stage,
	const char *name,
	const char *version,
	const char *arch,
	const char *target,
	const struct multilib_variant *multilib)
{
	char *qstage;
	char *cmd;
	FILE *f;
	int ret = -1;

	qstage =
		shell_quote(stage);

	if(!qstage)
		goto out;

	if(asprintf(
		&cmd,
		"%s/PKGBUILD",
		work) < 0)
		goto out;

	f =
		fopen(cmd, "w");

	if(!f) {

		fprintf(
			stderr,
			"packinstall: cannot write %s: %s\n",
			cmd,
			strerror(errno));

		free(cmd);

		goto out;
	}

	fprintf(
		f,
		"pkgname='%s'\n"
		"pkgver='%s'\n"
		"pkgrel='%s'\n"
		"pkgdesc='%s'\n"
		"arch=('%s')\n"
		"source=()\n"
		"sha256sums=()\n"
		"\n"
		"# Generated by packinstall\n"
		"# target=%s\n"
		"# multilib=%s\n"
		"# abi=%s\n"
		"# multilib_flags=%s\n"
		"\n"
		"package() {\n"
		"    cp -a -- %s/. \"$pkgdir\"/\n"
		"}\n",
		name,
		version,
		REL,
		name,
		arch,
		target ? target : "",
		multilib ?
			multilib->name :
			"none",
		multilib ?
			multilib->abi :
			"",
		(multilib &&
		 multilib->flags) ?
			multilib->flags :
			"",
		qstage);

	fclose(f);

	free(cmd);

	ret = 0;

out:
	free(qstage);

	return ret;
}

/* -------------------------------------------------------------------------
 * Package and install
 * ------------------------------------------------------------------------- */

static int package_and_install(
	const char *stage,
	const char *name,
	const char *version,
	const char *arch,
	const char *target,
	const struct multilib_variant *multilib,
	int do_install)
{
	char templ[] =
		"/tmp/packinstall.XXXXXX";

	char *work;
	char *qwork;
	char *cmd = NULL;

	int ret;

	work =
		mkdtemp(templ);

	if(!work) {

		fprintf(
			stderr,
			"packinstall: mkdtemp: %s\n",
			strerror(errno));

		return 1;
	}

	if(write_pkgbuild(
		work,
		stage,
		name,
		version,
		arch,
		target,
		multilib) != 0)
		return 1;

	qwork =
		shell_quote(work);

	if(!qwork)
		return 1;

	if(asprintf(
		&cmd,
		"cd %s && "
		"makepkg -f --skipinteg --nodeps",
		qwork) < 0) {

		free(qwork);

		return 1;
	}

	ret =
		run_command(cmd);

	free(cmd);
	free(qwork);

	if(ret != 0)
		return ret;

	fprintf(
		stdout,
		"packinstall: package built in %s\n",
		work);

	if(do_install) {

		char *q;
		FILE *p;
		char pkg[PATH_MAX];

		if(asprintf(
			&q,
			"find %s "
			"-maxdepth 1 "
			"-type f "
			"-name '*.pkg.tar.*' "
			"-print -quit",
			work) < 0)
			return 1;

		p =
			popen(q, "r");

		free(q);

		if(!p)
			return 1;

		if(!fgets(
			pkg,
			sizeof(pkg),
			p)) {

			pclose(p);

			return 1;
		}

		pclose(p);

		pkg[
			strcspn(
				pkg,
				"\n")
		] = '\0';

		q =
			shell_quote(pkg);

		if(!q)
			return 1;

		if(asprintf(
			&cmd,
			"kuzpkg -U --noconfirm %s",
			q) < 0) {

			free(q);

			return 1;
		}

		ret =
			run_command(cmd);

		free(cmd);
		free(q);
	}

	return ret;
}

/* -------------------------------------------------------------------------
 * Listings
 * ------------------------------------------------------------------------- */

static void list_architectures(void)
{
	size_t i;

	printf(
		"%-24s %-38s %-12s %-12s %-12s\n",
		"ARCH",
		"TARGET",
		"OS",
		"STATUS",
		"FAMILY");

	printf(
		"--------------------------------------------------------------------------------------------\n");

	for(i = 0; architectures[i].name; i++) {

		printf(
			"%-24s %-38s %-12s %-12s %-12s\n",
			architectures[i].name,
			architectures[i].triplet,
			target_os_name(
				architectures[i].os),
			architectures[i].historical ?
				"historical" :
				architectures[i].supported ?
					"supported" :
					"unsupported",
			architectures[i].family);
	}

	printf(
		"\n"
		"Wildcard examples:\n"
		"  *-*-elf\n"
		"  riscv64-*-elf\n"
		"  arm-*-eabi\n"
		"  *-*-linux-*\n"
		"  *-*-netbsd*\n"
		"  *-*-freebsd*\n"
		"  *-*-openbsd*\n");
}

static void list_multilib(void)
{
	size_t i;

	printf(
		"%-20s %-16s %-16s %s\n",
		"MULTILIB",
		"DIRECTORY",
		"ABI",
		"FLAGS");

	printf(
		"---------------------------------------------------------------\n");

	for(i = 0; multilib_variants[i].name; i++) {

		printf(
			"%-20s %-16s %-16s %s\n",
			multilib_variants[i].name,
			multilib_variants[i].directory,
			multilib_variants[i].abi,
			multilib_variants[i].flags ?
				multilib_variants[i].flags :
				"");
	}
}

/* -------------------------------------------------------------------------
 * Usage
 * ------------------------------------------------------------------------- */

static void usage(
	const char *argv0)
{
	fprintf(
		stderr,
		"Usage:\n"
		"  %s [options] <source-tree> <staging-dir>\n"
		"\n"
		"Options:\n"
		"  -a, --arch ARCH\n"
		"      Select package architecture.\n"
		"\n"
		"  -t, --target TARGET\n"
		"      GNU target triplet or wildcard.\n"
		"\n"
		"  -b, --build BUILD\n"
		"      Build GNU triplet.\n"
		"\n"
		"  -h, --host HOST\n"
		"      Host GNU triplet.\n"
		"\n"
		"  -m, --multilib VARIANT\n"
		"      Select multilib variant.\n"
		"\n"
		"      --all-multilib\n"
		"      Request all multilib variants.\n"
		"\n"
		"  -n, --no-install\n"
		"      Build package without installing.\n"
		"\n"
		"      --list-architectures\n"
		"      List known architectures.\n"
		"\n"
		"      --list-multilib\n"
		"      List multilib variants.\n"
		"\n"
		"Examples:\n"
		"  %s ./foo-1.0 /tmp/foo\n"
		"  %s --arch x86_64 --multilib lib32 "
			"./foo-1.0 /tmp/foo\n"
		"  %s --arch x86_64 --multilib libx32 "
			"./foo-1.0 /tmp/foo\n"
		"  %s --arch mips64el --multilib mips-o32 "
			"./foo-1.0 /tmp/foo\n"
		"  %s --arch mips64el --multilib mips-n32 "
			"./foo-1.0 /tmp/foo\n"
		"  %s --arch mips64el --multilib mips-n64 "
			"./foo-1.0 /tmp/foo\n"
		"  %s --arch powerpc64le --multilib ppc32 "
			"./foo-1.0 /tmp/foo\n"
		"  %s --target 'riscv64-*-elf' "
			"./foo-1.0 /tmp/foo\n"
		"  %s --target '*-*-netbsd*' "
			"./foo-1.0 /tmp/foo\n"
		"  %s --target '*-*-freebsd*' "
			"./foo-1.0 /tmp/foo\n",
		argv0,
		argv0,
		argv0,
		argv0,
		argv0,
		argv0,
		argv0,
		argv0,
		argv0,
		argv0,
		argv0);
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(
	int argc,
	char **argv)
{
	const char *src = NULL;
	const char *stage = NULL;

	const char *arch_name = NULL;
	const char *target_pattern = NULL;
	const char *build_triplet = NULL;
	const char *host_triplet = NULL;
	const char *multilib_name = NULL;

	const struct architecture *arch_entry = NULL;
	const struct multilib_variant *multilib = NULL;

	struct target_spec target;
	struct build_options opts;

	char *resolved_target = NULL;
	char *name = NULL;
	char *version = NULL;

	char cwd[PATH_MAX];

	int ret;

	memset(&target, 0, sizeof(target));
	memset(&opts, 0, sizeof(opts));

	opts.do_install = 1;

	/* Parse command line. */
	for(int i = 1; i < argc; i++) {

		const char *arg = argv[i];

		if(strcmp(arg, "-n") == 0 ||
		   strcmp(arg, "--no-install") == 0) {

			opts.do_install = 0;
			continue;
		}

		if(strcmp(arg, "--all-multilib") == 0) {

			opts.all_multilib = 1;
			continue;
		}

		if(strcmp(arg, "--list-architectures") == 0) {

			opts.list_architectures = 1;
			continue;
		}

		if(strcmp(arg, "--list-multilib") == 0) {

			opts.list_multilib = 1;
			continue;
		}

		if(strcmp(arg, "-a") == 0 ||
		   strcmp(arg, "--arch") == 0) {

			if(i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}

			arch_name = argv[++i];
			opts.arch_name = arch_name;
			continue;
		}

		if(strncmp(
			arg,
			"--arch=",
			7) == 0) {

			arch_name = arg + 7;
			opts.arch_name = arch_name;
			continue;
		}

		if(strcmp(arg, "-t") == 0 ||
		   strcmp(arg, "--target") == 0) {

			if(i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}

			target_pattern = argv[++i];
			opts.target_pattern = target_pattern;
			continue;
		}

		if(strncmp(
			arg,
			"--target=",
			9) == 0) {

			target_pattern = arg + 9;
			opts.target_pattern = target_pattern;
			continue;
		}

		if(strcmp(arg, "-b") == 0 ||
		   strcmp(arg, "--build") == 0) {

			if(i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}

			build_triplet = argv[++i];
			opts.build_triplet = build_triplet;
			continue;
		}

		if(strncmp(
			arg,
			"--build=",
			8) == 0) {

			build_triplet = arg + 8;
			opts.build_triplet = build_triplet;
			continue;
		}

		if(strcmp(arg, "-h") == 0 ||
		   strcmp(arg, "--host") == 0) {

			if(i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}

			host_triplet = argv[++i];
			opts.host_triplet = host_triplet;
			continue;
		}

		if(strncmp(
			arg,
			"--host=",
			7) == 0) {

			host_triplet = arg + 7;
			opts.host_triplet = host_triplet;
			continue;
		}

		if(strcmp(arg, "-m") == 0 ||
		   strcmp(arg, "--multilib") == 0) {

			if(i + 1 >= argc) {
				usage(argv[0]);
				return 2;
			}

			multilib_name = argv[++i];
			continue;
		}

		if(strncmp(
			arg,
			"--multilib=",
			11) == 0) {

			multilib_name = arg + 11;
			continue;
		}

		if(arg[0] == '-') {

			fprintf(
				stderr,
				"packinstall: unknown option: %s\n",
				arg);

			usage(argv[0]);

			return 2;
		}

		if(!src)
			src = arg;
		else if(!stage)
			stage = arg;
		else {

			fprintf(
				stderr,
				"packinstall: too many positional arguments\n");

			return 2;
		}
	}

	/* Listing commands. */
	if(opts.list_architectures)
		list_architectures();

	if(opts.list_multilib)
		list_multilib();

	if((opts.list_architectures ||
	    opts.list_multilib) &&
	   !src &&
	   !stage)
		return 0;

	if(!src || !stage) {

		usage(argv[0]);

		return 2;
	}

	/* Validate source directory. */
	if(!is_dir(src)) {

		fprintf(
			stderr,
			"packinstall: source tree '%s' "
			"does not exist or is not a directory\n",
			src);

		return 1;
	}

	if(!getcwd(
		cwd,
		sizeof(cwd))) {

		fprintf(
			stderr,
			"packinstall: getcwd: %s\n",
			strerror(errno));

		return 1;
	}

	if(chdir(src) != 0) {

		fprintf(
			stderr,
			"packinstall: cannot enter '%s': %s\n",
			src,
			strerror(errno));

		return 1;
	}

	/* Architecture selection. */
	if(arch_name) {

		arch_entry =
			find_architecture(arch_name);

		if(!arch_entry)
			arch_entry =
				find_architecture_triplet(
					arch_name);

		if(!arch_entry) {

			fprintf(
				stderr,
				"packinstall: unknown architecture '%s'\n",
				arch_name);

			goto fail;
		}

	} else {

		arch_entry =
			detect_host_architecture();
	}

	/* Resolve target. */
	if(target_pattern) {

		resolved_target =
			resolve_target_pattern(
				target_pattern);

		if(!resolved_target) {

			fprintf(
				stderr,
				"packinstall: target pattern '%s' "
				"did not match a known target\n",
				target_pattern);

			goto fail;
		}

	} else if(arch_entry) {

		resolved_target =
			strdup(
				arch_entry->triplet);

	} else {

		resolved_target =
			strdup(
				"unknown-unknown-linux-gnu");
	}

	if(!resolved_target)
		goto fail;

	/* Build triplet defaults to current host. */
	if(!build_triplet) {

		const struct architecture *host;

		host =
			detect_host_architecture();

		build_triplet =
			host ?
				host->triplet :
				"unknown-unknown-linux-gnu";

		opts.build_triplet =
			build_triplet;
	}

	/* Cross host defaults to selected target. */
	if(!host_triplet &&
	   target_pattern) {

		host_triplet =
			resolved_target;

		opts.host_triplet =
			host_triplet;
	}

	/* Parse target. */
	if(parse_target(
		resolved_target,
		&target) != 0) {

		fprintf(
			stderr,
			"packinstall: invalid target '%s'\n",
			resolved_target);

		goto fail;
	}

	/* Multilib. */
	if(multilib_name) {

		multilib =
			find_multilib(
				multilib_name);

		if(!multilib) {

			fprintf(
				stderr,
				"packinstall: unknown multilib '%s'\n",
				multilib_name);

			goto fail;
		}

		opts.multilib =
			multilib;
	}

	/* Package metadata. */
	name =
		package_name(src);

	version =
		package_version(src);

	if(!name || !version) {

		fprintf(
			stderr,
			"packinstall: out of memory\n");

		goto fail;
	}

	fprintf(
		stderr,
		"packinstall: package=%s "
		"version=%s "
		"arch=%s "
		"target=%s "
		"os=%s "
		"multilib=%s\n",
		name,
		version,
		arch_entry ?
			arch_entry->name :
			"unknown",
		target.triplet,
		target_os_name(target.os),
		multilib ?
			multilib->name :
			"none");

	if(target.elf) {

		fprintf(
			stderr,
			"packinstall: target format: ELF\n");
	}

	if(target.bare_metal) {

		fprintf(
			stderr,
			"packinstall: target type: bare-metal\n");
	}

	/* Clean staging. */
	if(clean_stage(stage) != 0)
		goto fail;

	/* Build. */
	ret =
		build_project(
			src,
			stage,
			&opts,
			&target);

	if(ret != 0)
		goto fail;

	/* Remove generated info directory. */
	if(remove_info_dir(stage) != 0)
		goto fail;

	/* Record multilib metadata. */
	if(normalize_multilib_layout(
		stage,
		multilib) != 0)
		goto fail;

	/* Create/install package. */
	ret =
		package_and_install(
			stage,
			name,
			version,
			arch_entry ?
				arch_entry->name :
				"any",
			target.triplet,
			multilib,
			opts.do_install);

	if(ret != 0)
		goto fail;

	free_target(&target);

	free(resolved_target);
	free(name);
	free(version);

	if(chdir(cwd) != 0)
		return 1;

	return 0;

fail:

	fprintf(
		stderr,
		"packinstall: failed\n");

	free_target(&target);

	free(resolved_target);
	free(name);
	free(version);

	if(chdir(cwd) != 0)
		return 1;

	return 1;
}
