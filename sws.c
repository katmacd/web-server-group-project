
/*
 * File: sws.c
 * Author: Alex Brodsky
 * Purpose: This file contains the implementation of a simple web server.
 *          It consists of two functions: main() which contains the main
 *          loop accept client connections, and init_client(), which
 *          processes each client request.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "network.h"
#include "priority_queue.h"

#define MAX_HTTP_SIZE 8000            /* size of buffer to allocate */
#define NUM_THREADS 1

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int rr_quantum = 4;
int counter = 0;
heap_t max_queue;
heap_t mid_queue;
heap_t min_queue;

int max_queue_quantum = 4;
int mid_queue_quantum = 8;

int is_sjf = 0;
int is_mlfb = 0;
int is_rr = 0;

pthread_t worker_threads[NUM_THREADS];
pthread_t client_serve_thread;
char alg_to_use[5];

rcb *create_rcb(FILE *fin, int fd, char *buffer) {
  rcb *new_rcb = (rcb *) malloc(sizeof(rcb));
  new_rcb->rcb_queue_level = 0;
  new_rcb->rcb_seq_num = counter; /* TODO: This may have to be fixed later */
  new_rcb->rcb_cli_desc = fd;
  new_rcb->rcb_serv_handle = fin;
  fseek(new_rcb->rcb_serv_handle, 0, SEEK_END);

  int len_file = ftell(new_rcb->rcb_serv_handle);
  rewind(new_rcb->rcb_serv_handle);
  if (is_sjf) {
    rr_quantum = MAX_HTTP_SIZE; //TODO: make this not temporary
    new_rcb->rcb_quantum = MAX_HTTP_SIZE;
    new_rcb->rcb_file_bytes_remain = len_file;
    new_rcb->rcb_priority = len_file;
  }
  if (is_rr) {
    new_rcb->rcb_quantum = rr_quantum;
    new_rcb->rcb_file_bytes_remain = len_file;
    new_rcb->rcb_priority = 1;
  }
  if (strcmp(alg_to_use, "MLFB") == 0) {
    rr_quantum = MAX_HTTP_SIZE; //TODO: make this not temporary
    new_rcb->rcb_quantum = max_queue_quantum;
    new_rcb->rcb_file_bytes_remain = len_file;
    new_rcb->rcb_priority = 1;
  }
  return new_rcb;
}

void lock_push(rcb *new_rcb, heap_t *queue) {
  pthread_mutex_lock(&mutex);
  printf("pushing with priority %d\n", new_rcb->rcb_priority);
  push(queue, new_rcb->rcb_priority, new_rcb);
  pthread_mutex_unlock(&mutex);
}

/* This function takes a file handle to a client, reads in the request,
 *    parses the request, and sends back the requested file.  If the
 *    request is improper or the file is not available, the appropriate
 *    error is sent back.
 * Parameters:
 *             fd : the file descriptor to the client connection
 * Returns: None
 */
void init_client(int fd) {
  char *buffer = malloc(MAX_HTTP_SIZE);                        /* request buffer */

  if (!buffer) {                     /* error check */
    perror("Error while allocating memory");
    abort();
  }

  char *req = NULL;                           /* ptr to req file */
  char *brk;                                  /* state used by strtok */
  char *tmp;                                  /* error checking ptr */
  FILE *fin;                                  /* input file handle */
  int len;                                    /* length of data read */

  if (read(fd, buffer, MAX_HTTP_SIZE) <= 0) { /* read req from client */
    perror("Error while reading request");
    abort();
  }

  /* standard requests are of the form
   *   GET /foo/bar/qux.html HTTP/1.1
   * We want the second token (the file path).
   */
  tmp = strtok_r(buffer, " ", &brk);        /* parse request */
  if (tmp && !strcmp("GET", tmp)) {
    req = strtok_r(NULL, " ", &brk);
  }

  if (!req) {                                /* is req valid? */
    len = sprintf(buffer, "HTTP/1.1 400 Bad request \n\n");
    write(fd, buffer, len);           /* if not, send err */
    close(fd);
  } else {                                    /* if so, open file */
    req++;                              /* skip leading / */

    fin = fopen(req, "r");            /* open file */

    if (!fin) {                        /* check if successful */
      len = sprintf(buffer, "HTTP/1.1 404 File not found \n\n");
      write(fd, buffer, len);   /* if not, send err */
      close(fd);
    } else {                            /* if so, send file */
      len = sprintf(buffer, "HTTP/1.1 200 OK\n\n");/* send success code */
      write(fd, buffer, len);
      rcb *push_rcb = create_rcb(fin, fd, buffer);
      lock_push(push_rcb, &max_queue);
    }
  }
  free(buffer);
}

void *starter_thread(void *data) {
  int fd;
  for (;;) {                                /* main loop */
    network_wait();                     /* wait for clients */

    for (fd = network_open(); fd >= 0; fd = network_open()) { /* get clients */
      init_client(fd);         /* process each client */
    }
  }
}

rcb *pop_assign(int len) {
/* length of data read */
  rcb *popped_rcb;
  pthread_mutex_lock(&mutex);
  if (max_queue.len > 0) {
    popped_rcb = pop(&max_queue);
    printf("pooping with priority %d\n", popped_rcb->rcb_priority);
    len = popped_rcb->rcb_file_bytes_remain;
  } else if (max_queue.len == 0 && mid_queue.len > 0 && is_mlfb) {
    popped_rcb = pop(&mid_queue);
    printf("pooping with priority %d\n", popped_rcb->rcb_priority);
    len = popped_rcb->rcb_file_bytes_remain;
  } else if (max_queue.len == 0 && mid_queue.len == 0 && min_queue.len > 0 && is_mlfb) {
    popped_rcb = pop(&min_queue);
    printf("pooping with priority %d\n", popped_rcb->rcb_priority);
    len = popped_rcb->rcb_file_bytes_remain;
  }
  pthread_mutex_unlock(&mutex);;
  return popped_rcb;
}

//id serve_client(int len, char *buffer, rcb *popped_rcb) {

//}

void *worker_thread(void *data) {
  char *buffer = malloc(sizeof(char) * MAX_HTTP_SIZE);                        /* request buffer */
  rcb *popped_rcb;

  if (!buffer) {                     /* error check */
    perror("Error while allocating memory");
    abort();
  }

  int len = -1;

  while (1) {
    popped_rcb = pop_assign(len);
    if (popped_rcb) {


      if ((is_mlfb)) {
        if (popped_rcb->rcb_file_bytes_remain != 0 && popped_rcb->rcb_queue_level == 0) {
          popped_rcb->rcb_queue_level = 1;
          popped_rcb->rcb_quantum = mid_queue_quantum;
        } else if (popped_rcb->rcb_file_bytes_remain != 0 && popped_rcb->rcb_queue_level == 1) {
          popped_rcb->rcb_queue_level = 2;
          popped_rcb->rcb_quantum = MAX_HTTP_SIZE;
        }
      }


      do {                                          /* loop, read & send file */
        len = fread(buffer, 1, popped_rcb->rcb_quantum, popped_rcb->rcb_serv_handle); /* read file chunk */
        popped_rcb->rcb_file_bytes_remain = popped_rcb->rcb_file_bytes_remain - len;


        if (len < 0) {                             /* check for errors */
          perror("Error while writing to client");
        } else if (len > 0) {                      /* if none, send chunk */

          printf("%s\n", buffer);
          len = write(popped_rcb->rcb_cli_desc, buffer, len);
          if (len < 1) {                           /* check for errors */
            perror("Error while writing to client");
          }
        }
      } while (len == MAX_HTTP_SIZE);              /* the last chunk < 8192 */



      if (!(is_mlfb)) {
        if (len == rr_quantum) {

          lock_push(popped_rcb, &max_queue);

        }
        if (len < rr_quantum) {
          close(popped_rcb->rcb_cli_desc);
          fclose(popped_rcb->rcb_serv_handle);
          free(popped_rcb);
        }
      } else {
        if (len == max_queue_quantum || len == mid_queue_quantum) {
          if (popped_rcb->rcb_queue_level == 1) {
            printf("pushed to mid q");
            lock_push(popped_rcb, &mid_queue);

          }
          if (popped_rcb->rcb_queue_level == 2) {
            printf("pushed to min q");
            lock_push(popped_rcb, &min_queue);
          }
        } else if (len < rr_quantum) {
          close(popped_rcb->rcb_cli_desc);
          fclose(popped_rcb->rcb_serv_handle);
          free(popped_rcb);
        }
      }
    }
    popped_rcb = NULL;
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

  if ((argc < 3)
      || (sscanf(argv[1], "%d", &port) < 1)
      || *(strcpy(alg_to_use, argv[2])) < 1) {
    printf("usage: sws <port> <algorithm>\n");
    return 0;
  }

  is_sjf = !strcmp(alg_to_use, "SJF");
  is_rr = !strcmp(alg_to_use, "RR");
  is_mlfb = !strcmp(alg_to_use, "MLFB");

  network_init(port);

  pthread_create(&client_serve_thread, NULL, worker_thread, NULL);
  int i;
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_create(&worker_threads[i], NULL, starter_thread, NULL);
  }
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_join(worker_threads[i], NULL);
  }
  pthread_join(client_serve_thread, NULL);
  return EXIT_SUCCESS;
}
