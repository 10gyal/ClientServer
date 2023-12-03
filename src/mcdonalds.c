//--------------------------------------------------------------------------------------------------
// Network Lab                               Fall 2023                           System Programming
//
/// @file
/// @brief Simple virtual McDonald's server for Network Lab
///
/// @author <your name>
/// @studid <your student id>
///
/// @section changelog Change Log
/// 2020/11/18 Hyunik Kim created
/// 2021/11/23 Jaume Mateu Cuadrat cleanup, add milestones
///
/// @section license_section License
/// Copyright (c) 2020-2023, Computer Systems and Platforms Laboratory, SNU
/// All rights reserved.
///
/// Redistribution and use in source and binary forms, with or without modification, are permitted
/// provided that the following conditions are met:
///
/// - Redistributions of source code must retain the above copyright notice, this list of condi-
///   tions and the following disclaimer.
/// - Redistributions in binary form must reproduce the above copyright notice, this list of condi-
///   tions and the following disclaimer in the documentation and/or other materials provided with
///   the distribution.
///
/// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
/// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED  TO, THE IMPLIED  WARRANTIES OF MERCHANTABILITY
/// AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
/// CONTRIBUTORS  BE LIABLE FOR ANY DIRECT,  INDIRECT, INCIDENTAL, SPECIAL,  EXEMPLARY,  OR CONSE-
/// QUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
/// LOSS OF USE, DATA,  OR PROFITS; OR BUSINESS INTERRUPTION)  HOWEVER CAUSED AND ON ANY THEORY OF
/// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
/// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
/// DAMAGE.
//--------------------------------------------------------------------------------------------------

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>

#include "net.h"
#include "burger.h"

/// @name Constant definitions
/// @{

#define CUSTOMER_MAX 20                                     ///< maximum number of clients
#define NUM_KITCHEN 5                                       ///< number of kitchen thread(s)

/// @}

/// @name Structures
/// @{

/// @brief general node element to implement a singly-linked list
typedef struct __node {
  struct __node *next;                                      ///< pointer to next node
  unsigned int customerID;                                  ///< customer ID that requested
  enum burger_type type;                                    ///< requested burger type
  bool is_ready;                                            ///< true if burger is ready
  pthread_cond_t cond;                                      ///< conditional variable
  pthread_mutex_t mutex;                                    ///< mutex variable
} Node;

/// @brief order data
typedef struct __order_list {
  Node *head;                                               ///< head of order list
  Node *tail;                                               ///< tail of order list
  unsigned int count;                                       ///< number of nodes in list
} OrderList;

/// @brief structure for server context
struct mcdonalds_ctx {
  unsigned int total_customers;                             ///< number of customers served
  unsigned int total_burgers[BURGER_TYPE_MAX];              ///< number of burgers produced by types
  unsigned int total_queueing;                              ///< number of customers in queue
  OrderList list;                                           ///< starting point of list structure
};

/// @}

/// @name Global variables
/// @{

int listenfd;                                               ///< listen file descriptor
struct mcdonalds_ctx server_ctx;                            ///< keeps server context
sig_atomic_t keep_running = 1;                              ///< keeps all the threads running
pthread_t kitchen_thread[NUM_KITCHEN];                      ///< thread for kitchen

/// @}


/// @brief Enqueue element in tail of the OrderList
/// @param customerID customer ID
/// @param type burger type
/// @retval Node* containing the node structure of the element
Node* issue_order(unsigned int customerID, enum burger_type type)
{
  Node *new_node = malloc(sizeof(Node));

  new_node->customerID = customerID;
  new_node->type = type;
  new_node->next = NULL;
  new_node->is_ready = false;
  pthread_cond_init(&new_node->cond, NULL);
  pthread_mutex_init(&new_node->mutex, NULL);

  if (server_ctx.list.tail == NULL) {
    server_ctx.list.head = new_node;
    server_ctx.list.tail = new_node;
  } else {
    server_ctx.list.tail->next = new_node;
    server_ctx.list.tail = new_node;
  }

  server_ctx.list.count++;

  return new_node;
}

/// @brief Dequeue element from the OrderList
/// @retval Node* Node from head of the list
Node* get_order(void)
{
  Node *target_node;

  if (server_ctx.list.head == NULL) return NULL;

  target_node = server_ctx.list.head;


  if (server_ctx.list.head == server_ctx.list.tail) {
    server_ctx.list.head = NULL;
    server_ctx.list.tail = NULL;
  } else {
    server_ctx.list.head = server_ctx.list.head->next;
  }

  server_ctx.list.count--;


  return target_node;
}

/// @brief Returns number of element left in OrderList
/// @retval number of element(s) in OrderList
unsigned int order_left(void)
{
  int ret;

  ret = server_ctx.list.count;

  return ret;
}

/// @brief Kitchen task for kitchen thread
void* kitchen_task(void *dummy)
{
  Node *order;
  enum burger_type type;
  pthread_t tid = pthread_self();

  printf("Kitchen thread %lu ready\n", tid);

  while (keep_running || order_left()) {
    order = get_order();
    if (order == NULL) {
      sleep(2);
      continue;
    }

    type = order->type;
    printf("[Thread %lu] generating %s burger\n", tid, burger_names[type]);
    sleep(5);
    printf("[Thread %lu] %s burger is ready\n", tid, burger_names[type]);

    server_ctx.total_burgers[type]++;

    order->is_ready = true;
    pthread_cond_signal(&order->cond);
  }

  printf("[Thread %lu] terminated\n", tid);
  pthread_exit(NULL);
}

/// @brief error function for the serve_client
/// @param clientfd file descriptor of the client*
/// @param newsock socketid of the client as void*
/// @param newsock buffer for the messages*
void error_client(int clientfd, void *newsock,char *buffer) {
  close(clientfd);
  free(newsock);
  free(buffer);

  server_ctx.total_queueing--;
}

/// @brief client task for client thread
/// @param newsock socketID of the client as void*
void* serve_client(void *newsock)
{
  ssize_t read, sent;
  size_t msglen;
  char *message, *burger, *buffer;
  unsigned int customerID;
  enum burger_type type;
  Node *order = NULL;
  int ret, i, clientfd;

  clientfd = *(int *) newsock;
  buffer = (char *) malloc(BUF_SIZE);
  msglen = BUF_SIZE;

  customerID = server_ctx.total_customers++;

  printf("Customer #%d visited\n", customerID);

  // send welcome to mcdonalds
  ret = asprintf(&message, "Welcome to McDonald's, customer #%d\n", customerID);
  if (ret < 0) {
    perror("asprintf");
    return NULL;
  }

  sent = put_line(clientfd, message, ret);
  if (sent < 0) {
    printf("Error: cannot send data to client\n");
    error_client(clientfd, newsock, buffer);
    return NULL;
  }
  free(message);

  //-------- remove for milestone 2 --------
  // close(clientfd);
  // free(newsock);
  // free(buffer);
  // server_ctx.total_queueing--;
  // return NULL;
  //-----------------------------------------

  // receive order from the customer
  // TODO
  read = get_line(clientfd, &buffer, &msglen);
  if (read <= 0) {
      printf("Error: cannot read from client\n");
      error_client(clientfd, newsock, buffer);
      return NULL;
  }

  burger = strtok(buffer, "\n"); // Assuming the order is a single line

  if (burger == NULL) {
      printf("Error: invalid order format\n");
      error_client(clientfd, newsock, buffer);
      return NULL;
  }

  // printf("Received order: %s\n", burger);

  // parse order from the customer
  // TODO
  // Parse the burger type
  type = BURGER_TYPE_MAX;
  for (i = 0; i < BURGER_TYPE_MAX; i++) {
      if (strcmp(burger, burger_names[i]) == 0) {
          type = i;
          break;
      }
  }

  if (type == BURGER_TYPE_MAX) {
      printf("Error: unknown burger type\n");
      error_client(clientfd, newsock, buffer);
      return NULL;
  }

  // issue an order to kitchen and wait
  order = issue_order(customerID, type);
  pthread_mutex_lock(&order->mutex);
  while (!order->is_ready) {
    pthread_cond_wait(&order->cond, &order->mutex);
  }
  pthread_mutex_unlock(&order->mutex);

  // // if burger is not available, exit connection
  // // TODO
  if (type == BURGER_TYPE_MAX) {
      printf("Error: unknown burger type\n");
      error_client(clientfd, newsock, buffer);
      return NULL;
  }
  

  // if order successfully handled, hand burger and say goodbye
  if (order->is_ready) {
    ret = asprintf(&message, "Your %s burger is ready! Goodbye!\n", burger_names[i]);
    sent = put_line(clientfd, message, ret);
    if (sent <= 0) {
      printf("Error: cannot send data to client\n");
      error_client(clientfd, newsock, buffer);
      return NULL;
    }
    free(message);
  }
  free(order);

  close(clientfd);

  // free(newsock);
  // printf("HERE\n");

  free(buffer);

  server_ctx.total_queueing--;

  return NULL;
}

void* serve_client_wrapper(void *newsock){
  int clientfd = *(int *)newsock;
  free(newsock);
  serve_client(&clientfd);
  return NULL;
}

/// @brief start server listening
void start_server()
{
  int clientfd, opt = 1;
  struct sockaddr_in client;
  struct addrinfo *ai, *ai_it;
  socklen_t addrlen;
  // TODO  
  // - get socket list
  ai = getsocklist(NULL, PORT, AF_UNSPEC, SOCK_STREAM, 1, NULL);
  if (ai == NULL) {
        perror("getsocklist");
        return;
    }

  // iterate until valid one is found
  ai_it = ai;
  while(ai_it != NULL){
    printf("  trying "); dump_sockaddr(ai_it->ai_addr); printf("..."); fflush(stdout);

    listenfd = socket(ai_it->ai_family, ai_it->ai_socktype, ai_it->ai_protocol);
    if(listenfd != -1){
      if((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt, sizeof(int)) == 0) && (bind(listenfd, ai_it->ai_addr, ai_it->ai_addrlen) == 0) && (listen(listenfd, 1) == 0)) break;
      close(listenfd);
    }
    printf("failed.\n");
    ai_it = ai_it->ai_next;
  }

  if (ai_it == NULL) perror("Cannot bind to port.");

  freeaddrinfo(ai);

  printf("Listening...\n");

  // TODO
  // - keep listening and accepting clients
  while(1){
    int *fd = malloc(sizeof(int));
    addrlen = sizeof(client);

    // pthread_mutex_lock(&queue_mutex);
    // printf("total queueing: %d\n", server_ctx.total_queueing);
    // if(server_ctx.total_queueing > CUSTOMER_MAX){
    //   pthread_mutex_unlock(&queue_mutex);
    //   printf("Max number of customers exceeded, Good bye!\n");
    //   return;
    // }
    // pthread_mutex_unlock(&queue_mutex);

    *fd = accept(listenfd, (struct sockaddr*)&client, &addrlen);
    if (*fd < 0){
      perror("accept");
      free(fd);
      break;
    }
    server_ctx.total_queueing++;

    // printf("customers: %d\n", server_ctx.total_customers);
    // printf("queue: %d\n", server_ctx.total_queueing);
    // printf("burgers: %d\n", server_ctx.total_burgers);


    if(server_ctx.total_queueing >= 20){
      printf("Max number of customers exceeded, Good bye!\n");
      close(*fd);
      free(fd);
      perror("max queue");
    }

    pthread_t client_thread;
    // printf("  connection from "); dump_sockaddr((struct sockaddr*)&client); printf("\n");
    pthread_create(&client_thread, NULL, serve_client_wrapper, fd);
    pthread_detach(client_thread);
    // pthread_detach(client_thread);
    // serve_client(&fd);
    // close(fd);
  }  
}

/// @brief prints overall statistics
void print_statistics(void)
{
  int i;

  printf("\n====== Statistics ======\n");
  printf("Number of customers visited: %u\n", server_ctx.total_customers);
  for (i = 0; i < BURGER_TYPE_MAX; i++) {
    printf("Number of %s burger made: %u\n", burger_names[i], server_ctx.total_burgers[i]);
  }
  printf("\n");
}

/// @brief exit function
void exit_mcdonalds(void)
{
  close(listenfd);
  print_statistics();
}

/// @brief Second SIGINT handler function
/// @param sig signal number
void sigint_handler2(int sig)
{
  exit_mcdonalds();
  exit(EXIT_SUCCESS);
}

/// @brief First SIGINT handler function
/// @param sig signal number
void sigint_handler(int sig)
{
  signal(SIGINT, sigint_handler2);
  printf("****** I'm tired, closing McDonald's ******\n");
  keep_running = 0;
  sleep(3);
  exit(EXIT_SUCCESS);
}

/// @brief init function initializes necessary variables and sets SIGINT handler
void init_mcdonalds(void)
{
  int i;

  printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@@@@@@@@(,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,(@@@@@@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@@@@,,,,,,,@@@@@@,,,,,,,@@@@@@@@@@@@@@(,,,,,,@@@@@@@,,,,,,,@@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@@@,,,,,,@@@@@@@@@@,,,,,,,@@@@@@@@@@@,,,,,,,@@@@@@@@@*,,,,,,@@@@@@@@@@@@\n");
  printf("@@@@@@@@@@.,,,,,,@@@@@@@@@@@@,,,,,,,@@@@@@@@@,,,,,,,@@@@@@@@@@@@,,,,,,/@@@@@@@@@@\n");
  printf("@@@@@@@@@,,,,,,,,@@@@@@@@@@@@@,,,,,,,@@@@@@@,,,,,,,@@@@@@@@@@@@@,,,,,,,,@@@@@@@@@\n");
  printf("@@@@@@@@,,,,,,,,@@@@@@@@@@@@@@@,,,,,,,@@@@@,,,,,,,@@@@@@@@@@@@@@@,,,,,,,,@@@@@@@@\n");
  printf("@@@@@@@@,,,,,,,@@@@@@@@@@@@@@@@,,,,,,,,@@@,,,,,,,,@@@@@@@@@@@@@@@@,,,,,,,@@@@@@@@\n");
  printf("@@@@@@@,,,,,,,,@@@@@@@@@@@@@@@@@,,,,,,,,@,,,,,,,,@@@@@@@@@@@@@@@@@,,,,,,,,@@@@@@@\n");
  printf("@@@@@@,,,,,,,,@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@,,,,,,,,@@@@@@\n");
  printf("@@@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@@@\n");
  printf("@@@@@,,,,,,,,@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@,,,,,,,,@@@@@\n");
  printf("@@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@@\n");
  printf("@@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@@\n");
  printf("@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@\n");
  printf("@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@\n");
  printf("@@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@@\n");
  printf("@@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@@\n");
  printf("@@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@@\n");
  printf("@@,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,@@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");
  printf("@,,,,,,,,,,@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@,,,,,,,,,,@\n");

  printf("\n\n                          I'm lovin it! McDonald's\n\n");

  signal(SIGINT, sigint_handler);

  server_ctx.total_customers = 0;
  server_ctx.total_queueing = 0;
  for (i = 0; i < BURGER_TYPE_MAX; i++) {
    server_ctx.total_burgers[i] = 0;
  }

  for (int i = 0; i < NUM_KITCHEN; i ++){
    if(pthread_create(&kitchen_thread[i], NULL, kitchen_task, NULL) != 0){
      perror("Failed to create a thread");
    }
    pthread_detach(kitchen_thread[i]);
  }
  // for (int i = 0; i < NUM_KITCHEN; i++){
  //   pthread_detach(kitchen_thread[i]);
  // }

}

/// @brief program entry point
int main(int argc, char *argv[])
{
  init_mcdonalds();
  start_server();
  exit_mcdonalds();

  return 0;
}
