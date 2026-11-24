#include "../include/all.h"
#include <libisofs/libisofs.h>
#include <libvirt/libvirt-qemu.h>
#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <node_api.h>
#include <uv.h>

// =============================================================
//                          Structs
// =============================================================

struct Node {
	char ip[INET_ADDRSTRLEN];
	// for now - load is the number of vms in the vmm
	// later - load is the actual cpu + memory + ram + io load
	int load;
	time_t lastAnswered;
};

// =============================================================
//                    Function Declarations
// =============================================================

napi_value Start(napi_env env, napi_value exports);
napi_value CreateVM(napi_env env, napi_callback_info info);
napi_value GetNodeIP(napi_env env, napi_callback_info info);

virDomainPtr getVM(char *domain_name, int vcpus, int memory, int disk, int port,
				   char *username, char *password);

void handle_heartbeats(uv_poll_t *handle, int status, int events);
void handle_timer(uv_poll_t *handle, int status, int events);

int connectQemu();
virDomainPtr domainExists(char *domain);
int getXml(char **xml, char *vm_name, int vm_name_length, int memory,
		   int currentMemory, int disk, int vcpus, char *vm_type,
		   int vm_type_length, int port, char *username, char *password);
int setUserData(char **userData, char *name, char *password);
int setMetaData(char *vm_name, char **metaData);
char *getNewVolume(char *vol_name, char *unit, int storage);
virStorageVolPtr getVol(char *vol_name, char *unit, int storage,
						virStoragePoolPtr pool);
virStoragePoolPtr getPool(char *pool_name);
int getStorageXml(char **xml, char *vol_name, char *unit, int storage);
int getStoragePoolXml(char **xml, char *pool_name);
int getStrLength(int num);
int toIp(char *ip, struct sockaddr_in *addr);
int validPacket();

napi_value getNapiGlobal(napi_env env);
napi_value napiBool(napi_env env, int boolean);
napi_value napiInt32(napi_env env, int i);
napi_value napiUndefined(napi_env env);
napi_value napiPanic(napi_env env);
void printNapiError(napi_env env, char *func_name);

// =============================================================
//                    Variable Declarations
// =============================================================

struct sockaddr_in *self_addr, *addr;
int addrLen = sizeof(struct sockaddr_in);
int mode = WORKER;

// sockets
int hb_fd, timer_fd;
uv_poll_t hb_poll, timer_poll;

// socket handling helpers
uint8_t *fd_buf;
int fdBufSize = sizeof(struct Node);
struct heartbeat_packet *hb;
int hb_size = sizeof(struct heartbeat_packet);

// QEMU
virConnectPtr vcp;

// gateway specific variables
struct Node nodes[GATEWAY_CAPACITY];

// worker specific variables
int domains;

// =============================================================
//                         Node Functions
// =============================================================

napi_value Start(napi_env env, napi_value exports) {
	self_addr = (struct sockaddr_in *)malloc(addrLen);
	struct ifaddrs *ifa = malloc(sizeof(struct ifaddrs));
	getInterface(ifa);
	memcpy(self_addr, ifa->ifa_addr, addrLen);

	printc(INFO, "Start", "C code started running, ip: %s\n",
		   getPrintableIP(self_addr));

	uv_loop_t *loop;
	napi_get_uv_event_loop(env, &loop);

	hb = malloc(hb_size);
	fd_buf = malloc(fdBufSize);
	addr = malloc(addrLen);

	// create the heartbeat socket
	hb_fd = getNewSocket(HEARTBEAT_PORT, HEARTBEAT_INTERVAL, SOCK_DGRAM);
	if (hb_fd == -1) {
		printc(RED, "Start", "Could not create hb_fd\n");
		perror("hb_fd");
		return napiBool(env, false);
	}

	for (int i = 0; i < GATEWAY_CAPACITY; i++) {
		memset(nodes[i].ip, 0, INET_ADDRSTRLEN);
		nodes[i].lastAnswered = -1;
		nodes[i].load = -1;
	}

	if (strcmp(getPrintableIP(self_addr), GATEWAY_IP) == 0) {
		mode = GATEWAY;
		// add heartbeat registering function to node epoll
		uv_poll_init(loop, &hb_poll, hb_fd);
		uv_poll_start(&hb_poll, UV_READABLE, handle_heartbeats);
	} else {
		// create the timer socket
		timer_fd = getNewTimerFD(CLOCK_MONOTONIC, HEARTBEAT_INTERVAL,
								 HEARTBEAT_INTERVAL, 1);
		if (timer_fd == -1) {
			printc(RED, "Start", "Could not create timer_fd\n");
			perror("timer_fd");
			return napiBool(env, false);
		}

		// add hearbeat sending function to node epoll
		uv_poll_init(loop, &timer_poll, timer_fd);
		uv_poll_start(&timer_poll, UV_READABLE, handle_timer);
	}

	printc(IMP, "Start", "Running in mode: %d\n", mode);

	if (!connectQemu()) {
		return napiBool(env, false);
	}

	napi_value GetNodeIPFN;
	napi_create_function(env, "getNodeIP", NAPI_AUTO_LENGTH, GetNodeIP, NULL,
						 &GetNodeIPFN);
	napi_set_named_property(env, exports, "getNodeIP", GetNodeIPFN);
	napi_value CreateVMFN;
	napi_create_function(env, "createVM", NAPI_AUTO_LENGTH, CreateVM, NULL,
						 &CreateVMFN);
	napi_set_named_property(env, exports, "createVM", CreateVMFN);
	napi_set_named_property(env, exports, "ok", napiBool(env, 1));

	return exports;
}

napi_value CreateVM(napi_env env, napi_callback_info info) {
	printc(INFO, "CreateVM", "new VM\n");

	napi_status status;
	size_t argc = 7;
	napi_value args[argc];
	int curArg = 0;

	status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	if (status != napi_ok) {
		printc(RED, "CreateVM", "Could not parse args\n");
		return napiUndefined(env);
	}

	// domain name
	napi_valuetype value_type;
	status = napi_typeof(env, args[curArg], &value_type);
	if (status != napi_ok || value_type != napi_string) {
		printc(RED, "CreateVM", "Could not get type of argument %d: %d, %d\n",
			   curArg, status != napi_ok, value_type != napi_string);
		printNapiError(env, "CreateVM");
		return napiUndefined(env);
	}

	size_t domainNameLength;
	status = napi_get_value_string_utf8(env, args[curArg], NULL, 0,
										&domainNameLength);
	if (status != napi_ok) {
		printc(RED, "CreateVM", "Could not get domainNameLength\n");
		printNapiError(env, "CreateVM");
		return napiUndefined(env);
	}

	char *domainName = malloc(domainNameLength + 1);
	status = napi_get_value_string_utf8(env, args[curArg], domainName,
										domainNameLength + 1, NULL);
	printc(INFO, "CreateVM", "prev domainName: %s\n", domainName);
	domainName[domainNameLength] = '\0';

	printc(INFO, "CreateVM", "domainName: %s\n", domainName);

	// vcpus
	curArg++;
	status = napi_typeof(env, args[curArg], &value_type);

	printc(INFO, "CreateVM", "status: %d, ok: %d, type: %d, num: %d\n", status,
		   napi_ok, value_type, napi_number);

	if (status != napi_ok || value_type != napi_number) {
		printc(RED, "CreateVM", "Argument %d not number\n", curArg);
		return napiUndefined(env);
	}

	int vcpus = 0;
	status = napi_get_value_int32(env, args[curArg], &vcpus);
	printc(INFO, "CreateVM", "status: %d, vcpus: %d\n", status, vcpus);

	if (status != napi_ok || vcpus <= 0 || vcpus > MAX_VCPUS) {
		printc(RED, "CreateVM", "Some error, number of VCPUs: %d\n", vcpus);
		return napiUndefined(env);
	}

	printc(INFO, "CreateVM", "vcpus: %d\n", vcpus);

	// vmemory
	curArg++;
	status = napi_typeof(env, args[curArg], &value_type);

	if (status != napi_ok || value_type != napi_number) {
		printc(RED, "CreateVM", "Argument %d not number\n", curArg);
		return napiUndefined(env);
	}

	int memory = 0;
	status = napi_get_value_int32(env, args[curArg], &memory);

	if (status != napi_ok || memory <= 0 || memory > MAX_VMEM) {
		printc(RED, "CreateVM", "Some error, VMEM demanded: %d\n", memory);
		return napiUndefined(env);
	}

	printc(INFO, "CreateVM", "vmem: %d\n", memory);

	// vdisk
	curArg++;
	status = napi_typeof(env, args[curArg], &value_type);

	if (status != napi_ok || value_type != napi_number) {
		printc(RED, "CreateVM", "Argument %d not number\n", curArg);
		return napiUndefined(env);
	}

	int disk = 0;
	status = napi_get_value_int32(env, args[curArg], &disk);

	if (status != napi_ok || disk <= 0 || disk > MAX_VDISK) {
		printc(RED, "CreateVM", "Some error, VDISK demanded: %d\n", disk);
		return napiUndefined(env);
	}

	printc(INFO, "CreateVM", "vdisk: %d\n", disk);

	// port
	curArg++;
	status = napi_typeof(env, args[curArg], &value_type);

	if (status != napi_ok || value_type != napi_number) {
		printc(RED, "CreateVM", "Argument %d not number\n", curArg);
		return napiUndefined(env);
	}

	int port = 0;
	status = napi_get_value_int32(env, args[curArg], &port);

	if (status != napi_ok || port <= 0 || port > MAX_PORT) {
		printc(RED, "CreateVM", "Some error, port given: %d\n", port);
		return napiUndefined(env);
	}

	printc(INFO, "CreateVM", "port: %d\n", port);

	// username
	curArg++;
	status = napi_typeof(env, args[curArg], &value_type);
	if (status != napi_ok || value_type != napi_string) {
		printc(RED, "CreateVM", "Could not get type of argument %d: %d, %d\n",
			   curArg, status != napi_ok, value_type != napi_string);
		printNapiError(env, "CreateVM");
		return napiUndefined(env);
	}

	size_t userNameLength;
	status =
		napi_get_value_string_utf8(env, args[curArg], NULL, 0, &userNameLength);
	if (status != napi_ok) {
		printc(RED, "CreateVM", "Could not get userNameLength\n");
		printNapiError(env, "CreateVM");
		return napiUndefined(env);
	}

	char *userName = malloc(userNameLength + 1);
	status = napi_get_value_string_utf8(env, args[curArg], userName,
										userNameLength + 1, NULL);
	userName[userNameLength] = '\0';

	printc(INFO, "CreateVM", "username: %s\n", userName);

	// password
	curArg++;
	status = napi_typeof(env, args[curArg], &value_type);
	if (status != napi_ok || value_type != napi_string) {
		printc(RED, "CreateVM", "Could not get type of argument %d: %d, %d\n",
			   curArg, status != napi_ok, value_type != napi_string);
		printNapiError(env, "CreateVM");
		return napiUndefined(env);
	}

	size_t passwordLength;
	status =
		napi_get_value_string_utf8(env, args[curArg], NULL, 0, &passwordLength);
	if (status != napi_ok) {
		printc(RED, "CreateVM", "Could not get passwordLength\n");
		printNapiError(env, "CreateVM");
		return napiUndefined(env);
	}

	char *password = malloc(passwordLength + 1);
	status = napi_get_value_string_utf8(env, args[curArg], password,
										passwordLength + 1, NULL);
	password[passwordLength] = '\0';

	printc(INFO, "CreateVM", "pwd: %s\n", password);

	if (domains < MAX_DOMAINS) {
		int returnedPort =
			getVM(domainName, vcpus, memory, disk, port, userName, password);
		if (returnedPort == -1) {
			printc(RED, "CreateVM", "Could not create VM!\n");
			return napiUndefined(env);
		}

		domains++;
		return napiBool(env, 1);
	}

	printc(WARN, "CreateVM", "No space left in VMM for more VMs\n");
	return napiBool(env, 0);
}

napi_value GetNodeIP(napi_env env, napi_callback_info info) {
	printc(INFO, "GetNodeIP", "request\n");

	if (mode == GATEWAY) {
		// the response function will be sent as an argument
		printc(INFO, "GetNodeIP", "Is Gateway\n");
		napi_status status;
		size_t argc = 2;
		napi_value args[argc];

		status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);

		if (status != napi_ok) {
			printc(RED, "GetNodeIP", "Could not parse args\n");
			return napiBool(env, 0);
		}

		// send the ip of the lowest loaded node
		int lowest = MAX_DOMAINS;
		struct Node *n = NULL;
		for (int i = 0; i < GATEWAY_CAPACITY; i++) {
			if (nodes[i].load >= 0 && lowest > nodes[i].load) {
				lowest = nodes[i].load;
				n = &nodes[i];
			}
		}

		printc(INFO, "GetNodeIP", "lowest: %d\n", lowest);

		if (lowest < MAX_DOMAINS && n != NULL) {
			// create the message to pass
			napi_value ip_str;
			status =
				napi_create_string_utf8(env, n->ip, NAPI_AUTO_LENGTH, &ip_str);
			if (status != napi_ok) {
				printc(RED, "GetNodeIP", "Failed to create a string\n");
				return napiBool(env, 0);
			}

			n->load++;
			printc(INFO, "GetNodeIP", "ip: %s\n", n->ip);
			return ip_str;
		}

		// no node capable to serve, system at full capacity
		printc(WARN, "GetNodeIP",
			   "No node capable to serve, system at full capacity! Add new "
			   "nodes!");
		return napiBool(env, 0);
	}

	return napiBool(env, 0);
}

NAPI_MODULE(NODE_GYP_GATEWAY, Start);

// =============================================================
//                         VM Functions
// =============================================================

/**
 * Gets the virDomainPtr of the VM if it exists
 * else it creates a new VM and gives the virDomainPtr
 */
virDomainPtr getVM(char *domain_name, int vcpus, int memory, int disk, int port,
				   char *username, char *password) {
	connectQemu();

	virDomainPtr vdp = NULL;

	if ((vdp = domainExists(domain_name)) == NULL && vcpus > 0 && memory > 0) {
		char *xml = NULL;
		getXml(&xml, domain_name, strlen(domain_name), memory, memory, disk,
			   vcpus, "hvm", 3, port, username, password);
		vdp = virDomainCreateXML(vcp, xml, 0);
		free(xml);
	}

	return vdp;
}

// =============================================================
//                      System Functions
// =============================================================

/**
 * Simply registers heartbeats as they come
 */
void handle_heartbeats(uv_poll_t *handle, int status, int events) {
	(void)handle;
	(void)status;
	(void)events;

	while (1) {
		int res =
			getNextDGRAMPacket(hb_fd, fd_buf, fdBufSize, 0, addr, addrLen);

		if (res == EXIT_SUCCESS) {
			int packet_type = validPacket();

			// TODO when creating the array make all node loads to -1
			// also do the same when destroying a node entry
			if (packet_type == HEARTBEAT_PACKET) {
				// received a heartbeat from a node
				for (int i = 0; i < GATEWAY_CAPACITY; i++) {
					if (nodes[i].load >= 0 &&
						strcmp(nodes[i].ip, getPrintableIP(addr)) == 0) {
						nodes[i].load = hb->load;
						nodes[i].lastAnswered = getCurrTime();
						break;
					} else if (nodes[i].load < 0 ||
							   (nodes[i].lastAnswered > 0 &&
								getCurrTime() - nodes[i].lastAnswered >
									EXPIRE_PERIOD)) {
						// new node and an empty gap
						memcpy(nodes[i].ip, getPrintableIP(addr),
							   INET_ADDRSTRLEN);
						printc(INFO, "handle_heartbeats", "new node: %s\n",
							   nodes[i].ip);
						nodes[i].load = hb->load;
						nodes[i].lastAnswered = getCurrTime();
						break;
					}
				}
			} else {
				printc(RED, "monitor - handle_hb_fd",
					   "Some unknown packet on the port: %d\n", packet_type);
			}
		} else if (res != 2) {
			break;
		}
	}
}

/**
 * Simply sends heartbeats at intervals
 */
void handle_timer(uv_poll_t *handle, int status, int events) {
	(void)handle;
	(void)status;
	(void)events;

	readTimerFD(timer_fd);

	// send heartbeat
	hb->packet_ID = PACKET_ID;
	hb->packet_type = HEARTBEAT_PACKET;
	hb->load = domains;
	int gotIp = toIp(GATEWAY_IP, addr);
	if (gotIp == NO) {
		printc(WARN, "handle_timer", "Could not get ip\n");
	}
	addr->sin_family = AF_INET;
	addr->sin_port = getPort(HEARTBEAT_PORT);
	sendto(hb_fd, hb, hb_size, 0, addr, addrLen);
}

// =============================================================
//                          Utils
// =============================================================

/**
 * Check the connection and create one if dead
 */
int connectQemu() {
	if (vcp == NULL || virConnectIsAlive(vcp) <= 0) {
		if (vcp != NULL) {
			virConnectClose(vcp);
		}
		vcp = virConnectOpen("qemu:///system");
		if (vcp == NULL) {
			printc(RED, "createVM", "Could not open connection");
			perror("virConnectOpen");
			return NO;
		}
	}

	return YES;
}

/**
 * Checks if the domain already exists
 * Returns the domain ptr if the domain exists
 * else returns NULL
 */
virDomainPtr domainExists(char *domain) {
	return virDomainLookupByName(vcp, domain);
}

/**
 * Fill in the xml file
 */
int getXml(char **xml, char *vm_name, int vm_name_length, int memory,
		   int currentMemory, int disk, int vcpus, char *vm_type,
		   int vm_type_length, int port, char *username, char *password) {
	FILE *f = fopen(TEMPLATE_XML_FILE_PATH, "r");
	if (f == NULL) {
		printc(RED, "getXml", "Failed to read %s\n", TEMPLATE_XML_FILE_PATH);
		perror("fopen");
		return NO;
	}

	int seeked = fseek(f, 0L, SEEK_END);
	if (seeked != 0) {
		fclose(f);
		printc(RED, "getXml", "Failed to seek %s to end\n",
			   TEMPLATE_XML_FILE_PATH);
		perror("fseek");
		return NO;
	}
	long fileSize = ftell(f) + 1;
	if (fileSize == -1) {
		fclose(f);
		printc(RED, "getXml", "Failed ftell for %s\n", TEMPLATE_XML_FILE_PATH);
		perror("ftell");
		return NO;
	}

	char *disk_path = getNewVolume(vm_name, "G", disk);
	// this is the length of all strings combined (the ints will also be
	// converted to strings later on)
	int paramsSize =
		vm_name_length + getStrLength(memory) + getStrLength(currentMemory) +
		getStrLength(vcpus) + vm_type_length + strlen(GATEWAY_IP) +
		strlen(username) + strlen(password) + vm_name_length +
		strlen(QEMU_EMULATOR_FILE) + strlen(disk_path) + getStrLength(port);
	// this includes %d and %s
	int placeholdersSize = 11 * 2;
	int required_size = fileSize - placeholdersSize + paramsSize;
	*xml = malloc((size_t)(required_size + 1));
	char *template = malloc((size_t)(required_size + 1));
	seeked = fseek(f, 0L, SEEK_SET);
	if (seeked != 0) {
		fclose(f);
		printc(RED, "getXml", "Failed to seek %s to start\n",
			   TEMPLATE_XML_FILE_PATH);
		perror("fseek");
		return NO;
	}

	memset(*xml, 0, required_size + 1);
	memset(template, 0, required_size + 1);
	int last = fread(template, 1, required_size, f);
	if (last != required_size && ferror(f)) {
		fclose(f);
		printc(RED, "getXml", "Failed to read %s\n", TEMPLATE_XML_FILE_PATH);
		perror("fread");
		return NO;
	}
	template[last] = '\0';
	printc(INFO, "getXml", "template:\n%s\n", template);

	int written =
		snprintf(*xml, required_size, template, vm_name, memory, currentMemory,
				 vcpus, vm_type, GATEWAY_IP, username, password, vm_name,
				 QEMU_EMULATOR_FILE, disk_path, port);
	if (written > required_size) {
		// space was not enough, warn
		printc(WARN, "getXml",
			   "The xml buffer did not have enough space, output truncated!\n"
			   "Beware of errors later on!\n"
			   "Allocated space: %d, required_size: %d, paramsSize: %d, "
			   "placeholdersSize: %d, port: %d\n",
			   required_size - placeholdersSize + paramsSize + 1, required_size,
			   paramsSize, placeholdersSize, port);
	}

	fclose(f);
	free(template);

	printc(INFO, "getXml", "xml:\n%s\n", *xml);

	return YES;
}

/**
 * This function sets the username and password for a VM image in the tmp
 * directory and stores the absolute path
 */
int setUserData(char **userData, char *name, char *password) {
	FILE *fu = fopen(USERDATA_TEMPLATE_FILE_PATH, "r");
	if (fu == NULL) {
		printc(RED, "setUserData", "Failed to open %s\n",
			   USERDATA_TEMPLATE_FILE_PATH);
		perror("fopen");
		return NO;
	}

	int seeked = fseek(fu, 0L, SEEK_END);
	if (seeked != 0) {
		fclose(fu);
		printc(RED, "setUserData", "Failed to seek %s to end\n",
			   USERDATA_TEMPLATE_FILE_PATH);
		perror("fseek");
		return NO;
	}

	long fileSize = ftell(fu) + 1;
	if (fileSize == -1) {
		fclose(fu);
		printc(RED, "setUserData", "Failed ftell for %s\n",
			   USERDATA_TEMPLATE_FILE_PATH);
		perror("ftell");
		return NO;
	}

	// this is the length of all strings combined (the ints will also be
	// converted to strings later on)
	int paramsSize = strlen(name) + strlen(password);
	// this includes %d and %s
	int placeholdersSize = 2 * 2;
	int required_size = fileSize - placeholdersSize + paramsSize;
	*userData = malloc((size_t)(required_size + 1));
	char *template = malloc((size_t)(fileSize));
	seeked = fseek(fu, 0L, SEEK_SET);
	if (seeked != 0) {
		fclose(fu);
		printc(RED, "setUserData", "Failed to seek %s to start\n",
			   USERDATA_TEMPLATE_FILE_PATH);
		perror("fseek");
		free(*userData);
		free(template);
		return NO;
	}

	memset(*userData, 0, required_size + 1);
	memset(template, 0, fileSize);
	int last = fread(template, 1, fileSize, fu);
	if (last != fileSize && ferror(fu)) {
		fclose(fu);
		printc(RED, "setUserData", "Failed to read %s\n",
			   USERDATA_TEMPLATE_FILE_PATH);
		perror("fread");
		free(*userData);
		free(template);
		return NO;
	}
	template[last] = '\0';
	printc(INFO, "setUserData", "template:\n%s\n", template);

	int written = snprintf(*userData, required_size, template, name, password);
	if (written > required_size) {
		// space was not enough, warn
		printc(WARN, "setUserData",
			   "The xml buffer did not have enough space, output truncated!\n"
			   "Beware of errors later on!\n"
			   "Allocated space: %d, required_size: %d, paramsSize: %d, "
			   "placeholdersSize: %d\n",
			   required_size - placeholdersSize + paramsSize + 1, required_size,
			   paramsSize, placeholdersSize);
	}

	fclose(fu);
	free(template);

	printc(INFO, "setUserData", "userData:\n%s\n", *userData);

	return YES;
}

/**
 * This function sets the meta data for a VM image in the tmp directory and
 * stores the absolute path
 */
int setMetaData(char *vm_name, char **metaData) {
	FILE *fm = fopen(METADATA_TEMPLATE_FILE_PATH, "r");
	if (fm == NULL) {
		printc(RED, "setMetaData", "Failed to open %s\n",
			   METADATA_TEMPLATE_FILE_PATH);
		perror("fopen");
		return NO;
	}

	int seeked = fseek(fm, 0L, SEEK_END);
	if (seeked != 0) {
		fclose(fm);
		printc(RED, "setMetaData", "Failed to seek %s to end\n",
			   METADATA_TEMPLATE_FILE_PATH);
		perror("fseek");
		return NO;
	}

	long fileSize = ftell(fm) + 1;
	if (fileSize == -1) {
		fclose(fm);
		printc(RED, "setMetaData", "Failed ftell for %s\n",
			   METADATA_TEMPLATE_FILE_PATH);
		perror("ftell");
		return NO;
	}

	// this is the length of all strings combined (the ints will also be
	// converted to strings later on)
	int paramsSize = strlen(vm_name);
	// this includes %d and %s
	int placeholdersSize = 2 * 2;
	int required_size = fileSize - placeholdersSize + paramsSize;
	*metaData = malloc((size_t)(required_size + 1));
	char *template = malloc((size_t)(fileSize));
	seeked = fseek(fm, 0L, SEEK_SET);
	if (seeked != 0) {
		fclose(fm);
		printc(RED, "setMetaData", "Failed to seek %s to start\n",
			   METADATA_TEMPLATE_FILE_PATH);
		perror("fseek");
		free(*metaData);
		free(template);
		return NO;
	}

	memset(*metaData, 0, required_size + 1);
	memset(template, 0, fileSize);
	int last = fread(template, 1, fileSize, fm);
	if (last != fileSize && ferror(fm)) {
		fclose(fm);
		printc(RED, "setMetaData", "Failed to read %s\n",
			   METADATA_TEMPLATE_FILE_PATH);
		perror("fread");
		free(*metaData);
		free(template);
		return NO;
	}
	template[last] = '\0';
	printc(INFO, "setMetaData", "template:\n%s\n", template);

	int written = snprintf(*metaData, required_size, template, vm_name);
	if (written > required_size) {
		// space was not enough, warn
		printc(WARN, "setMetaData",
			   "The xml buffer did not have enough space, output truncated!\n"
			   "Beware of errors later on!\n"
			   "Allocated space: %d, required_size: %d, paramsSize: %d, "
			   "placeholdersSize: %d\n",
			   required_size - placeholdersSize + paramsSize + 1, required_size,
			   paramsSize, placeholdersSize);
	}

	fclose(fm);
	free(template);

	printc(INFO, "setMetaData", "metaData:\n%s\n", *metaData);

	return YES;
}

/**
 * Gets the length of a number as a string.
 * Could also be said to count the digits in a number
 */
int getStrLength(int num) {
	if (num == 0)
		return 1;
	int ans = 0;
	if (num < 0) {
		ans++;
		num = -num;
	}
	while (num != 0) {
		num /= 10;
		ans++;
	}
	return ans;
}

/**
 * This function gets the path to the new volume needed
 */
char *getNewVolume(char *vol_name, char *unit, int storage) {
	connectQemu();
	virStoragePoolPtr vspp = getPool("default");
	if (vspp == NULL) {
		return NULL;
	}

	virStorageVolPtr userVsvp = getVol(vol_name, unit, storage, vspp);
	return virStorageVolGetPath(userVsvp);
}

/**
 * This function gives an existing/new volume with the specified name
 */
virStorageVolPtr getVol(char *vol_name, char *unit, int storage,
						virStoragePoolPtr pool) {
	connectQemu();
	// get the read only image first
	virStorageVolPtr readOnlyImage =
		virStorageVolLookupByName(pool, READ_ONLY_VOL_NAME);
	if (readOnlyImage == NULL) {
		printc(RED, "getVol",
			   "Could not find the read only volume: '%s'. Make sure you have "
			   "it installed",
			   READ_ONLY_VOL_NAME);
		return NULL;
	}
	virStorageVolPtr vsvp = virStorageVolLookupByName(pool, vol_name);
	if (vsvp == NULL) {
		// create the volume
		char *xml = NULL;
		int got = getStorageXml(&xml, vol_name, unit, storage);
		if (got == NO) {
			printc(RED, "getVol", "Could not get volume's xml with name: %s\n",
				   vol_name);
			return NULL;
		}
		vsvp = virStorageVolCreateXML(pool, xml, 0);
		if (vsvp == NULL) {
			printc(RED, "getVol", "Could not create vol with name %s\n",
				   vol_name);
			return NULL;
		}
	}

	return vsvp;
}

/**
 * This function gives an existing/new pool with the specified name
 */
virStoragePoolPtr getPool(char *pool_name) {
	connectQemu();
	virStoragePoolPtr vspp = virStoragePoolLookupByName(vcp, pool_name);
	if (vspp == NULL) {
		// create the pool
		char *xml;
		int got = getStoragePoolXml(&xml, pool_name);
		if (got == NO) {
			printc(RED, "getPool", "Could not get pool with name: %s\n",
				   pool_name);
			return NULL;
		}
		vspp = virStoragePoolCreateXML(vcp, xml, 0);
		if (vspp == NULL) {
			printc(RED, "getPool", "Could not create pool with name %s\n",
				   pool_name);
			return NULL;
		}
		virStoragePoolBuild(vspp, 0);
		virStoragePoolCreate(vspp, 0);
		virStoragePoolSetAutostart(vspp, 1);
	} else {
		if (virStoragePoolIsActive(vspp) != 1) {
			virStoragePoolCreate(vspp, 0);
		}
	}

	virStoragePoolRefresh(vspp, 0);

	return vspp;
}

/**
 * This function gets the storage xml
 */
int getStorageXml(char **xml, char *vol_name, char *unit, int storage) {
	FILE *f = fopen(MEMORY_TEMPLATE_XML_FILE_PATH, "r");
	if (f == NULL) {
		printc(RED, "getStorageXml", "Failed to read %s\n",
			   MEMORY_TEMPLATE_XML_FILE_PATH);
		perror("fopen");
		return NO;
	}

	int seeked = fseek(f, 0L, SEEK_END);
	if (seeked != 0) {
		fclose(f);
		printc(RED, "getStorageXml", "Failed to seek %s to end\n",
			   MEMORY_TEMPLATE_XML_FILE_PATH);
		perror("fseek");
		return NO;
	}
	long fileSize = ftell(f) + 1;
	if (fileSize == -1) {
		fclose(f);
		printc(RED, "getStorageXml", "Failed ftell for %s\n",
			   MEMORY_TEMPLATE_XML_FILE_PATH);
		perror("ftell");
		return NO;
	}
	// this is the length of all strings combined (the ints will also be
	// converted to strings later on)
	int paramsSize = strlen(vol_name) + strlen(unit) + getStrLength(storage) +
					 strlen(READ_ONLY_VOL_NAME);
	// this includes %d and %s
	int placeholdersSize = 4 * 2;
	int required_size = fileSize - placeholdersSize + paramsSize;
	*xml = malloc((size_t)(required_size + 1));
	char *template = malloc((size_t)(fileSize));
	seeked = fseek(f, 0L, SEEK_SET);
	if (seeked != 0) {
		fclose(f);
		printc(RED, "getStorageXml", "Failed to seek %s to start\n",
			   MEMORY_TEMPLATE_XML_FILE_PATH);
		perror("fseek");
		return NO;
	}

	memset(*xml, 0, required_size + 1);
	memset(template, 0, fileSize);
	int last = fread(template, 1, fileSize, f);
	if (last != fileSize && ferror(f)) {
		fclose(f);
		printc(RED, "getStorageXml", "Failed to read %s\n",
			   MEMORY_TEMPLATE_XML_FILE_PATH);
		perror("fread");
		return NO;
	}
	template[last] = '\0';
	printc(INFO, "getStorageXml", "template:\n%s\n", template);

	int written = snprintf(*xml, required_size, template, vol_name, unit,
						   storage, READ_ONLY_VOL_NAME);
	if (written > required_size) {
		// space was not enough, warn
		printc(WARN, "getStorageXml",
			   "The xml buffer did not have enough space, output truncated!\n"
			   "Beware of errors later on!\n"
			   "Allocated space: %d, required_size: %d, paramsSize: %d, "
			   "placeholdersSize: %d\n",
			   required_size - placeholdersSize + paramsSize + 1, required_size,
			   paramsSize, placeholdersSize);
	}

	fclose(f);
	free(template);

	printc(INFO, "getStorageXml", "xml:\n%s\n", *xml);

	return YES;
}

/**
 * This function gets the storage xml
 */
int getStoragePoolXml(char **xml, char *pool_name) {
	FILE *f = fopen(POOL_TEMPLATE_XML_FILE_PATH, "r");
	if (f == NULL) {
		printc(RED, "getStoragePoolXml", "Failed to read %s\n",
			   POOL_TEMPLATE_XML_FILE_PATH);
		perror("fopen");
		return NO;
	}

	int seeked = fseek(f, 0L, SEEK_END);
	if (seeked != 0) {
		fclose(f);
		printc(RED, "getStoragePoolXml", "Failed to seek %s to end\n",
			   POOL_TEMPLATE_XML_FILE_PATH);
		perror("fseek");
		return NO;
	}
	long fileSize = ftell(f) + 1;
	if (fileSize == -1) {
		fclose(f);
		printc(RED, "getStoragePoolXml", "Failed ftell for %s\n",
			   POOL_TEMPLATE_XML_FILE_PATH);
		perror("ftell");
		return NO;
	}
	// this is the length of all strings combined (the ints will also be
	// converted to strings later on)
	int paramsSize = strlen(pool_name);
	// this includes %d and %s
	int placeholdersSize = 1 * 2;
	int required_size = fileSize - placeholdersSize + paramsSize;
	*xml = malloc((size_t)(required_size + 1));
	char *template = malloc((size_t)(required_size + 1));
	seeked = fseek(f, 0L, SEEK_SET);
	if (seeked != 0) {
		fclose(f);
		printc(RED, "getStoragePoolXml", "Failed to seek %s to start\n",
			   POOL_TEMPLATE_XML_FILE_PATH);
		perror("fseek");
		return NO;
	}

	memset(*xml, 0, required_size + 1);
	memset(template, 0, required_size + 1);
	int last = fread(template, 1, required_size, f);
	if (last != required_size && ferror(f)) {
		fclose(f);
		printc(RED, "getStoragePoolXml", "Failed to read %s\n",
			   POOL_TEMPLATE_XML_FILE_PATH);
		perror("fread");
		return NO;
	}
	template[last] = '\0';
	printc(INFO, "getStoragePoolXml", "template:\n%s\n", template);

	int written = snprintf(*xml, required_size, template, pool_name);
	if (written > required_size) {
		// space was not enough, warn
		printc(WARN, "getStoragePoolXml",
			   "The xml buffer did not have enough space, output truncated!\n"
			   "Beware of errors later on!\n"
			   "Allocated space: %d, required_size: %d, paramsSize: %d, "
			   "placeholdersSize: %d\n",
			   required_size - placeholdersSize + paramsSize + 1, required_size,
			   paramsSize, placeholdersSize);
	}

	fclose(f);
	free(template);

	printc(INFO, "getStoragePoolXml", "xml:\n%s\n", *xml);

	return YES;
}

/**
 * Gives the ip address from the ip string
 * The string has to be of the form "ddd.ddd.ddd.ddd"
 */
int toIp(char *ip, struct sockaddr_in *addr) {
	if (inet_pton(AF_INET, ip, &addr->sin_addr.s_addr) == 1) {
		return YES;
	} else {
		return NO;
	}
}

/**
 * Checks if the packet is valid
 * If the packet is valid returns the packet type
 * If the packet is invalid, returns NO
 */
int validPacket() {
	memcpy(hb, fd_buf, hb_size);

	// now check the packet_ID and packet_type
	if (hb->packet_ID == PACKET_ID) {
		return hb->packet_type;
	}
	return NO;
}

/**
 * Returns napi_value of js global
 */
napi_value getNapiGlobal(napi_env env) {
	napi_value res;
	napi_status status = napi_get_global(env, &res);
	if (status != napi_ok) {
		printNapiError(env, "getNapiGlobal");
		return NULL;
	}
	return res;
}

/**
 * Returns napi_value of js boolean (true, false)
 */
napi_value napiBool(napi_env env, int boolean) {
	napi_value res;
	napi_status status = napi_get_boolean(env, boolean, &res);
	if (status != napi_ok) {
		return napiUndefined(env);
	}
	return res;
}

/**
 * Returns napi_value of js int (32 bit)
 */
napi_value napiInt32(napi_env env, int i) {
	napi_value res;
	napi_status status = napi_create_int32(env, i, &res);
	if (status != napi_ok) {
		return napiUndefined(env);
	}
	return res;
}

/**
 * Returns napi_value of js undefined
 */
napi_value napiUndefined(napi_env env) {
	napi_value res;
	napi_status status = napi_get_undefined(env, &res);
	if (status != napi_ok) {
		return napiPanic(env);
	}
	return res;
}

/**
 * Throws a panic error, to be used in the worst case.
 */
napi_value napiPanic(napi_env env) {
	napi_throw_error(env, NULL, "Cannot return a valid N-API type");
	return NULL;
}

/**
 * Prints V8 error in an understandable manner
 */
void printNapiError(napi_env env, char *func_name) {
	const napi_extended_error_info *neei = NULL;
	napi_get_last_error_info(env, &neei);
	printc(RED, func_name, "Error: %s\n",
		   neei->error_message == NULL ? "Unknown N-API error"
									   : neei->error_message);
}
