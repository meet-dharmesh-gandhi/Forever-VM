#include "storage_management.h"
#define _GNU_SOURCE
#include<stdio.h>
#include<libvirt/libvirt.h>

int create_vdisk(virConnectPtr conn, char *name, int size){
	virStoragePoolPtr pool;
	virStorageVolPtr vol;
	char *xml;
	bool errflag = false;

	if(pool = virStoragePoolLookupByTargetPath(conn, "/var/lib/libvirt/images")){
		asprintf(&xml,
	        "<volume type='file'>\n"
        	"        <name>%s.qcow2</name>\n"
        	"       <capacity unit='GiB'>%d</capacity>\n"
	        "       <target>\n"
        	"               <format type='qcow2'/>\n"
        	"       </target>\n"
        	"       <backingStore>\n"
        	"               <path>/var/lib/libvirt/images/golden-img.qcow2</path>\n"
        	"               <format type='qcow2'/>\n"
        	"       </backingStore>\n"
        	"</volume>", name, size);

		if(vol = virStorageVolCreateXML(pool, xml, 0)){
			printf("Virtual Disk Creation Successful\n");
		}else{
			printf("Virtual Disk Creation Failed\n");
			errflag = true;
		}
	}else{
		printf("Storage Pool Not Found\n");
		errflag = true;
	}

        if(errflag){
                return -1;
        }else{
                return 0;
        }
}

int delete_vdisk(virConnectPtr conn, char *name){
	virStorageVolPtr vol;
	char *path;
	bool errflag = false;

	asprintf(&path, "/var/lib/libvirt/images/%s.qcow2", name);

	if(vol = virStorageVolLookupByPath(conn, path)){
		if(virStorageVolDelete(vol, 0) == 0){
			printf("Virtual Disk Deletion Successful\n");
		}else{
			printf("Virtual Disk Deletion Failed\n");
			errflag = true;
		}
	}else{
		printf("Virtual Disk Not Found");
		errflag = true;
	}

	if(errflag){
                return -1;
        }else{
                return 0;
        }
}
