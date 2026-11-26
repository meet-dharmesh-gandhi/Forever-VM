#ifndef VM_MANAGEMENT_H
#define VM_MANAGEMENT_H

int create_vm(char *name, int ram, int vcpu, int vdisk, char* passwd);
int launch_vm(char *name);
int delete_vm(char *name, bool vdisk);

#endif
