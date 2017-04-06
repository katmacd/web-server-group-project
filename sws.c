/* This server creates request control blocks from the client request.
 * The request control block contains pointers to the file and the client
 * file descriptor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "network.h"
#include "priority_queue.h"

#define MAX_FILE_LENGTH 255
#define MAX_HTTP_SIZE 8892
#define NUM_THREADS 64

queue max_queue;
queue mid_queue;
queue min_queue;
int max_queue_quantum = 2048;
int mid_queue_quantum = 4096;
int rr_quantum = 2048;
int counter = 0;
int is_sjf = 0;
int is_mlfb = 0;
int is_rr = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

char alg_to_use[5];


void algorithm_init(int *, int *);

void work_init_t(int *);

void rcb_init_t();

void *init_client(void *);

void *work(void *);

rcb *make_rbc(FILE *, int, char *, char *req);

void lock_enqueue(rcb *, queue *);

rcb *lock_dequeue(int);

/* Accept command line arguments and initialize the server */
int main(int argc, char **argv) {
  int port = -1;
  int num_threads = 64;

  if ((argc < 4)
      || (sscanf(argv[1], "%d", &port) < 1)
      || *(strcpy(alg_to_use, argv[2])) < 1
      || sscanf(argv[3], "%d", &num_threads) < 1) {
    printf("usage: sws <port> <algorithm> <thread number>\n");
    return 0;
  }

  algorithm_init(&port, &num_threads);
  network_init(port);
  work_init_t(&num_threads);
  return EXIT_SUCCESS;
}

/* Give information on selected algorithm */
void algorithm_init(int *port, int *num_threads) {
  is_sjf = !(strcmp(alg_to_use, "SJF") && strcmp(alg_to_use, "sjf"));
  is_rr = !(strcmp(alg_to_use, "RR") && strcmp(alg_to_use, "rr"));
  is_mlfb = !(strcmp(alg_to_use, "MLFB") && strcmp(alg_to_use, "mlfb"));

  if (is_sjf) printf("Using shortest job first scheduling algorithm ");
  else if (is_rr) printf("Using round robin scheduling algorithm ");
  else if (is_mlfb) printf("Using multilevel feedback scheduling algorithm ");
  else {
    printf("Not using a valid scheduling algorithm. Goodbye!\n");
    abort();
  }
  printf("with %d threads on port %d\n", *num_threads, *port);

}

/* Initialize the worker threads */
void work_init_t(int *num_threads) {
  int i;
  pthread_t client_serve_threads[*num_threads];
  for (i = 0; i < *num_threads; i++) {
    pthread_create(&client_serve_threads[i], NULL, work, NULL);
  }
  rcb_init_t();
  for (i = 0; i < *num_threads; i++) {
    pthread_join(client_serve_threads[i], NULL);
  }
}

/* Initialize request control block creation threads */
void rcb_init_t() {
  int fd;

  for (;;) {
    printf("Waiting for client request...\n");
    network_wait();
    for (fd = network_open(); fd >= 0; fd = network_open()) {
      pthread_t init_thread;
      int *fdp = (int *) malloc(sizeof(int));
      *fdp = fd;
      pthread_create(&init_thread, NULL, init_client, fdp);
    }
  }
}

/* Thread safe access to queue */
void lock_enqueue(rcb *new_rcb, queue *queue) {
  pthread_mutex_lock(&mutex);
  enqueue(queue, new_rcb->rcb_priority, new_rcb);
  pthread_mutex_unlock(&mutex);
}

/* Runner responsible for creating request control blocks
 * for the passed file descriptor. The buffer is local to
 * threads to avoid race conditions. */
void *init_client(void *data) {
  int *fdp = (int *) data;
  int fd = *fdp;

  char *buffer = malloc(sizeof(char) * MAX_HTTP_SIZE);
  memset(buffer, 0, sizeof(char) * MAX_HTTP_SIZE);

  if (!buffer) {
    perror("Error while allocating memory");
    abort();
  }

  char *req = NULL;
  char *brk;
  char *tmp;
  FILE *fin;
  int len;

  if (read(fd, buffer, MAX_HTTP_SIZE) <= 0) {
    perror("Error while reading request");
    abort();
  }

  tmp = strtok_r(buffer, " ", &brk);
  if (tmp && !strcmp("GET", tmp)) {
    req = strtok_r(NULL, " ", &brk);
  }

  if (!req) {
    len = sprintf(buffer, "HTTP/1.1 400 Bad request \n\n");
    write(fd, buffer, len);
    close(fd);
  } else {
    req++;
    printf("Request for file '%s' admitted.\n", req);
    fin = fopen(req, "r");
    char request[MAX_FILE_LENGTH];
    memset(request, '\0', sizeof(request));
    strcpy(request, req);

    if (!fin) {
      len = sprintf(buffer, "HTTP/1.1 404 File not found \n\n");
      write(fd, buffer, len);
      close(fd);
    } else { // OK so we schedule the rcb;
      len = sprintf(buffer, "HTTP/1.1 200 OK\n\n");
      write(fd, buffer, len);
      rcb *push_rcb = make_rbc(fin, fd, buffer, request);
      lock_enqueue(push_rcb, &max_queue);
    }
  }
  free(buffer);
  return 0;
}

/* Request control block creation - slight differences in
 * initialization for scheduling algorithm selection
 * The buffer is local to threads to avoid race conditions. */

rcb *make_rbc(FILE *fin, int fd, char *buffer, char *req) {
  rcb *new_rcb = (rcb *) malloc(sizeof(rcb));
  new_rcb->rcb_queue_level = 0;
  new_rcb->rcb_seq_num = counter;
  new_rcb->rcb_cli_desc = fd;
  new_rcb->rcb_file_name = req;
  new_rcb->rcb_serv_handle = fin;
  fseek(new_rcb->rcb_serv_handle, 0, SEEK_END);
  int len = ftell(new_rcb->rcb_serv_handle); //get file size
  rewind(new_rcb->rcb_serv_handle);
  if (is_sjf) {
    new_rcb->rcb_quantum = MAX_HTTP_SIZE;
    new_rcb->rcb_file_bytes_remain = len;
    new_rcb->rcb_priority = len;
  }
  if (is_rr) {
    new_rcb->rcb_quantum = rr_quantum;
    new_rcb->rcb_file_bytes_remain = len;
    new_rcb->rcb_priority = 1;
  }
  if (is_mlfb) {
    rr_quantum = MAX_HTTP_SIZE; //TODO: make this not temporary
    new_rcb->rcb_quantum = max_queue_quantum;
    new_rcb->rcb_file_bytes_remain = len;
    new_rcb->rcb_priority = 1;
  }
  return new_rcb;
}

/* Thread safe access to queue
 * initialization for scheduling algorithm selection */
rcb *lock_dequeue(int len) {
  rcb *popped_rcb;
  pthread_mutex_lock(&mutex);
  if (max_queue.len > 0) {
    popped_rcb = dequeue(&max_queue);
    len = popped_rcb->rcb_file_bytes_remain;
  } else if (max_queue.len == 0 && mid_queue.len > 0) {
    popped_rcb = dequeue(&mid_queue);
    len = popped_rcb->rcb_file_bytes_remain;
  } else if (max_queue.len == 0 && mid_queue.len == 0 && min_queue.len > 0) {
    popped_rcb = dequeue(&min_queue);
    len = popped_rcb->rcb_file_bytes_remain;
  } else {
    popped_rcb = NULL;
  }
  pthread_mutex_unlock(&mutex);;
  return popped_rcb;
}

/* Runner to process request control blocks */
void *work(void *data) {
  char *buffer = malloc(sizeof(char) * MAX_HTTP_SIZE);
  memset(buffer, 0, sizeof(char) * MAX_HTTP_SIZE);
  rcb *popped_rcb;
  if (!buffer) {
    perror("Error while allocating memory");
    abort();
  }
  while (1) {
    int len = -1;
    //TODO: Fix this busy waiting
    popped_rcb = lock_dequeue(len); //get the next block in schedule
    if (popped_rcb) {
      if ((is_mlfb)) {
        if (popped_rcb->rcb_queue_level == 0) {
          popped_rcb->rcb_queue_level = 1;
        } else if (popped_rcb->rcb_queue_level == 1) {
          popped_rcb->rcb_queue_level = 2;
        }
      }
      do {
        len = fread(buffer, 1, popped_rcb->rcb_quantum, popped_rcb->rcb_serv_handle);
        popped_rcb->rcb_file_bytes_remain = popped_rcb->rcb_file_bytes_remain - len;
        if (len < 0) {
          perror("Error while writing to client");
        } else if (len > 0) {
          printf("Sent %d bytes of file '%s'\n",
                 len, popped_rcb->rcb_file_name);
          len = write(popped_rcb->rcb_cli_desc, buffer, len);
          if (len < 1) {
            perror("Error while writing to client");
          }
        }
      } while (len == MAX_HTTP_SIZE); // write to client in chunks

      if (is_rr) { // enqueue for quantum unless last block
        if (popped_rcb->rcb_file_bytes_remain > popped_rcb->rcb_quantum) {
          lock_enqueue(popped_rcb, &max_queue);
        } else {
          close(popped_rcb->rcb_cli_desc);
          fclose(popped_rcb->rcb_serv_handle);
          printf("Request for file '%s' completed.\n", popped_rcb->rcb_file_name);
          free(popped_rcb);
        }
      } else if (is_sjf) { // never returned to the queue
        printf("Request for file '%s' completed.\n", popped_rcb->rcb_file_name);
        fflush(stdout);
        close(popped_rcb->rcb_cli_desc);
        fclose(popped_rcb->rcb_serv_handle);
        free(popped_rcb);
      } else if (is_mlfb) { // adjusts values before returning to one of the queues
        if (len == max_queue_quantum || len == mid_queue_quantum) {
          if (popped_rcb->rcb_queue_level == 1) {
            popped_rcb->rcb_quantum = mid_queue_quantum;
            lock_enqueue(popped_rcb, &mid_queue);
          } else if (popped_rcb->rcb_queue_level == 2) {
            popped_rcb->rcb_quantum = MAX_HTTP_SIZE;
            lock_enqueue(popped_rcb, &min_queue);
          }
        } else if (len < popped_rcb->rcb_quantum) { // unless last block
          printf("Request for file '%s' completed.\n", popped_rcb->rcb_file_name);
          fflush(stdout);
          close(popped_rcb->rcb_cli_desc);
          fclose(popped_rcb->rcb_serv_handle);
          free(popped_rcb);
        }
      }
    }
  }
}