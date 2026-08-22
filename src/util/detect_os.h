/* SPDX-License-Identifier: MIT */
/* Copyright 2008 VMware, Inc. */

/**
 * Auto-detect the operating system family.
 *
 * See also:
 * - http://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html
 * - echo | gcc -dM -E - | sort
 * - http://msdn.microsoft.com/en-us/library/b0084kay.aspx
 *
 * @author José Fonseca <jfonseca@vmware.com>
 */

#ifndef DETECT_OS_H
#define DETECT_OS_H

#if defined(__linux__)
#define DETECT_OS_LINUX 1
#define DETECT_OS_POSIX 1
#endif

/*
 * Android defines __linux__, so DETECT_OS_LINUX and DETECT_OS_POSIX will
 * also be defined.
 */
#if defined(__ANDROID__)
#define DETECT_OS_ANDROID 1
#endif

/* The PS4 is a FreeBSD KERNEL with a musl LIBC, so __FreeBSD__ is defined while none of FreeBSD's own
 * libc interfaces exist - no sys/umtx.h, no machine/cpu.h, no kinfo_file, no KERN_PROC_ARGS. Every
 * DETECT_OS_FREEBSD arm in util/ reaches for one of those, so this platform wants the generic POSIX
 * arms instead. Thirteen of this port's fourteen remaining build errors were exactly that.
 *
 * Stated as its own case rather than by editing four call sites: "the kernel is FreeBSD" and "the libc
 * is FreeBSD's" are different facts, and only the second one is what those arms need.
 */
#if defined(__PS4__) || defined(__ORBIS__)
/* BSD is TRUE - the kernel is one - while FREEBSD is deliberately not set, because that macro is what
 * util/ uses to reach for FreeBSD's LIBC. The distinction is the whole point of this case.
 *
 * ⚠ "DETECT_OS_BSD with no specific BSD set" is a state nothing in the tree had produced before, and two
 * places assume it cannot happen. Both are guarded by a sysconf() probe today, so both are unreachable
 * on this toolchain - and both would fail confusingly rather than usefully if that ever changed:
 *
 *   os_misc.c, os_get_total_physical_memory()  picks a sysctl mib by BSD flavour and ends its chain in
 *                                              `#error Unsupported *BSD`. Guarded by HAVE_SYSCONF.
 *   u_cpu_detect.c, the DETECT_OS_BSD arm      passes `int *` to sysctl()'s `size_t *oldlen`, which is
 *                                              an incompatible-pointer error rather than a warning
 *                                              here. Guarded by _SC_NPROCESSORS_CONF, which musl
 *                                              defines. That one is an upstream bug on every BSD; it is
 *                                              simply never compiled.
 */
#define DETECT_OS_BSD 1
#define DETECT_OS_POSIX 1

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#define DETECT_OS_FREEBSD 1
#define DETECT_OS_BSD 1
#define DETECT_OS_POSIX 1
#endif

#if defined(__OpenBSD__)
#define DETECT_OS_OPENBSD 1
#define DETECT_OS_BSD 1
#define DETECT_OS_POSIX 1
#endif

#if defined(__NetBSD__)
#define DETECT_OS_NETBSD 1
#define DETECT_OS_BSD 1
#define DETECT_OS_POSIX 1
#endif

#if defined(__DragonFly__)
#define DETECT_OS_DRAGONFLY 1
#define DETECT_OS_BSD 1
#define DETECT_OS_POSIX 1
#endif

#if defined(__Fuchsia__)
#define DETECT_OS_FUCHSIA 1
#define DETECT_OS_POSIX_LITE 1
#endif

#if defined(__GNU__)
#define DETECT_OS_HURD 1
#define DETECT_OS_POSIX 1
#endif

#if defined(__sun)
#define DETECT_OS_SOLARIS 1
#define DETECT_OS_POSIX 1
#endif

#if defined(__APPLE__)
#define DETECT_OS_APPLE 1
#define DETECT_OS_POSIX 1
#endif

#if defined(_WIN32)
#define DETECT_OS_WINDOWS 1
#endif

#if defined(__HAIKU__)
#define DETECT_OS_HAIKU 1
#define DETECT_OS_POSIX 1
#endif

#if defined(__CYGWIN__)
#define DETECT_OS_CYGWIN 1
#define DETECT_OS_POSIX 1
#endif

#if defined(__managarm__)
#define DETECT_OS_MANAGARM 1
#define DETECT_OS_POSIX 1
#endif


/*
 * Make sure DETECT_OS_* are always defined, so that they can be used with #if
 */
#ifndef DETECT_OS_ANDROID
#define DETECT_OS_ANDROID 0
#endif
#ifndef DETECT_OS_APPLE
#define DETECT_OS_APPLE 0
#endif
#ifndef DETECT_OS_BSD
#define DETECT_OS_BSD 0
#endif
#ifndef DETECT_OS_CYGWIN
#define DETECT_OS_CYGWIN 0
#endif
#ifndef DETECT_OS_DRAGONFLY
#define DETECT_OS_DRAGONFLY 0
#endif
#ifndef DETECT_OS_FREEBSD
#define DETECT_OS_FREEBSD 0
#endif
#ifndef DETECT_OS_HAIKU
#define DETECT_OS_HAIKU 0
#endif
#ifndef DETECT_OS_FUCHSIA
#define DETECT_OS_FUCHSIA 0
#endif
#ifndef DETECT_OS_HURD
#define DETECT_OS_HURD 0
#endif
#ifndef DETECT_OS_LINUX
#define DETECT_OS_LINUX 0
#endif
#ifndef DETECT_OS_NETBSD
#define DETECT_OS_NETBSD 0
#endif
#ifndef DETECT_OS_OPENBSD
#define DETECT_OS_OPENBSD 0
#endif
#ifndef DETECT_OS_SOLARIS
#define DETECT_OS_SOLARIS 0
#endif
#ifndef DETECT_OS_POSIX
#define DETECT_OS_POSIX 0
#endif
#ifndef DETECT_OS_POSIX_LITE
#define DETECT_OS_POSIX_LITE DETECT_OS_POSIX
#endif
#ifndef DETECT_OS_WINDOWS
#define DETECT_OS_WINDOWS 0
#endif
#ifndef DETECT_OS_MANAGARM
#define DETECT_OS_MANAGARM 0
#endif

#endif /* DETECT_OS_H */
