#ifndef PRIORITY_QUEUE_H

typedef struct {
  int rcb_seq_num;
  int rcb_cli_desc;
  FILE *rcb_serv_handle;
  int rcb_file_bytes_remain;
  int rcb_quantum;
  int rcb_priority;
  int rcb_queue_level;
} rcb;

typedef struct {
  int priority;
  rcb *data;
} node_t;

typedef struct {
  node_t *nodes;
  int len;
  int size;
} heap_t;

void push(heap_t *, int, rcb *);

rcb *pop(heap_t *);

#define PRIORITY_QUEUE_H

#endif