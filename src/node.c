#include "../include/all.h"
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

struct DomainDef {
	int used;
	void *buf;
	int buf_filled;
	int buf_size;
	virDomainPtr vdp;
	virStreamPtr vsp;
	int frame_delay;
};

napi_value OnRequest(napi_env env, napi_callback_info info);
napi_value napiBool(napi_env env, int boolean);
napi_value napiUndefined(napi_env env);
napi_value napiPanic(napi_env env);

struct sockaddr_in *self_addr, *addr;
int addrLen = sizeof(struct sockaddr_in);
int mode = WORKER;
struct Node *nodes;
int hb_fd, timer_fd;
uint8_t *fd_buf;
int fdBufSize = sizeof(struct Node);
struct heartbeat_packet *hb;
int hb_size = sizeof(struct heartbeat_packet);
int tasks;
virConnectPtr vcp;
struct DomainDef domains[MAX_DOMAINS];
// TODO domains needs to be initialized and destroyed

napi_value Start(napi_env env, napi_value exports) {
	self_addr = (struct sockaddr_in *)malloc(addrLen);
	struct ifaddrs *ifa = malloc(sizeof(struct ifaddrs));
	getInterface(ifa);
	memcpy(self_addr, ifa->ifa_addr, addrLen);

	printc(INFO, "Start", "C code started running, ip: %s\n",
		   getPrintableIP(self_addr));

	if (strcmp(getPrintableIP(self_addr), GATEWAY_IP) == 0) {
		mode = GATEWAY;
	}

	printc(IMP, "Start", "Running in mode: %d\n", mode);

	connectQemu();

	napi_value OnRequestFN;
	napi_create_function(env, "onRequest", NAPI_AUTO_LENGTH, OnRequest, NULL,
						 &OnRequestFN);
	napi_set_named_property(env, exports, "onRequest", OnRequestFN);
	napi_value NewVMFN;
	napi_create_function(env, "newVM", NAPI_AUTO_LENGTH, NewVM, NULL, &NewVMFN);
	napi_set_named_property(env, exports, "newVM", NewVMFN);
	napi_set_named_property(env, exports, "ok", napiBool(env, 1));

	return exports;
}

NAPI_MODULE(NODE_GYP_GATEWAY, Start);

napi_value NewVM(napi_env env, napi_callback_info info) {
	printc(INFO, "NewVM", "new VM\n");

	napi_status status;
	size_t argc = 3;
	napi_value args[argc];

	status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);

	if (status != napi_ok) {
		printc(RED, "NewVM", "Could not parse args\n");
		return napiUndefined(env);
	}

	// domain name
	napi_valuetype value_type;
	status = napi_typeof(env, args[0], &value_type);
	if (status != napi_ok || value_type != napi_string) {
		printc(RED, "NewVM", "Could not get type of first argument\n");
		printNapiError(env, "NewVM");
		return napiUndefined(env);
	}

	size_t domainNameLength;
	status =
		napi_get_value_string_utf8(env, args[0], NULL, 0, &domainNameLength);
	if (status != napi_ok) {
		printc(RED, "NewVM", "Could not get domainNameLength\n");
		printNapiError(env, "NewVM");
		return napiUndefined(env);
	}

	char *domainName = malloc(domainNameLength + 1);
	status = napi_get_value_string_utf8(env, args[0], domainName,
										domainNameLength, NULL);
	domainName[domainNameLength] = '\0';

	// vcpus
	status = napi_typeof(env, args[1], &value_type);

	if (status != napi_ok || value_type != napi_number) {
		printc(RED, "NewVM", "Second argument not number\n");
		return napiUndefined(env);
	}

	int vcpus = 0;
	status = napi_get_value_int32(env, args[1], &vcpus);

	if (status != napi_ok || vcpus <= 0 || vcpus > MAX_VCPUS) {
		printc(RED, "NewVM", "Some error, number of VCPUs: %d\n", vcpus);
		return napiUndefined(env);
	}

	// vmemory
	status = napi_typeof(env, args[2], &value_type);

	if (status != napi_ok || value_type != napi_number) {
		printc(RED, "NewVM", "Third argument not number\n");
		return napiUndefined(env);
	}

	int memory = 0;
	status = napi_get_value_int32(env, args[2], &memory);

	if (status != napi_ok || memory <= 0 || memory > MAX_VMEM) {
		printc(RED, "NewVM", "Some error, VMEM demanded: %d\n", memory);
		return napiUndefined(env);
	}

	// frame delay
	status = napi_typeof(env, args[3], &value_type);

	if (status != napi_ok || value_type != napi_number) {
		printc(RED, "NewVM", "Fourth argument not number\n");
		return napiUndefined(env);
	}

	int frame_delay = 0;
	status = napi_get_value_int32(env, args[3], &frame_delay);

	if (status != napi_ok || frame_delay <= 0 ||
		frame_delay > MAX_FRAME_DELAY) {
		printc(RED, "NewVM", "Some error, frame delay demanded: %d\n",
			   frame_delay);
		return napiUndefined(env);
	}

	for (int i = 0; i < MAX_DOMAINS; i++) {
		if (domains[i].used == 0) {
			domains[i].used = 1;
			domains[i].vdp = getVM(domainName, vcpus, memory);
			domains[i].frame_delay = frame_delay;
			return napiBool(env, 1);
		}
	}

	printc(WARN, "NewVM", "No space left in VMM for more VMs\n");
	return napiBool(env, 0);
}

napi_value OnRequest(napi_env env, napi_callback_info info) {
	printc(INFO, "OnRequest", "request\n");

	if (mode == GATEWAY) {
		// the response function will be sent as an argument
		napi_status status;
		size_t argc = 2;
		napi_value args[argc];

		status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);

		if (status != napi_ok) {
			printc(RED, "OnRequest", "Could not parse args\n");
			return napiUndefined(env);
		}

		int taskID;
		status = napi_get_value_int32(env, args[0], &taskID);
		if (status != napi_ok || taskID < 0 || taskID >= GATEWAY_CAPACITY) {
			printc(RED, "OnRequest",
				   "Could not get TaskID or TaskID out of range\n");
			printNapiError(env, "OnRequest - taskID");
			return napiUndefined(env);
		}

		napi_valuetype value_type;
		status = napi_typeof(env, args[1], &value_type);

		if (status != napi_ok || value_type != napi_function) {
			printc(RED, "OnRequest", "First argument not function\n");
			return napiInt32(env, UNKNOWN);
		}

		// send the ip of the lowest loaded node
		int lowest = GATEWAY_CAPACITY;
		struct Node *n;
		for (int i = 0; i < GATEWAY_CAPACITY; i++) {
			if (nodes[i].load >= 0 && lowest > nodes[i].load) {
				lowest = nodes[i].load;
				n = &nodes[i];
			}
		}

		if (lowest < GATEWAY_CAPACITY) {
			// create the message to pass
			napi_value ip_str;
			status = napi_create_string_utf8(env, nodes[lowest].ip,
											 NAPI_AUTO_LENGTH, &ip_str);
			if (status != napi_ok) {
				printc(RED, "OnRequest", "Failed to create a string\n");
				return napiInt32(env, UNKNOWN);
			}

			// call the function
			napi_value fn_args = {ip_str};
			status = napi_call_function(env, getNapiGlobal(env), args[1], 1,
										fn_args, NULL);
			if (status != napi_ok) {
				printc(RED, "OnRequest", "Failed to call function\n");
				return napiInt32(env, UNKNOWN);
			}

			return napiInt32(env, YES);
		}

		// no node capable to serve, system at full capacity
		printc(WARN, "OnRequest",
			   "No node capable to serve, system at full capacity! Add new "
			   "nodes!");
		return napiInt32(env, NO);
	} else {
		// normal node, create a vm or give input to the vm
		// TODO
		printc(INFO, "OnRequest", "got the request");

		napi_status status;
		size_t argc = 1;
		napi_value args[argc];

		status = napi_get_cb_info(env, info, &argc, args, NULL, NULL);

		if (status != napi_ok) {
			printc(RED, "OnRequest", "Could not parse args\n");
			return napiUndefined(env);
		}

		size_t domain_name_length;
		status = napi_get_value_string_utf8(env, args[0], NULL, 0,
											&domain_name_length);
		if (status != napi_ok) {
			printc(RED, "OnRequest", "Could not get domain name string\n");
			printNapiError(env, "OnRequest - domain_name");
			return napiUndefined(env);
		}

		char *domain_name = malloc(domain_name_length + 1);
		napi_get_value_string_utf8(env, args[0], domain_name,
								   domain_name_length + 1, NULL);
		domain_name[domain_name_length] = '\0';

		int inputType = KEYBOARD;

		status = napi_get_value_int32(env, args[1], &inputType);
		if (status != napi_ok) {
			printc(RED, "OnRequest", "Could not get input type\n");
			printNapiError(env, "onRequest");
			return napiUndefined(env);
		} else if (inputType != KEYBOARD && inputType != MOUSE) {
			return napiBool(env, 0);
		}

		virDomainPtr vdp = getVM(domain_name, -1, -1);

		if (inputType == KEYBOARD) {
			int nkeys = 0;
			status = napi_get_value_int32(env, args[2], &nkeys);
			if (status != napi_ok) {
				printc(RED, "OnRequest", "Could not get nkeys\n");
				printNapiError(env, "onRequest");
				return napiUndefined(env);
			}

			int *keys[nkeys];
			for (int i = 0; i < nkeys; i++) {
				status = napi_get_value_int32(env, args[i + 3], &keys[i]);
				if (status != napi_ok) {
					printc(RED, "OnRequest", "Could not get key %d\n", i);
					printNapiError(env, "OnRequest");
					return napiUndefined(env);
				} else {
					keys[i] = NO;
				}
			}

			sendKeys(vdp, nkeys, keys);
		} else if (inputType == MOUSE) {
			int x, y, btn, pressed;

			status = napi_get_value_int32(env, args[2], &x);
			if (status != napi_ok) {
				printc(RED, "OnRequest", "Could not get x\n");
				printNapiError(env, "onRequest");
				return napiUndefined(env);
			}

			status = napi_get_value_int32(env, args[2], &y);
			if (status != napi_ok) {
				printc(RED, "OnRequest", "Could not get y\n");
				printNapiError(env, "onRequest");
				return napiUndefined(env);
			}

			status = napi_get_value_int32(env, args[2], &btn);
			if (status != napi_ok) {
				printc(RED, "OnRequest", "Could not get btn\n");
				printNapiError(env, "onRequest");
				return napiUndefined(env);
			}

			status = napi_get_value_int32(env, args[2], &pressed);
			if (status != napi_ok) {
				printc(RED, "OnRequest", "Could not get pressed\n");
				printNapiError(env, "onRequest");
				return napiUndefined(env);
			}

			sendCoords(vdp, x, y, btn, pressed);
		}
	}
}

// =============================================================
//                         VM Functions
// =============================================================

/**
 * Gets the virDomainPtr if the VM exists
 * else it creates a new VM and gives the virDomainPtr
 */
virDomainPtr getVM(char *domain_name, int vcpus, int memory) {
	connectQemu();

	virDomainPtr vdp = NULL;

	if ((vdp = domainExists(domain_name)) == NULL && vcpus > 0 && memory > 0) {
		char *xml;
		getXml(xml, domain_name, strlen(domain_name), memory, memory, vcpus,
			   "hvm", 4, "qemu", 5, "qeum.qcow2", 11);
		vdp = virDomainCreateXML(vcp, xml, 0);
	}

	return vdp;
}

void sendFrame(int domainNum) {
	struct DomainDef dd = domains[domainNum];
	if (dd.vdp == NULL) {
		printc(RED, "sendFrame", "NULL vdp\n");
		return UNKNOWN;
	}

	virStreamPtr vsp =
		dd.vsp == NULL ? virStreamNew(vcp, VIR_STREAM_NONBLOCK) : dd.vsp;
	if (vsp == NULL) {
		printc(RED, "sendFrame", "Could not get virStreamPtr\n");
		return UNKNOWN;
	}

	char *mime = virDomainScreenshot(dd.vdp, vsp, 0, 0);
	char *types = {"image/png", "image/x-portable-pixmap"};

	if (strcmp(mime, types[0]) == 0) {
		// png
		while (1) {
			int recved = virStreamRecv(vsp, dd.buf + dd.buf_filled,
									   dd.buf_size - dd.buf_filled);
			if (recved == -2) {
				// no data for now, come back later
				return NO;
			} else if (recved > 0) {
				// new data
				dd.buf_size += recved;
			} else if (recved == 0) {
				// end of data
				// TODO get the thread safe function to transmit the data
			} else {
				// some error
				printc(RED, "sendFrame",
					   "Some error occured while reading frame\n");
				perror("sendFrame");
				return UNKNOWN;
			}
		}
	} else if (strcmp(mime, types[1]) == 0) {
		// ppm
	}
}

// =============================================================
//                      System Functions
// =============================================================

/**
 * Simply registers heartbeats as they come
 */
void handle_heartbeats(uv_poll_t *handle, int status, int events) {
	while (1) {
		int res =
			getNextDGRAMPacket(hb_fd, fd_buf, fdBufSize, 0, addr, addrLen);

		if (res == EXIT_SUCCESS) {
			int packet_type = validPacket();

			// TODO when creating the array make all node loads to -1
			// also do the same when destroying a node entry
			if (packet_type == HEARTBEAT_PACKET) {
				// received a heartbeat from a node
				int found = NO;
				for (int i = 0; i < GATEWAY_CAPACITY; i++) {
					if (getCurrTime() - nodes[i].lastAnswered > EXPIRE_PERIOD) {
						nodes[i].load = -1;
					}
					if (nodes[i].load < 0 && found == NO) {
						found = i;
					}
					if (nodes[i].load >= 0 &&
						strcmp(nodes[i].ip, getPrintableIP(addr)) == 0) {
						nodes[i].load = hb->load;
						nodes[i].lastAnswered = getCurrTime();
						printc(INFO, "handle_heartbeats", "updated node: %d\n",
							   i);
						found = YES;
					}
				}

				if (found != YES && found != NO) {
					// new node and an empty gap
					memcpy(nodes[found].ip, getPrintableIP(addr),
						   INET_ADDRSTRLEN);
					printc(INFO, "handle_heartbeats", "new node: %d, %s\n",
						   found, nodes[found].ip);
					nodes[found].load = -1;
					nodes[found].lastAnswered = getCurrTime();
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

void handle_timer(uv_poll_t *handle, int status, int events) {
	readTimerFD(timer_fd);

	// send heartbeat
	hb->packet_ID = PACKET_ID;
	hb->packet_type = HEARTBEAT_PACKET;
	hb->load = tasks;
	toIp(GATEWAY_IP, addr);
	sendto(hb_fd, hb, hb_size, 0, addr, addrLen);
}

// =============================================================
//                          Utils
// =============================================================

/**
 * Receives the screen data and stores it non-blockingly
 */

/**
 * Send keyboard keys to the domain
 */
void sendKeys(virDomainPtr vdp, int nkeycodes, int *keys) {
	unsigned int keycodes[VIR_DOMAIN_SEND_KEY_MAX_KEYS];

	for (int i = 0; i < nkeycodes; i++) {
		keycodes[i % VIR_DOMAIN_SEND_KEY_MAX_KEYS] = keys[i];
		if (((i + 1) % VIR_DOMAIN_SEND_KEY_MAX_KEYS == 0) && keys[i] != NO) {
			virDomainSendKey(vdp, VIR_KEYCODE_SET_LINUX, 50, keycodes,
							 VIR_DOMAIN_SEND_KEY_MAX_KEYS, 0);
		}
	}
}

/**
 * Send mouse pointer to the domain
 * x and y have to be in range MIN_MOUSE_COORD and MAX_MOUSE_COORD
 * btn can either LEFT, RIGHT or MIDDLE
 * pressed can either be UP or DOWN
 */
void sendCoords(virDomainPtr vdp, int x, int y, int btn, int pressed) {
	if (x < MIN_MOUSE_COORD || x > MAX_MOUSE_COORD || y < MIN_MOUSE_COORD ||
		y > MAX_MOUSE_COORD) {
		printc(RED, "sendCoords", "x or y exceeded range %d %d\n", x, y);
		return;
	}

	if (btn != LEFT && btn != MIDDLE && btn != RIGHT) {
		printc(RED, "sendCoords", "Invalid btn %d\n", btn);
		return;
	}

	if (pressed != UP && pressed != DOWN) {
		printc(RED, "sendCoords", "Invalid pressed state %d\n", pressed);
		return;
	}

	char *btn_str = btn == LEFT ? "left" : btn == MIDDLE ? "middle" : "right";
	char *pressed_str = pressed == UP ? "false" : "true";

	char *json;
	getJson(json, x, y, btn_str, pressed_str);

	char *result = NULL;
	virDomainQemuMonitorCommand(vdp, json, *result, 0);
	if (result)
		free(result);
}

/**
 * Check the connection and create one if dead
 */
void connectQemu() {
	if (vcp == NULL || virConnectIsAlive(vcp) <= 0) {
		if (vcp != NULL) {
			virConnectClose(vcp);
		}
		vcp = virConnectOpen("qemu:///system");
		if (vcp == NULL) {
			printc(RED, "createVM", "Could not open connection");
			perror("virConnectOpen");
		}
	}
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
 * Fill in the qemu command json
 */
int getJson(char *json, int x, int y, int btn, int pressed) {
	FILE *f = fopen(QEMU_CMD_TEMPLATE_FILE_PATH, "r");
	if (f == NULL) {
		printc(RED, "getJson", "Failed to read %s\n",
			   QEMU_CMD_TEMPLATE_FILE_PATH);
		perror("fopen");
		return NO;
	}

	int seeked = fseek(f, 0L, SEEK_END);
	if (seeked != 0) {
		fclose(f);
		printc(RED, "getJson", "Failed to seek %s to end\n",
			   QEMU_CMD_TEMPLATE_FILE_PATH);
		perror("fseek");
		return NO;
	}
	long fileSize = ftell(f) + 1;
	if (fileSize == -1) {
		fclose(f);
		printc(RED, "getJson", "Failed ftell for %s\n",
			   QEMU_CMD_TEMPLATE_FILE_PATH);
		perror("ftell");
		return NO;
	}
	// this is the length of all strings combined (the ints will also be
	// converted to strings later on)
	int paramsSize = getStrLength(x) + getStrLength(y) + getStrLength(btn) +
					 getStrLength(pressed);
	// this includes %d and %s
	int placeholdersSize = 4 * 2;
	json = malloc((size_t)(fileSize - placeholdersSize + paramsSize + 1));
	seeked = fseek(f, 0L, SEEK_SET);
	if (seeked != 0) {
		fclose(f);
		printc(RED, "getJson", "Failed to seek %s to start\n",
			   QEMU_CMD_TEMPLATE_FILE_PATH);
		perror("fseek");
		return NO;
	}

	memset(json, NULL, fileSize + 1);
	int last = fread(json, 1, fileSize, f);
	if (last != fileSize && ferror(f)) {
		fclose(f);
		printc(RED, "getJson", "Failed to read %s\n",
			   QEMU_CMD_TEMPLATE_FILE_PATH);
		perror("fread");
		return NO;
	}
	json[last] = '\0';

	int written = snprintf(json, fileSize, x, y, btn, pressed);
	if (written > fileSize) {
		// space was not enough, warn
		printc(WARN, "getJson",
			   "The json buffer did not have enough space, output truncated!\n"
			   "Beware of errors later on!\n"
			   "Allocated space: %d, fileSize: %d, paramsSize: %d, "
			   "placeholdersSize: %d\n",
			   fileSize - placeholdersSize + paramsSize + 1, fileSize,
			   paramsSize, placeholdersSize);
	}

	fclose(f);
}

/**
 * Fill in the xml file
 */
int getXml(char *xml, char *vm_name, int vm_name_length, int memory,
		   int currentMemory, int vcpus, char *vm_type, int vm_type_length,
		   char *emulator_name, int emulator_name_length, char *vdisk_file,
		   int vdisk_file_length) {
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
	// this is the length of all strings combined (the ints will also be
	// converted to strings later on)
	int paramsSize = vm_name_length + getStrLength(memory) +
					 getStrLength(currentMemory) + getStrLength(vcpus) +
					 vm_type_length + emulator_name_length + vdisk_file_length;
	// this includes %d and %s
	int placeholdersSize = 7 * 2;
	xml = malloc((size_t)(fileSize - placeholdersSize + paramsSize + 1));
	seeked = fseek(f, 0L, SEEK_SET);
	if (seeked != 0) {
		fclose(f);
		printc(RED, "getXml", "Failed to seek %s to start\n",
			   TEMPLATE_XML_FILE_PATH);
		perror("fseek");
		return NO;
	}

	memset(xml, NULL, fileSize + 1);
	int last = fread(xml, 1, fileSize, f);
	if (last != fileSize && ferror(f)) {
		fclose(f);
		printc(RED, "getXml", "Failed to read %s\n", TEMPLATE_XML_FILE_PATH);
		perror("fread");
		return NO;
	}
	xml[last] = '\0';

	int written = snprintf(xml, fileSize, vm_name, memory, currentMemory, vcpus,
						   vm_type, emulator_name, vdisk_file);
	if (written > fileSize) {
		// space was not enough, warn
		printc(WARN, "getXml",
			   "The xml buffer did not have enough space, output truncated!\n"
			   "Beware of errors later on!\n"
			   "Allocated space: %d, fileSize: %d, paramsSize: %d, "
			   "placeholdersSize: %d\n",
			   fileSize - placeholdersSize + paramsSize + 1, fileSize,
			   paramsSize, placeholdersSize);
	}

	fclose(f);
}

int getStrLength(int num) {
	int ans = 0;
	while (num != 0) {
		num = num >> 1;
		ans++;
	}
	return ans;
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

napi_value getNapiGlobal(napi_env env) {
	napi_value res;
	napi_status status = napi_get_global(env, &res);
	if (status != napi_ok) {
		printNapiError(env, "getNapiGlobal");
		return NULL;
	}
	return res;
}

napi_value napiBool(napi_env env, int boolean) {
	napi_value res;
	napi_status status = napi_get_boolean(env, boolean, &res);
	if (status != napi_ok) {
		return napiUndefined(env);
	}
	return res;
}

napi_value napiInt32(napi_env env, int i) {
	napi_value res;
	napi_status status = napi_create_int32(env, i, &res);
	if (status != napi_ok) {
		return napiUndefined(env);
	}
	return res;
}

napi_value napiUndefined(napi_env env) {
	napi_value res;
	napi_status status = napi_get_undefined(env, &res);
	if (status != napi_ok) {
		return napiPanic(env);
	}
	return res;
}

napi_value napiPanic(napi_env env) {
	napi_throw_error(env, NULL, "Cannot return a valid N-API type");
	return NULL;
}

void printNapiError(napi_env env, char *func_name) {
	const napi_extended_error_info *neei = NULL;
	napi_get_last_error_info(env, &neei);
	printc(RED, func_name, "Error: %s\n",
		   neei->error_message == NULL ? "Unknown N-API error"
									   : neei->error_message);
}
