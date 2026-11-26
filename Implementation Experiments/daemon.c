#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/wait.h>
#include <spawn.h>

//#define SERVER_PORT 5432
#define MAX_PENDING 5
#define MAX_LINE 1024

void usage(void);

typedef struct {
	int s;
	char* dest;
} peerThreadArg;
typedef struct {
	int s;
	int port;
} orchesThreadArg;
typedef struct {
	char* ip;
	int port;
	char* file;
}reqThreadArg;

void *peer_handler(void *arg){
	peerThreadArg info = (peerThreadArg *) arg;
	char buf[MAX_LINE];
	char *token;
  	char *f;
	pid_t pid;

	while (len = recv(info->s, buf, sizeof(buf), 0)){
      		fputs(buf, stdout);
      		token = strtok(buf," ");
      		if(strcmp(token, "G") == 0){
        		token = strtok(NULL," ");
			token[strlen(token)-1] = '\0';
			f = strcat(token, ".qcow2")

			char *argv[] = {"rsync","-aS","--partial","--inplace",(char *)strcat("/var/lib/libvirt/images/",f),(char *)strcat(info->dest,f),NULL};
			int status = posix_spawnp(&pid, "rsync", NULL, NULL, argv, environ);
    			if ((status == 0) & (waitpid(pid, &status, 0) != -1) & WIFEXITED(status) & (WEXITSTATUS(status) == 0)) {
        			send(info->s,"S", 1, 0);
			}else{
				send(info->s,"F", 1, 0);
				break;
			}
      		}elif(strcmp(token, "S") == 0)){
			token = strtok(NULL," ");
			delete_vm(token, true);
			break;
		}
    	}
    	close(info->s);

	return NULL;
}
void *requester(void *arg){
	reqThreadArg info = (reqThreadArg*) arg;
	struct sockaddr_in sin;
  	char buf[MAX_LINE];
  	int s;
  	int len;

	/* build address data structure */
  	bzero((char *)&sin, sizeof(sin));
  	sin.sin_family = AF_INET;
  	inet_pton(AF_INET, info->ip, &sin.sin_addr);
  	sin.sin_port = htons(info->port);

	/* active open */
  	if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
    		perror("simplex-talk: socket");
    		exit(1);
  	}else
    		printf("Client created socket.\n");

	if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0){
	      perror("simplex-talk: connect");
      	      close(s);
      	      exit(1);
    	}else
    		printf("Client connected.\n");

  	/* main loop: get and send lines of text */
  	buf = info->file;
    	buf[MAX_LINE-1] = '\0';
    	len = strlen(buf) + 1;
    	send(s, buf, len, 0);
    	recv(s, buf, sizeof(buf), 0);
	if(strcmp(buf, "S") == 0){
		return (void*)(intptr_t)0;
	}else if(strcmp(buf, "F") == 0){
		recv(s, buf, sizeof(buf), 0);
		if(strcmp(buf, "S") == 0){
			return (void*)(intptr_t)0;
		}
	}

	return (void*)(intptr_t)-1;
}
void *orches_handler(void *arg){
	orchesThreadArg info = (orchesThreadArg *) arg;
	char buf[MAX_LINE];
	char *token;
	char *f;
	pthread_t tid;
	void* thread_return;

	while(len = recv(info->s, buf, sizeof(buf), 0)){
		token = strtok(buf, " ");
		if(strcmp(token, "H") == 0){
			//send back stats of cureent resource utilization
		}else if(strcmp(token, "L") == 0){
                        if(launch_vm(strtok(NULL," ")) == 0){
                                send(info->s, "S", 1, 0);
                        }else{
                                send(info->s, "F", 1, 0);
                        }
                }else if(strcmp(token, "F") == 0){
			reqThreadArg *arg = {strtok(NULL," "), info->port, strtok(NULL, " ")};
                        pthread_create(&tid, NULL, requester, arg);

			pthread_join(thread, &thread_return);

			if(*(int*)thread_return == 0){
	                        if(create_vm(arg->file, atoi(strtok(NULL," ")), atoi(strtok(NULL," ")), 0, strtok(NULL," ")) == 0){
        	                        send(info->s, "S", 1, 0);
                	        }else{
                        	        send(info->s, "CF", 2, 0);
                        	}
			}else{
				send(info->s, "F", 1, 0);
			}
                }else if(strcmp(token, "C") == 0){
			if(create_vm(strtok(NULL," "), atoi(strtok(NULL, " ")), atoi(strtok(NULL, " ")), atoi(strtok(NULL, " ")), strtok(NULL," ")) == 0){
				send(info->s, "S", 1, 0);
			}else{
				send(info->s, "F", 1, 0);
			}
		}else if(strcmp(token, "D") == 0){
                        if(delete_vm(strtok(NULL," "), atoi(strtok(NULL," "))) == 0){
                                send(info->s, "S", 1, 0);
                        }else{
                                send(info->s, "F", 1, 0);
                        }
                }
	}
	close(info->s);

	return NULL;
}

int main(int argc, char *argv[]){

  struct sockaddr_in sin;
  socklen_t len;
  int s, new_s;
  char str[INET_ADDRSTRLEN];
  char c;
  int i=0;
  int server_port;
  struct in_addr orches_ip;
  pthread_t tid;

  while((c = getopt(argc, argv, "p:")) != -1){
	switch(c){
		case 'p':
			server_port = atoi(optarg);
			break;
		default:
			usage();
	}
  }

  /* build address data structure */
  bzero((char *)&sin, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = INADDR_ANY;
  sin.sin_port = htons(SERVER_PORT);

  /* setup passive open */
  if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
    perror("simplex-talk: socket");
    exit(1);
  }

  inet_ntop(AF_INET, &(sin.sin_addr), str, INET_ADDRSTRLEN);
  printf("Server is using address %s and port %d.\n", str, SERVER_PORT);

  if ((bind(s, (struct sockaddr *)&sin, sizeof(sin))) < 0) {
    perror("simplex-talk: bind");
    exit(1);
  }
  else
    printf("Server bind done.\n");

  listen(s, MAX_PENDING);

  /* wait for connection, then receive and print text */
  while(1) {
    if ((new_s = accept(s, (struct sockaddr *)&sin, &len)) < 0) {
      perror("simplex-talk: accept");
      exit(1);
    }

    inet_pton(AF_INET, "", &orches_ip);
    if(sin.sin_addr.s_addr == orches_ip.s_addr){
	orchesThreadArg *arg = {new_s, server_port};

	if(pthread_create(&tid, NULL, orches_handler, arg) != 0){
		close(new_s);
		continue;
	}
    }else{
	dest = strcat(strcat("admin@",orches_ip.s_addr),":/var/lib/libvirt/images/");
	peerThreadArg *arg = {new_s, dest};

	if(pthread_create(&tid, NULL, peer_handler, arg) != 0){
		close(new_s);
		continue;
	}
    }

    pthread_detach(tid);
  }
}

void usage(void){
	printf("Flag Usage:\n");
	printf("-p <port number>\n");
	exit(1);
}
