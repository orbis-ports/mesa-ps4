/*
 * Copyright © 2015 Intel
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef UTIL_FUTEX_H
#define UTIL_FUTEX_H

#if THREAD_SANITIZER
#define UTIL_FUTEX_SUPPORTED 0
#else
#if defined(HAVE_LINUX_FUTEX_H) && defined(__linux__)
#define UTIL_FUTEX_SUPPORTED 1
/* ⚠ __PS4__/__ORBIS__ IS NOT REDUNDANT BESIDE __FreeBSD__, AND LEAVING IT OUT SPLITS THE ABI.
 *
 * The PS4 kernel is FreeBSD and clang defines __FreeBSD__ 12 for its triple, so the C half of this
 * tree takes the arm below and gets UTIL_FUTEX_SUPPORTED 1. The C++ half does not: the OpenOrbis
 * SDK's libc++ __config_site does `#undef __FreeBSD__` (it pairs libc++ with a musl libc and does
 * not want FreeBSD's arms), so any .cpp that reaches a libc++ header before this one - which is
 * every .cpp in Mesa, <stdint.h> is enough - sees UTIL_FUTEX_SUPPORTED 0.
 *
 * That is not a missing optimisation. simple_mtx_t is `{uint32_t}` in the futex build and
 * `{util_once_flag; mtx_t}` in the other, and struct util_queue_fence changes with it, so every
 * struct in the tree containing one has two different layouts and two different sizes depending on
 * which compiler saw it. It surfaced as undefined references to simple_mtx_init,
 * _simple_mtx_plain_init_once and util_queue_fence_init from zink_draw.cpp; the references are the
 * lucky half of the symptom.
 *
 * __PS4__ and __ORBIS__ come from the cross file's command line, which no header can take away.
 */
#elif defined(__FreeBSD__) || defined(__PS4__) || defined(__ORBIS__)
#define UTIL_FUTEX_SUPPORTED 1
#elif defined(__OpenBSD__)
#define UTIL_FUTEX_SUPPORTED 1
#elif defined(_WIN32) && !defined(WINDOWS_NO_FUTEX)
#define UTIL_FUTEX_SUPPORTED 1
#else
#define UTIL_FUTEX_SUPPORTED 0
#endif
#endif

#if UTIL_FUTEX_SUPPORTED
#include <stdint.h>
#include <c11/time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if UTIL_FUTEX_SUPPORTED
int futex_wake(uint32_t *addr, int32_t count);
int futex_wait(uint32_t *addr, int32_t value, const struct timespec *timeout);
#endif

#ifdef __cplusplus
}
#endif

#endif /* UTIL_FUTEX_H */
