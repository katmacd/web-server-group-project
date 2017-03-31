/* 
 * File: sws.c
 * Author: Alex Brodsky
 * Purpose: This file contains the implementation of a simple web server.
 *          It consists of two functions: main() which contains the main 
 *          loop accept client connections, and serve_client(), which
 *          processes each client request.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "network.h"
#include "priority_queue.h"

#define MAX_HTTP_SIZE 8192                 /* size of buffer to allocate */
#define NUM_THREADS 1

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

int counter = 0;
heap_t heap;
pthread_t worker_threads[NUM_THREADS];
pthread_t init_thread;
char alg_to_use[5];

void decide_and_push(FILE *fin, int fd, char *buffer) {

  rcb *new_rcb = ( rcb * ) malloc(sizeof(rcb *));
  new_rcb->rcb_seq_num = counter; /* TODO: This may have to be fixed later */
  new_rcb->rcb_cli_desc = fd;
  new_rcb->rcb_serv_handle = fin;
  new_rcb->rcb_file_bytes_remain = fread(buffer, 1, MAX_HTTP_SIZE, fin);  /* read file chunk */
  pthread_mutex_lock(&mutex);
  printf("pushing with priority %d\n ", new_rcb->rcb_file_bytes_remain);
  push(&heap, new_rcb->rcb_file_bytes_remain, new_rcb);
  pthread_mutex_unlock(&mutex);
}

rcb *loop_and_pop() {

  printf("thread wants to pop\n ");
  pthread_mutex_lock(&mutex);
  rcb *popped_rcb = pop(&heap);
  printf("pooping with priority %d\n ", popped_rcb->rcb_file_bytes_remain);
  pthread_mutex_unlock(&mutex);
  return popped_rcb;
}

/* This function takes a file handle to a client, reads in the request, 
 *    parses the request, and sends back the requested file.  If the
 *    request is improper or the file is not available, the appropriate
 *    error is sent back.
 * Parameters: 
 *             fd : the file descriptor to the client connection
 * Returns: None
 */
static void serve_client(int fd) {
  static char *buffer;                              /* request buffer */
  char *req = NULL;                                 /* ptr to req file */
  char *brk;                                        /* state used by strtok */
  char *tmp;                                        /* error checking ptr */
  FILE *fin;                                        /* input file handle */
  int len;                                          /* length of data read */

  if (!buffer) {                                   /* 1st time, alloc buffer */
    buffer = malloc(MAX_HTTP_SIZE);
    if (!buffer) {                                 /* error check */
      perror("Error while allocating memory");
      abort();
    }
  }

  if (read(fd, buffer, MAX_HTTP_SIZE) <= 0) {    /* read req from client */
    perror("Error while reading request");
    abort();
  }

  /* standard requests are of the form
   *   GET /foo/bar/qux.html HTTP/1.1
   * We want the second token (the file path).
   */
  tmp = strtok_r(buffer, " ", &brk);              /* parse request */
  if (tmp && !strcmp("GET", tmp)) {
    req = strtok_r(NULL, " ", &brk);
  }

  if (!req) {                                      /* is req valid? */
    len = sprintf(buffer, "HTTP/1.1 400 Bad request \n\n");
    write(fd, buffer, len);                       /* if not, send err */
    close(fd);
  } else {                                          /* if so, open file */
    req++;                                          /* skip leading / */

    fin = fopen(req, "r");                        /* open file */

    if (!fin) {                                    /* check if successful */
      len = sprintf(buffer, "HTTP/1.1 404 File not found \n\n");
      write(fd, buffer, len);                     /* if not, send err */
      close(fd);
    } else {                                        /* if so, send file */
      len = sprintf(buffer, "HTTP/1.1 200 OK\n\n");/* send success code */
      write(fd, buffer, len);

      do {                                          /* loop, read & send file */
        decide_and_push(fin, fd, buffer);
        rcb *popped_rcb = loop_and_pop();
        len = popped_rcb->rcb_file_bytes_remain;
        if (len < 0) {                             /* check for errors */
          perror("Error while writing to client");
        } else if (len > 0) {                      /* if none, send chunk */
          printf("fd:%d len:%d\n", popped_rcb->rcb_cli_desc, popped_rcb->rcb_file_bytes_remain);

          len = write(popped_rcb->rcb_cli_desc, buffer, len);

          close(popped_rcb->rcb_cli_desc);
          free(popped_rcb);
          if (len < 1) {                           /* check for errors */
            perror("Error while writing to client");
          }
        }
      } while (len==MAX_HTTP_SIZE);              /* the last chunk < 8192 */
      fclose(fin);
    }
  }
}

void *worker_thread(void *data) {
  int fd;
  for (;;) {                                       /* main loop */
    network_wait();                                 /* wait for clients */

    for (fd = network_open(); fd >= 0; fd = network_open()) { /* get clients */
      serve_client(fd);                           /* process each client */
    }
  }
}

/* This function is where the program starts running.
 *    The function first parses its command line parameters to determine port #
 *    Then, it initializes, the network and enters the main loop.
 *    The main loop waits for a client (1 or more to connect, and then processes
 *    all clients by calling the seve_client() function for each one.
 * Parameters: 
 *             argc : number of command line parameters (including program name
 *             argv : array of pointers to command line parameters
 * Returns: an integer status code, 0 for success, something else for error.
 */
int main(int argc, char **argv) {
  int port = -1;

  if ((argc < 3) || (sscanf(argv[1], "%d", &port) < 1)
      || *(strcpy(alg_to_use, argv[2])) < 1) {
    printf("usage: sws <port> <algorithm>\n");
    return 0;
  }

  strcpy(alg_to_use, argv[2]);
  network_init(port);

  int i;
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_create(&worker_threads[i], NULL, worker_thread, NULL);
  }
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_join(worker_threads[i], NULL);
  }
  pthread_join(init_thread, NULL);
  return EXIT_SUCCESS;
}
