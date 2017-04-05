#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "network.h"
#include "priority_queue.h"

#define MAX_HTTP_SIZE 80 //8892
#define NUM_THREADS 64

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int rr_quantum = 2; // = 2048;
int counter = 0;
heap_t max_queue;
heap_t mid_queue;
heap_t min_queue;

int max_queue_quantum = 2; //= 2048;
int mid_queue_quantum = 4; //= 4096;

int is_sjf = 0;
int is_mlfb = 0;
int is_rr = 0;

pthread_t init_threads[NUM_THREADS];
pthread_t client_serve_threads[NUM_THREADS];

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
  //printf("pushing with priority %d\n", new_rcb->rcb_priority);
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

  /* standard requests are of the form
   *   GET /foo/bar/qux.html HTTP/1.1
   * We want the second token (the file path).
   */
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
    fin = fopen(req, "r");
    if (!fin) {
      len = sprintf(buffer, "HTTP/1.1 404 File not found \n\n");
      write(fd, buffer, len);
      close(fd);
    } else {
      len = sprintf(buffer, "HTTP/1.1 200 OK\n\n");
      write(fd, buffer, len);
      rcb *push_rcb = create_rcb(fin, fd, buffer);
      lock_push(push_rcb, &max_queue);
    }
  }
  free(buffer);
}

void *init(void *data) {
  int fd;
  for (;;) {
    network_wait();

    for (fd = network_open(); fd >= 0; fd = network_open()) {
      init_client(fd);
    }
  }
}

rcb *pop_assign(int len) {
  rcb *popped_rcb;
  pthread_mutex_lock(&mutex);
  if (max_queue.len > 0) {
    popped_rcb = pop(&max_queue);
    //printf("pooping with priority %d\n", popped_rcb->rcb_priority);
    len = popped_rcb->rcb_file_bytes_remain;
  } else if (max_queue.len == 0 && mid_queue.len > 0 && is_mlfb) {
    popped_rcb = pop(&mid_queue);
    //printf("pooping with priority %d\n", popped_rcb->rcb_priority);
    len = popped_rcb->rcb_file_bytes_remain;
  } else if (max_queue.len == 0 && mid_queue.len == 0 && min_queue.len > 0 && is_mlfb) {
    popped_rcb = pop(&min_queue);
    //printf("pooping with priority %d\n", popped_rcb->rcb_priority);
    len = popped_rcb->rcb_file_bytes_remain;
  } else {
    popped_rcb = NULL;
  }
  pthread_mutex_unlock(&mutex);;
  return popped_rcb;
}

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

      do {
        len = fread(buffer, 1, popped_rcb->rcb_quantum, popped_rcb->rcb_serv_handle); /* read file chunk */
        popped_rcb->rcb_file_bytes_remain = popped_rcb->rcb_file_bytes_remain - len;
        if (len < 0) {
          perror("Error while writing to client");
        } else if (len > 0) {
          /* send chunk */
          //printf("%s\n", buffer);
          len = write(popped_rcb->rcb_cli_desc, buffer, len);
          if (len < 1) {
            perror("Error while writing to client");
          }
        }
      } while (len == MAX_HTTP_SIZE);

      if (is_rr) {
        if (popped_rcb->rcb_file_bytes_remain > popped_rcb->rcb_quantum) {
          lock_push(popped_rcb, &max_queue);
        } else {
          close(popped_rcb->rcb_cli_desc);
          fclose(popped_rcb->rcb_serv_handle);
          free(popped_rcb);
        }
      }

      if (is_sjf) {
        close(popped_rcb->rcb_cli_desc);
        fclose(popped_rcb->rcb_serv_handle);
        free(popped_rcb);
      }

      if (is_mlfb) {
        if (len == max_queue_quantum || len == mid_queue_quantum) {
          if (popped_rcb->rcb_queue_level == 1) {
            lock_push(popped_rcb, &mid_queue);
          }
          if (popped_rcb->rcb_queue_level == 2) {
            lock_push(popped_rcb, &min_queue);
          }
        } else if (len < rr_quantum) {
          close(popped_rcb->rcb_cli_desc);
          fclose(popped_rcb->rcb_serv_handle);
          free(popped_rcb);
        }
      }

    }
  }
}

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

  int i;
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_create(&client_serve_threads[i], NULL, work, NULL);
  }
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_create(&init_threads[i], NULL, init, NULL);
  }
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_join(init_threads[i], NULL);
  }
  for (i = 0; i < NUM_THREADS; i++) {
    pthread_join(client_serve_threads[i], NULL);
  }
  return EXIT_SUCCESS;
}