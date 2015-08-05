/*
Copyright 2013-present Barefoot Networks, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/queue.h>
#include <sys/types.h>

#include <p4ns_common/ctl_messages.h>
#include <p4ns_common/p4ns_db.h>
#include <p4ns_common/p4ns_utils.h>

#include <BMI/bmi_port.h>

#include <getopt.h>
#include <assert.h>
#include <signal.h>

#include <p4_sim/rmt.h>
#include <p4_sim/pd_rpc_server.h>

#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

TAILQ_HEAD(interfaces_head, entry) interfaces;
struct entry {
  char *str;
  TAILQ_ENTRY(entry) entries;
};

static bmi_port_mgr_t *port_mgr;
static uint64_t dpid = 0;
static char *listener_str = NULL;
static char *datapath_name = "p4ns";
static char *p4nsdb_str = NULL;
static char *pd_server_str = NULL;
static int dump_pcap = 1;
static p4ns_tcp_over_ip_t p4nsdb_addr;
static p4ns_tcp_over_ip_t listener_addr;
static p4ns_tcp_over_ip_t pd_server_addr;
static bool no_veth = false;

pthread_t ctl_listener_thread;

/**
 * The maximum number of ports to support:
 * @fixme should be runtime parameter
 */
#define PORT_COUNT 256
#define PD_SERVER_DEFAULT_PORT 9090

struct sigaction old_action_SIGTERM;
struct sigaction old_action_SIGINT;

#define NUM_VETH_INTERFACES   9

/**
 * Check an operation and return if there's an error.
 */
#define CHECK(op)                                                       \
    do {                                                                \
        int _rv;                                                        \
        if ((_rv = (op)) < 0) {                                         \
            fprintf(stderr, "%s: ERROR %d at %s:%d",                    \
                    #op, _rv, __FILE__, __LINE__);                      \
            return _rv;                                                 \
        }                                                               \
    } while (0)

#ifdef SWITCHAPI_ENABLE
extern int switch_api_init(int);
extern int start_switch_api_rpc_server(void);
extern int start_switch_api_packet_driver(void);
#endif /* SWITCHAPI_ENABLE */

#ifdef SWITCHSAI_ENABLE
#define SWITCH_SAI_THRIFT_RPC_SERVER_PORT 9092
extern int start_p4_sai_thrift_rpc_server(int port);
#endif /* SWITCHSAI_ENABLE */

#ifdef SWITCHLINK_ENABLE
extern int switchlink_init(void);
#endif /* SWITCHLINK_ENABLE */

static int add_port(char *iface, uint16_t port_num) {
  fprintf(stderr, "switch is adding port %s as %u\n", iface, port_num);
  if (dump_pcap != 0) {
    char pcap_filename[1024];
    snprintf(pcap_filename, 1024,
      "/p4ns.%s-port%.2d.pcap", datapath_name, port_num);
    return bmi_port_interface_add(port_mgr, iface, port_num, pcap_filename);
  }
  else {
    return bmi_port_interface_add(port_mgr, iface, port_num, NULL);
  }
}

static int del_port(int iface) {
  fprintf(stderr, "switch is deleting port %d\n", iface);
  return bmi_port_interface_remove(port_mgr, iface);
}

static int send_status_reply(int client, uint64_t request_id, int status) {
  ctl_msg_status_t msg;
  msg.code = CTL_MSG_STATUS;
  msg.request_id = request_id;
  msg.status = status;
  sendall(client, (char *) &msg, sizeof(ctl_msg_status_t));
  return 0;
}

static int process_add_port(int client) {
  ctl_msg_add_port_t msg;
  msg.code = CTL_MSG_ADD_PORT_CODE;
  int n = recvall(client, ((char *) &msg) + 1, sizeof(ctl_msg_add_port_t) - 1);
  assert(n == sizeof(ctl_msg_add_port_t) - 1);
//int status = bmi_port_interface_add(port_mgr, msg.iface, msg.port_num); //send_status_reply(client, msg.request_id, status);
  return 0;
}

static int process_del_port(int client) {
  ctl_msg_del_port_t msg;
  msg.code = CTL_MSG_DEL_PORT_CODE;
  int n = recvall(client, ((char *) &msg) + 1, sizeof(ctl_msg_del_port_t) - 1);
  assert(n == sizeof(ctl_msg_del_port_t) - 1);
  errno = 0;
  int port_num = strtol(msg.iface, NULL, 10);
  // FIXME: Not sure how to convert interface string to int.
  assert(errno == 0);
  int status = del_port(port_num);
  send_status_reply(client, msg.request_id, status);
  return 0;
}

static int process_request(int client) {
  char msg_code;
  int n = recvall(client, &msg_code, 1);
  if (n <= 0) return n;
  switch(msg_code) {
  case CTL_MSG_ADD_PORT_CODE:
    process_add_port(client);
    return CTL_MSG_ADD_PORT_CODE;
  case CTL_MSG_DEL_PORT_CODE:
    process_del_port(client);
    return CTL_MSG_DEL_PORT_CODE;
  default:
    printf("Unknown message format\n");
  }
  return -1;
}

static void *ctl_listen(void *arg) {
  p4ns_tcp_over_ip_t listener = *(p4ns_tcp_over_ip_t *) arg;

  printf("ctl port: Starting TCP server\n");

  int listenfd,connfd,n;
  struct sockaddr_in servaddr,cliaddr;
  socklen_t clilen;

  listenfd=socket(AF_INET,SOCK_STREAM,0);

  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  /* inet_pton(AF_INET, listener.ip, &servaddr.sin_addr.s_addr); */
  servaddr.sin_addr.s_addr=htonl(INADDR_ANY);
  servaddr.sin_port=htons(listener.port);
  bind(listenfd,(struct sockaddr *)&servaddr,sizeof(servaddr));

  listen(listenfd,1024);

  /* Evrything runs in the same thread -> only one client is expected */
  for(;;)
    {
      clilen=sizeof(cliaddr);
      connfd = accept(listenfd,(struct sockaddr *)&cliaddr,&clilen);
      printf("ctl port: Connection opened\n");

      while((n = process_request(connfd)) > 0);
      printf("ctl port: Connection closed\n");
      close(connfd);
    }
  return NULL;
}

/* Wrapper for transmit operation */
static void
transmit_wrapper(p4_port_t egress, void *pkt, int len) {
    if (bmi_port_send(port_mgr, egress, pkt, len) < 0) {
        printf("Error sending packet\n");
    }
}


static void
packet_handler(int port_num, const char *buffer, int length)
{
    /* @fixme log vector */
    printf("Packet in on port %d length %d; first bytes:\n", port_num, length);
    int i = 0;
    for (i = 0; i < 16; i++) {
        if (i && ((i % 4) == 0)) {
            printf(" ");
        }
        printf("%02x", (uint8_t) buffer[i]);
    }
    printf("\n");
    printf("rmt proc returns %d\n", rmt_process_pkt(port_num, (char*)buffer, length));
}


extern int lg_pd_ucli_create(char *prompt);
extern int lg_pd_ucli_thread_spawn(void);


static void
parse_options(int argc, char **argv)
{
  struct entry *np = NULL;
  TAILQ_INIT(&interfaces);

  while (1) {
    int option_index = 0;
    /* Options without short equivalents */
    enum long_opts {
      OPT_START = 256,
      OPT_NAME,
      OPT_DPID,
      OPT_VERSION,
      OPT_P4NSDB,
      OPT_NOPCAP,
      OPT_PDSERVER,
      OPT_NOVETH,
    };
    static struct option long_options[] = {
      {"verbose", no_argument, 0, 'v' },
      {"trace", no_argument, 0, 't' },
      {"interface", required_argument, 0, 'i' },
      {"listener", required_argument, 0, 'l' },
      {"dpid", required_argument, 0, OPT_DPID },
      {"help", no_argument, 0, 'h' },
      {"version", no_argument, 0, OPT_VERSION },
      {"name", required_argument, 0, OPT_NAME },
      {"p4nsdb", required_argument, 0, OPT_P4NSDB },
      {"pd-server", required_argument, 0, OPT_PDSERVER },
      {"no-pcap", no_argument, 0, OPT_NOPCAP },
      {"no-veth", no_argument, 0, OPT_NOVETH },
      {0, 0, 0, 0 }
    };
    int c = getopt_long(argc, argv, "vtl:i:h",
                        long_options, &option_index);
    if (c == -1) {
      break;
    }
    switch (c) {
    case 'v':
      assert(0);
      /* loglevel = LOGLEVEL_VERBOSE; */
      break;
    case 't':
      assert(0);
      /* loglevel = LOGLEVEL_TRACE; */
      break;
    case 'l':
      listener_str = strdup(optarg);
      break;
    case 'i':
      np = malloc(sizeof(struct entry));
      np->str = strdup(optarg);
      TAILQ_INSERT_TAIL(&interfaces, np, entries);
      break;
    case OPT_NAME:
      datapath_name = strdup(optarg);
      break;
    case OPT_DPID:
      /* AIM_ASSERT(optarg != NULL, "clang-analyzer workaround"); */
      dpid = strtoll(optarg, NULL, 16);
      break;
    case OPT_VERSION:
      printf("This is the first version ever, duh!\n");
      exit(0);
      break;
    case OPT_P4NSDB:
      p4nsdb_str = strdup(optarg);
      break;
    case OPT_PDSERVER:
      pd_server_str = strdup(optarg);
      break;
    case OPT_NOPCAP:
      dump_pcap = 0;
      break;
    case OPT_NOVETH:
      no_veth = true;
      break;
    case 'h':
    case '?':
      printf("ivs: Barefoot Networks Virtual Switch\n");
      printf("Usage: ivs [OPTION]...\n");
      printf("\n");
      printf(" -v, --verbose Verbose logging\n");
      printf(" -t, --trace Very verbose logging\n");
      printf(" -l, --listener=IP:PORT Listen for connections\n");
      printf(" --p4nsdb=IP:PORT Connect to the P4NSDB\n");
      printf(" --pd-server=IP:PORT Listen for PD RPC calls\n");
      printf(" --no-veth No veth interfaces\n");
      printf(" --no-pcap Do not dump to pcap files\n");
      printf(" -i, --interface=INTERFACE Attach a network interface at startup\n");
      printf(" --name=NAME Set the name of the datapath (default p4ns)\n");
      printf(" --dpid=DPID Set datapath ID (default autogenerated)\n");
      printf(" -h,--help Display this help message and exit\n");
      printf(" --version Display version information and exit\n");
      exit(c == 'h' ? 0 : 1);
      break;
    }
  }
}


static p4ns_db_cxt_t connect_to_p4nsdb() {
  p4ns_db_cxt_t c = p4ns_db_connect(p4nsdb_addr.ip, p4nsdb_addr.port);
  if(!c) {
    fprintf(stderr, "Could not connect to P4NS DB\n");
    return c;
  }
  fprintf(stderr, "Connected to P4NS DB\n");
  return c;
}

static void clean_up() {
  p4ns_db_cxt_t c = p4ns_db_connect(p4nsdb_addr.ip, p4nsdb_addr.port);
  if(c) {
    p4ns_db_del_datapath(c, datapath_name);
  }
}

static void clean_up_on_signal(int sig) {
  clean_up();
  sigaction(SIGINT, &old_action_SIGINT, NULL);
  sigaction(SIGTERM, &old_action_SIGTERM, NULL);
  raise(sig);
}

int
main(int argc, char* argv[])
{
    int rv = 0;
    char veth_name[16];
    int n_veth;
  
    CHECK(bmi_port_create_mgr(&port_mgr));

    srand (time(NULL));

    parse_options(argc, argv);

    /*************************************************************
     * Check for root access as uses veth interfaces
     * @fixme allow specifying vpi names as runtime config
     ************************************************************/
    if (geteuid() != 0) { 
        fprintf(stderr, "This uses (v)eth interfaces; run under sudo\n");
        return 1; 
    }

    if (!dpid) {
        int r1 = rand();
        int r2 = rand();
        dpid = (((uint64_t) r1) << 32) | ((uint64_t) r2);
    }

    if (!pd_server_str) {
        fprintf(stderr, "No PD RPC server address specified, "
                "using 127.0.0.1:%u\n", PD_SERVER_DEFAULT_PORT);
        parse_connection("127.0.0.1", &pd_server_addr, PD_SERVER_DEFAULT_PORT);
    } else {
        if (parse_connection(pd_server_str, &pd_server_addr,
                             PD_SERVER_DEFAULT_PORT) != 0)
            return -1;
        fprintf(stderr, "PD server address is %s:%u\n",
                pd_server_addr.ip, pd_server_addr.port);
    }

    if (!listener_str) {
        fprintf(stderr, "No listener specified, switch will run in "
                "standalone mode\n");
        if (p4nsdb_str) {
            fprintf(stderr, "P4NSDB will be ignored\n");
            free(p4nsdb_str);
            p4nsdb_str = NULL;
        }
    }

    /*************************************************************
     * Initialize Modules. 
     ************************************************************/
    
    rmt_init();
    rmt_logger_set((p4_logging_f) printf);
    rmt_log_level_set(P4_LOG_LEVEL_TRACE);
    rmt_transmit_register(transmit_wrapper);

    /* Start up the PD RPC server */
    CHECK(start_p4_pd_rpc_server(pd_server_addr.port));

    /* Start up the API RPC server */
#ifdef SWITCHAPI_ENABLE
    CHECK(switch_api_init(0));
    CHECK(start_switch_api_rpc_server());
    CHECK(start_switch_api_packet_driver());
#endif /* SWITCHAPI_DISABLE */

#ifdef SWITCHSAI_ENABLE
    CHECK(start_p4_sai_thrift_rpc_server(SWITCH_SAI_THRIFT_RPC_SERVER_PORT));
#endif /*SWITCHSAI_ENABLE */

#ifdef SWITCHLINK_ENABLE
    CHECK(switchlink_init());
#endif /* SWITCHLINK_ENABLE */

    if (!listener_str && !no_veth) {  /* standalone mode */
        for (n_veth = 0; n_veth < NUM_VETH_INTERFACES; n_veth++) {
            sprintf(veth_name, "veth%d", n_veth*2);
            char pcap_filename[1024];
            pcap_filename[0] = 0;
            if (dump_pcap) {
                snprintf(pcap_filename, 1024,
                         "p4ns.%s-port%.2d.pcap", datapath_name, n_veth);
            }
            CHECK(bmi_port_interface_add(port_mgr, veth_name, n_veth,
                  pcap_filename));
        }
    }

#ifdef SWITCHAPI_ENABLE
    if (!listener_str) {
        // add CPU port, port 64
        char pcap_filename[1024];
        pcap_filename[0] = 0;
        if (dump_pcap) {
            snprintf(pcap_filename, 1024,
                     "p4ns.%s-port%.2d.pcap", datapath_name, 64);
        }
        CHECK(bmi_port_interface_add(port_mgr, "veth250", 64, pcap_filename));
    }
#endif /* SWITCHAPI_ENABLE */

    if (listener_str) {
        parse_connection(listener_str, &listener_addr, 0);
        if(listener_addr.port == 0) {
            fprintf(stderr, "No port was specified for the listener");
            return -1;
        }

        if (!p4nsdb_str) {
            fprintf(stderr, "No p4nsdb address specified, using 127.0.0.1:%u\n",
                    P4NSDB_DEFAULT_PORT);
            parse_connection("127.0.0.1", &p4nsdb_addr, P4NSDB_DEFAULT_PORT);
        }
        else {
            if (parse_connection(p4nsdb_str, &p4nsdb_addr,
                                 P4NSDB_DEFAULT_PORT) != 0)
                return -1;
            fprintf(stderr, "p4nsdb address is %s:%u\n",
                    p4nsdb_addr.ip, p4nsdb_addr.port);
        }

        struct sigaction sa;
        sa.sa_handler = clean_up_on_signal;
        sigaction(SIGINT, &sa, &old_action_SIGINT);
        sigaction(SIGTERM, &sa, &old_action_SIGTERM);

        p4ns_db_cxt_t c = connect_to_p4nsdb();
        if (!c) return -1;
        if (p4ns_db_add_datapath(c, datapath_name, dpid)){
            fprintf(stderr, "P4NSDB: could not create datapath, %s already "
                    "exists\n", datapath_name);
            return -1;
        }
        atexit(clean_up);
        p4ns_db_set_listener(c, datapath_name, &listener_addr);

        /* TODO: improve this code */
        uint16_t port_no = 0;
        /* Add interfaces from command line */
        struct entry *np;
        for (np = interfaces.tqh_first; np != NULL; np = np->entries.tqe_next) {
            printf("Adding interface %s (port %d)\n", np->str, port_no);
            if(add_port(np->str, port_no) < 0) {
                printf("Failed to add interface %s\n", np->str);
                return -1;
            }
            p4ns_db_add_port(c, datapath_name, np->str, port_no);
            port_no++;
        }

        p4ns_db_free(c);

        pthread_create(&ctl_listener_thread, NULL,
                       ctl_listen, (void *) &listener_addr);
    } else if (no_veth) {
        uint16_t port_no = 0;
        struct entry *np;
        for (np = interfaces.tqh_first; np != NULL; np = np->entries.tqe_next) {
            printf("Adding interface %s (port %d)\n", np->str, port_no);
            if(add_port(np->str, port_no) < 0) {
                printf("Failed to add interface %s\n", np->str);
                return -1;
            }
            port_no++;
        }
    }

    CHECK(bmi_set_packet_handler(port_mgr, packet_handler));

    while (1) pause();

    bmi_port_destroy_mgr(port_mgr);

    return rv;
}
