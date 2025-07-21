#include <stdio.h>
#include <stdlib.h>
#include "loader.h"

int module_load(const char *path)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "insmod %s", path);
    return system(cmd);
}

int module_unload(const char *name)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rmmod %s", name);
    return system(cmd);
}

int set_sysfs_param(const char *param, int value)
{
    char path[256];
    FILE *f;

    snprintf(path, sizeof(path), "/sys/module/virtio_nic/parameters/%s", param);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "%d\n", value);
    fclose(f);
    return 0;
}
