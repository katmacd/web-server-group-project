#ifndef PRIORITY_QUEUE_H

typedef struct {
  int rcb_seq_num;
  int rcb_cli_desc;
  FILE *rcb_serv_handle;
  int rcb_file_bytes_remain;
  int rcb_quantum;
  int rcb_priority;
  int rcb_queue_level;
  char* rcb_file_name;
} rcb;

typedef struct {
  int priority;
  rcb *data;
} node;

typedef struct {
  node *nodes;
  int len;
  int size;
} queue;

void enqueue(queue *, int, rcb *);
rcb *dequeue(queue *);

#define PRIORITY_QUEUE_H

#endif