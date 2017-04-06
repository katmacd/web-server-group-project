#include <stdio.h>
#include <stdlib.h>
#include "priority_queue.h"

void enqueue(queue *h, int priority, rcb *data) {
  if (h->len + 1 >= h->size) {
    h->size = h->size ? h->size * 2 : 4;
    h->nodes = (node *) realloc(h->nodes, h->size * sizeof(node));
  }
  int i = h->len + 1;
  int j = i / 2;
  while (i > 1 && h->nodes[j].priority > priority) {
    h->nodes[i] = h->nodes[j];
    i = j;
    j = j / 2;
  }
  h->nodes[i].priority = priority;
  h->nodes[i].data = data;
  h->len++;
}

rcb *dequeue(queue *h) {
  int i, j, k;
  if (!h->len) return NULL;
  rcb *data = h->nodes[1].data;
  h->nodes[1] = h->nodes[h->len];
  h->len--;
  i = 1;
  while (1) {
    k = i;
    j = 2 * i;
    if (j <= h->len && h->nodes[j].priority < h->nodes[k].priority)
      k = j;
    if (j + 1 <= h->len && h->nodes[j + 1].priority < h->nodes[k].priority)
      k = j + 1;
    if (k == i)
      break;
    h->nodes[i] = h->nodes[k];
    i = k;
  }
  h->nodes[i] = h->nodes[h->len + 1];
  return data;
}
