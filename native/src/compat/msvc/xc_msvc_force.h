/*
 * xbox-cast: force-included (/FI) into every C file of castcore and the vendored UxPlay library when building
 * with MSVC. Based on src/compat/msvc_force.h of github.com/vproHacks/screen-mirror-xbox (GPL-3.0), plus the
 * POSIX string functions UxPlay uses.
 */
#ifndef XC_MSVC_FORCE_H
#define XC_MSVC_FORCE_H
/* CMake's WindowsStore platform adds /DUNICODE; UxPlay calls the narrow Win32 APIs (FormatMessage with char*) */
#undef UNICODE
#undef _UNICODE
#include <BaseTsd.h>
#include <string.h>
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#endif
