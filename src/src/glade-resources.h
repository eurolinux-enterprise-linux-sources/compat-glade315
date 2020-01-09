#ifndef __RESOURCE_glade_resources_H__
#define __RESOURCE_glade_resources_H__

#include <gio/gio.h>

extern GResource *glade_resources_get_resource (void);

extern void glade_resources_register_resource (void);
extern void glade_resources_unregister_resource (void);

#endif
