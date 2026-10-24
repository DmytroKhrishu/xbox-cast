/*
 * xbox-cast: MSVC / UWP portability shim for the vendored UxPlay library (used only when building with MSVC).
 * Copied from src/compat/unistd.h of github.com/vproHacks/screen-mirror-xbox (GPL-3.0), unchanged except this header.
 */
/* Minimal unistd.h for MSVC / UWP (only what the UxPlay library uses). */
#ifndef COMPAT_UNISTD_H
#define COMPAT_UNISTD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <BaseTsd.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

static __inline int usleep(unsigned int usec) {
    Sleep((usec + 999) / 1000);
    return 0;
}

#endif
