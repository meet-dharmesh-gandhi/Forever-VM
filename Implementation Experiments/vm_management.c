#include"vm_management.h"
#include"storage_management.h"
#define _GNU_SOURCE
#include<stdio.h>
#include<libvirt/libvirt.h>
#include<libvirt/virterror.h>

int create_vm(char *name, int ram, int vcpu, int vdisk, char* passwd){
	virConnectPtr conn;
	virDomainPtr domain;
	char *xml;
	int tlsPort = -1;
	char *i;

	conn = virConnectOpen("qemu:///system");

	if(conn != NULL){
		printf("Connection to Hypervisor Successful\n");
	}else{
		printf("Connection to Hypervisor Failed\n");
		return -1;
	}

	if(vdisk == 0 || create_vdisk(conn, name, vdisk) == 0){

		asprintf(&xml,
		"<domain type='kvm'>\n"
		"	 <name>%s</name>\n"
  		"	<memory unit='GiB'>%d</memory>\n"
  		"	<vcpu>%d</vcpu>\n"
		"	<features>\n"
		"   		<acpi/>\n"
		"    		<apic/>\n"
		"	</features>\n"
  		"	<os>\n"
    		"		<type arch='x86_64'>hvm</type>\n"
    		"		<boot dev='hd'/>\n"
  		"	</os>\n"
  		"	<devices>\n"
    		"		<disk type='file' device='disk'>\n"
      		"			<driver name='qemu' type='qcow2'/>\n"
      		"			<source file='/var/lib/libvirt/images/%s.qcow2'/>\n"
      		"			<target dev='vda' bus='virtio'/>\n"
    		"		</disk>\n"
    		"		<interface type='bridge'>\n"
        	"			<source bridge='virbr0'/>\n"
        	"			<model type='virtio'/>\n"
    		"		</interface>\n"
    		"		<graphics type='spice' tlsPort='-1' autoport='yes' defaultMode='secure' passwd='%s'>\n"
      		"			<image compression='auto_glz'/>\n"
  		"			<streaming mode='filter'/>\n"
		"			<mouse mode='client'/>\n"
    		"		</graphics>\n"
		"		<video>\n"
    		"			<model type='qxl'/>\n"
		"		</video>\n"
  		"	</devices>\n"
		"</domain>", name, ram, vcpu, name, passwd);

		if(domain = virDomainDefineXML(conn, xml)){
			printf("Domain Defination Successful\n");

			if(virDomainCreate(domain) == 0){
	                	printf("Domain Creation Successful\n");

				xml = virDomainGetXMLDesc(domain, 0);
				i = strstr(xml, "tlsPort=");
				sscanf(i+9, "%d", &tlsPort);

				virConnectClose(conn);
				return tlsPort;
        		}else{
                		printf("Domain Creation Failed\n");
        		}
		}else{
			printf("Domain Defination Failed\n");
		}
	}

	virConnectClose(conn);
        return -1;
}

int launch_vm(char *name){
	virConnectPtr conn;
        virDomainPtr domain;
	char *xml;
        int tlsPort = -1;
        char *i;

	conn = virConnectOpen("qemu:///system");

        if(conn != NULL){
                printf("Connection to Hypervisor Successful\n");
        }else{
                printf("Connection to Hypervisor Failed\n");
                return -1;
        }

	if(domain = virDomainLookupByName(conn, name)){
		if(virDomainCreate(domain) == 0){
	                printf("Domain Launched Successful\n");

			xml = virDomainGetXMLDesc(domain, 0);
                     	i = strstr(xml, "tlsPort=");
                      	sscanf(i+9, "%d", &tlsPort);

                        virConnectClose(conn);
                     	return tlsPort;
        	}else{
                	printf("Domain Launch Failed\n");
        	}
	}else{
		virErrorPtr err = virConnGetLastError(conn);

		if (err && err->code == VIR_ERR_NO_DOMAIN) {
        		printf("Domain Doesn't Exist\n");
    		}
	}

	virConnectClose(conn);
	return -1;
}

int delete_vm(char *name, bool vdisk){
	virConnectPtr conn;
        virDomainPtr domain;

        conn = virConnectOpen("qemu:///system");

        if(conn != NULL){
                printf("Connection to Hypervisor Successful\n");
        }else{
                printf("Connection to Hypervisor Failed\n");
                return -1;
        }

        if(domain = virDomainLookupByName(conn, name)){
                if(virDomainUndefine(domain) == 0){
			if(!vdisk || (delete_vdisk(conn, name) != -1)){
				printf("Domain Deletion Successful\n");

				virConnectClose(conn);
			        return 0;
			}
                }else{
                        printf("Domain Deletion Failed\n");
                }
        }else{
                virErrorPtr err = virConnGetLastError(conn);

                if (err && err->code == VIR_ERR_NO_DOMAIN) {
                        printf("Domain Doesn't Exist\n");
                }
        }

        virConnectClose(conn);
        return -1;
}
