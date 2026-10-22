#include "../include/all.h"
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

napi_value OnRequest(napi_env env, napi_callback_info info);
napi_value OnResponse(napi_env env, napi_callback_info info);
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

	napi_value OnRequestFN;
	napi_create_function(env, "onRequest", NAPI_AUTO_LENGTH, OnRequest, NULL,
						 &OnRequestFN);
	napi_set_named_property(env, exports, "onRequest", OnRequestFN);
	napi_set_named_property(env, exports, "ok", napiBool(env, 1));

	return exports;
}

NAPI_MODULE(NODE_GYP_GATEWAY, Start);

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
		printc(YLW, "OnRequest",
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

		int taskID;
		status = napi_get_value_int32(env, args[0], &taskID);
		if (status != napi_ok || taskID >= GATEWAY_CAPACITY) {
			printc(RED, "OnRequest",
				   "Could not get TaskID or TaskID out of range\n");
			printNapiError(env, "OnRequest - taskID");
			return napiUndefined(env);
		}

		if (taskID < 0) {
			// new request, add to the node list
		}
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
