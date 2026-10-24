/*
 * UWP link check for castcore (xbox-cast). GPL-3.0-or-later.
 * Built only for CMAKE_SYSTEM_NAME=WindowsStore: a DLL that pulls in every public castcore entry point,
 * so the linker must resolve all of UxPlay + OpenSSL + libplist + system imports with the app's settings.
 */
#include <stddef.h>
#include <stdint.h>

#include "castcore.h"

__declspec(dllexport) int xc_linkcheck(void)
{
    xc_config cfg;
    xc_callbacks cb;
    xc_receiver *r;
    int ok;
    char *p = (char *) &cfg;
    for (size_t i = 0; i < sizeof(cfg); i++) p[i] = 0;
    p = (char *) &cb;
    for (size_t i = 0; i < sizeof(cb); i++) p[i] = 0;
    cfg.name = "linkcheck";
    cfg.builtin_mdns = 1;
    r = xc_create(&cfg, &cb);
    if (!r) return -1;
    xc_set_log_level(r, XC_LOG_DEBUG);
    xc_set_low_delay_sps(r, 1);
    ok = xc_start(r);
    xc_disconnect(r);
    xc_stop(r);
    xc_destroy(r);
    return ok + xc_api_version() + (int) (xc_now_ns() & 1) + (xc_version_string() != NULL);
}
