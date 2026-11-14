// The command-line host's side of the Linux front end interface (front_cli.c is Android's, shared): no window, so no surface extensions
#include "dm.h"
#include "gfx_vk.h"
uint32_t front_vk_instance_extensions(const char *const **names) { *names = NULL; return 0; }
