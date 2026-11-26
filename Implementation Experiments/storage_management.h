#ifndef STORAGE_MANAGEMENT_H
#define STORAGE_MANAGEMENT_H
#include<libvirt/libvirt.h>

int create_vdisk(virConnectPtr conn, char *name, int size);
int delete_vdisk(virConnectPtr conn, char *name);

#endif
