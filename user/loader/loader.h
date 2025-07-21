#ifndef LOADER_H
#define LOADER_H

int module_load(const char *path);
int module_unload(const char *name);
int set_sysfs_param(const char *param, int value);

#endif /* LOADER_H */
