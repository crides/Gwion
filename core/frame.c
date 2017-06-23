#include <stdlib.h>
#include "map.h"
#include "frame.h"

Frame* new_frame() {
  Frame* frame = calloc(1, sizeof(Frame));
  vector_init(&frame->stack);
  return frame;
}

void free_frame(Frame* a) {
  vtype i;
  for(i = vector_size(&a->stack) + 1; --i;)
    free((Local*)vector_at(&a->stack, i - 1));
  vector_release(&a->stack);
  free(a);
}

Local* frame_alloc_local(Frame* frame, m_uint size, m_str name, m_bool is_ref, m_bool is_obj) {
  Local* local = calloc(1, sizeof(Local));
  local->size = size;
  local->offset = frame->curr_offset;
  local->is_ref = is_ref;
  local->is_obj = is_obj;
  frame->curr_offset += local->size;
  local->name = name;
  vector_add(&frame->stack, (vtype)local);
  return local;
}

void frame_push_scope(Frame* frame) {
  vector_add(&frame->stack, (vtype)NULL);
}

void frame_pop_scope(Frame* frame, Vector v) {
  m_uint i;
  while((i = vector_size(&frame->stack) && vector_back(&frame->stack))) {
    Local* local = (Local*)vector_pop(&frame->stack);
    frame->curr_offset -= local->size;
    vector_add(v, (vtype)local);
  }
  vector_pop(&frame->stack);
}
