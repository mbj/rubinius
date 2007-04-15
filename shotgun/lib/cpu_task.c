#include "shotgun.h"
#include "cpu.h"
#include "machine.h"
#include <stdlib.h>
#include "tuple.h"
#include "methctx.h"
#include "object.h"
#include "bytearray.h"
#include "string.h"
#include "class.h"
#include "hash.h"
#include "symbol.h"
#include "list.h"
#include "array.h"
#include <glib.h>

#include "rubinius.h"

void cpu_task_cleanup(STATE, OBJECT self) {
  struct cpu_task *task;
  
  task = (struct cpu_task*)BYTES_OF(self);
  
  if(!task->stack_slave) {
    free(task->stack_top);
  }
}

void Init_cpu_task(STATE) {
  OBJECT tup;
  state_add_cleanup(state, state->global->task, cpu_task_cleanup);
  
  tup = tuple_new2(state, 7, list_new(state), list_new(state), list_new(state),
    list_new(state), list_new(state), list_new(state), list_new(state));
  
  state->global->scheduled_threads = tup;
  rbs_const_set(state, BASIC_CLASS(task), "ScheduledThreads", tup);
  
  BASIC_CLASS(channel) = rbs_class_new(state, "Channel", 2, BASIC_CLASS(object));
  BASIC_CLASS(thread) =  rbs_class_new(state, "Thread", 4, BASIC_CLASS(object));
  class_set_instance_flags(BASIC_CLASS(thread), I2N(0x02)); /* ivars allowed. */
  
  cpu_event_init(state);
}

OBJECT cpu_task_dup(STATE, cpu c, OBJECT cur) {
  struct cpu_task *cur_task, *task;
  
  OBJECT obj;
  
  if(NIL_P(cur)) {
    cur_task = (struct cpu_task*)CPU_TASKS_LOCATION(c);
  } else {
    cur_task = (struct cpu_task*)BYTES_OF(cur);
  }
  
  NEW_STRUCT(obj, task, state->global->task, struct cpu_task);
  memcpy(task, cur_task, sizeof(struct cpu_task));
  task->stack_slave = 1;
  
  return obj;
}

void cpu_task_select(STATE, cpu c, OBJECT nw) {
  struct cpu_task *cur_task, *new_task, *ct;
  OBJECT home, cur;
  cpu_save_registers(state, c);
  
  cur = c->current_task;
  
  ct = (struct cpu_task*)CPU_TASKS_LOCATION(c);
  cur_task = (struct cpu_task*)BYTES_OF(cur);
  new_task = (struct cpu_task*)BYTES_OF(nw);
  
  memcpy(cur_task, ct, sizeof(struct cpu_task));
  // printf(" Saving to task %p\t(%lu / %lu / %p / %p / %p)\n", (void*)cur, c->sp, c->ip, c->method, c->active_context, c->home_context);
  memcpy(ct, new_task, sizeof(struct cpu_task));
  
  home = NIL_P(c->home_context) ? c->active_context : c->home_context;
  
  cpu_restore_context_with_home(state, c, c->active_context, home, FALSE, FALSE);
  // printf("Swaping to task %p\t(%lu / %lu / %p / %p / %p)\n", (void*)nw, c->sp, c->ip, c->method, c->active_context, c->home_context);
  
  c->current_task = nw;
}

OBJECT cpu_task_associate(STATE, OBJECT self, OBJECT be) {
  OBJECT bc;
  struct cpu_task *task;
  
  task = (struct cpu_task*)BYTES_OF(self);
  
  bc = blokenv_create_context(state, be, Qnil, 0);
  task->stack_top = (OBJECT*)calloc(InitialStackSize, sizeof(OBJECT));
  task->sp_ptr = task->stack_top;
  task->stack_size = InitialStackSize;
  task->stack_slave = 0;
  
  task->main = bc;
  task->active_context = bc;
  task->home_context = blokenv_get_home(be);
  
  return bc;
}

void cpu_task_push(STATE, OBJECT self, OBJECT val) {
  struct cpu_task *task;
  
  task = (struct cpu_task*)BYTES_OF(self);
  task->sp_ptr++;
  *(task->sp_ptr) = val;
}

void cpu_task_set_top(STATE, OBJECT self, OBJECT val) {
  struct cpu_task *task;
  
  task = (struct cpu_task*)BYTES_OF(self);
  *(task->sp_ptr) = val;
}

OBJECT cpu_task_pop(STATE, OBJECT self) {
  struct cpu_task *task;
  
  task = (struct cpu_task*)BYTES_OF(self);
  return *task->sp_ptr--;
}

OBJECT cpu_task_top(STATE, OBJECT self) {
  struct cpu_task *task;
  
  task = (struct cpu_task*)BYTES_OF(self);
  return *task->sp_ptr;
}

void cpu_task_set_outstanding(STATE, OBJECT self, OBJECT ary) {
  struct cpu_task *task;
  
  task = (struct cpu_task*)BYTES_OF(self);
  task->outstanding = ary;
}

OBJECT cpu_task_get_outstanding(STATE, OBJECT self) {
  struct cpu_task *task;
  
  task = (struct cpu_task*)BYTES_OF(self);
  return task->outstanding;
}


#define thread_set_priority(obj, val) SET_FIELD(obj, 1, val)
#define thread_set_task(obj, val) SET_FIELD(obj, 2, val)
#define thread_set_joins(obj, val) SET_FIELD(obj, 3, val)

#define thread_get_priority(obj) NTH_FIELD(obj, 1)
#define thread_get_task(obj) NTH_FIELD(obj, 2)
#define thread_get_joins(obj) NTH_FIELD(obj, 3)

OBJECT cpu_thread_new(STATE, cpu c) {
  OBJECT thr;
  
  thr = rbs_class_new_instance(state, BASIC_CLASS(thread));
  if(c->current_thread) {
    thread_set_priority(thr, thread_get_priority(c->current_thread));
  } else {
    thread_set_priority(thr, I2N(2));
  }
  thread_set_task(thr, cpu_task_dup(state, c, Qnil));
  thread_set_joins(thr, list_new(state));
  return thr;
}

OBJECT cpu_thread_get_task(STATE, OBJECT self) {
  return thread_get_task(self);
}

void cpu_thread_schedule(STATE, OBJECT self) {
  long int prio, rprio;
  OBJECT lst;
  
  prio = FIXNUM_TO_INT(thread_get_priority(self));
  
  if(prio < 1) { 
    rprio = 1;
  } else if(prio > 7) {
    rprio = 7;
  } else {
    rprio = prio;
  }
  
  rprio--;
  lst = tuple_at(state, state->global->scheduled_threads, rprio);
  list_append(state, lst, self);
}

OBJECT cpu_thread_find_highest(STATE) {
  int i, t;
  OBJECT lst, tup;
  
  cpu_event_update(state);

  while(1) {
    tup = state->global->scheduled_threads;
    t = NUM_FIELDS(tup);
    for(i = t - 1; i >= 0; i--) {
      lst = tuple_at(state, tup, i);
      if(FIXNUM_TO_INT(list_get_count(lst)) != 0) {
        return list_shift(state, lst);
      }
    }
    // printf("Nothing to do, waiting for events.\n");
    if(!cpu_event_outstanding_p(state)) {
      printf("DEADLOCK!\n");
      abort();
    }
    cpu_event_run(state);
  }
}

void cpu_thread_switch(STATE, cpu c, OBJECT thr) {
  OBJECT task;

  /* Edge case. We could realize that we need to restore
     the already running thread (via the current thread waiting
     for an event), and we thus don't need to restore it. */
  if(thr == c->current_thread) return;
  
  /* Save the current task back into the current thread, in case
     Task's were used inside the thread itself (not just for the thread). */
  thread_set_task(c->current_thread, c->current_task);
  task = thread_get_task(thr);
  cpu_task_select(state, c, task);
  c->current_thread = thr;
}

void cpu_thread_run_best(STATE, cpu c) {
  OBJECT thr;
  
  thr = cpu_thread_find_highest(state);
  cpu_thread_switch(state, c, thr);
}

OBJECT cpu_channel_new(STATE) {
  OBJECT chan;
  chan = rbs_class_new_instance(state, BASIC_CLASS(channel));
  channel_set_waiting(chan, list_new(state));
  return chan;
}

void _cpu_channel_clear_outstanding(STATE, cpu c, OBJECT thr, OBJECT ary) {
  OBJECT t1, t2, t3;
  int k, j;
  
  t1 = array_get_tuple(ary);
  k = FIXNUM_TO_INT(array_get_total(ary));
  
  for(j = 0; j < k; j++) {
    t2 = tuple_at(state, t1, j);
    
    t3 = channel_get_waiting(t2);
    list_delete(state, t3, thr);
  }
  
}

OBJECT cpu_channel_send(STATE, cpu c, OBJECT self, OBJECT obj) {
  OBJECT lst, lst2, thr, task, tup, out;
  long int cur_prio, new_prio;
  
  /* Sort of like setjmp, don't allow nil to be sent, because nil means there
     is no value. I could use undef here, but I still don't like exposing
     undef. */
  
  if(NIL_P(obj)) obj = Qtrue;
  lst = channel_get_waiting(self);
  
  if(list_empty_p(lst)) {
    lst2 = channel_get_value(self);
    if(NIL_P(lst2)) {
      lst2 = list_new(state);
      channel_set_value(self, lst2);
    }
    list_append(state, lst2, obj);
  } else {
    tup = tuple_new2(state, 2, self, obj);
    thr = list_shift(state, lst);
    task = thread_get_task(thr);
    /* Edge case. After going all around, we've decided that the current
       task needs to be restored. Since it's not yet saved, we push it
       to the current stack, since thats the current task's stack. */
    if(task == c->current_task) {
      stack_pop();
      stack_push(tup);
      if(!NIL_P(c->outstanding)) {
        _cpu_channel_clear_outstanding(state, c, thr, c->outstanding);
      }
    } else {
      cpu_task_set_top(state, task, tup);
      out = cpu_task_get_outstanding(state, task);
      if(!NIL_P(out)) {
        _cpu_channel_clear_outstanding(state, c, thr, out);
      }
    }
    /* If we're resuming a thread thats of higher priority than we are, 
       we run it now, otherwise, we just schedule it to be run. */
    cur_prio = FIXNUM_TO_INT(thread_get_priority(c->current_thread));
    new_prio = FIXNUM_TO_INT(thread_get_priority(thr));
    if(new_prio > cur_prio) {
      cpu_thread_schedule(state, c->current_thread);
      cpu_thread_switch(state, c, thr);
    } else {
      cpu_thread_schedule(state, thr);
    }
  }
  
  return obj;
}

void cpu_channel_register(STATE, cpu c, OBJECT self, OBJECT cur_thr) {
  OBJECT lst;
    
  lst = channel_get_waiting(self);
  list_append(state, lst, cur_thr);
}

void cpu_channel_receive(STATE, cpu c, OBJECT self, OBJECT cur_thr) {
  OBJECT val, obj, lst;
  
  val = channel_get_value(self);
  if(!NIL_P(val) && !list_empty_p(val)) {
    obj = list_shift(state, val);
    stack_push(tuple_new2(state, 2, self, obj));
    return;
  }
  
  /* We push nil on the stack to reserve a place to put the result. */
  stack_push(I2N(343434));
  
  lst = channel_get_waiting(self);
  list_append(state, lst, cur_thr);
  cpu_thread_run_best(state, c);
}

