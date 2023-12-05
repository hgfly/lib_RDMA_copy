#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "agent.h"
#include "connection.h"
#include "log.h"
#include "message.h"
#include "reactor.h"
#include "timer.h"
#include "utils.h"
#include "verbs_wrap.h"

#define MAX_INSTANCE_NUM 64
#define REQUEST_NUM 1000

struct agent_context *agents[MAX_INSTANCE_NUM];
int is_server;

double drop_rate = 0.1;

void app_on_pre_connect_cb(struct conn_context *ctx) {
  void *send_buf, *recv_buf;
  // allocate memory
  int ret;
  ret = posix_memalign((void **)&send_buf, sysconf(_SC_PAGESIZE), MAX_MR_SIZE);
  if (ret) {
    ERROR_LOG("failed to allocate memory.");
    exit(EXIT_FAILURE);
  }

  ret = posix_memalign((void **)&recv_buf, sysconf(_SC_PAGESIZE), MAX_MR_SIZE);
  if (ret) {
    ERROR_LOG("failed to allocate memory.");
    exit(EXIT_FAILURE);
  }

  // num_local_mrs and mr_ctxs must be set
  ctx->num_local_mrs = 2;
  ctx->mr_ctxs = (struct mr_context *)calloc(ctx->num_local_mrs,
                                             sizeof(struct mr_context));
  ctx->mr_ctxs[0].addr = send_buf;
  ctx->mr_ctxs[0].length = MAX_MR_SIZE;
  ctx->mr_ctxs[1].addr = recv_buf;
  ctx->mr_ctxs[1].length = MAX_MR_SIZE;

  // register memory
  register_mr(ctx, ctx->num_local_mrs, ctx->mr_ctxs);

  // pre-post
  memset(send_buf, 0, MAX_MR_SIZE);
  memset(recv_buf, 0, MAX_MR_SIZE);
  struct ibv_sge sge;
  sge.addr = ctx->local_mr[1]->addr;
  if (is_server) {  // server side
    sge.length = 1024;
  } else {  // client side
    sge.length = 1;
  }
  sge.lkey = ctx->local_mr[1]->lkey;

  for (int i = 0; i < REQUEST_NUM; i++) {
    post_recv_async(ctx, 1, &sge);
  }
}

void app_on_connect_cb(struct conn_context *ctx) {
  if (!is_server) {  // client side
    struct ibv_sge sge;
    sge.addr = ctx->local_mr[0]->addr;
    sge.length = 1024;
    sge.lkey = ctx->local_mr[0]->lkey;

    int num = REQUEST_NUM;
    int drop = num * drop_rate;
    long tot = 0;
    for (int i = 0; i < num; i++) {
      struct timespec start = timer_start();
      post_send_async(ctx, 1, &sge, 0);
      spin_till_completion(ctx, i + 1, 0);  // waiting for the recv event
      long duration = timer_end(start);
      if (i >= drop) {
        tot += duration;
      }
    }
    INFO_LOG(
        "RDMA SEND/RECV average duration: %lu usec [times:%d data_size:%dB].",
        tot / (num - drop) / 1000, num, sge.length);

    // disconnect when done
    usleep(2000);
    rdma_disconnect(ctx->id);
  }
}

void app_on_complete_cb(struct conn_context *ctx, struct ibv_wc *wc) {
  switch (wc->opcode) {
    case IBV_WC_SEND:
      break;
    case IBV_WC_RECV: {
      if (is_server) {  // respond to the client
        struct ibv_sge sge;
        sge.addr = ctx->local_mr[0]->addr;
        sge.length = 1;
        sge.lkey = ctx->local_mr[0]->lkey;
        post_send_async(ctx, 1, &sge, 0);
      }
      break;
    }
    case IBV_WC_RECV_RDMA_WITH_IMM:
      break;
    case IBV_WC_RDMA_WRITE:
      break;
    case IBV_WC_RDMA_READ:
      break;
    case IBV_WC_COMP_SWAP:
      break;
    case IBV_WC_FETCH_ADD:
      break;
    default:
      INFO_LOG("unknown opcode:%i, wr_id:%lu.", wc->opcode, wc->wr_id);
      break;
  }
}

// client side
pthread_t create_instance(int node_id, int inst_id) {
  // create client
  struct agent_context *client = create_client(node_id, inst_id);
  agents[inst_id] = client;

  char *dst_addr = "10.10.10.2";
  char *port = "12345";
  struct conn_param rc_options = {.poll_mode = CQ_POLL_MODE_POLLING,
                                  .on_pre_connect_cb = app_on_pre_connect_cb,
                                  .on_connect_cb = app_on_connect_cb,
                                  .on_complete_cb = app_on_complete_cb};
  int sockfd = add_connection_rc(client, dst_addr, port, &rc_options);
  struct conn_context *rc_ctx = get_connection(client, sockfd);

  // start run
  pthread_create(&rc_ctx->rdma_event_thread, NULL, client_loop, rc_ctx);

  return rc_ctx->rdma_event_thread;
}

int main(int argc, char *argv[]) {
  if (argc == 1) {
    // if no command-line arguments are provided, give a prompt
    ERROR_LOG("Please provide an argument 's' for server or 'c' for client.");
    return 0;
  } else if (argc == 2) {
    // if there is one command-line argument
    if (strcmp(argv[1], "s") == 0) {
      is_server = 1;
      INFO_LOG("Starting the server...");
    } else if (strcmp(argv[1], "c") == 0) {
      is_server = 0;
      INFO_LOG("Starting the client...");
    } else {
      ERROR_LOG("Invalid argument: %s.", argv[1]);
      return 0;
    }
  } else {
    // if the number of arguments is incorrect
    ERROR_LOG("Please provide the correct number of arguments.\n");
    return 0;
  }

  if (is_server) {  // server side
    // setup server accept conn param
    struct conn_param accept_options = {
        .poll_mode = CQ_POLL_MODE_POLLING,
        .on_pre_connect_cb = app_on_pre_connect_cb,
        .on_connect_cb = app_on_connect_cb,
        .on_complete_cb = app_on_complete_cb};
    struct agent_context *server = create_server(1, 1, &accept_options);

    // sockfd for listening
    char *src_addr = "10.10.10.2";
    char *port = "12345";
    int listen_fd = server_listen(server, src_addr, port);
    struct conn_context *listen_ctx = get_connection(server, listen_fd);
    struct rdma_event_channel *listen_channel = listen_ctx->id->channel;

    pthread_create(&listen_ctx->rdma_event_thread, NULL, server_loop,
                   listen_ctx);
    pthread_join(listen_ctx->rdma_event_thread, NULL);

    // free resourses
    destroy_agent(server);
  } else {  // client side
    int num_instances = 1;
    pthread_t thread[num_instances];
    int node_id = 1;

    // create client instance
    for (int i = 0; i < num_instances; i++) {
      thread[i] = create_instance(node_id, i);
    }

    for (int i = 0; i < num_instances; i++) {
      pthread_join(thread[i], NULL);
    }

    // free resources
    for (int i = 0; i < num_instances; i++) {
      destroy_agent(agents[i]);
    }
  }

  return 0;
}