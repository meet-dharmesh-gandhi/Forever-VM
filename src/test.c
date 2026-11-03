#include <libvirt/libvirt-qemu.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <stdio.h>

int main(int argc, char const *argv[]) {
	FILE *f = fopen("./desc.xml", "r");

	fseek(f, 0L, SEEK_END);
	size_t fileSize = ftell(f) + 1;
	char content[fileSize];
	rewind(f);

	int last = fread(content, 1, fileSize, f);
	content[last] = '\0';

	printf(content, "Test VM", 10, 20, 3, "hvm", "some_path",
		   "some_qcow2_path");

	fclose(f);
	return 0;
}
