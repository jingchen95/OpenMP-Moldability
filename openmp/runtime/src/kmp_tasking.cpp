/*
 * kmp_tasking.cpp -- OpenMP 3.0 tasking support.
 */

//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "kmp.h"
#include "kmp_i18n.h"
#include "kmp_itt.h"
#include "kmp_stats.h"
#include "kmp_wait_release.h"
#include "kmp_taskdeps.h"

//ME1
#if INTERPOLATION == ONLINE
#include "interpolation.h"
#endif
using namespace std::chrono_literals;

std::unordered_map<kmp_routine_entry_t, task_definition_t> task_map;
std::mutex task_map_m;

std::mutex performance_model_m; // lock for the kmp_performance struct
std::mutex scheduler_m; // lock for the scheduler construct
kmp_scheduler kmp_sched;
kmp_scheduler *kmp_sched_p = &kmp_sched;
kmp_performance kmp_perf;
kmp_performance *kmp_perf_p = &kmp_perf;
//ME2


#if OMPT_SUPPORT
#include "ompt-specific.h"
#endif

/* forward declaration */
static void __kmp_enable_tasking(kmp_task_team_t *task_team,
                                 kmp_info_t *this_thr);
static void __kmp_alloc_task_deque(kmp_info_t *thread,
                                   kmp_thread_data_t *thread_data);
static int __kmp_realloc_task_threads_data(kmp_info_t *thread,
                                           kmp_task_team_t *task_team);
static void __kmp_bottom_half_finish_proxy(kmp_int32 gtid, kmp_task_t *ptask);

//ME1
static kmp_int32 __kmp_schedule_task(kmp_info_t *thread, kmp_task_t *task,
                                kmp_int32 tid);
static void __kmp_performance_model_init();
//static void __kmp_performance_model_reset(kmp_uint8 cluster); // unused
static void __kmp_performance_model_add(kmp_uint8 cluster, kmp_uint8 tasktype, kmp_uint32 execution_time, kmp_uint8 width);
static kmp_uint32 __kmp_performance_model_get(const kmp_uint8 cluster, const kmp_uint8 tasktype, const kmp_uint8 width);
static void __kmp_thread_active_status(kmp_uint8 cluster, kmp_uint8 pos, kmp_uint8 status);
static void __kmp_scheduler_init(kmp_info_t *thread);
static kmp_uint8 __kmp_scheduler_add_thread(kmp_uint8 cluster, kmp_int32 tid);
#if PERF_DYNAMIC_ON
static void __kmp_perf_enable(kmp_info_t *thread);
static void __kmp_perf_disable(kmp_info_t *thread);
#endif
static void __kmp_perf_init_counters(kmp_info_t *thread);
//static kmp_int32 __kmp_task_mapping(kmp_info_t *thread, kmp_task_t *task, kmp_int32 tid);
//ME2

#ifdef BUILD_TIED_TASK_STACK

//  __kmp_trace_task_stack: print the tied tasks from the task stack in order
//  from top do bottom
//
//  gtid: global thread identifier for thread containing stack
//  thread_data: thread data for task team thread containing stack
//  threshold: value above which the trace statement triggers
//  location: string identifying call site of this function (for trace)
static void __kmp_trace_task_stack(kmp_int32 gtid,
                                   kmp_thread_data_t *thread_data,
                                   int threshold, char *location) {
  kmp_task_stack_t *task_stack = &thread_data->td.td_susp_tied_tasks;
  kmp_taskdata_t **stack_top = task_stack->ts_top;
  kmp_int32 entries = task_stack->ts_entries;
  kmp_taskdata_t *tied_task;

  KA_TRACE(
      threshold,
      ("__kmp_trace_task_stack(start): location = %s, gtid = %d, entries = %d, "
       "first_block = %p, stack_top = %p \n",
       location, gtid, entries, task_stack->ts_first_block, stack_top));

  KMP_DEBUG_ASSERT(stack_top != NULL);
  KMP_DEBUG_ASSERT(entries > 0);

  while (entries != 0) {
    KMP_DEBUG_ASSERT(stack_top != &task_stack->ts_first_block.sb_block[0]);
    // fix up ts_top if we need to pop from previous block
    if (entries & TASK_STACK_INDEX_MASK == 0) {
      kmp_stack_block_t *stack_block = (kmp_stack_block_t *)(stack_top);

      stack_block = stack_block->sb_prev;
      stack_top = &stack_block->sb_block[TASK_STACK_BLOCK_SIZE];
    }

    // finish bookkeeping
    stack_top--;
    entries--;

    tied_task = *stack_top;

    KMP_DEBUG_ASSERT(tied_task != NULL);
    KMP_DEBUG_ASSERT(tied_task->td_flags.tasktype == TASK_TIED);

    KA_TRACE(threshold,
             ("__kmp_trace_task_stack(%s):             gtid=%d, entry=%d, "
              "stack_top=%p, tied_task=%p\n",
              location, gtid, entries, stack_top, tied_task));
  }
  KMP_DEBUG_ASSERT(stack_top == &task_stack->ts_first_block.sb_block[0]);

  KA_TRACE(threshold,
           ("__kmp_trace_task_stack(exit): location = %s, gtid = %d\n",
            location, gtid));
}

//  __kmp_init_task_stack: initialize the task stack for the first time
//  after a thread_data structure is created.
//  It should not be necessary to do this again (assuming the stack works).
//
//  gtid: global thread identifier of calling thread
//  thread_data: thread data for task team thread containing stack
static void __kmp_init_task_stack(kmp_int32 gtid,
                                  kmp_thread_data_t *thread_data) {
  kmp_task_stack_t *task_stack = &thread_data->td.td_susp_tied_tasks;
  kmp_stack_block_t *first_block;

  // set up the first block of the stack
  first_block = &task_stack->ts_first_block;
  task_stack->ts_top = (kmp_taskdata_t **)first_block;
  memset((void *)first_block, '\0',
         TASK_STACK_BLOCK_SIZE * sizeof(kmp_taskdata_t *));

  // initialize the stack to be empty
  task_stack->ts_entries = TASK_STACK_EMPTY;
  first_block->sb_next = NULL;
  first_block->sb_prev = NULL;
}

//  __kmp_free_task_stack: free the task stack when thread_data is destroyed.
//
//  gtid: global thread identifier for calling thread
//  thread_data: thread info for thread containing stack
static void __kmp_free_task_stack(kmp_int32 gtid,
                                  kmp_thread_data_t *thread_data) {
  kmp_task_stack_t *task_stack = &thread_data->td.td_susp_tied_tasks;
  kmp_stack_block_t *stack_block = &task_stack->ts_first_block;

  KMP_DEBUG_ASSERT(task_stack->ts_entries == TASK_STACK_EMPTY);
  // free from the second block of the stack
  while (stack_block != NULL) {
    kmp_stack_block_t *next_block = (stack_block) ? stack_block->sb_next : NULL;

    stack_block->sb_next = NULL;
    stack_block->sb_prev = NULL;
    if (stack_block != &task_stack->ts_first_block) {
      __kmp_thread_free(thread,
                        stack_block); // free the block, if not the first
    }
    stack_block = next_block;
  }
  // initialize the stack to be empty
  task_stack->ts_entries = 0;
  task_stack->ts_top = NULL;
}

//  __kmp_push_task_stack: Push the tied task onto the task stack.
//     Grow the stack if necessary by allocating another block.
//
//  gtid: global thread identifier for calling thread
//  thread: thread info for thread containing stack
//  tied_task: the task to push on the stack
static void __kmp_push_task_stack(kmp_int32 gtid, kmp_info_t *thread,
                                  kmp_taskdata_t *tied_task) {
  // GEH - need to consider what to do if tt_threads_data not allocated yet
  kmp_thread_data_t *thread_data =
      &thread->th.th_task_team->tt.tt_threads_data[__kmp_tid_from_gtid(gtid)];
  kmp_task_stack_t *task_stack = &thread_data->td.td_susp_tied_tasks;

  if (tied_task->td_flags.team_serial || tied_task->td_flags.tasking_ser) {
    return; // Don't push anything on stack if team or team tasks are serialized
  }

  KMP_DEBUG_ASSERT(tied_task->td_flags.tasktype == TASK_TIED);
  KMP_DEBUG_ASSERT(task_stack->ts_top != NULL);

  KA_TRACE(20,
           ("__kmp_push_task_stack(enter): GTID: %d; THREAD: %p; TASK: %p\n",
            gtid, thread, tied_task));
  // Store entry
  *(task_stack->ts_top) = tied_task;

  // Do bookkeeping for next push
  task_stack->ts_top++;
  task_stack->ts_entries++;

  if (task_stack->ts_entries & TASK_STACK_INDEX_MASK == 0) {
    // Find beginning of this task block
    kmp_stack_block_t *stack_block =
        (kmp_stack_block_t *)(task_stack->ts_top - TASK_STACK_BLOCK_SIZE);

    // Check if we already have a block
    if (stack_block->sb_next !=
        NULL) { // reset ts_top to beginning of next block
      task_stack->ts_top = &stack_block->sb_next->sb_block[0];
    } else { // Alloc new block and link it up
      kmp_stack_block_t *new_block = (kmp_stack_block_t *)__kmp_thread_calloc(
          thread, sizeof(kmp_stack_block_t));

      task_stack->ts_top = &new_block->sb_block[0];
      stack_block->sb_next = new_block;
      new_block->sb_prev = stack_block;
      new_block->sb_next = NULL;

      KA_TRACE(
          30,
          ("__kmp_push_task_stack(): GTID: %d; TASK: %p; Alloc new block: %p\n",
           gtid, tied_task, new_block));
    }
  }
  KA_TRACE(20, ("__kmp_push_task_stack(exit): GTID: %d; TASK: %p\n", gtid,
                tied_task));
}

//  __kmp_pop_task_stack: Pop the tied task from the task stack.  Don't return
//  the task, just check to make sure it matches the ending task passed in.
//
//  gtid: global thread identifier for the calling thread
//  thread: thread info structure containing stack
//  tied_task: the task popped off the stack
//  ending_task: the task that is ending (should match popped task)
static void __kmp_pop_task_stack(kmp_int32 gtid, kmp_info_t *thread,
                                 kmp_taskdata_t *ending_task) {
  // GEH - need to consider what to do if tt_threads_data not allocated yet
  kmp_thread_data_t *thread_data =
      &thread->th.th_task_team->tt_threads_data[__kmp_tid_from_gtid(gtid)];
  kmp_task_stack_t *task_stack = &thread_data->td.td_susp_tied_tasks;
  kmp_taskdata_t *tied_task;

  if (ending_task->td_flags.team_serial || ending_task->td_flags.tasking_ser) {
    // Don't pop anything from stack if team or team tasks are serialized
    return;
  }

  KMP_DEBUG_ASSERT(task_stack->ts_top != NULL);
  KMP_DEBUG_ASSERT(task_stack->ts_entries > 0);

  KA_TRACE(20, ("__kmp_pop_task_stack(enter): GTID: %d; THREAD: %p\n", gtid,
                thread));

  // fix up ts_top if we need to pop from previous block
  if (task_stack->ts_entries & TASK_STACK_INDEX_MASK == 0) {
    kmp_stack_block_t *stack_block = (kmp_stack_block_t *)(task_stack->ts_top);

    stack_block = stack_block->sb_prev;
    task_stack->ts_top = &stack_block->sb_block[TASK_STACK_BLOCK_SIZE];
  }

  // finish bookkeeping
  task_stack->ts_top--;
  task_stack->ts_entries--;

  tied_task = *(task_stack->ts_top);

  KMP_DEBUG_ASSERT(tied_task != NULL);
  KMP_DEBUG_ASSERT(tied_task->td_flags.tasktype == TASK_TIED);
  KMP_DEBUG_ASSERT(tied_task == ending_task); // If we built the stack correctly

  KA_TRACE(20, ("__kmp_pop_task_stack(exit): GTID: %d; TASK: %p\n", gtid,
                tied_task));
  return;
}
#endif /* BUILD_TIED_TASK_STACK */
//ME1
//Gets the definition of a task from its routine entry
static task_definition_t __kmp_get_def(kmp_routine_entry_t routine){
    // lock map
    std::lock_guard<std::mutex> lock(task_map_m);
    task_definition_t res = task_map[routine];
    return res;
}

//Checks if a routine is present in the map
static bool __kmp_contains_def(kmp_routine_entry_t routine){
    std::lock_guard<std::mutex> lock(task_map_m);
    auto search = task_map.find( routine);
    if (search != task_map.end()) {
        return true;
    } else {
        return false;
    }
}

//Adds a <routine,definition> pair to the map, may overwrite a previous value.
static void __kmp_add_def(kmp_routine_entry_t routine, task_definition_t definition){
    std::lock_guard<std::mutex> lock(task_map_m);
    task_map[routine] = definition;
}

//ME2

// returns 1 if new task is allowed to execute, 0 otherwise
// checks Task Scheduling constraint (if requested) and
// mutexinoutset dependencies if any
static bool __kmp_task_is_allowed(int gtid, const kmp_int32 is_constrained,
                                  const kmp_taskdata_t *tasknew,
                                  const kmp_taskdata_t *taskcurr) {
  if (is_constrained && (tasknew->td_flags.tiedness == TASK_TIED)) {
    // Check if the candidate obeys the Task Scheduling Constraints (TSC)
    // only descendant of all deferred tied tasks can be scheduled, checking
    // the last one is enough, as it in turn is the descendant of all others
    kmp_taskdata_t *current = taskcurr->td_last_tied;
    KMP_DEBUG_ASSERT(current != NULL);
    // check if the task is not suspended on barrier
    if (current->td_flags.tasktype == TASK_EXPLICIT ||
        current->td_taskwait_thread > 0) { // <= 0 on barrier
      kmp_int32 level = current->td_level;
      kmp_taskdata_t *parent = tasknew->td_parent;
      while (parent != current && parent->td_level > level) {
        // check generation up to the level of the current task
        parent = parent->td_parent;
        KMP_DEBUG_ASSERT(parent != NULL);
      }
      if (parent != current)
        return false;
    }
  }
  // Check mutexinoutset dependencies, acquire locks
  kmp_depnode_t *node = tasknew->td_depnode;
  if (UNLIKELY(node && (node->dn.mtx_num_locks > 0))) {
    for (int i = 0; i < node->dn.mtx_num_locks; ++i) {
      KMP_DEBUG_ASSERT(node->dn.mtx_locks[i] != NULL);
      if (__kmp_test_lock(node->dn.mtx_locks[i], gtid))
        continue;
      // could not get the lock, release previous locks
      for (int j = i - 1; j >= 0; --j)
        __kmp_release_lock(node->dn.mtx_locks[j], gtid);
      return false;
    }
    // negative num_locks means all locks acquired successfully
    node->dn.mtx_num_locks = -node->dn.mtx_num_locks;
  }
  return true;
}

// __kmp_realloc_task_deque:
// Re-allocates a task deque for a particular thread, copies the content from
// the old deque and adjusts the necessary data structures relating to the
// deque. This operation must be done with the deque_lock being held
static void __kmp_realloc_task_deque(kmp_info_t *thread,
                                     kmp_thread_data_t *thread_data) {
  kmp_int32 size = TASK_DEQUE_SIZE(thread_data->td);
  KMP_DEBUG_ASSERT(TCR_4(thread_data->td.td_deque_ntasks) == size);
  kmp_int32 new_size = 2 * size;

  KE_TRACE(10, ("__kmp_realloc_task_deque: T#%d reallocating deque[from %d to "
                "%d] for thread_data %p\n",
                __kmp_gtid_from_thread(thread), size, new_size, thread_data));

  kmp_taskdata_t **new_deque =
      (kmp_taskdata_t **)__kmp_allocate(new_size * sizeof(kmp_taskdata_t *));

  int i, j;
  for (i = thread_data->td.td_deque_head, j = 0; j < size;
       i = (i + 1) & TASK_DEQUE_MASK(thread_data->td), j++)
    new_deque[j] = thread_data->td.td_deque[i];

  __kmp_free(thread_data->td.td_deque);

  thread_data->td.td_deque_head = 0;
  thread_data->td.td_deque_tail = size;
  thread_data->td.td_deque = new_deque;
  thread_data->td.td_deque_size = new_size;
}

//  __kmp_push_task: Add a task to the thread's deque
static kmp_int32 __kmp_push_task(kmp_int32 gtid, kmp_task_t *task) {
  kmp_info_t *thread = __kmp_threads[gtid];
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);

  // If we encounter a hidden helper task, and the current thread is not a
  // hidden helper thread, we have to give the task to any hidden helper thread
  // starting from its shadow one.
  if (UNLIKELY(taskdata->td_flags.hidden_helper &&
               !KMP_HIDDEN_HELPER_THREAD(gtid))) {
    kmp_int32 shadow_gtid = KMP_GTID_TO_SHADOW_GTID(gtid);
    __kmpc_give_task(task, __kmp_tid_from_gtid(shadow_gtid));
    // Signal the hidden helper threads.
    __kmp_hidden_helper_worker_thread_signal();
    return TASK_SUCCESSFULLY_PUSHED;
  }

  kmp_task_team_t *task_team = thread->th.th_task_team;
  kmp_int32 tid = __kmp_tid_from_gtid(gtid);
  kmp_thread_data_t *thread_data;

  KA_TRACE(20,
           ("__kmp_push_task: T#%d trying to push task %p.\n", gtid, taskdata));

  if (UNLIKELY(taskdata->td_flags.tiedness == TASK_UNTIED)) {
    // untied task needs to increment counter so that the task structure is not
    // freed prematurely
    kmp_int32 counter = 1 + KMP_ATOMIC_INC(&taskdata->td_untied_count);
    KMP_DEBUG_USE_VAR(counter);
    KA_TRACE(
        20,
        ("__kmp_push_task: T#%d untied_count (%d) incremented for task %p\n",
         gtid, counter, taskdata));
  }

  // The first check avoids building task_team thread data if serialized
  if (UNLIKELY(taskdata->td_flags.task_serial)) {
    KA_TRACE(20, ("__kmp_push_task: T#%d team serialized; returning "
                  "TASK_NOT_PUSHED for task %p\n",
                  gtid, taskdata));
    return TASK_NOT_PUSHED;
  }

  // Now that serialized tasks have returned, we can assume that we are not in
  // immediate exec mode
  KMP_DEBUG_ASSERT(__kmp_tasking_mode != tskm_immediate_exec);
  if (UNLIKELY(!KMP_TASKING_ENABLED(task_team))) {
    __kmp_enable_tasking(task_team, thread);
  }
  KMP_DEBUG_ASSERT(TCR_4(task_team->tt.tt_found_tasks) == TRUE);
  KMP_DEBUG_ASSERT(TCR_PTR(task_team->tt.tt_threads_data) != NULL);

  // Find tasking deque specific to encountering thread
  thread_data = &task_team->tt.tt_threads_data[tid];

  // No lock needed since only owner can allocate. If the task is hidden_helper,
  // we don't need it either because we have initialized the dequeue for hidden
  // helper thread data.
  if (UNLIKELY(thread_data->td.td_deque == NULL)) {
    __kmp_alloc_task_deque(thread, thread_data);
  }

  int locked = 0;
  // Check if deque is full
  if (TCR_4(thread_data->td.td_deque_ntasks) >=
      TASK_DEQUE_SIZE(thread_data->td)) {
    if (__kmp_enable_task_throttling &&
        __kmp_task_is_allowed(gtid, __kmp_task_stealing_constraint, taskdata,
                              thread->th.th_current_task)) {
      KA_TRACE(20, ("__kmp_push_task: T#%d deque is full; returning "
                    "TASK_NOT_PUSHED for task %p\n",
                    gtid, taskdata));
      return TASK_NOT_PUSHED;
    } else {
      __kmp_acquire_bootstrap_lock(&thread_data->td.td_deque_lock);
      locked = 1;
      if (TCR_4(thread_data->td.td_deque_ntasks) >=
          TASK_DEQUE_SIZE(thread_data->td)) {
        // expand deque to push the task which is not allowed to execute
        __kmp_realloc_task_deque(thread, thread_data);
      }
    }
  }
  // Lock the deque for the task push operation
  if (!locked) {
    __kmp_acquire_bootstrap_lock(&thread_data->td.td_deque_lock);
    // Need to recheck as we can get a proxy task from thread outside of OpenMP
    if (TCR_4(thread_data->td.td_deque_ntasks) >=
        TASK_DEQUE_SIZE(thread_data->td)) {
      if (__kmp_enable_task_throttling &&
          __kmp_task_is_allowed(gtid, __kmp_task_stealing_constraint, taskdata,
                                thread->th.th_current_task)) {
        __kmp_release_bootstrap_lock(&thread_data->td.td_deque_lock);
        KA_TRACE(20, ("__kmp_push_task: T#%d deque is full on 2nd check; "
                      "returning TASK_NOT_PUSHED for task %p\n",
                      gtid, taskdata));
        return TASK_NOT_PUSHED;
      } else {
        // expand deque to push the task which is not allowed to execute
        __kmp_realloc_task_deque(thread, thread_data);
      }
    }
  }
  // Must have room since no thread can add tasks but calling thread
  KMP_DEBUG_ASSERT(TCR_4(thread_data->td.td_deque_ntasks) <
                   TASK_DEQUE_SIZE(thread_data->td));

  //ME1
   __kmp_release_bootstrap_lock(&thread_data->td.td_deque_lock);
  kmp_int32 schedule_result = __kmp_schedule_task(thread, task, tid);
  __kmp_acquire_bootstrap_lock(&thread_data->td.td_deque_lock);
  kmp_int32 push_result;
  if (schedule_result  == TASK_SUCCESSFULLY_SCHEDULED){
      push_result = TASK_SUCCESSFULLY_PUSHED;
  }else if (schedule_result == INVOKE_NOW &&
          __kmp_task_is_allowed(gtid, __kmp_task_stealing_constraint, taskdata,
                                thread->th.th_current_task)){
      push_result = TASK_NOT_PUSHED;
  }
  else{
    thread_data->td.td_deque[thread_data->td.td_deque_tail] =
        taskdata; // Push taskdata
#if DEBUG_PRINT_THREAD_INFO
    printf("Task %d pushed by thread %d to own queue\n", taskdata->td_task_id, __kmp_tid_from_gtid(gtid));
#endif

    thread_data->td.td_deque_tail =
        (thread_data->td.td_deque_tail + 1) & TASK_DEQUE_MASK(thread_data->td);
    TCW_4(thread_data->td.td_deque_ntasks,
          TCR_4(thread_data->td.td_deque_ntasks) + 1); // Adjust task count
    KMP_FSYNC_RELEASING(thread->th.th_current_task); // releasing self
    KMP_FSYNC_RELEASING(taskdata); // releasing child
    push_result = TASK_SUCCESSFULLY_PUSHED;
    KA_TRACE(20,
             ("__kmp_push_task: T#%d returning TASK_SUCCESSFULLY_PUSHED: "
              "task=%p ntasks=%d head=%u tail=%u\n",
              gtid, taskdata, thread_data->td.td_deque_ntasks,
              thread_data->td.td_deque_head, thread_data->td.td_deque_tail));
  }
  //ME2

  //ME1
  /*
  thread_data->td.td_deque[thread_data->td.td_deque_tail] =
      taskdata; // Push taskdata
  // Wrap index.
  thread_data->td.td_deque_tail =
      (thread_data->td.td_deque_tail + 1) & TASK_DEQUE_MASK(thread_data->td);
  TCW_4(thread_data->td.td_deque_ntasks,
        TCR_4(thread_data->td.td_deque_ntasks) + 1); // Adjust task count
  KMP_FSYNC_RELEASING(thread->th.th_current_task); // releasing self
  KMP_FSYNC_RELEASING(taskdata); // releasing child
  KA_TRACE(20, ("__kmp_push_task: T#%d returning TASK_SUCCESSFULLY_PUSHED: "
                "task=%p ntasks=%d head=%u tail=%u\n",
                gtid, taskdata, thread_data->td.td_deque_ntasks,
                thread_data->td.td_deque_head, thread_data->td.td_deque_tail));
   */
  //ME2
  __kmp_release_bootstrap_lock(&thread_data->td.td_deque_lock);

  return push_result;
}

// __kmp_pop_current_task_from_thread: set up current task from called thread
// when team ends
//
// this_thr: thread structure to set current_task in.
void __kmp_pop_current_task_from_thread(kmp_info_t *this_thr) {
  KF_TRACE(10, ("__kmp_pop_current_task_from_thread(enter): T#%d "
                "this_thread=%p, curtask=%p, "
                "curtask_parent=%p\n",
                0, this_thr, this_thr->th.th_current_task,
                this_thr->th.th_current_task->td_parent));

  this_thr->th.th_current_task = this_thr->th.th_current_task->td_parent;

  KF_TRACE(10, ("__kmp_pop_current_task_from_thread(exit): T#%d "
                "this_thread=%p, curtask=%p, "
                "curtask_parent=%p\n",
                0, this_thr, this_thr->th.th_current_task,
                this_thr->th.th_current_task->td_parent));
}

// __kmp_push_current_task_to_thread: set up current task in called thread for a
// new team
//
// this_thr: thread structure to set up
// team: team for implicit task data
// tid: thread within team to set up
void __kmp_push_current_task_to_thread(kmp_info_t *this_thr, kmp_team_t *team,
                                       int tid) {
  // current task of the thread is a parent of the new just created implicit
  // tasks of new team
  KF_TRACE(10, ("__kmp_push_current_task_to_thread(enter): T#%d this_thread=%p "
                "curtask=%p "
                "parent_task=%p\n",
                tid, this_thr, this_thr->th.th_current_task,
                team->t.t_implicit_task_taskdata[tid].td_parent));

  KMP_DEBUG_ASSERT(this_thr != NULL);

  if (tid == 0) {
    if (this_thr->th.th_current_task != &team->t.t_implicit_task_taskdata[0]) {
      team->t.t_implicit_task_taskdata[0].td_parent =
          this_thr->th.th_current_task;
      this_thr->th.th_current_task = &team->t.t_implicit_task_taskdata[0];
    }
  } else {
    team->t.t_implicit_task_taskdata[tid].td_parent =
        team->t.t_implicit_task_taskdata[0].td_parent;
    this_thr->th.th_current_task = &team->t.t_implicit_task_taskdata[tid];
  }

  KF_TRACE(10, ("__kmp_push_current_task_to_thread(exit): T#%d this_thread=%p "
                "curtask=%p "
                "parent_task=%p\n",
                tid, this_thr, this_thr->th.th_current_task,
                team->t.t_implicit_task_taskdata[tid].td_parent));
}


// __kmp_task_start: bookkeeping for a task starting execution
//
// GTID: global thread id of calling thread
// task: task starting execution
// current_task: task suspending
static void __kmp_task_start(kmp_int32 gtid, kmp_task_t *task,
                             kmp_taskdata_t *current_task) {
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  kmp_info_t *thread = __kmp_threads[gtid];

  KA_TRACE(10,
           ("__kmp_task_start(enter): T#%d starting task %p: current_task=%p\n",
            gtid, taskdata, current_task));

  KMP_DEBUG_ASSERT(taskdata->td_flags.tasktype == TASK_EXPLICIT);
  // mark currently executing task as suspended
  // TODO: GEH - make sure root team implicit task is initialized properly.
  // KMP_DEBUG_ASSERT( current_task -> td_flags.executing == 1 );
  current_task->td_flags.executing = 0;

  //ME1
  // store away the execution time of current task before starting on the new one

  // If counters aren't init, do so
  if (thread->th.th_perf_init_flag == 0) __kmp_perf_init_counters(thread);

  kmp_real64 current_time = 0;
  __kmp_read_system_time(&current_time);
  // Add extra execution time
  current_task->td_previous_exectime +=  (current_time - current_task->td_starttime);
#if DEBUG_PRINT_THREAD_INFO
  kmp_int32 tid = __kmp_tid_from_gtid(gtid);
  // Debug message
  printf("Started working on task %d on thread %d\n", taskdata->td_task_id, tid);
#endif


  //Classification just testing so far

  /*
  if(__kmp_contains_def(task->routine)){
      printf("Routine %d already in map with value %d\n", task->routine, __kmp_get_def(task->routine));
  }else{
      __kmp_add_def(task->routine, compute_bound);
      printf("adding routine %d\n", task->routine);
  }
  */
  // Only read the cache miss counter if the task is undefined
  if (taskdata->td_task_type == TASK_UNDEFINED) {
      // PERF
      // Get value

      #if !PERF_DISABLE

      #if PERF_DYNAMIC_ON
      // Start the counters if not active
      if (thread->th.th_counters_active == 0) __kmp_perf_enable(thread);
      #endif

      // If active, increment the number of tasks currently using them
      // TODO this may cause trouble if task migrates between threads.... removed for now
      //else ++thread->th.th_counters_active;

      kmp_uint64 current_cycles = 0;
      kmp_uint64 current_cachemiss = 0;
      // Read from counters
      read(thread->th.th_counter_cycles, &current_cycles, sizeof(current_cycles));

      #if DEBUG_PRINT_TASK_INFO
      kmp_uint64 current_instructions = 0;
      read(thread->th.th_counter_instructions, &current_instructions, sizeof(current_instructions));
      current_task->td_instructions_prev += (current_instructions - current_task->td_instructions_start);
      taskdata->td_instructions_start = current_instructions;
      #endif

      read(thread->th.th_counter_cachemiss, &current_cachemiss, sizeof(current_cachemiss));
      // Add on previous counters
      current_task->td_cycles_prev += (current_cycles - current_task->td_cycles_start);
      current_task->td_cachemiss_prev += (current_cachemiss - current_task->td_cachemiss_start);
      // Update starting counters
      taskdata->td_cycles_start = current_cycles;
      taskdata->td_cachemiss_start = current_cachemiss;

      // Starting counters takes time, read system time again
      __kmp_read_system_time(&current_time);

      #endif
  }
  // Set task start time
  taskdata->td_starttime = current_time;

  //ME2

// Add task to stack if tied
#ifdef BUILD_TIED_TASK_STACK
  if (taskdata->td_flags.tiedness == TASK_TIED) {
    __kmp_push_task_stack(gtid, thread, taskdata);
  }
#endif /* BUILD_TIED_TASK_STACK */

  // mark starting task as executing and as current task
  thread->th.th_current_task = taskdata;

  KMP_DEBUG_ASSERT(taskdata->td_flags.started == 0 ||
                   taskdata->td_flags.tiedness == TASK_UNTIED);
  KMP_DEBUG_ASSERT(taskdata->td_flags.executing == 0 ||
                   taskdata->td_flags.tiedness == TASK_UNTIED);
  taskdata->td_flags.started = 1;
  taskdata->td_flags.executing = 1;
  KMP_DEBUG_ASSERT(taskdata->td_flags.complete == 0);
  KMP_DEBUG_ASSERT(taskdata->td_flags.freed == 0);

  // GEH TODO: shouldn't we pass some sort of location identifier here?
  // APT: yes, we will pass location here.
  // need to store current thread state (in a thread or taskdata structure)
  // before setting work_state, otherwise wrong state is set after end of task

  KA_TRACE(10, ("__kmp_task_start(exit): T#%d task=%p\n", gtid, taskdata));

  return;
}

#if OMPT_SUPPORT
//------------------------------------------------------------------------------
// __ompt_task_init:
//   Initialize OMPT fields maintained by a task. This will only be called after
//   ompt_start_tool, so we already know whether ompt is enabled or not.

static inline void __ompt_task_init(kmp_taskdata_t *task, int tid) {
  // The calls to __ompt_task_init already have the ompt_enabled condition.
  task->ompt_task_info.task_data.value = 0;
  task->ompt_task_info.frame.exit_frame = ompt_data_none;
  task->ompt_task_info.frame.enter_frame = ompt_data_none;
  task->ompt_task_info.frame.exit_frame_flags =
      ompt_frame_runtime | ompt_frame_framepointer;
  task->ompt_task_info.frame.enter_frame_flags =
      ompt_frame_runtime | ompt_frame_framepointer;
}

// __ompt_task_start:
//   Build and trigger task-begin event
static inline void __ompt_task_start(kmp_task_t *task,
                                     kmp_taskdata_t *current_task,
                                     kmp_int32 gtid) {
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  ompt_task_status_t status = ompt_task_switch;
  if (__kmp_threads[gtid]->th.ompt_thread_info.ompt_task_yielded) {
    status = ompt_task_yield;
    __kmp_threads[gtid]->th.ompt_thread_info.ompt_task_yielded = 0;
  }
  /* let OMPT know that we're about to run this task */
  if (ompt_enabled.ompt_callback_task_schedule) {
    ompt_callbacks.ompt_callback(ompt_callback_task_schedule)(
        &(current_task->ompt_task_info.task_data), status,
        &(taskdata->ompt_task_info.task_data));
  }
  taskdata->ompt_task_info.scheduling_parent = current_task;
}

// __ompt_task_finish:
//   Build and trigger final task-schedule event
static inline void __ompt_task_finish(kmp_task_t *task,
                                      kmp_taskdata_t *resumed_task,
                                      ompt_task_status_t status) {
  if (ompt_enabled.ompt_callback_task_schedule) {
    kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
    if (__kmp_omp_cancellation && taskdata->td_taskgroup &&
        taskdata->td_taskgroup->cancel_request == cancel_taskgroup) {
      status = ompt_task_cancel;
    }

    /* let OMPT know that we're returning to the callee task */
    ompt_callbacks.ompt_callback(ompt_callback_task_schedule)(
        &(taskdata->ompt_task_info.task_data), status,
        (resumed_task ? &(resumed_task->ompt_task_info.task_data) : NULL));
  }
}
#endif

template <bool ompt>
static void __kmpc_omp_task_begin_if0_template(ident_t *loc_ref, kmp_int32 gtid,
                                               kmp_task_t *task,
                                               void *frame_address,
                                               void *return_address) {
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  kmp_taskdata_t *current_task = __kmp_threads[gtid]->th.th_current_task;

  KA_TRACE(10, ("__kmpc_omp_task_begin_if0(enter): T#%d loc=%p task=%p "
                "current_task=%p\n",
                gtid, loc_ref, taskdata, current_task));

  if (UNLIKELY(taskdata->td_flags.tiedness == TASK_UNTIED)) {
    // untied task needs to increment counter so that the task structure is not
    // freed prematurely
    kmp_int32 counter = 1 + KMP_ATOMIC_INC(&taskdata->td_untied_count);
    KMP_DEBUG_USE_VAR(counter);
    KA_TRACE(20, ("__kmpc_omp_task_begin_if0: T#%d untied_count (%d) "
                  "incremented for task %p\n",
                  gtid, counter, taskdata));
  }

  taskdata->td_flags.task_serial =
      1; // Execute this task immediately, not deferred.
  __kmp_task_start(gtid, task, current_task);

#if OMPT_SUPPORT
  if (ompt) {
    if (current_task->ompt_task_info.frame.enter_frame.ptr == NULL) {
      current_task->ompt_task_info.frame.enter_frame.ptr =
          taskdata->ompt_task_info.frame.exit_frame.ptr = frame_address;
      current_task->ompt_task_info.frame.enter_frame_flags =
          taskdata->ompt_task_info.frame.exit_frame_flags =
              ompt_frame_application | ompt_frame_framepointer;
    }
    if (ompt_enabled.ompt_callback_task_create) {
      ompt_task_info_t *parent_info = &(current_task->ompt_task_info);
      ompt_callbacks.ompt_callback(ompt_callback_task_create)(
          &(parent_info->task_data), &(parent_info->frame),
          &(taskdata->ompt_task_info.task_data),
          ompt_task_explicit | TASK_TYPE_DETAILS_FORMAT(taskdata), 0,
          return_address);
    }
    __ompt_task_start(task, current_task, gtid);
  }
#endif // OMPT_SUPPORT

  KA_TRACE(10, ("__kmpc_omp_task_begin_if0(exit): T#%d loc=%p task=%p,\n", gtid,
                loc_ref, taskdata));
}

#if OMPT_SUPPORT
OMPT_NOINLINE
static void __kmpc_omp_task_begin_if0_ompt(ident_t *loc_ref, kmp_int32 gtid,
                                           kmp_task_t *task,
                                           void *frame_address,
                                           void *return_address) {
  __kmpc_omp_task_begin_if0_template<true>(loc_ref, gtid, task, frame_address,
                                           return_address);
}
#endif // OMPT_SUPPORT

// __kmpc_omp_task_begin_if0: report that a given serialized task has started
// execution
//
// loc_ref: source location information; points to beginning of task block.
// gtid: global thread number.
// task: task thunk for the started task.
void __kmpc_omp_task_begin_if0(ident_t *loc_ref, kmp_int32 gtid,
                               kmp_task_t *task) {
#if OMPT_SUPPORT
  if (UNLIKELY(ompt_enabled.enabled)) {
    OMPT_STORE_RETURN_ADDRESS(gtid);
    __kmpc_omp_task_begin_if0_ompt(loc_ref, gtid, task,
                                   OMPT_GET_FRAME_ADDRESS(1),
                                   OMPT_LOAD_RETURN_ADDRESS(gtid));
    return;
  }
#endif
  __kmpc_omp_task_begin_if0_template<false>(loc_ref, gtid, task, NULL, NULL);
}

#ifdef TASK_UNUSED
// __kmpc_omp_task_begin: report that a given task has started execution
// NEVER GENERATED BY COMPILER, DEPRECATED!!!
void __kmpc_omp_task_begin(ident_t *loc_ref, kmp_int32 gtid, kmp_task_t *task) {
  kmp_taskdata_t *current_task = __kmp_threads[gtid]->th.th_current_task;

  KA_TRACE(
      10,
      ("__kmpc_omp_task_begin(enter): T#%d loc=%p task=%p current_task=%p\n",
       gtid, loc_ref, KMP_TASK_TO_TASKDATA(task), current_task));

  __kmp_task_start(gtid, task, current_task);

  KA_TRACE(10, ("__kmpc_omp_task_begin(exit): T#%d loc=%p task=%p,\n", gtid,
                loc_ref, KMP_TASK_TO_TASKDATA(task)));
  return;
}
#endif // TASK_UNUSED

// __kmp_free_task: free the current task space and the space for shareds
//
// gtid: Global thread ID of calling thread
// taskdata: task to free
// thread: thread data structure of caller
static void __kmp_free_task(kmp_int32 gtid, kmp_taskdata_t *taskdata,
                            kmp_info_t *thread) {
  KA_TRACE(30, ("__kmp_free_task: T#%d freeing data from task %p\n", gtid,
                taskdata));

  // Check to make sure all flags and counters have the correct values
  KMP_DEBUG_ASSERT(taskdata->td_flags.tasktype == TASK_EXPLICIT);
  KMP_DEBUG_ASSERT(taskdata->td_flags.executing == 0);
  KMP_DEBUG_ASSERT(taskdata->td_flags.complete == 1);
  KMP_DEBUG_ASSERT(taskdata->td_flags.freed == 0);
  KMP_DEBUG_ASSERT(taskdata->td_allocated_child_tasks == 0 ||
                   taskdata->td_flags.task_serial == 1);
  KMP_DEBUG_ASSERT(taskdata->td_incomplete_child_tasks == 0);

  taskdata->td_flags.freed = 1;
// deallocate the taskdata and shared variable blocks associated with this task
#if USE_FAST_MEMORY
  __kmp_fast_free(thread, taskdata);
#else /* ! USE_FAST_MEMORY */
  __kmp_thread_free(thread, taskdata);
#endif
  KA_TRACE(20, ("__kmp_free_task: T#%d freed task %p\n", gtid, taskdata));
}

// __kmp_free_task_and_ancestors: free the current task and ancestors without
// children
//
// gtid: Global thread ID of calling thread
// taskdata: task to free
// thread: thread data structure of caller
static void __kmp_free_task_and_ancestors(kmp_int32 gtid,
                                          kmp_taskdata_t *taskdata,
                                          kmp_info_t *thread) {
  // Proxy tasks must always be allowed to free their parents
  // because they can be run in background even in serial mode.
  kmp_int32 team_serial =
      (taskdata->td_flags.team_serial || taskdata->td_flags.tasking_ser) &&
      !taskdata->td_flags.proxy;
  KMP_DEBUG_ASSERT(taskdata->td_flags.tasktype == TASK_EXPLICIT);

  kmp_int32 children = KMP_ATOMIC_DEC(&taskdata->td_allocated_child_tasks) - 1;
  KMP_DEBUG_ASSERT(children >= 0);

  // Now, go up the ancestor tree to see if any ancestors can now be freed.
  while (children == 0) {
    kmp_taskdata_t *parent_taskdata = taskdata->td_parent;

    KA_TRACE(20, ("__kmp_free_task_and_ancestors(enter): T#%d task %p complete "
                  "and freeing itself\n",
                  gtid, taskdata));

    // --- Deallocate my ancestor task ---
    __kmp_free_task(gtid, taskdata, thread);

    taskdata = parent_taskdata;

    if (team_serial)
      return;
    // Stop checking ancestors at implicit task instead of walking up ancestor
    // tree to avoid premature deallocation of ancestors.
    if (taskdata->td_flags.tasktype == TASK_IMPLICIT) {
      if (taskdata->td_dephash) { // do we need to cleanup dephash?
        int children = KMP_ATOMIC_LD_ACQ(&taskdata->td_incomplete_child_tasks);
        kmp_tasking_flags_t flags_old = taskdata->td_flags;
        if (children == 0 && flags_old.complete == 1) {
          kmp_tasking_flags_t flags_new = flags_old;
          flags_new.complete = 0;
          if (KMP_COMPARE_AND_STORE_ACQ32(
                  RCAST(kmp_int32 *, &taskdata->td_flags),
                  *RCAST(kmp_int32 *, &flags_old),
                  *RCAST(kmp_int32 *, &flags_new))) {
            KA_TRACE(100, ("__kmp_free_task_and_ancestors: T#%d cleans "
                           "dephash of implicit task %p\n",
                           gtid, taskdata));
            // cleanup dephash of finished implicit task
            __kmp_dephash_free_entries(thread, taskdata->td_dephash);
          }
        }
      }
      return;
    }
    // Predecrement simulated by "- 1" calculation
    children = KMP_ATOMIC_DEC(&taskdata->td_allocated_child_tasks) - 1;
    KMP_DEBUG_ASSERT(children >= 0);
  }

  KA_TRACE(
      20, ("__kmp_free_task_and_ancestors(exit): T#%d task %p has %d children; "
           "not freeing it yet\n",
           gtid, taskdata, children));
}

// Only need to keep track of child task counts if any of the following:
// 1. team parallel and tasking not serialized;
// 2. it is a proxy or detachable or hidden helper task
// 3. the children counter of its parent task is greater than 0.
// The reason for the 3rd one is for serialized team that found detached task,
// hidden helper task, T. In this case, the execution of T is still deferred,
// and it is also possible that a regular task depends on T. In this case, if we
// don't track the children, task synchronization will be broken.
static bool __kmp_track_children_task(kmp_taskdata_t *taskdata) {
  kmp_tasking_flags_t flags = taskdata->td_flags;
  bool ret = !(flags.team_serial || flags.tasking_ser);
  ret = ret || flags.proxy == TASK_PROXY ||
        flags.detachable == TASK_DETACHABLE || flags.hidden_helper;
  ret = ret ||
        KMP_ATOMIC_LD_ACQ(&taskdata->td_parent->td_incomplete_child_tasks) > 0;
  return ret;
}

// __kmp_task_finish: bookkeeping to do when a task finishes execution
//
// gtid: global thread ID for calling thread
// task: task to be finished
// resumed_task: task to be resumed.  (may be NULL if task is serialized)
//
// template<ompt>: effectively ompt_enabled.enabled!=0
// the version with ompt=false is inlined, allowing to optimize away all ompt
// code in this case
template <bool ompt>
static void __kmp_task_finish(kmp_int32 gtid, kmp_task_t *task,
                              kmp_taskdata_t *resumed_task) {
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  kmp_info_t *thread = __kmp_threads[gtid];
  kmp_task_team_t *task_team =
      thread->th.th_task_team; // might be NULL for serial teams...

  //ME1
  // Calculates the task's execution time
  kmp_real64 current_time = 0;
  __kmp_read_system_time(&current_time);

  // Time in microseconds
  current_time = (current_time - taskdata->td_starttime + taskdata->td_previous_exectime) * 1000000;
  kmp_uint32 finish_time = static_cast<kmp_uint32>(current_time);

#if PERF_DYNAMIC_ON
  if (taskdata->td_task_type == TASK_UNDEFINED && thread->th.th_counters_active) {
#else
  if (taskdata->td_task_type == TASK_UNDEFINED) {
#endif
      #if !PERF_DISABLE
      //PERF
      kmp_uint64 current_cycles = 0;
      kmp_uint64 current_cachemiss = 0;
      // Read from counters
      read(thread->th.th_counter_cycles, &current_cycles, sizeof(current_cycles));
      read(thread->th.th_counter_cachemiss, &current_cachemiss, sizeof(current_cachemiss));

      #if DEBUG_PRINT_TASK_INFO
      kmp_uint64 current_instructions = 0;
      read(thread->th.th_counter_instructions, &current_instructions, sizeof(current_instructions));
      kmp_uint64 instructions_finish =
      current_instructions - taskdata->td_instructions_start + taskdata->td_instructions_prev;

      #endif
      // Get final counter values
      kmp_uint64 cycles_finish = current_cycles - taskdata->td_cycles_start + taskdata->td_cycles_prev;
      kmp_uint64 cachemiss_finish = current_cachemiss - taskdata->td_cachemiss_start + taskdata->td_cachemiss_prev;
      
      #if PERF_DYNAMIC_ON
      // Close the counters since were not using them anymore
      //TODO check if resumed task is undefined, then keep them open..
      //TODO may cause problems if there is no resumed task?
      __kmp_perf_disable(thread);
      #endif
      #endif
  

#if DEBUG_PRINT_TASK_INFO
      kmp_int32 tid = __kmp_tid_from_gtid(gtid);
      // Debug print
      // freq in MHZ
      if(finish_time) {
          kmp_uint32 frequency = cycles_finish / finish_time;
          printf("Finished task %d on thread %d in %f microseconds\n Used %llu CPU cycles, %llu instructions freq=%d MHz\n",
                 taskdata->td_task_id, tid, current_time, cycles_finish, instructions_finish, frequency);
      }
#endif
      // If the routine haven't been classified during this execution, classify the task
      if(!__kmp_contains_def(task->routine)) {
          task_definition_t task_type;

          #if !PERF_DISABLE
          if (cachemiss_finish == 0) cachemiss_finish += 1; // Avoid dividing by zero
          kmp_int32 arithmetic_intensity = (cycles_finish * FLOPS_PER_CYCLE) / (64 * cachemiss_finish);
          if (arithmetic_intensity > AI_CPU_LIMIT) {
              taskdata->td_task_type = TASK_CPU;
              task_type = compute_bound;
          } else if (arithmetic_intensity > AI_CACHE_LIMIT) {
              taskdata->td_task_type = TASK_CACHE;
              task_type = cache_intensive;
          } else {
              taskdata->td_task_type = TASK_MEMORY;
              task_type = memory_bound;
          }
          #else
         taskdata->td_task_type = TASK_MEMORY;
         task_type = memory_bound;

          #endif
#if DEBUG_PRINT_TASK_INFO
          std::string str;
                if (arithmetic_intensity < AI_CACHE_LIMIT) str = "TASK_MEMORY";
                else if (arithmetic_intensity < AI_CPU_LIMIT) str = "TASK_CACHE";
                else str = "TASK_CPU";
                printf("ARITHMETIC INTENSITY = %d, categorised as %s on thread %d with %llu cachemisses\n",
                       arithmetic_intensity, str.c_str(), tid, cachemiss_finish);
#endif
          // Add the task routine with a defined task type
          __kmp_add_def(task->routine, task_type);
      }
      // A task with the same routine already classified the task, get the classification
      else{
          taskdata->td_task_type = __kmp_get_def(task->routine);
      }

  }
  #if DEBUG_PRINT_TASK_INFO
  else{
    kmp_int32 tid = __kmp_tid_from_gtid(gtid);
    printf("Finished task %d on thread %d\n", taskdata->td_task_id, tid);
  }
  #endif
  // If we don't get a valid time, we ignore the history of this task
  // (Defined tasks are less than TASK_UNDEFINED, to keep Pattern tasks out)
  if (finish_time != 0 && taskdata->td_task_type < TASK_UNDEFINED){
#if MEASURE_ACCURACY
      {
          static std::mutex mutex;
          std::lock_guard<std::mutex> lock(mutex);
          struct passwd *pw = getpwuid(getuid());
          std::string homedir = pw->pw_dir;
          std::string filename = "accuracy_data.txt";
          std::string full = homedir + std::string("/") + filename;
          std::ofstream myfile (full, std::ios::out | std::ios::app );
          if (myfile.is_open()){
              myfile << finish_time << " " << taskdata->td_predicted_exectime <<
                    " " << int (taskdata->td_taskwidth) << std::endl;
              myfile.close();
          }
          else printf("Unable to open file");



      }
      printf("Measured exec time: %d, predicted exec time: %d\n", finish_time, taskdata->td_predicted_exectime);
#endif
      __kmp_performance_model_add(thread->th.th_cluster, taskdata->td_task_type,
                                  finish_time, taskdata->td_taskwidth);
  }

  //ME2

#if KMP_DEBUG
  kmp_int32 children = 0;
#endif
  KA_TRACE(10, ("__kmp_task_finish(enter): T#%d finishing task %p and resuming "
                "task %p\n",
                gtid, taskdata, resumed_task));

  KMP_DEBUG_ASSERT(taskdata->td_flags.tasktype == TASK_EXPLICIT);

// Pop task from stack if tied
#ifdef BUILD_TIED_TASK_STACK
  if (taskdata->td_flags.tiedness == TASK_TIED) {
    __kmp_pop_task_stack(gtid, thread, taskdata);
  }
#endif /* BUILD_TIED_TASK_STACK */

  if (UNLIKELY(taskdata->td_flags.tiedness == TASK_UNTIED)) {
    // untied task needs to check the counter so that the task structure is not
    // freed prematurely
    kmp_int32 counter = KMP_ATOMIC_DEC(&taskdata->td_untied_count) - 1;
    KA_TRACE(
        20,
        ("__kmp_task_finish: T#%d untied_count (%d) decremented for task %p\n",
         gtid, counter, taskdata));
    if (counter > 0) {
      // untied task is not done, to be continued possibly by other thread, do
      // not free it now
      if (resumed_task == NULL) {
        KMP_DEBUG_ASSERT(taskdata->td_flags.task_serial);
        resumed_task = taskdata->td_parent; // In a serialized task, the resumed
        // task is the parent
      }
      thread->th.th_current_task = resumed_task; // restore current_task
      resumed_task->td_flags.executing = 1; // resume previous task
      KA_TRACE(10, ("__kmp_task_finish(exit): T#%d partially done task %p, "
                    "resuming task %p\n",
                    gtid, taskdata, resumed_task));
      return;
    }
  }

  // bookkeeping for resuming task:
  // GEH - note tasking_ser => task_serial
  KMP_DEBUG_ASSERT(
      (taskdata->td_flags.tasking_ser || taskdata->td_flags.task_serial) ==
      taskdata->td_flags.task_serial);
  if (taskdata->td_flags.task_serial) {
    if (resumed_task == NULL) {
      resumed_task = taskdata->td_parent; // In a serialized task, the resumed
      // task is the parent
    }
  } else {
    KMP_DEBUG_ASSERT(resumed_task !=
                     NULL); // verify that resumed task is passed as argument
  }

  /* If the tasks' destructor thunk flag has been set, we need to invoke the
     destructor thunk that has been generated by the compiler. The code is
     placed here, since at this point other tasks might have been released
     hence overlapping the destructor invocations with some other work in the
     released tasks.  The OpenMP spec is not specific on when the destructors
     are invoked, so we should be free to choose. */
  if (UNLIKELY(taskdata->td_flags.destructors_thunk)) {
    kmp_routine_entry_t destr_thunk = task->data1.destructors;
    KMP_ASSERT(destr_thunk);
    destr_thunk(gtid, task);
  }

  KMP_DEBUG_ASSERT(taskdata->td_flags.complete == 0);
  KMP_DEBUG_ASSERT(taskdata->td_flags.started == 1);
  KMP_DEBUG_ASSERT(taskdata->td_flags.freed == 0);

  bool detach = false;
  if (UNLIKELY(taskdata->td_flags.detachable == TASK_DETACHABLE)) {
    if (taskdata->td_allow_completion_event.type ==
        KMP_EVENT_ALLOW_COMPLETION) {
      // event hasn't been fulfilled yet. Try to detach task.
      __kmp_acquire_tas_lock(&taskdata->td_allow_completion_event.lock, gtid);
      if (taskdata->td_allow_completion_event.type ==
          KMP_EVENT_ALLOW_COMPLETION) {
        // task finished execution
        KMP_DEBUG_ASSERT(taskdata->td_flags.executing == 1);
        taskdata->td_flags.executing = 0; // suspend the finishing task

#if OMPT_SUPPORT
        // For a detached task, which is not completed, we switch back
        // the omp_fulfill_event signals completion
        // locking is necessary to avoid a race with ompt_task_late_fulfill
        if (ompt)
          __ompt_task_finish(task, resumed_task, ompt_task_detach);
#endif

        // no access to taskdata after this point!
        // __kmp_fulfill_event might free taskdata at any time from now

        taskdata->td_flags.proxy = TASK_PROXY; // proxify!
        detach = true;
      }
      __kmp_release_tas_lock(&taskdata->td_allow_completion_event.lock, gtid);
    }
  }

  if (!detach) {
    taskdata->td_flags.complete = 1; // mark the task as completed

#if OMPT_SUPPORT
    // This is not a detached task, we are done here
    if (ompt)
      __ompt_task_finish(task, resumed_task, ompt_task_complete);
#endif
    // TODO: What would be the balance between the conditions in the function
    // and an atomic operation?
    if (__kmp_track_children_task(taskdata)) {
      __kmp_release_deps(gtid, taskdata);
      // Predecrement simulated by "- 1" calculation
#if KMP_DEBUG
      children = -1 +
#endif
          KMP_ATOMIC_DEC(&taskdata->td_parent->td_incomplete_child_tasks);
      KMP_DEBUG_ASSERT(children >= 0);
      if (taskdata->td_taskgroup)
        KMP_ATOMIC_DEC(&taskdata->td_taskgroup->count);
    } else if (task_team && (task_team->tt.tt_found_proxy_tasks ||
                             task_team->tt.tt_hidden_helper_task_encountered)) {
      // if we found proxy or hidden helper tasks there could exist a dependency
      // chain with the proxy task as origin
      __kmp_release_deps(gtid, taskdata);
    }
    // td_flags.executing must be marked as 0 after __kmp_release_deps has been
    // called. Othertwise, if a task is executed immediately from the
    // release_deps code, the flag will be reset to 1 again by this same
    // function
    KMP_DEBUG_ASSERT(taskdata->td_flags.executing == 1);
    taskdata->td_flags.executing = 0; // suspend the finishing task
  }

  KA_TRACE(
      20, ("__kmp_task_finish: T#%d finished task %p, %d incomplete children\n",
           gtid, taskdata, children));

  // Free this task and then ancestor tasks if they have no children.
  // Restore th_current_task first as suggested by John:
  // johnmc: if an asynchronous inquiry peers into the runtime system
  // it doesn't see the freed task as the current task.
  thread->th.th_current_task = resumed_task;
  if (!detach)
    __kmp_free_task_and_ancestors(gtid, taskdata, thread);

  // TODO: GEH - make sure root team implicit task is initialized properly.
  // KMP_DEBUG_ASSERT( resumed_task->td_flags.executing == 0 );

  //ME1
  __kmp_read_system_time(&resumed_task->td_starttime);

  #if !PERF_DISABLE
  //PERF
  if (resumed_task->td_task_type == TASK_UNDEFINED) {
      kmp_uint64 current_cycles = 0;
      kmp_uint64 current_cachemiss = 0;

      #if PERF_DYNAMIC_ON
      // Open the counters if not active
      if (!thread->th.th_counters_active) __kmp_perf_enable(thread);
      #endif

      // Read from counters
      read(thread->th.th_counter_cycles, &current_cycles, sizeof(current_cycles));
      read(thread->th.th_counter_cachemiss, &current_cachemiss, sizeof(current_cachemiss));

      #if DEBUG_PRINT_TASK_INFO
      kmp_uint64 current_instructions = 0;
      read(thread->th.th_counter_instructions, &current_instructions, sizeof(current_instructions));
      resumed_task->td_instructions_start = current_instructions;
      #endif

      resumed_task->td_cycles_start = current_cycles;
      resumed_task->td_cachemiss_start = current_cachemiss;
  }
  //ME2
  #endif
  resumed_task->td_flags.executing = 1; // resume previous task

  KA_TRACE(
      10, ("__kmp_task_finish(exit): T#%d finished task %p, resuming task %p\n",
           gtid, taskdata, resumed_task));

  return;
}

template <bool ompt>
static void __kmpc_omp_task_complete_if0_template(ident_t *loc_ref,
                                                  kmp_int32 gtid,
                                                  kmp_task_t *task) {
  KA_TRACE(10, ("__kmpc_omp_task_complete_if0(enter): T#%d loc=%p task=%p\n",
                gtid, loc_ref, KMP_TASK_TO_TASKDATA(task)));
  KMP_DEBUG_ASSERT(gtid >= 0);
  // this routine will provide task to resume
  __kmp_task_finish<ompt>(gtid, task, NULL);

  KA_TRACE(10, ("__kmpc_omp_task_complete_if0(exit): T#%d loc=%p task=%p\n",
                gtid, loc_ref, KMP_TASK_TO_TASKDATA(task)));

#if OMPT_SUPPORT
  if (ompt) {
    ompt_frame_t *ompt_frame;
    __ompt_get_task_info_internal(0, NULL, NULL, &ompt_frame, NULL, NULL);
    ompt_frame->enter_frame = ompt_data_none;
    ompt_frame->enter_frame_flags =
        ompt_frame_runtime | ompt_frame_framepointer;
  }
#endif

  return;
}

#if OMPT_SUPPORT
OMPT_NOINLINE
void __kmpc_omp_task_complete_if0_ompt(ident_t *loc_ref, kmp_int32 gtid,
                                       kmp_task_t *task) {
  __kmpc_omp_task_complete_if0_template<true>(loc_ref, gtid, task);
}
#endif // OMPT_SUPPORT

// __kmpc_omp_task_complete_if0: report that a task has completed execution
//
// loc_ref: source location information; points to end of task block.
// gtid: global thread number.
// task: task thunk for the completed task.
void __kmpc_omp_task_complete_if0(ident_t *loc_ref, kmp_int32 gtid,
                                  kmp_task_t *task) {
#if OMPT_SUPPORT
  if (UNLIKELY(ompt_enabled.enabled)) {
    __kmpc_omp_task_complete_if0_ompt(loc_ref, gtid, task);
    return;
  }
#endif
  __kmpc_omp_task_complete_if0_template<false>(loc_ref, gtid, task);
}

#ifdef TASK_UNUSED
// __kmpc_omp_task_complete: report that a task has completed execution
// NEVER GENERATED BY COMPILER, DEPRECATED!!!
void __kmpc_omp_task_complete(ident_t *loc_ref, kmp_int32 gtid,
                              kmp_task_t *task) {
  KA_TRACE(10, ("__kmpc_omp_task_complete(enter): T#%d loc=%p task=%p\n", gtid,
                loc_ref, KMP_TASK_TO_TASKDATA(task)));

  __kmp_task_finish<false>(gtid, task,
                           NULL); // Not sure how to find task to resume

  KA_TRACE(10, ("__kmpc_omp_task_complete(exit): T#%d loc=%p task=%p\n", gtid,
                loc_ref, KMP_TASK_TO_TASKDATA(task)));
  return;
}
#endif // TASK_UNUSED

// __kmp_init_implicit_task: Initialize the appropriate fields in the implicit
// task for a given thread
//
// loc_ref:  reference to source location of parallel region
// this_thr:  thread data structure corresponding to implicit task
// team: team for this_thr
// tid: thread id of given thread within team
// set_curr_task: TRUE if need to push current task to thread
// NOTE: Routine does not set up the implicit task ICVS.  This is assumed to
// have already been done elsewhere.
// TODO: Get better loc_ref.  Value passed in may be NULL
void __kmp_init_implicit_task(ident_t *loc_ref, kmp_info_t *this_thr,
                              kmp_team_t *team, int tid, int set_curr_task) {
  kmp_taskdata_t *task = &team->t.t_implicit_task_taskdata[tid];

  KF_TRACE(
      10,
      ("__kmp_init_implicit_task(enter): T#:%d team=%p task=%p, reinit=%s\n",
       tid, team, task, set_curr_task ? "TRUE" : "FALSE"));

  task->td_task_id = KMP_GEN_TASK_ID();
  task->td_team = team;
  //    task->td_parent   = NULL;  // fix for CQ230101 (broken parent task info
  //    in debugger)
  task->td_ident = loc_ref;
  task->td_taskwait_ident = NULL;
  task->td_taskwait_counter = 0;
  task->td_taskwait_thread = 0;

  task->td_flags.tiedness = TASK_TIED;
  task->td_flags.tasktype = TASK_IMPLICIT;
  task->td_flags.proxy = TASK_FULL;

  // All implicit tasks are executed immediately, not deferred
  task->td_flags.task_serial = 1;
  task->td_flags.tasking_ser = (__kmp_tasking_mode == tskm_immediate_exec);
  task->td_flags.team_serial = (team->t.t_serialized) ? 1 : 0;

  task->td_flags.started = 1;
  task->td_flags.executing = 1;
  task->td_flags.complete = 0;
  task->td_flags.freed = 0;
  task->td_depnode = NULL;
  task->td_last_tied = task;
  task->td_allow_completion_event.type = KMP_EVENT_UNINITIALIZED;

  if (set_curr_task) { // only do this init first time thread is created
    KMP_ATOMIC_ST_REL(&task->td_incomplete_child_tasks, 0);
    // Not used: don't need to deallocate implicit task
    KMP_ATOMIC_ST_REL(&task->td_allocated_child_tasks, 0);
    task->td_taskgroup = NULL; // An implicit task does not have taskgroup
    task->td_dephash = NULL;
    __kmp_push_current_task_to_thread(this_thr, team, tid);
  } else {
    KMP_DEBUG_ASSERT(task->td_incomplete_child_tasks == 0);
    KMP_DEBUG_ASSERT(task->td_allocated_child_tasks == 0);
  }

#if OMPT_SUPPORT
  if (UNLIKELY(ompt_enabled.enabled))
    __ompt_task_init(task, tid);
#endif

  KF_TRACE(10, ("__kmp_init_implicit_task(exit): T#:%d team=%p task=%p\n", tid,
                team, task));
}

// __kmp_finish_implicit_task: Release resources associated to implicit tasks
// at the end of parallel regions. Some resources are kept for reuse in the next
// parallel region.
//
// thread:  thread data structure corresponding to implicit task
void __kmp_finish_implicit_task(kmp_info_t *thread) {
  kmp_taskdata_t *task = thread->th.th_current_task;
  if (task->td_dephash) {
    int children;
    task->td_flags.complete = 1;
    children = KMP_ATOMIC_LD_ACQ(&task->td_incomplete_child_tasks);
    kmp_tasking_flags_t flags_old = task->td_flags;
    if (children == 0 && flags_old.complete == 1) {
      kmp_tasking_flags_t flags_new = flags_old;
      flags_new.complete = 0;
      if (KMP_COMPARE_AND_STORE_ACQ32(RCAST(kmp_int32 *, &task->td_flags),
                                      *RCAST(kmp_int32 *, &flags_old),
                                      *RCAST(kmp_int32 *, &flags_new))) {
        KA_TRACE(100, ("__kmp_finish_implicit_task: T#%d cleans "
                       "dephash of implicit task %p\n",
                       thread->th.th_info.ds.ds_gtid, task));
        __kmp_dephash_free_entries(thread, task->td_dephash);
      }
    }
  }
}

// __kmp_free_implicit_task: Release resources associated to implicit tasks
// when these are destroyed regions
//
// thread:  thread data structure corresponding to implicit task
void __kmp_free_implicit_task(kmp_info_t *thread) {
  kmp_taskdata_t *task = thread->th.th_current_task;
  if (task && task->td_dephash) {
    __kmp_dephash_free(thread, task->td_dephash);
    task->td_dephash = NULL;
  }
}

// Round up a size to a power of two specified by val: Used to insert padding
// between structures co-allocated using a single malloc() call
static size_t __kmp_round_up_to_val(size_t size, size_t val) {
  if (size & (val - 1)) {
    size &= ~(val - 1);
    if (size <= KMP_SIZE_T_MAX - val) {
      size += val; // Round up if there is no overflow.
    }
  }
  return size;
} // __kmp_round_up_to_va


//ME1
// Debug global task counter
#if DEBUG_PRINT_THREAD_INFO | DEBUG_PRINT_TASK_INFO | DEBUG_PRINT_TASK_PERFORMANCE_MODEL_INFO
int global_taskcounter = 0;
#endif
//ME2


// __kmp_task_alloc: Allocate the taskdata and task data structures for a task
//
// loc_ref: source location information
// gtid: global thread number.
// flags: include tiedness & task type (explicit vs. implicit) of the ''new''
// task encountered. Converted from kmp_int32 to kmp_tasking_flags_t in routine.
// sizeof_kmp_task_t:  Size in bytes of kmp_task_t data structure including
// private vars accessed in task.
// sizeof_shareds:  Size in bytes of array of pointers to shared vars accessed
// in task.
// task_entry: Pointer to task code entry point generated by compiler.
// returns: a pointer to the allocated kmp_task_t structure (task).
kmp_task_t *__kmp_task_alloc(ident_t *loc_ref, kmp_int32 gtid,
                             kmp_tasking_flags_t *flags,
                             size_t sizeof_kmp_task_t, size_t sizeof_shareds,
                             kmp_routine_entry_t task_entry) {
  kmp_task_t *task;
  kmp_taskdata_t *taskdata;
  kmp_info_t *thread = __kmp_threads[gtid];
  kmp_team_t *team = thread->th.th_team;
  kmp_taskdata_t *parent_task = thread->th.th_current_task;
  size_t shareds_offset;

  if (UNLIKELY(!TCR_4(__kmp_init_middle)))
    __kmp_middle_initialize();

  if (flags->hidden_helper) {
    if (__kmp_enable_hidden_helper) {
      if (!TCR_4(__kmp_init_hidden_helper))
        __kmp_hidden_helper_initialize();
    } else {
      // If the hidden helper task is not enabled, reset the flag to FALSE.
      flags->hidden_helper = FALSE;
    }
  }

  KA_TRACE(10, ("__kmp_task_alloc(enter): T#%d loc=%p, flags=(0x%x) "
                "sizeof_task=%ld sizeof_shared=%ld entry=%p\n",
                gtid, loc_ref, *((kmp_int32 *)flags), sizeof_kmp_task_t,
                sizeof_shareds, task_entry));

  KMP_DEBUG_ASSERT(parent_task);
  if (parent_task->td_flags.final) {
    if (flags->merged_if0) {
    }
    flags->final = 1;
  }

  if (flags->tiedness == TASK_UNTIED && !team->t.t_serialized) {
    // Untied task encountered causes the TSC algorithm to check entire deque of
    // the victim thread. If no untied task encountered, then checking the head
    // of the deque should be enough.
    KMP_CHECK_UPDATE(thread->th.th_task_team->tt.tt_untied_task_encountered, 1);
  }

  // Detachable tasks are not proxy tasks yet but could be in the future. Doing
  // the tasking setup
  // when that happens is too late.
  if (UNLIKELY(flags->proxy == TASK_PROXY ||
               flags->detachable == TASK_DETACHABLE || flags->hidden_helper)) {
    if (flags->proxy == TASK_PROXY) {
      flags->tiedness = TASK_UNTIED;
      flags->merged_if0 = 1;
    }
    /* are we running in a sequential parallel or tskm_immediate_exec... we need
       tasking support enabled */
    if ((thread->th.th_task_team) == NULL) {
      /* This should only happen if the team is serialized
          setup a task team and propagate it to the thread */
      KMP_DEBUG_ASSERT(team->t.t_serialized);
      KA_TRACE(30,
               ("T#%d creating task team in __kmp_task_alloc for proxy task\n",
                gtid));
      // 1 indicates setup the current team regardless of nthreads
      __kmp_task_team_setup(thread, team, 1);
      thread->th.th_task_team = team->t.t_task_team[thread->th.th_task_state];
    }
    kmp_task_team_t *task_team = thread->th.th_task_team;

    /* tasking must be enabled now as the task might not be pushed */
    if (!KMP_TASKING_ENABLED(task_team)) {
      KA_TRACE(
          30,
          ("T#%d enabling tasking in __kmp_task_alloc for proxy task\n", gtid));
      __kmp_enable_tasking(task_team, thread);
      kmp_int32 tid = thread->th.th_info.ds.ds_tid;
      kmp_thread_data_t *thread_data = &task_team->tt.tt_threads_data[tid];
      // No lock needed since only owner can allocate
      if (thread_data->td.td_deque == NULL) {
        __kmp_alloc_task_deque(thread, thread_data);
      }
    }

    if ((flags->proxy == TASK_PROXY || flags->detachable == TASK_DETACHABLE) &&
        task_team->tt.tt_found_proxy_tasks == FALSE)
      TCW_4(task_team->tt.tt_found_proxy_tasks, TRUE);
    if (flags->hidden_helper &&
        task_team->tt.tt_hidden_helper_task_encountered == FALSE)
      TCW_4(task_team->tt.tt_hidden_helper_task_encountered, TRUE);
  }

  // Calculate shared structure offset including padding after kmp_task_t struct
  // to align pointers in shared struct
  shareds_offset = sizeof(kmp_taskdata_t) + sizeof_kmp_task_t;
  shareds_offset = __kmp_round_up_to_val(shareds_offset, sizeof(void *));

  // Allocate a kmp_taskdata_t block and a kmp_task_t block.
  KA_TRACE(30, ("__kmp_task_alloc: T#%d First malloc size: %ld\n", gtid,
                shareds_offset));
  KA_TRACE(30, ("__kmp_task_alloc: T#%d Second malloc size: %ld\n", gtid,
                sizeof_shareds));

  // Avoid double allocation here by combining shareds with taskdata
#if USE_FAST_MEMORY
  taskdata = (kmp_taskdata_t *)__kmp_fast_allocate(thread, shareds_offset +
                                                               sizeof_shareds);
#else /* ! USE_FAST_MEMORY */
  taskdata = (kmp_taskdata_t *)__kmp_thread_malloc(thread, shareds_offset +
                                                               sizeof_shareds);
#endif /* USE_FAST_MEMORY */

  task = KMP_TASKDATA_TO_TASK(taskdata);

// Make sure task & taskdata are aligned appropriately
#if KMP_ARCH_X86 || KMP_ARCH_PPC64 || !KMP_HAVE_QUAD
  KMP_DEBUG_ASSERT((((kmp_uintptr_t)taskdata) & (sizeof(double) - 1)) == 0);
  KMP_DEBUG_ASSERT((((kmp_uintptr_t)task) & (sizeof(double) - 1)) == 0);
#else
  KMP_DEBUG_ASSERT((((kmp_uintptr_t)taskdata) & (sizeof(_Quad) - 1)) == 0);
  KMP_DEBUG_ASSERT((((kmp_uintptr_t)task) & (sizeof(_Quad) - 1)) == 0);
#endif
  if (sizeof_shareds > 0) {
    // Avoid double allocation here by combining shareds with taskdata
    task->shareds = &((char *)taskdata)[shareds_offset];
    // Make sure shareds struct is aligned to pointer size
    KMP_DEBUG_ASSERT((((kmp_uintptr_t)task->shareds) & (sizeof(void *) - 1)) ==
                     0);
  } else {
    task->shareds = NULL;
  }
  task->routine = task_entry;
  task->part_id = 0; // AC: Always start with 0 part id

  taskdata->td_task_id = KMP_GEN_TASK_ID();
  taskdata->td_team = thread->th.th_team;
  taskdata->td_alloc_thread = thread;
  taskdata->td_parent = parent_task;
  taskdata->td_level = parent_task->td_level + 1; // increment nesting level
  KMP_ATOMIC_ST_RLX(&taskdata->td_untied_count, 0);
  taskdata->td_ident = loc_ref;
  taskdata->td_taskwait_ident = NULL;
  taskdata->td_taskwait_counter = 0;
  taskdata->td_taskwait_thread = 0;
  KMP_DEBUG_ASSERT(taskdata->td_parent != NULL);
  // avoid copying icvs for proxy tasks
  if (flags->proxy == TASK_FULL)
    copy_icvs(&taskdata->td_icvs, &taskdata->td_parent->td_icvs);

  //ME1
  taskdata->td_cluster = CLUSTER_UNASSIGNED;
  taskdata->td_taskwidth = 1;
  taskdata->td_no_steal = 0;
  // debug variable for task ID
  //global_taskcounter += 1;
  //taskdata->td_task_id = global_taskcounter;
  // Initialisations
  //taskdata->td_starttime = 0;
  //taskdata->td_previous_exectime = 0;
  //ME2

  taskdata->td_flags = *flags;
  taskdata->td_task_team = thread->th.th_task_team;
  taskdata->td_size_alloc = shareds_offset + sizeof_shareds;
  taskdata->td_flags.tasktype = TASK_EXPLICIT;
  // If it is hidden helper task, we need to set the team and task team
  // correspondingly.
  if (flags->hidden_helper) {
    kmp_info_t *shadow_thread = __kmp_threads[KMP_GTID_TO_SHADOW_GTID(gtid)];
    taskdata->td_team = shadow_thread->th.th_team;
    taskdata->td_task_team = shadow_thread->th.th_task_team;
  }

  // GEH - TODO: fix this to copy parent task's value of tasking_ser flag
  taskdata->td_flags.tasking_ser = (__kmp_tasking_mode == tskm_immediate_exec);

  // GEH - TODO: fix this to copy parent task's value of team_serial flag
  taskdata->td_flags.team_serial = (team->t.t_serialized) ? 1 : 0;

  // GEH - Note we serialize the task if the team is serialized to make sure
  // implicit parallel region tasks are not left until program termination to
  // execute. Also, it helps locality to execute immediately.

  taskdata->td_flags.task_serial =
      (parent_task->td_flags.final || taskdata->td_flags.team_serial ||
       taskdata->td_flags.tasking_ser || flags->merged_if0);

  taskdata->td_flags.started = 0;
  taskdata->td_flags.executing = 0;
  taskdata->td_flags.complete = 0;
  taskdata->td_flags.freed = 0;

  KMP_ATOMIC_ST_RLX(&taskdata->td_incomplete_child_tasks, 0);
  // start at one because counts current task and children
  KMP_ATOMIC_ST_RLX(&taskdata->td_allocated_child_tasks, 1);
  taskdata->td_taskgroup =
      parent_task->td_taskgroup; // task inherits taskgroup from the parent task
  taskdata->td_dephash = NULL;
  taskdata->td_depnode = NULL;
  if (flags->tiedness == TASK_UNTIED)
    taskdata->td_last_tied = NULL; // will be set when the task is scheduled
  else
    taskdata->td_last_tied = taskdata;
  taskdata->td_allow_completion_event.type = KMP_EVENT_UNINITIALIZED;
#if OMPT_SUPPORT
  if (UNLIKELY(ompt_enabled.enabled))
    __ompt_task_init(taskdata, gtid);
#endif
  // TODO: What would be the balance between the conditions in the function and
  // an atomic operation?
  if (__kmp_track_children_task(taskdata)) {
    KMP_ATOMIC_INC(&parent_task->td_incomplete_child_tasks);
    if (parent_task->td_taskgroup)
      KMP_ATOMIC_INC(&parent_task->td_taskgroup->count);
    // Only need to keep track of allocated child tasks for explicit tasks since
    // implicit not deallocated
    if (taskdata->td_parent->td_flags.tasktype == TASK_EXPLICIT) {
      KMP_ATOMIC_INC(&taskdata->td_parent->td_allocated_child_tasks);
    }
    if (flags->hidden_helper) {
      taskdata->td_flags.task_serial = FALSE;
      // Increment the number of hidden helper tasks to be executed
      KMP_ATOMIC_INC(&__kmp_unexecuted_hidden_helper_tasks);
    }
  }

  KA_TRACE(20, ("__kmp_task_alloc(exit): T#%d created task %p parent=%p\n",
                gtid, taskdata, taskdata->td_parent));

  return task;
}

kmp_task_t *__kmpc_omp_task_alloc(ident_t *loc_ref, kmp_int32 gtid,
                                  kmp_int32 flags, size_t sizeof_kmp_task_t,
                                  size_t sizeof_shareds,
                                  kmp_routine_entry_t task_entry) {
  kmp_task_t *retval;
  kmp_tasking_flags_t *input_flags = (kmp_tasking_flags_t *)&flags;
  __kmp_assert_valid_gtid(gtid);
  input_flags->native = FALSE;
  // __kmp_task_alloc() sets up all other runtime flags
  KA_TRACE(10, ("__kmpc_omp_task_alloc(enter): T#%d loc=%p, flags=(%s %s %s) "
                "sizeof_task=%ld sizeof_shared=%ld entry=%p\n",
                gtid, loc_ref, input_flags->tiedness ? "tied  " : "untied",
                input_flags->proxy ? "proxy" : "",
                input_flags->detachable ? "detachable" : "", sizeof_kmp_task_t,
                sizeof_shareds, task_entry));

  retval = __kmp_task_alloc(loc_ref, gtid, input_flags, sizeof_kmp_task_t,
                            sizeof_shareds, task_entry);

  KA_TRACE(20, ("__kmpc_omp_task_alloc(exit): T#%d retval %p\n", gtid, retval));

  return retval;
}

kmp_task_t *__kmpc_omp_target_task_alloc(ident_t *loc_ref, kmp_int32 gtid,
                                         kmp_int32 flags,
                                         size_t sizeof_kmp_task_t,
                                         size_t sizeof_shareds,
                                         kmp_routine_entry_t task_entry,
                                         kmp_int64 device_id) {
  auto &input_flags = reinterpret_cast<kmp_tasking_flags_t &>(flags);
  // target task is untied defined in the specification
  input_flags.tiedness = TASK_UNTIED;

  if (__kmp_enable_hidden_helper)
    input_flags.hidden_helper = TRUE;

  return __kmpc_omp_task_alloc(loc_ref, gtid, flags, sizeof_kmp_task_t,
                               sizeof_shareds, task_entry);
}

/*!
@ingroup TASKING
@param loc_ref location of the original task directive
@param gtid Global Thread ID of encountering thread
@param new_task task thunk allocated by __kmpc_omp_task_alloc() for the ''new
task''
@param naffins Number of affinity items
@param affin_list List of affinity items
@return Returns non-zero if registering affinity information was not successful.
 Returns 0 if registration was successful
This entry registers the affinity information attached to a task with the task
thunk structure kmp_taskdata_t.
*/
kmp_int32
__kmpc_omp_reg_task_with_affinity(ident_t *loc_ref, kmp_int32 gtid,
                                  kmp_task_t *new_task, kmp_int32 naffins,
                                  kmp_task_affinity_info_t *affin_list) {
  return 0;
}

//  __kmp_invoke_task: invoke the specified task
//
// gtid: global thread ID of caller
// task: the task to invoke
// current_task: the task to resume after task invocation
static void __kmp_invoke_task(kmp_int32 gtid, kmp_task_t *task,
                              kmp_taskdata_t *current_task) {
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  kmp_info_t *thread;
  int discard = 0 /* false */;
  KA_TRACE(
      30, ("__kmp_invoke_task(enter): T#%d invoking task %p, current_task=%p\n",
           gtid, taskdata, current_task));
  KMP_DEBUG_ASSERT(task);
  if (UNLIKELY(taskdata->td_flags.proxy == TASK_PROXY &&
               taskdata->td_flags.complete == 1)) {
    // This is a proxy task that was already completed but it needs to run
    // its bottom-half finish
    KA_TRACE(
        30,
        ("__kmp_invoke_task: T#%d running bottom finish for proxy task %p\n",
         gtid, taskdata));

    __kmp_bottom_half_finish_proxy(gtid, task);

    KA_TRACE(30, ("__kmp_invoke_task(exit): T#%d completed bottom finish for "
                  "proxy task %p, resuming task %p\n",
                  gtid, taskdata, current_task));

    return;
  }

#if OMPT_SUPPORT
  // For untied tasks, the first task executed only calls __kmpc_omp_task and
  // does not execute code.
  ompt_thread_info_t oldInfo;
  if (UNLIKELY(ompt_enabled.enabled)) {
    // Store the threads states and restore them after the task
    thread = __kmp_threads[gtid];
    oldInfo = thread->th.ompt_thread_info;
    thread->th.ompt_thread_info.wait_id = 0;
    thread->th.ompt_thread_info.state = (thread->th.th_team_serialized)
                                            ? ompt_state_work_serial
                                            : ompt_state_work_parallel;
    taskdata->ompt_task_info.frame.exit_frame.ptr = OMPT_GET_FRAME_ADDRESS(0);
  }
#endif

  // Decreament the counter of hidden helper tasks to be executed
  if (taskdata->td_flags.hidden_helper) {
    // Hidden helper tasks can only be executed by hidden helper threads
    KMP_ASSERT(KMP_HIDDEN_HELPER_THREAD(gtid));
    KMP_ATOMIC_DEC(&__kmp_unexecuted_hidden_helper_tasks);
  }

  // Proxy tasks are not handled by the runtime
  if (taskdata->td_flags.proxy != TASK_PROXY) {
    __kmp_task_start(gtid, task, current_task); // OMPT only if not discarded
  }

  // TODO: cancel tasks if the parallel region has also been cancelled
  // TODO: check if this sequence can be hoisted above __kmp_task_start
  // if cancellation has been enabled for this run ...
  if (UNLIKELY(__kmp_omp_cancellation)) {
    thread = __kmp_threads[gtid];
    kmp_team_t *this_team = thread->th.th_team;
    kmp_taskgroup_t *taskgroup = taskdata->td_taskgroup;
    if ((taskgroup && taskgroup->cancel_request) ||
        (this_team->t.t_cancel_request == cancel_parallel)) {
#if OMPT_SUPPORT && OMPT_OPTIONAL
      ompt_data_t *task_data;
      if (UNLIKELY(ompt_enabled.ompt_callback_cancel)) {
        __ompt_get_task_info_internal(0, NULL, &task_data, NULL, NULL, NULL);
        ompt_callbacks.ompt_callback(ompt_callback_cancel)(
            task_data,
            ((taskgroup && taskgroup->cancel_request) ? ompt_cancel_taskgroup
                                                      : ompt_cancel_parallel) |
                ompt_cancel_discarded_task,
            NULL);
      }
#endif
      KMP_COUNT_BLOCK(TASK_cancelled);
      // this task belongs to a task group and we need to cancel it
      discard = 1 /* true */;
    }
  }

  // Invoke the task routine and pass in relevant data.
  // Thunks generated by gcc take a different argument list.
  if (!discard) {
    if (taskdata->td_flags.tiedness == TASK_UNTIED) {
      taskdata->td_last_tied = current_task->td_last_tied;
      KMP_DEBUG_ASSERT(taskdata->td_last_tied);
    }
#if KMP_STATS_ENABLED
    KMP_COUNT_BLOCK(TASK_executed);
    switch (KMP_GET_THREAD_STATE()) {
    case FORK_JOIN_BARRIER:
      KMP_PUSH_PARTITIONED_TIMER(OMP_task_join_bar);
      break;
    case PLAIN_BARRIER:
      KMP_PUSH_PARTITIONED_TIMER(OMP_task_plain_bar);
      break;
    case TASKYIELD:
      KMP_PUSH_PARTITIONED_TIMER(OMP_task_taskyield);
      break;
    case TASKWAIT:
      KMP_PUSH_PARTITIONED_TIMER(OMP_task_taskwait);
      break;
    case TASKGROUP:
      KMP_PUSH_PARTITIONED_TIMER(OMP_task_taskgroup);
      break;
    default:
      KMP_PUSH_PARTITIONED_TIMER(OMP_task_immediate);
      break;
    }
#endif // KMP_STATS_ENABLED

// OMPT task begin
#if OMPT_SUPPORT
    if (UNLIKELY(ompt_enabled.enabled))
      __ompt_task_start(task, current_task, gtid);
#endif

#if OMPD_SUPPORT
    if (ompd_state & OMPD_ENABLE_BP)
      ompd_bp_task_begin();
#endif

#if USE_ITT_BUILD && USE_ITT_NOTIFY
    kmp_uint64 cur_time;
    kmp_int32 kmp_itt_count_task =
        __kmp_forkjoin_frames_mode == 3 && !taskdata->td_flags.task_serial &&
        current_task->td_flags.tasktype == TASK_IMPLICIT;
    if (kmp_itt_count_task) {
      thread = __kmp_threads[gtid];
      // Time outer level explicit task on barrier for adjusting imbalance time
      if (thread->th.th_bar_arrive_time)
        cur_time = __itt_get_timestamp();
      else
        kmp_itt_count_task = 0; // thread is not on a barrier - skip timing
    }
    KMP_FSYNC_ACQUIRED(taskdata); // acquired self (new task)
#endif

    if (task->routine != NULL) {
#ifdef KMP_GOMP_COMPAT
      if (taskdata->td_flags.native) {
        ((void (*)(void *))(*(task->routine)))(task->shareds);
      } else
#endif /* KMP_GOMP_COMPAT */
      {
        (*(task->routine))(gtid, task);
      }
    }
    KMP_POP_PARTITIONED_TIMER();

#if USE_ITT_BUILD && USE_ITT_NOTIFY
    if (kmp_itt_count_task) {
      // Barrier imbalance - adjust arrive time with the task duration
      thread->th.th_bar_arrive_time += (__itt_get_timestamp() - cur_time);
    }
    KMP_FSYNC_CANCEL(taskdata); // destroy self (just executed)
    KMP_FSYNC_RELEASING(taskdata->td_parent); // releasing parent
#endif
  }

#if OMPD_SUPPORT
  if (ompd_state & OMPD_ENABLE_BP)
    ompd_bp_task_end();
#endif

  // Proxy tasks are not handled by the runtime
  if (taskdata->td_flags.proxy != TASK_PROXY) {
#if OMPT_SUPPORT
    if (UNLIKELY(ompt_enabled.enabled)) {
      thread->th.ompt_thread_info = oldInfo;
      if (taskdata->td_flags.tiedness == TASK_TIED) {
        taskdata->ompt_task_info.frame.exit_frame = ompt_data_none;
      }
      __kmp_task_finish<true>(gtid, task, current_task);
    } else
#endif
      __kmp_task_finish<false>(gtid, task, current_task);
  }

  KA_TRACE(
      30,
      ("__kmp_invoke_task(exit): T#%d completed task %p, resuming task %p\n",
       gtid, taskdata, current_task));
  return;
}

// __kmpc_omp_task_parts: Schedule a thread-switchable task for execution
//
// loc_ref: location of original task pragma (ignored)
// gtid: Global Thread ID of encountering thread
// new_task: task thunk allocated by __kmp_omp_task_alloc() for the ''new task''
// Returns:
//    TASK_CURRENT_NOT_QUEUED (0) if did not suspend and queue current task to
//    be resumed later.
//    TASK_CURRENT_QUEUED (1) if suspended and queued the current task to be
//    resumed later.
kmp_int32 __kmpc_omp_task_parts(ident_t *loc_ref, kmp_int32 gtid,
                                kmp_task_t *new_task) {
  kmp_taskdata_t *new_taskdata = KMP_TASK_TO_TASKDATA(new_task);

  KA_TRACE(10, ("__kmpc_omp_task_parts(enter): T#%d loc=%p task=%p\n", gtid,
                loc_ref, new_taskdata));

#if OMPT_SUPPORT
  kmp_taskdata_t *parent;
  if (UNLIKELY(ompt_enabled.enabled)) {
    parent = new_taskdata->td_parent;
    if (ompt_enabled.ompt_callback_task_create) {
      ompt_callbacks.ompt_callback(ompt_callback_task_create)(
          &(parent->ompt_task_info.task_data), &(parent->ompt_task_info.frame),
          &(new_taskdata->ompt_task_info.task_data), ompt_task_explicit, 0,
          OMPT_GET_RETURN_ADDRESS(0));
    }
  }
#endif

  /* Should we execute the new task or queue it? For now, let's just always try
     to queue it.  If the queue fills up, then we'll execute it.  */

  if (__kmp_push_task(gtid, new_task) == TASK_NOT_PUSHED) // if cannot defer
  { // Execute this task immediately
    kmp_taskdata_t *current_task = __kmp_threads[gtid]->th.th_current_task;
    new_taskdata->td_flags.task_serial = 1;
    __kmp_invoke_task(gtid, new_task, current_task);
  }

  KA_TRACE(
      10,
      ("__kmpc_omp_task_parts(exit): T#%d returning TASK_CURRENT_NOT_QUEUED: "
       "loc=%p task=%p, return: TASK_CURRENT_NOT_QUEUED\n",
       gtid, loc_ref, new_taskdata));

#if OMPT_SUPPORT
  if (UNLIKELY(ompt_enabled.enabled)) {
    parent->ompt_task_info.frame.enter_frame = ompt_data_none;
  }
#endif
  return TASK_CURRENT_NOT_QUEUED;
}

// __kmp_omp_task: Schedule a non-thread-switchable task for execution
//
// gtid: Global Thread ID of encountering thread
// new_task:non-thread-switchable task thunk allocated by __kmp_omp_task_alloc()
// serialize_immediate: if TRUE then if the task is executed immediately its
// execution will be serialized
// Returns:
//    TASK_CURRENT_NOT_QUEUED (0) if did not suspend and queue current task to
//    be resumed later.
//    TASK_CURRENT_QUEUED (1) if suspended and queued the current task to be
//    resumed later.
kmp_int32 __kmp_omp_task(kmp_int32 gtid, kmp_task_t *new_task,
                         bool serialize_immediate) {
  kmp_taskdata_t *new_taskdata = KMP_TASK_TO_TASKDATA(new_task);

    //ME1
    // debug variable for task ID
    #if DEBUG_PRINT_THREAD_INFO | DEBUG_PRINT_TASK_INFO | DEBUG_PRINT_TASK_PERFORMANCE_MODEL_INFO
    global_taskcounter += 1;
    new_taskdata->td_task_id = global_taskcounter;
    #endif
    // Initialisations
    new_taskdata->td_starttime = 0;
    new_taskdata->td_previous_exectime = 0;
    //ME2

  /* Should we execute the new task or queue it? For now, let's just always try
     to queue it.  If the queue fills up, then we'll execute it.  */
  if (new_taskdata->td_flags.proxy == TASK_PROXY ||
      __kmp_push_task(gtid, new_task) == TASK_NOT_PUSHED) // if cannot defer
  { // Execute this task immediately
    kmp_taskdata_t *current_task = __kmp_threads[gtid]->th.th_current_task;
    if (serialize_immediate)
      new_taskdata->td_flags.task_serial = 1;

#if DEBUG_PRINT_THREAD_INFO
    printf("Start invoking task %d now on thread %d\n", new_taskdata->td_task_id, __kmp_get_tid());
#endif

    __kmp_invoke_task(gtid, new_task, current_task);

#if DEBUG_PRINT_THREAD_INFO
    printf("Done invoking now on thread %d\n", __kmp_get_tid());
#endif

  }

  return TASK_CURRENT_NOT_QUEUED;
}

// __kmpc_omp_task: Wrapper around __kmp_omp_task to schedule a
// non-thread-switchable task from the parent thread only!
//
// loc_ref: location of original task pragma (ignored)
// gtid: Global Thread ID of encountering thread
// new_task: non-thread-switchable task thunk allocated by
// __kmp_omp_task_alloc()
// Returns:
//    TASK_CURRENT_NOT_QUEUED (0) if did not suspend and queue current task to
//    be resumed later.
//    TASK_CURRENT_QUEUED (1) if suspended and queued the current task to be
//    resumed later.
kmp_int32 __kmpc_omp_task(ident_t *loc_ref, kmp_int32 gtid,
                          kmp_task_t *new_task) {
  kmp_int32 res;
  KMP_SET_THREAD_STATE_BLOCK(EXPLICIT_TASK);

#if KMP_DEBUG || OMPT_SUPPORT
  kmp_taskdata_t *new_taskdata = KMP_TASK_TO_TASKDATA(new_task);
#endif
  KA_TRACE(10, ("__kmpc_omp_task(enter): T#%d loc=%p task=%p\n", gtid, loc_ref,
                new_taskdata));
  __kmp_assert_valid_gtid(gtid);

#if OMPT_SUPPORT
  kmp_taskdata_t *parent = NULL;
  if (UNLIKELY(ompt_enabled.enabled)) {
    if (!new_taskdata->td_flags.started) {
      OMPT_STORE_RETURN_ADDRESS(gtid);
      parent = new_taskdata->td_parent;
      if (!parent->ompt_task_info.frame.enter_frame.ptr) {
        parent->ompt_task_info.frame.enter_frame.ptr =
            OMPT_GET_FRAME_ADDRESS(0);
      }
      if (ompt_enabled.ompt_callback_task_create) {
        ompt_callbacks.ompt_callback(ompt_callback_task_create)(
            &(parent->ompt_task_info.task_data),
            &(parent->ompt_task_info.frame),
            &(new_taskdata->ompt_task_info.task_data),
            ompt_task_explicit | TASK_TYPE_DETAILS_FORMAT(new_taskdata), 0,
            OMPT_LOAD_RETURN_ADDRESS(gtid));
      }
    } else {
      // We are scheduling the continuation of an UNTIED task.
      // Scheduling back to the parent task.
      __ompt_task_finish(new_task,
                         new_taskdata->ompt_task_info.scheduling_parent,
                         ompt_task_switch);
      new_taskdata->ompt_task_info.frame.exit_frame = ompt_data_none;
    }
  }
#endif

  res = __kmp_omp_task(gtid, new_task, true);

  KA_TRACE(10, ("__kmpc_omp_task(exit): T#%d returning "
                "TASK_CURRENT_NOT_QUEUED: loc=%p task=%p\n",
                gtid, loc_ref, new_taskdata));
#if OMPT_SUPPORT
  if (UNLIKELY(ompt_enabled.enabled && parent != NULL)) {
    parent->ompt_task_info.frame.enter_frame = ompt_data_none;
  }
#endif
  return res;
}

// __kmp_omp_taskloop_task: Wrapper around __kmp_omp_task to schedule
// a taskloop task with the correct OMPT return address
//
// loc_ref: location of original task pragma (ignored)
// gtid: Global Thread ID of encountering thread
// new_task: non-thread-switchable task thunk allocated by
// __kmp_omp_task_alloc()
// codeptr_ra: return address for OMPT callback
// Returns:
//    TASK_CURRENT_NOT_QUEUED (0) if did not suspend and queue current task to
//    be resumed later.
//    TASK_CURRENT_QUEUED (1) if suspended and queued the current task to be
//    resumed later.
kmp_int32 __kmp_omp_taskloop_task(ident_t *loc_ref, kmp_int32 gtid,
                                  kmp_task_t *new_task, void *codeptr_ra) {
  kmp_int32 res;
  KMP_SET_THREAD_STATE_BLOCK(EXPLICIT_TASK);

#if KMP_DEBUG || OMPT_SUPPORT
  kmp_taskdata_t *new_taskdata = KMP_TASK_TO_TASKDATA(new_task);
#endif
  KA_TRACE(10, ("__kmpc_omp_task(enter): T#%d loc=%p task=%p\n", gtid, loc_ref,
                new_taskdata));

#if OMPT_SUPPORT
  kmp_taskdata_t *parent = NULL;
  if (UNLIKELY(ompt_enabled.enabled && !new_taskdata->td_flags.started)) {
    parent = new_taskdata->td_parent;
    if (!parent->ompt_task_info.frame.enter_frame.ptr)
      parent->ompt_task_info.frame.enter_frame.ptr = OMPT_GET_FRAME_ADDRESS(0);
    if (ompt_enabled.ompt_callback_task_create) {
      ompt_callbacks.ompt_callback(ompt_callback_task_create)(
          &(parent->ompt_task_info.task_data), &(parent->ompt_task_info.frame),
          &(new_taskdata->ompt_task_info.task_data),
          ompt_task_explicit | TASK_TYPE_DETAILS_FORMAT(new_taskdata), 0,
          codeptr_ra);
    }
  }
#endif

  res = __kmp_omp_task(gtid, new_task, true);

  KA_TRACE(10, ("__kmpc_omp_task(exit): T#%d returning "
                "TASK_CURRENT_NOT_QUEUED: loc=%p task=%p\n",
                gtid, loc_ref, new_taskdata));
#if OMPT_SUPPORT
  if (UNLIKELY(ompt_enabled.enabled && parent != NULL)) {
    parent->ompt_task_info.frame.enter_frame = ompt_data_none;
  }
#endif
  return res;
}

template <bool ompt>
static kmp_int32 __kmpc_omp_taskwait_template(ident_t *loc_ref, kmp_int32 gtid,
                                              void *frame_address,
                                              void *return_address) {
  kmp_taskdata_t *taskdata = nullptr;
  kmp_info_t *thread;
  int thread_finished = FALSE;
  KMP_SET_THREAD_STATE_BLOCK(TASKWAIT);

  KA_TRACE(10, ("__kmpc_omp_taskwait(enter): T#%d loc=%p\n", gtid, loc_ref));
  KMP_DEBUG_ASSERT(gtid >= 0);

  if (__kmp_tasking_mode != tskm_immediate_exec) {
    thread = __kmp_threads[gtid];
    taskdata = thread->th.th_current_task;

#if OMPT_SUPPORT && OMPT_OPTIONAL
    ompt_data_t *my_task_data;
    ompt_data_t *my_parallel_data;

    if (ompt) {
      my_task_data = &(taskdata->ompt_task_info.task_data);
      my_parallel_data = OMPT_CUR_TEAM_DATA(thread);

      taskdata->ompt_task_info.frame.enter_frame.ptr = frame_address;

      if (ompt_enabled.ompt_callback_sync_region) {
        ompt_callbacks.ompt_callback(ompt_callback_sync_region)(
            ompt_sync_region_taskwait, ompt_scope_begin, my_parallel_data,
            my_task_data, return_address);
      }

      if (ompt_enabled.ompt_callback_sync_region_wait) {
        ompt_callbacks.ompt_callback(ompt_callback_sync_region_wait)(
            ompt_sync_region_taskwait, ompt_scope_begin, my_parallel_data,
            my_task_data, return_address);
      }
    }
#endif // OMPT_SUPPORT && OMPT_OPTIONAL

// Debugger: The taskwait is active. Store location and thread encountered the
// taskwait.
#if USE_ITT_BUILD
// Note: These values are used by ITT events as well.
#endif /* USE_ITT_BUILD */
    taskdata->td_taskwait_counter += 1;
    taskdata->td_taskwait_ident = loc_ref;
    taskdata->td_taskwait_thread = gtid + 1;

#if USE_ITT_BUILD
    void *itt_sync_obj = NULL;
#if USE_ITT_NOTIFY
    KMP_ITT_TASKWAIT_STARTING(itt_sync_obj);
#endif /* USE_ITT_NOTIFY */
#endif /* USE_ITT_BUILD */

    bool must_wait =
        !taskdata->td_flags.team_serial && !taskdata->td_flags.final;

    must_wait = must_wait || (thread->th.th_task_team != NULL &&
                              thread->th.th_task_team->tt.tt_found_proxy_tasks);
    // If hidden helper thread is encountered, we must enable wait here.
    must_wait =
        must_wait ||
        (__kmp_enable_hidden_helper && thread->th.th_task_team != NULL &&
         thread->th.th_task_team->tt.tt_hidden_helper_task_encountered);

    if (must_wait) {
      kmp_flag_32<false, false> flag(
          RCAST(std::atomic<kmp_uint32> *,
                &(taskdata->td_incomplete_child_tasks)),
          0U);
      while (KMP_ATOMIC_LD_ACQ(&taskdata->td_incomplete_child_tasks) != 0) {
        flag.execute_tasks(thread, gtid, FALSE,
                           &thread_finished USE_ITT_BUILD_ARG(itt_sync_obj),
                           __kmp_task_stealing_constraint);
      }
    }
#if USE_ITT_BUILD
    KMP_ITT_TASKWAIT_FINISHED(itt_sync_obj);
    KMP_FSYNC_ACQUIRED(taskdata); // acquire self - sync with children
#endif /* USE_ITT_BUILD */

    // Debugger:  The taskwait is completed. Location remains, but thread is
    // negated.
    taskdata->td_taskwait_thread = -taskdata->td_taskwait_thread;

#if OMPT_SUPPORT && OMPT_OPTIONAL
    if (ompt) {
      if (ompt_enabled.ompt_callback_sync_region_wait) {
        ompt_callbacks.ompt_callback(ompt_callback_sync_region_wait)(
            ompt_sync_region_taskwait, ompt_scope_end, my_parallel_data,
            my_task_data, return_address);
      }
      if (ompt_enabled.ompt_callback_sync_region) {
        ompt_callbacks.ompt_callback(ompt_callback_sync_region)(
            ompt_sync_region_taskwait, ompt_scope_end, my_parallel_data,
            my_task_data, return_address);
      }
      taskdata->ompt_task_info.frame.enter_frame = ompt_data_none;
    }
#endif // OMPT_SUPPORT && OMPT_OPTIONAL

  }

  KA_TRACE(10, ("__kmpc_omp_taskwait(exit): T#%d task %p finished waiting, "
                "returning TASK_CURRENT_NOT_QUEUED\n",
                gtid, taskdata));

  return TASK_CURRENT_NOT_QUEUED;
}

#if OMPT_SUPPORT && OMPT_OPTIONAL
OMPT_NOINLINE
static kmp_int32 __kmpc_omp_taskwait_ompt(ident_t *loc_ref, kmp_int32 gtid,
                                          void *frame_address,
                                          void *return_address) {
  return __kmpc_omp_taskwait_template<true>(loc_ref, gtid, frame_address,
                                            return_address);
}
#endif // OMPT_SUPPORT && OMPT_OPTIONAL

// __kmpc_omp_taskwait: Wait until all tasks generated by the current task are
// complete
kmp_int32 __kmpc_omp_taskwait(ident_t *loc_ref, kmp_int32 gtid) {
#if OMPT_SUPPORT && OMPT_OPTIONAL
  if (UNLIKELY(ompt_enabled.enabled)) {
    OMPT_STORE_RETURN_ADDRESS(gtid);
    return __kmpc_omp_taskwait_ompt(loc_ref, gtid, OMPT_GET_FRAME_ADDRESS(0),
                                    OMPT_LOAD_RETURN_ADDRESS(gtid));
  }
#endif
  return __kmpc_omp_taskwait_template<false>(loc_ref, gtid, NULL, NULL);
}

// __kmpc_omp_taskyield: switch to a different task
kmp_int32 __kmpc_omp_taskyield(ident_t *loc_ref, kmp_int32 gtid, int end_part) {
  kmp_taskdata_t *taskdata = NULL;
  kmp_info_t *thread;
  int thread_finished = FALSE;

  KMP_COUNT_BLOCK(OMP_TASKYIELD);
  KMP_SET_THREAD_STATE_BLOCK(TASKYIELD);

  KA_TRACE(10, ("__kmpc_omp_taskyield(enter): T#%d loc=%p end_part = %d\n",
                gtid, loc_ref, end_part));
  __kmp_assert_valid_gtid(gtid);

  if (__kmp_tasking_mode != tskm_immediate_exec && __kmp_init_parallel) {
    thread = __kmp_threads[gtid];
    taskdata = thread->th.th_current_task;
// Should we model this as a task wait or not?
// Debugger: The taskwait is active. Store location and thread encountered the
// taskwait.
#if USE_ITT_BUILD
// Note: These values are used by ITT events as well.
#endif /* USE_ITT_BUILD */
    taskdata->td_taskwait_counter += 1;
    taskdata->td_taskwait_ident = loc_ref;
    taskdata->td_taskwait_thread = gtid + 1;

#if USE_ITT_BUILD
    void *itt_sync_obj = NULL;
#if USE_ITT_NOTIFY
    KMP_ITT_TASKWAIT_STARTING(itt_sync_obj);
#endif /* USE_ITT_NOTIFY */
#endif /* USE_ITT_BUILD */
    if (!taskdata->td_flags.team_serial) {
      kmp_task_team_t *task_team = thread->th.th_task_team;
      if (task_team != NULL) {
        if (KMP_TASKING_ENABLED(task_team)) {
#if OMPT_SUPPORT
          if (UNLIKELY(ompt_enabled.enabled))
            thread->th.ompt_thread_info.ompt_task_yielded = 1;
#endif
          __kmp_execute_tasks_32(
              thread, gtid, (kmp_flag_32<> *)NULL, FALSE,
              &thread_finished USE_ITT_BUILD_ARG(itt_sync_obj),
              __kmp_task_stealing_constraint);
#if OMPT_SUPPORT
          if (UNLIKELY(ompt_enabled.enabled))
            thread->th.ompt_thread_info.ompt_task_yielded = 0;
#endif
        }
      }
    }
#if USE_ITT_BUILD
    KMP_ITT_TASKWAIT_FINISHED(itt_sync_obj);
#endif /* USE_ITT_BUILD */

    // Debugger:  The taskwait is completed. Location remains, but thread is
    // negated.
    taskdata->td_taskwait_thread = -taskdata->td_taskwait_thread;
  }

  KA_TRACE(10, ("__kmpc_omp_taskyield(exit): T#%d task %p resuming, "
                "returning TASK_CURRENT_NOT_QUEUED\n",
                gtid, taskdata));

  return TASK_CURRENT_NOT_QUEUED;
}

// Task Reduction implementation
//
// Note: initial implementation didn't take into account the possibility
// to specify omp_orig for initializer of the UDR (user defined reduction).
// Corrected implementation takes into account the omp_orig object.
// Compiler is free to use old implementation if omp_orig is not specified.

/*!
@ingroup BASIC_TYPES
@{
*/

/*!
Flags for special info per task reduction item.
*/
typedef struct kmp_taskred_flags {
  /*! 1 - use lazy alloc/init (e.g. big objects, #tasks < #threads) */
  unsigned lazy_priv : 1;
  unsigned reserved31 : 31;
} kmp_taskred_flags_t;

/*!
Internal struct for reduction data item related info set up by compiler.
*/
typedef struct kmp_task_red_input {
  void *reduce_shar; /**< shared between tasks item to reduce into */
  size_t reduce_size; /**< size of data item in bytes */
  // three compiler-generated routines (init, fini are optional):
  void *reduce_init; /**< data initialization routine (single parameter) */
  void *reduce_fini; /**< data finalization routine */
  void *reduce_comb; /**< data combiner routine */
  kmp_taskred_flags_t flags; /**< flags for additional info from compiler */
} kmp_task_red_input_t;

/*!
Internal struct for reduction data item related info saved by the library.
*/
typedef struct kmp_taskred_data {
  void *reduce_shar; /**< shared between tasks item to reduce into */
  size_t reduce_size; /**< size of data item */
  kmp_taskred_flags_t flags; /**< flags for additional info from compiler */
  void *reduce_priv; /**< array of thread specific items */
  void *reduce_pend; /**< end of private data for faster comparison op */
  // three compiler-generated routines (init, fini are optional):
  void *reduce_comb; /**< data combiner routine */
  void *reduce_init; /**< data initialization routine (two parameters) */
  void *reduce_fini; /**< data finalization routine */
  void *reduce_orig; /**< original item (can be used in UDR initializer) */
} kmp_taskred_data_t;

/*!
Internal struct for reduction data item related info set up by compiler.

New interface: added reduce_orig field to provide omp_orig for UDR initializer.
*/
typedef struct kmp_taskred_input {
  void *reduce_shar; /**< shared between tasks item to reduce into */
  void *reduce_orig; /**< original reduction item used for initialization */
  size_t reduce_size; /**< size of data item */
  // three compiler-generated routines (init, fini are optional):
  void *reduce_init; /**< data initialization routine (two parameters) */
  void *reduce_fini; /**< data finalization routine */
  void *reduce_comb; /**< data combiner routine */
  kmp_taskred_flags_t flags; /**< flags for additional info from compiler */
} kmp_taskred_input_t;
/*!
@}
*/

template <typename T> void __kmp_assign_orig(kmp_taskred_data_t &item, T &src);
template <>
void __kmp_assign_orig<kmp_task_red_input_t>(kmp_taskred_data_t &item,
                                             kmp_task_red_input_t &src) {
  item.reduce_orig = NULL;
}
template <>
void __kmp_assign_orig<kmp_taskred_input_t>(kmp_taskred_data_t &item,
                                            kmp_taskred_input_t &src) {
  if (src.reduce_orig != NULL) {
    item.reduce_orig = src.reduce_orig;
  } else {
    item.reduce_orig = src.reduce_shar;
  } // non-NULL reduce_orig means new interface used
}

template <typename T> void __kmp_call_init(kmp_taskred_data_t &item, size_t j);
template <>
void __kmp_call_init<kmp_task_red_input_t>(kmp_taskred_data_t &item,
                                           size_t offset) {
  ((void (*)(void *))item.reduce_init)((char *)(item.reduce_priv) + offset);
}
template <>
void __kmp_call_init<kmp_taskred_input_t>(kmp_taskred_data_t &item,
                                          size_t offset) {
  ((void (*)(void *, void *))item.reduce_init)(
      (char *)(item.reduce_priv) + offset, item.reduce_orig);
}

template <typename T>
void *__kmp_task_reduction_init(int gtid, int num, T *data) {
  __kmp_assert_valid_gtid(gtid);
  kmp_info_t *thread = __kmp_threads[gtid];
  kmp_taskgroup_t *tg = thread->th.th_current_task->td_taskgroup;
  kmp_uint32 nth = thread->th.th_team_nproc;
  kmp_taskred_data_t *arr;

  // check input data just in case
  KMP_ASSERT(tg != NULL);
  KMP_ASSERT(data != NULL);
  KMP_ASSERT(num > 0);
  if (nth == 1) {
    KA_TRACE(10, ("__kmpc_task_reduction_init: T#%d, tg %p, exiting nth=1\n",
                  gtid, tg));
    return (void *)tg;
  }
  KA_TRACE(10, ("__kmpc_task_reduction_init: T#%d, taskgroup %p, #items %d\n",
                gtid, tg, num));
  arr = (kmp_taskred_data_t *)__kmp_thread_malloc(
      thread, num * sizeof(kmp_taskred_data_t));
  for (int i = 0; i < num; ++i) {
    size_t size = data[i].reduce_size - 1;
    // round the size up to cache line per thread-specific item
    size += CACHE_LINE - size % CACHE_LINE;
    KMP_ASSERT(data[i].reduce_comb != NULL); // combiner is mandatory
    arr[i].reduce_shar = data[i].reduce_shar;
    arr[i].reduce_size = size;
    arr[i].flags = data[i].flags;
    arr[i].reduce_comb = data[i].reduce_comb;
    arr[i].reduce_init = data[i].reduce_init;
    arr[i].reduce_fini = data[i].reduce_fini;
    __kmp_assign_orig<T>(arr[i], data[i]);
    if (!arr[i].flags.lazy_priv) {
      // allocate cache-line aligned block and fill it with zeros
      arr[i].reduce_priv = __kmp_allocate(nth * size);
      arr[i].reduce_pend = (char *)(arr[i].reduce_priv) + nth * size;
      if (arr[i].reduce_init != NULL) {
        // initialize all thread-specific items
        for (size_t j = 0; j < nth; ++j) {
          __kmp_call_init<T>(arr[i], j * size);
        }
      }
    } else {
      // only allocate space for pointers now,
      // objects will be lazily allocated/initialized if/when requested
      // note that __kmp_allocate zeroes the allocated memory
      arr[i].reduce_priv = __kmp_allocate(nth * sizeof(void *));
    }
  }
  tg->reduce_data = (void *)arr;
  tg->reduce_num_data = num;
  return (void *)tg;
}

/*!
@ingroup TASKING
@param gtid      Global thread ID
@param num       Number of data items to reduce
@param data      Array of data for reduction
@return The taskgroup identifier

Initialize task reduction for the taskgroup.

Note: this entry supposes the optional compiler-generated initializer routine
has single parameter - pointer to object to be initialized. That means
the reduction either does not use omp_orig object, or the omp_orig is accessible
without help of the runtime library.
*/
void *__kmpc_task_reduction_init(int gtid, int num, void *data) {
  return __kmp_task_reduction_init(gtid, num, (kmp_task_red_input_t *)data);
}

/*!
@ingroup TASKING
@param gtid      Global thread ID
@param num       Number of data items to reduce
@param data      Array of data for reduction
@return The taskgroup identifier

Initialize task reduction for the taskgroup.

Note: this entry supposes the optional compiler-generated initializer routine
has two parameters, pointer to object to be initialized and pointer to omp_orig
*/
void *__kmpc_taskred_init(int gtid, int num, void *data) {
  return __kmp_task_reduction_init(gtid, num, (kmp_taskred_input_t *)data);
}

// Copy task reduction data (except for shared pointers).
template <typename T>
void __kmp_task_reduction_init_copy(kmp_info_t *thr, int num, T *data,
                                    kmp_taskgroup_t *tg, void *reduce_data) {
  kmp_taskred_data_t *arr;
  KA_TRACE(20, ("__kmp_task_reduction_init_copy: Th %p, init taskgroup %p,"
                " from data %p\n",
                thr, tg, reduce_data));
  arr = (kmp_taskred_data_t *)__kmp_thread_malloc(
      thr, num * sizeof(kmp_taskred_data_t));
  // threads will share private copies, thunk routines, sizes, flags, etc.:
  KMP_MEMCPY(arr, reduce_data, num * sizeof(kmp_taskred_data_t));
  for (int i = 0; i < num; ++i) {
    arr[i].reduce_shar = data[i].reduce_shar; // init unique shared pointers
  }
  tg->reduce_data = (void *)arr;
  tg->reduce_num_data = num;
}

/*!
@ingroup TASKING
@param gtid    Global thread ID
@param tskgrp  The taskgroup ID (optional)
@param data    Shared location of the item
@return The pointer to per-thread data

Get thread-specific location of data item
*/
void *__kmpc_task_reduction_get_th_data(int gtid, void *tskgrp, void *data) {
  __kmp_assert_valid_gtid(gtid);
  kmp_info_t *thread = __kmp_threads[gtid];
  kmp_int32 nth = thread->th.th_team_nproc;
  if (nth == 1)
    return data; // nothing to do

  kmp_taskgroup_t *tg = (kmp_taskgroup_t *)tskgrp;
  if (tg == NULL)
    tg = thread->th.th_current_task->td_taskgroup;
  KMP_ASSERT(tg != NULL);
  kmp_taskred_data_t *arr = (kmp_taskred_data_t *)(tg->reduce_data);
  kmp_int32 num = tg->reduce_num_data;
  kmp_int32 tid = thread->th.th_info.ds.ds_tid;

  KMP_ASSERT(data != NULL);
  while (tg != NULL) {
    for (int i = 0; i < num; ++i) {
      if (!arr[i].flags.lazy_priv) {
        if (data == arr[i].reduce_shar ||
            (data >= arr[i].reduce_priv && data < arr[i].reduce_pend))
          return (char *)(arr[i].reduce_priv) + tid * arr[i].reduce_size;
      } else {
        // check shared location first
        void **p_priv = (void **)(arr[i].reduce_priv);
        if (data == arr[i].reduce_shar)
          goto found;
        // check if we get some thread specific location as parameter
        for (int j = 0; j < nth; ++j)
          if (data == p_priv[j])
            goto found;
        continue; // not found, continue search
      found:
        if (p_priv[tid] == NULL) {
          // allocate thread specific object lazily
          p_priv[tid] = __kmp_allocate(arr[i].reduce_size);
          if (arr[i].reduce_init != NULL) {
            if (arr[i].reduce_orig != NULL) { // new interface
              ((void (*)(void *, void *))arr[i].reduce_init)(
                  p_priv[tid], arr[i].reduce_orig);
            } else { // old interface (single parameter)
              ((void (*)(void *))arr[i].reduce_init)(p_priv[tid]);
            }
          }
        }
        return p_priv[tid];
      }
    }
    tg = tg->parent;
    arr = (kmp_taskred_data_t *)(tg->reduce_data);
    num = tg->reduce_num_data;
  }
  KMP_ASSERT2(0, "Unknown task reduction item");
  return NULL; // ERROR, this line never executed
}

// Finalize task reduction.
// Called from __kmpc_end_taskgroup()
static void __kmp_task_reduction_fini(kmp_info_t *th, kmp_taskgroup_t *tg) {
  kmp_int32 nth = th->th.th_team_nproc;
  KMP_DEBUG_ASSERT(nth > 1); // should not be called if nth == 1
  kmp_taskred_data_t *arr = (kmp_taskred_data_t *)tg->reduce_data;
  kmp_int32 num = tg->reduce_num_data;
  for (int i = 0; i < num; ++i) {
    void *sh_data = arr[i].reduce_shar;
    void (*f_fini)(void *) = (void (*)(void *))(arr[i].reduce_fini);
    void (*f_comb)(void *, void *) =
        (void (*)(void *, void *))(arr[i].reduce_comb);
    if (!arr[i].flags.lazy_priv) {
      void *pr_data = arr[i].reduce_priv;
      size_t size = arr[i].reduce_size;
      for (int j = 0; j < nth; ++j) {
        void *priv_data = (char *)pr_data + j * size;
        f_comb(sh_data, priv_data); // combine results
        if (f_fini)
          f_fini(priv_data); // finalize if needed
      }
    } else {
      void **pr_data = (void **)(arr[i].reduce_priv);
      for (int j = 0; j < nth; ++j) {
        if (pr_data[j] != NULL) {
          f_comb(sh_data, pr_data[j]); // combine results
          if (f_fini)
            f_fini(pr_data[j]); // finalize if needed
          __kmp_free(pr_data[j]);
        }
      }
    }
    __kmp_free(arr[i].reduce_priv);
  }
  __kmp_thread_free(th, arr);
  tg->reduce_data = NULL;
  tg->reduce_num_data = 0;
}

// Cleanup task reduction data for parallel or worksharing,
// do not touch task private data other threads still working with.
// Called from __kmpc_end_taskgroup()
static void __kmp_task_reduction_clean(kmp_info_t *th, kmp_taskgroup_t *tg) {
  __kmp_thread_free(th, tg->reduce_data);
  tg->reduce_data = NULL;
  tg->reduce_num_data = 0;
}

template <typename T>
void *__kmp_task_reduction_modifier_init(ident_t *loc, int gtid, int is_ws,
                                         int num, T *data) {
  __kmp_assert_valid_gtid(gtid);
  kmp_info_t *thr = __kmp_threads[gtid];
  kmp_int32 nth = thr->th.th_team_nproc;
  __kmpc_taskgroup(loc, gtid); // form new taskgroup first
  if (nth == 1) {
    KA_TRACE(10,
             ("__kmpc_reduction_modifier_init: T#%d, tg %p, exiting nth=1\n",
              gtid, thr->th.th_current_task->td_taskgroup));
    return (void *)thr->th.th_current_task->td_taskgroup;
  }
  kmp_team_t *team = thr->th.th_team;
  void *reduce_data;
  kmp_taskgroup_t *tg;
  reduce_data = KMP_ATOMIC_LD_RLX(&team->t.t_tg_reduce_data[is_ws]);
  if (reduce_data == NULL &&
      __kmp_atomic_compare_store(&team->t.t_tg_reduce_data[is_ws], reduce_data,
                                 (void *)1)) {
    // single thread enters this block to initialize common reduction data
    KMP_DEBUG_ASSERT(reduce_data == NULL);
    // first initialize own data, then make a copy other threads can use
    tg = (kmp_taskgroup_t *)__kmp_task_reduction_init<T>(gtid, num, data);
    reduce_data = __kmp_thread_malloc(thr, num * sizeof(kmp_taskred_data_t));
    KMP_MEMCPY(reduce_data, tg->reduce_data, num * sizeof(kmp_taskred_data_t));
    // fini counters should be 0 at this point
    KMP_DEBUG_ASSERT(KMP_ATOMIC_LD_RLX(&team->t.t_tg_fini_counter[0]) == 0);
    KMP_DEBUG_ASSERT(KMP_ATOMIC_LD_RLX(&team->t.t_tg_fini_counter[1]) == 0);
    KMP_ATOMIC_ST_REL(&team->t.t_tg_reduce_data[is_ws], reduce_data);
  } else {
    while (
        (reduce_data = KMP_ATOMIC_LD_ACQ(&team->t.t_tg_reduce_data[is_ws])) ==
        (void *)1) { // wait for task reduction initialization
      KMP_CPU_PAUSE();
    }
    KMP_DEBUG_ASSERT(reduce_data > (void *)1); // should be valid pointer here
    tg = thr->th.th_current_task->td_taskgroup;
    __kmp_task_reduction_init_copy<T>(thr, num, data, tg, reduce_data);
  }
  return tg;
}

/*!
@ingroup TASKING
@param loc       Source location info
@param gtid      Global thread ID
@param is_ws     Is 1 if the reduction is for worksharing, 0 otherwise
@param num       Number of data items to reduce
@param data      Array of data for reduction
@return The taskgroup identifier

Initialize task reduction for a parallel or worksharing.

Note: this entry supposes the optional compiler-generated initializer routine
has single parameter - pointer to object to be initialized. That means
the reduction either does not use omp_orig object, or the omp_orig is accessible
without help of the runtime library.
*/
void *__kmpc_task_reduction_modifier_init(ident_t *loc, int gtid, int is_ws,
                                          int num, void *data) {
  return __kmp_task_reduction_modifier_init(loc, gtid, is_ws, num,
                                            (kmp_task_red_input_t *)data);
}

/*!
@ingroup TASKING
@param loc       Source location info
@param gtid      Global thread ID
@param is_ws     Is 1 if the reduction is for worksharing, 0 otherwise
@param num       Number of data items to reduce
@param data      Array of data for reduction
@return The taskgroup identifier

Initialize task reduction for a parallel or worksharing.

Note: this entry supposes the optional compiler-generated initializer routine
has two parameters, pointer to object to be initialized and pointer to omp_orig
*/
void *__kmpc_taskred_modifier_init(ident_t *loc, int gtid, int is_ws, int num,
                                   void *data) {
  return __kmp_task_reduction_modifier_init(loc, gtid, is_ws, num,
                                            (kmp_taskred_input_t *)data);
}

/*!
@ingroup TASKING
@param loc       Source location info
@param gtid      Global thread ID
@param is_ws     Is 1 if the reduction is for worksharing, 0 otherwise

Finalize task reduction for a parallel or worksharing.
*/
void __kmpc_task_reduction_modifier_fini(ident_t *loc, int gtid, int is_ws) {
  __kmpc_end_taskgroup(loc, gtid);
}

// __kmpc_taskgroup: Start a new taskgroup
void __kmpc_taskgroup(ident_t *loc, int gtid) {
  __kmp_assert_valid_gtid(gtid);
  kmp_info_t *thread = __kmp_threads[gtid];
  kmp_taskdata_t *taskdata = thread->th.th_current_task;
  kmp_taskgroup_t *tg_new =
      (kmp_taskgroup_t *)__kmp_thread_malloc(thread, sizeof(kmp_taskgroup_t));
  KA_TRACE(10, ("__kmpc_taskgroup: T#%d loc=%p group=%p\n", gtid, loc, tg_new));
  KMP_ATOMIC_ST_RLX(&tg_new->count, 0);
  KMP_ATOMIC_ST_RLX(&tg_new->cancel_request, cancel_noreq);
  tg_new->parent = taskdata->td_taskgroup;
  tg_new->reduce_data = NULL;
  tg_new->reduce_num_data = 0;
  tg_new->gomp_data = NULL;
  taskdata->td_taskgroup = tg_new;

#if OMPT_SUPPORT && OMPT_OPTIONAL
  if (UNLIKELY(ompt_enabled.ompt_callback_sync_region)) {
    void *codeptr = OMPT_LOAD_RETURN_ADDRESS(gtid);
    if (!codeptr)
      codeptr = OMPT_GET_RETURN_ADDRESS(0);
    kmp_team_t *team = thread->th.th_team;
    ompt_data_t my_task_data = taskdata->ompt_task_info.task_data;
    // FIXME: I think this is wrong for lwt!
    ompt_data_t my_parallel_data = team->t.ompt_team_info.parallel_data;

    ompt_callbacks.ompt_callback(ompt_callback_sync_region)(
        ompt_sync_region_taskgroup, ompt_scope_begin, &(my_parallel_data),
        &(my_task_data), codeptr);
  }
#endif
}

// __kmpc_end_taskgroup: Wait until all tasks generated by the current task
//                       and its descendants are complete
void __kmpc_end_taskgroup(ident_t *loc, int gtid) {
  __kmp_assert_valid_gtid(gtid);
  kmp_info_t *thread = __kmp_threads[gtid];
  kmp_taskdata_t *taskdata = thread->th.th_current_task;
  kmp_taskgroup_t *taskgroup = taskdata->td_taskgroup;
  int thread_finished = FALSE;

#if OMPT_SUPPORT && OMPT_OPTIONAL
  kmp_team_t *team;
  ompt_data_t my_task_data;
  ompt_data_t my_parallel_data;
  void *codeptr = nullptr;
  if (UNLIKELY(ompt_enabled.enabled)) {
    team = thread->th.th_team;
    my_task_data = taskdata->ompt_task_info.task_data;
    // FIXME: I think this is wrong for lwt!
    my_parallel_data = team->t.ompt_team_info.parallel_data;
    codeptr = OMPT_LOAD_RETURN_ADDRESS(gtid);
    if (!codeptr)
      codeptr = OMPT_GET_RETURN_ADDRESS(0);
  }
#endif

  KA_TRACE(10, ("__kmpc_end_taskgroup(enter): T#%d loc=%p\n", gtid, loc));
  KMP_DEBUG_ASSERT(taskgroup != NULL);
  KMP_SET_THREAD_STATE_BLOCK(TASKGROUP);

  if (__kmp_tasking_mode != tskm_immediate_exec) {
    // mark task as waiting not on a barrier
    taskdata->td_taskwait_counter += 1;
    taskdata->td_taskwait_ident = loc;
    taskdata->td_taskwait_thread = gtid + 1;
#if USE_ITT_BUILD
    // For ITT the taskgroup wait is similar to taskwait until we need to
    // distinguish them
    void *itt_sync_obj = NULL;
#if USE_ITT_NOTIFY
    KMP_ITT_TASKWAIT_STARTING(itt_sync_obj);
#endif /* USE_ITT_NOTIFY */
#endif /* USE_ITT_BUILD */

#if OMPT_SUPPORT && OMPT_OPTIONAL
    if (UNLIKELY(ompt_enabled.ompt_callback_sync_region_wait)) {
      ompt_callbacks.ompt_callback(ompt_callback_sync_region_wait)(
          ompt_sync_region_taskgroup, ompt_scope_begin, &(my_parallel_data),
          &(my_task_data), codeptr);
    }
#endif

    if (!taskdata->td_flags.team_serial ||
        (thread->th.th_task_team != NULL &&
         (thread->th.th_task_team->tt.tt_found_proxy_tasks ||
          thread->th.th_task_team->tt.tt_hidden_helper_task_encountered))) {
      kmp_flag_32<false, false> flag(
          RCAST(std::atomic<kmp_uint32> *, &(taskgroup->count)), 0U);
      while (KMP_ATOMIC_LD_ACQ(&taskgroup->count) != 0) {
        flag.execute_tasks(thread, gtid, FALSE,
                           &thread_finished USE_ITT_BUILD_ARG(itt_sync_obj),
                           __kmp_task_stealing_constraint);
      }
    }
    taskdata->td_taskwait_thread = -taskdata->td_taskwait_thread; // end waiting

#if OMPT_SUPPORT && OMPT_OPTIONAL
    if (UNLIKELY(ompt_enabled.ompt_callback_sync_region_wait)) {
      ompt_callbacks.ompt_callback(ompt_callback_sync_region_wait)(
          ompt_sync_region_taskgroup, ompt_scope_end, &(my_parallel_data),
          &(my_task_data), codeptr);
    }
#endif

#if USE_ITT_BUILD
    KMP_ITT_TASKWAIT_FINISHED(itt_sync_obj);
    KMP_FSYNC_ACQUIRED(taskdata); // acquire self - sync with descendants
#endif /* USE_ITT_BUILD */
  }
  KMP_DEBUG_ASSERT(taskgroup->count == 0);

  if (taskgroup->reduce_data != NULL &&
      !taskgroup->gomp_data) { // need to reduce?
    int cnt;
    void *reduce_data;
    kmp_team_t *t = thread->th.th_team;
    kmp_taskred_data_t *arr = (kmp_taskred_data_t *)taskgroup->reduce_data;
    // check if <priv> data of the first reduction variable shared for the team
    void *priv0 = arr[0].reduce_priv;
    if ((reduce_data = KMP_ATOMIC_LD_ACQ(&t->t.t_tg_reduce_data[0])) != NULL &&
        ((kmp_taskred_data_t *)reduce_data)[0].reduce_priv == priv0) {
      // finishing task reduction on parallel
      cnt = KMP_ATOMIC_INC(&t->t.t_tg_fini_counter[0]);
      if (cnt == thread->th.th_team_nproc - 1) {
        // we are the last thread passing __kmpc_reduction_modifier_fini()
        // finalize task reduction:
        __kmp_task_reduction_fini(thread, taskgroup);
        // cleanup fields in the team structure:
        // TODO: is relaxed store enough here (whole barrier should follow)?
        __kmp_thread_free(thread, reduce_data);
        KMP_ATOMIC_ST_REL(&t->t.t_tg_reduce_data[0], NULL);
        KMP_ATOMIC_ST_REL(&t->t.t_tg_fini_counter[0], 0);
      } else {
        // we are not the last thread passing __kmpc_reduction_modifier_fini(),
        // so do not finalize reduction, just clean own copy of the data
        __kmp_task_reduction_clean(thread, taskgroup);
      }
    } else if ((reduce_data = KMP_ATOMIC_LD_ACQ(&t->t.t_tg_reduce_data[1])) !=
                   NULL &&
               ((kmp_taskred_data_t *)reduce_data)[0].reduce_priv == priv0) {
      // finishing task reduction on worksharing
      cnt = KMP_ATOMIC_INC(&t->t.t_tg_fini_counter[1]);
      if (cnt == thread->th.th_team_nproc - 1) {
        // we are the last thread passing __kmpc_reduction_modifier_fini()
        __kmp_task_reduction_fini(thread, taskgroup);
        // cleanup fields in team structure:
        // TODO: is relaxed store enough here (whole barrier should follow)?
        __kmp_thread_free(thread, reduce_data);
        KMP_ATOMIC_ST_REL(&t->t.t_tg_reduce_data[1], NULL);
        KMP_ATOMIC_ST_REL(&t->t.t_tg_fini_counter[1], 0);
      } else {
        // we are not the last thread passing __kmpc_reduction_modifier_fini(),
        // so do not finalize reduction, just clean own copy of the data
        __kmp_task_reduction_clean(thread, taskgroup);
      }
    } else {
      // finishing task reduction on taskgroup
      __kmp_task_reduction_fini(thread, taskgroup);
    }
  }
  // Restore parent taskgroup for the current task
  taskdata->td_taskgroup = taskgroup->parent;
  __kmp_thread_free(thread, taskgroup);

  KA_TRACE(10, ("__kmpc_end_taskgroup(exit): T#%d task %p finished waiting\n",
                gtid, taskdata));

#if OMPT_SUPPORT && OMPT_OPTIONAL
  if (UNLIKELY(ompt_enabled.ompt_callback_sync_region)) {
    ompt_callbacks.ompt_callback(ompt_callback_sync_region)(
        ompt_sync_region_taskgroup, ompt_scope_end, &(my_parallel_data),
        &(my_task_data), codeptr);
  }
#endif
}

// __kmp_remove_my_task: remove a task from my own deque
static kmp_task_t *__kmp_remove_my_task(kmp_info_t *thread, kmp_int32 gtid,
                                        kmp_task_team_t *task_team,
                                        kmp_int32 is_constrained) {
  kmp_task_t *task;
  kmp_taskdata_t *taskdata;
  kmp_thread_data_t *thread_data;
  kmp_uint32 tail;

  KMP_DEBUG_ASSERT(__kmp_tasking_mode != tskm_immediate_exec);
  KMP_DEBUG_ASSERT(task_team->tt.tt_threads_data !=
                   NULL); // Caller should check this condition

  thread_data = &task_team->tt.tt_threads_data[__kmp_tid_from_gtid(gtid)];

  KA_TRACE(10, ("__kmp_remove_my_task(enter): T#%d ntasks=%d head=%u tail=%u\n",
                gtid, thread_data->td.td_deque_ntasks,
                thread_data->td.td_deque_head, thread_data->td.td_deque_tail));

  if (TCR_4(thread_data->td.td_deque_ntasks) == 0) {
    KA_TRACE(10,
             ("__kmp_remove_my_task(exit #1): T#%d No tasks to remove: "
              "ntasks=%d head=%u tail=%u\n",
              gtid, thread_data->td.td_deque_ntasks,
              thread_data->td.td_deque_head, thread_data->td.td_deque_tail));
    return NULL;
  }

  __kmp_acquire_bootstrap_lock(&thread_data->td.td_deque_lock);

  if (TCR_4(thread_data->td.td_deque_ntasks) == 0) {
    __kmp_release_bootstrap_lock(&thread_data->td.td_deque_lock);
    KA_TRACE(10,
             ("__kmp_remove_my_task(exit #2): T#%d No tasks to remove: "
              "ntasks=%d head=%u tail=%u\n",
              gtid, thread_data->td.td_deque_ntasks,
              thread_data->td.td_deque_head, thread_data->td.td_deque_tail));
    return NULL;
  }

  tail = (thread_data->td.td_deque_tail - 1) &
         TASK_DEQUE_MASK(thread_data->td); // Wrap index.
  taskdata = thread_data->td.td_deque[tail];

  if (!__kmp_task_is_allowed(gtid, is_constrained, taskdata,
                             thread->th.th_current_task)) {
    // The TSC does not allow to steal victim task
    __kmp_release_bootstrap_lock(&thread_data->td.td_deque_lock);
    KA_TRACE(10,
             ("__kmp_remove_my_task(exit #3): T#%d TSC blocks tail task: "
              "ntasks=%d head=%u tail=%u\n",
              gtid, thread_data->td.td_deque_ntasks,
              thread_data->td.td_deque_head, thread_data->td.td_deque_tail));
    return NULL;
  }

  thread_data->td.td_deque_tail = tail;
  TCW_4(thread_data->td.td_deque_ntasks, thread_data->td.td_deque_ntasks - 1);

  __kmp_release_bootstrap_lock(&thread_data->td.td_deque_lock);

  KA_TRACE(10, ("__kmp_remove_my_task(exit #4): T#%d task %p removed: "
                "ntasks=%d head=%u tail=%u\n",
                gtid, taskdata, thread_data->td.td_deque_ntasks,
                thread_data->td.td_deque_head, thread_data->td.td_deque_tail));

  task = KMP_TASKDATA_TO_TASK(taskdata);
  return task;
}

// __kmp_steal_task: remove a task from another thread's deque
// Assume that calling thread has already checked existence of
// task_team thread_data before calling this routine.
static kmp_task_t *__kmp_steal_task(kmp_info_t *victim_thr, kmp_int32 gtid,
                                    kmp_task_team_t *task_team,
                                    std::atomic<kmp_int32> *unfinished_threads,
                                    int *thread_finished,
                                    kmp_int32 is_constrained) {
  kmp_task_t *task;
  kmp_taskdata_t *taskdata;
  kmp_taskdata_t *current;
  kmp_thread_data_t *victim_td, *threads_data;
  kmp_int32 target;
  kmp_int32 victim_tid;

  KMP_DEBUG_ASSERT(__kmp_tasking_mode != tskm_immediate_exec);

  threads_data = task_team->tt.tt_threads_data;
  KMP_DEBUG_ASSERT(threads_data != NULL); // Caller should check this condition

  victim_tid = victim_thr->th.th_info.ds.ds_tid;
  victim_td = &threads_data[victim_tid];

  //ME1
  #if TASK_STEALING_POLICY
  kmp_info_t *thread = __kmp_threads[gtid];
  #endif

  #if TASK_STEALING_POLICY == NO_TASK_STEALING
  if(thread->th.th_cluster != victim_thr->th.th_cluster) return NULL;
  #endif
  //ME2

  KA_TRACE(10, ("__kmp_steal_task(enter): T#%d try to steal from T#%d: "
                "task_team=%p ntasks=%d head=%u tail=%u\n",
                gtid, __kmp_gtid_from_thread(victim_thr), task_team,
                victim_td->td.td_deque_ntasks, victim_td->td.td_deque_head,
                victim_td->td.td_deque_tail));

  if (TCR_4(victim_td->td.td_deque_ntasks) == 0) {
    KA_TRACE(10, ("__kmp_steal_task(exit #1): T#%d could not steal from T#%d: "
                  "task_team=%p ntasks=%d head=%u tail=%u\n",
                  gtid, __kmp_gtid_from_thread(victim_thr), task_team,
                  victim_td->td.td_deque_ntasks, victim_td->td.td_deque_head,
                  victim_td->td.td_deque_tail));
    return NULL;
  }

  __kmp_acquire_bootstrap_lock(&victim_td->td.td_deque_lock);

  int ntasks = TCR_4(victim_td->td.td_deque_ntasks);
  // Check again after we acquire the lock
  if (ntasks == 0) {
    __kmp_release_bootstrap_lock(&victim_td->td.td_deque_lock);
    KA_TRACE(10, ("__kmp_steal_task(exit #2): T#%d could not steal from T#%d: "
                  "task_team=%p ntasks=%d head=%u tail=%u\n",
                  gtid, __kmp_gtid_from_thread(victim_thr), task_team, ntasks,
                  victim_td->td.td_deque_head, victim_td->td.td_deque_tail));
    return NULL;
  }

  KMP_DEBUG_ASSERT(victim_td->td.td_deque != NULL);
  current = __kmp_threads[gtid]->th.th_current_task;
  taskdata = victim_td->td.td_deque[victim_td->td.td_deque_head];

  //ME1
  if (taskdata->td_no_steal){
    __kmp_release_bootstrap_lock(&victim_td->td.td_deque_lock);
    return NULL;
  }
  #if TASK_STEALING_POLICY == NO_TASKLOOP_STEALING
  if(taskdata->td_cluster == CLUSTER_UNASSIGNED && thread->th.th_cluster != taskdata->td_cluster){
    __kmp_release_bootstrap_lock(&victim_td->td.td_deque_lock);
    return NULL;
  }
  #endif
  //ME2

  if (__kmp_task_is_allowed(gtid, is_constrained, taskdata, current)) {
    // Bump head pointer and Wrap.
    victim_td->td.td_deque_head =
        (victim_td->td.td_deque_head + 1) & TASK_DEQUE_MASK(victim_td->td);
  } else {
    if (!task_team->tt.tt_untied_task_encountered) {
      // The TSC does not allow to steal victim task
      __kmp_release_bootstrap_lock(&victim_td->td.td_deque_lock);
      KA_TRACE(10, ("__kmp_steal_task(exit #3): T#%d could not steal from "
                    "T#%d: task_team=%p ntasks=%d head=%u tail=%u\n",
                    gtid, __kmp_gtid_from_thread(victim_thr), task_team, ntasks,
                    victim_td->td.td_deque_head, victim_td->td.td_deque_tail));
      return NULL;
    }
    int i;
    // walk through victim's deque trying to steal any task
    target = victim_td->td.td_deque_head;
    taskdata = NULL;
    for (i = 1; i < ntasks; ++i) {
      target = (target + 1) & TASK_DEQUE_MASK(victim_td->td);
      taskdata = victim_td->td.td_deque[target];
      if (__kmp_task_is_allowed(gtid, is_constrained, taskdata, current)) {
        break; // found victim task
      } else {
        taskdata = NULL;
      }
    }
    if (taskdata == NULL) {
      // No appropriate candidate to steal found
      __kmp_release_bootstrap_lock(&victim_td->td.td_deque_lock);
      KA_TRACE(10, ("__kmp_steal_task(exit #4): T#%d could not steal from "
                    "T#%d: task_team=%p ntasks=%d head=%u tail=%u\n",
                    gtid, __kmp_gtid_from_thread(victim_thr), task_team, ntasks,
                    victim_td->td.td_deque_head, victim_td->td.td_deque_tail));
      return NULL;
    }
    int prev = target;
    for (i = i + 1; i < ntasks; ++i) {
      // shift remaining tasks in the deque left by 1
      target = (target + 1) & TASK_DEQUE_MASK(victim_td->td);
      victim_td->td.td_deque[prev] = victim_td->td.td_deque[target];
      prev = target;
    }
    KMP_DEBUG_ASSERT(
        victim_td->td.td_deque_tail ==
        (kmp_uint32)((target + 1) & TASK_DEQUE_MASK(victim_td->td)));
    victim_td->td.td_deque_tail = target; // tail -= 1 (wrapped))
  }
  if (*thread_finished) {
    // We need to un-mark this victim as a finished victim.  This must be done
    // before releasing the lock, or else other threads (starting with the
    // primary thread victim) might be prematurely released from the barrier!!!
#if KMP_DEBUG
    kmp_int32 count =
#endif
        KMP_ATOMIC_INC(unfinished_threads);
    KA_TRACE(
        20,
        ("__kmp_steal_task: T#%d inc unfinished_threads to %d: task_team=%p\n",
         gtid, count + 1, task_team));
    *thread_finished = FALSE;
  }
  TCW_4(victim_td->td.td_deque_ntasks, ntasks - 1);

  __kmp_release_bootstrap_lock(&victim_td->td.td_deque_lock);

  KMP_COUNT_BLOCK(TASK_stolen);
  KA_TRACE(10,
           ("__kmp_steal_task(exit #5): T#%d stole task %p from T#%d: "
            "task_team=%p ntasks=%d head=%u tail=%u\n",
            gtid, taskdata, __kmp_gtid_from_thread(victim_thr), task_team,
            ntasks, victim_td->td.td_deque_head, victim_td->td.td_deque_tail));

  task = KMP_TASKDATA_TO_TASK(taskdata);
  return task;
}

//ME1
//perf counter syscall
uint64_t perf_event_open(struct perf_event_attr * hw,
                         pid_t pid, kmp_int32 cpu, kmp_int32 grp, kmp_uint64 flags)
{
    return syscall(__NR_perf_event_open, hw, pid, cpu, grp, flags);
}

// Initiate and starts the perf counters
static void __kmp_perf_init_counters(kmp_info_t *thread){

    kmp_int32 tid = __kmp_get_tid();
    thread->th.th_cpu = sched_getcpu();

    // TODO Fix cluster initialisation here, currently hardcoded
#if TX2
    if(thread->th.th_cpu == 1 || thread->th.th_cpu == 2) thread->th.th_cluster = CLUSTER_B;
    else thread->th.th_cluster = CLUSTER_A;
#else
    thread->th.th_cluster = CLUSTER_A;
#endif
    // add your tid and cluster to the scheduler
    thread->th.th_sched_pos = __kmp_scheduler_add_thread(thread->th.th_cluster, tid);
    
    #if DEBUG_PRINT_THREAD_INFO
    printf("TID = %d, GTID = %d, CPU = %u \n, CLUSTER = %d", tid, __kmp_get_gtid(), thread->th.th_cpu, thread->th.th_cluster);
    #endif

    thread->th.th_perf_init_flag = 1;

    #if !PERF_DISABLE
    thread->th.th_counters_active = 1;

    // Perf config structure
    struct perf_event_attr pe;
    
    //TODO test if this is necessary
    memset(&pe, 0, sizeof(struct perf_event_attr));

    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(struct perf_event_attr);
    pe.disabled = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;

    // Systemcalls to open perf counters 
    #if DEBUG_PRINT_TASK_INFO
    pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    thread->th.th_counter_instructions = perf_event_open(&pe, 0, thread->th.th_cpu, -1, 0);
    if(thread->th.th_counter_instructions < 0) printf("Failed instructions\n");
    #endif

    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    thread->th.th_counter_cycles = perf_event_open(&pe, 0, thread->th.th_cpu, -1, 0);
    if(thread->th.th_counter_cycles < 0) printf("Failed cycles\n");

    pe.config = PERF_COUNT_HW_CACHE_MISSES;
    thread->th.th_counter_cachemiss = perf_event_open(&pe, 0, thread->th.th_cpu, -1, 0);
    if(thread->th.th_counter_cachemiss < 0) printf("Failed cache misses\n");
    
    #endif

    return;
}



//ME2



// __kmp_execute_tasks_template: Choose and execute tasks until either the
// condition is statisfied (return true) or there are none left (return false).
//
// final_spin is TRUE if this is the spin at the release barrier.
// thread_finished indicates whether the thread is finished executing all
// the tasks it has on its deque, and is at the release barrier.
// spinner is the location on which to spin.
// spinner == NULL means only execute a single task and return.
// checker is the value to check to terminate the spin.
template <class C>
static inline int __kmp_execute_tasks_template(
    kmp_info_t *thread, kmp_int32 gtid, C *flag, int final_spin,
    int *thread_finished USE_ITT_BUILD_ARG(void *itt_sync_obj),
    kmp_int32 is_constrained) {
  kmp_task_team_t *task_team = thread->th.th_task_team;
  kmp_thread_data_t *threads_data;
  kmp_task_t *task;
  kmp_info_t *other_thread;
  kmp_taskdata_t *current_task = thread->th.th_current_task;
  std::atomic<kmp_int32> *unfinished_threads;
  kmp_int32 nthreads, victim_tid = -2, use_own_tasks = 1, new_victim = 0,
                      tid = thread->th.th_info.ds.ds_tid;

  //ME1
  // Locks and mutex for the exponential sleep, used to let threads wake up other threads
  std::condition_variable cv;
  std::mutex cv_m;
  //ME2


  KMP_DEBUG_ASSERT(__kmp_tasking_mode != tskm_immediate_exec);
  KMP_DEBUG_ASSERT(thread == __kmp_threads[gtid]);

  if (task_team == NULL || current_task == NULL)
    return FALSE;

  KA_TRACE(15, ("__kmp_execute_tasks_template(enter): T#%d final_spin=%d "
                "*thread_finished=%d\n",
                gtid, final_spin, *thread_finished));

  thread->th.th_reap_state = KMP_NOT_SAFE_TO_REAP;
  threads_data = (kmp_thread_data_t *)TCR_PTR(task_team->tt.tt_threads_data);

  KMP_DEBUG_ASSERT(threads_data != NULL);

  nthreads = task_team->tt.tt_nproc;
  unfinished_threads = &(task_team->tt.tt_unfinished_threads);
  KMP_DEBUG_ASSERT(nthreads > 1 || task_team->tt.tt_found_proxy_tasks ||
                   task_team->tt.tt_hidden_helper_task_encountered);
  KMP_DEBUG_ASSERT(*unfinished_threads >= 0);

  //ME1
  // Init function call, only called once,
  // TODO Probably a stupid place to have it, find a better
  if (thread->th.th_perf_init_flag == 0) __kmp_perf_init_counters(thread);
  //ME2

  while (1) { // Outer loop keeps trying to find tasks in case of single thread
    // getting tasks from target constructs
    while (1) { // Inner loop to find a task and execute it
      task = NULL;
      if (use_own_tasks) { // check on own queue first
        task = __kmp_remove_my_task(thread, gtid, task_team, is_constrained);
      }
      if ((task == NULL) && (nthreads > 1)) { // Steal a task
        int asleep = 1;
        use_own_tasks = 0;
        // Try to steal from the last place I stole from successfully.
        if (victim_tid == -2) { // haven't stolen anything yet
          victim_tid = threads_data[tid].td.td_deque_last_stolen;
          if (victim_tid !=
              -1) // if we have a last stolen from victim, get the thread
            other_thread = threads_data[victim_tid].td.td_thr;
        }
        if (victim_tid != -1) { // found last victim
          asleep = 0;
        } else if (!new_victim) { // no recent steals and we haven't already
          // used a new victim; select a random thread
          do { // Find a different thread to steal work from.
            // Pick a random thread. Initial plan was to cycle through all the
            // threads, and only return if we tried to steal from every thread,
            // and failed.  Arch says that's not such a great idea.
            victim_tid = __kmp_get_random(thread) % (nthreads - 1);
            if (victim_tid >= tid) {
              ++victim_tid; // Adjusts random distribution to exclude self
            }
            // Found a potential victim
            other_thread = threads_data[victim_tid].td.td_thr;
            // There is a slight chance that __kmp_enable_tasking() did not wake
            // up all threads waiting at the barrier.  If victim is sleeping,
            // then wake it up. Since we were going to pay the cache miss
            // penalty for referencing another thread's kmp_info_t struct
            // anyway,
            // the check shouldn't cost too much performance at this point. In
            // extra barrier mode, tasks do not sleep at the separate tasking
            // barrier, so this isn't a problem.
            asleep = 0;
            if ((__kmp_tasking_mode == tskm_task_teams) &&
                (__kmp_dflt_blocktime != KMP_MAX_BLOCKTIME) &&
                (TCR_PTR(CCAST(void *, other_thread->th.th_sleep_loc)) !=
                 NULL)) {
              asleep = 1;
              __kmp_null_resume_wrapper(other_thread);
              // A sleeping thread should not have any tasks on it's queue.
              // There is a slight possibility that it resumes, steals a task
              // from another thread, which spawns more tasks, all in the time
              // that it takes this thread to check => don't write an assertion
              // that the victim's queue is empty.  Try stealing from a
              // different thread.
            }
          } while (asleep);
        }

        if (!asleep) {
          // We have a victim to try to steal from
          task = __kmp_steal_task(other_thread, gtid, task_team,
                                  unfinished_threads, thread_finished,
                                  is_constrained);
        }
        if (task != NULL) { // set last stolen to victim
          if (threads_data[tid].td.td_deque_last_stolen != victim_tid) {
            threads_data[tid].td.td_deque_last_stolen = victim_tid;
            // The pre-refactored code did not try more than 1 successful new
            // vicitm, unless the last one generated more local tasks;
            // new_victim keeps track of this
            new_victim = 1;
          }
        } else { // No tasks found; unset last_stolen
          KMP_CHECK_UPDATE(threads_data[tid].td.td_deque_last_stolen, -1);
          victim_tid = -2; // no successful victim found
        }
      }
      #if !SLEEP_DISABLED
      //ME1
      //No task was stolen
      if (task == NULL) {
          ++thread->th.th_steal_attempts;
          if (thread->th.th_steal_attempts == MAX_STEAL_ATTEMPTS) {
              //sleep
              // notify the scheduler that you are sleeping
              __kmp_thread_active_status(thread->th.th_cluster, thread->th.th_sched_pos, THREAD_SLEEP);
              std::unique_lock <std::mutex> lk(cv_m);
              //printf("Thread %d going to sleep for %d ms\n", __kmp_tid_from_gtid(gtid),
              //       (1 << thread->th.th_sleep_shift));
              if (thread->th.th_cv.wait_for(lk, (1 << thread->th.th_sleep_shift) * SLEEP_DURATION) == std::cv_status::timeout) {
                  //Slept the entire specified duration
                  ++thread->th.th_sleep_shift;
                  if (thread->th.th_sleep_shift > MAX_SLEEP_SHIFT) {
                      thread->th.th_sleep_shift = MAX_SLEEP_SHIFT;
                  }
              } else {
                  //Woken up by another thread
#if DEBUG_PRINT_THREAD_INFO
                  printf("Thread %d woken up\n", __kmp_tid_from_gtid(gtid));
#endif
                  thread->th.th_sleep_shift = 0;
                  task = __kmp_remove_my_task(thread, gtid, task_team, is_constrained);
              }
              // notify the scheduler that you are awake
              __kmp_thread_active_status(thread->th.th_cluster, thread->th.th_sched_pos, THREAD_AWAKE);
              thread->th.th_steal_attempts = 0;
          }
      }else{
          thread->th.th_steal_attempts = 0;
          thread->th.th_sleep_shift = 0;
      }
      //ME2
      #endif
      if (task == NULL)
        break; // break out of tasking loop

// Found a task; execute it
#if USE_ITT_BUILD && USE_ITT_NOTIFY
      if (__itt_sync_create_ptr || KMP_ITT_DEBUG) {
        if (itt_sync_obj == NULL) { // we are at fork barrier where we could not
          // get the object reliably
          itt_sync_obj = __kmp_itt_barrier_object(gtid, bs_forkjoin_barrier);
        }
        __kmp_itt_task_starting(itt_sync_obj);
      }
#endif /* USE_ITT_BUILD && USE_ITT_NOTIFY */
    //ME1
    //Successful steal, reset both steal attempts and sleep time

    //ME2
      __kmp_invoke_task(gtid, task, current_task);
#if USE_ITT_BUILD
      if (itt_sync_obj != NULL)
        __kmp_itt_task_finished(itt_sync_obj);
#endif /* USE_ITT_BUILD */
      // If this thread is only partway through the barrier and the condition is
      // met, then return now, so that the barrier gather/release pattern can
      // proceed. If this thread is in the last spin loop in the barrier,
      // waiting to be released, we know that the termination condition will not
      // be satisfied, so don't waste any cycles checking it.
      if (flag == NULL || (!final_spin && flag->done_check())) {
        KA_TRACE(
            15,
            ("__kmp_execute_tasks_template: T#%d spin condition satisfied\n",
             gtid));
        return TRUE;
      }
      if (thread->th.th_task_team == NULL) {
        break;
      }
      KMP_YIELD(__kmp_library == library_throughput); // Yield before next task
      // If execution of a stolen task results in more tasks being placed on our
      // run queue, reset use_own_tasks
      if (!use_own_tasks && TCR_4(threads_data[tid].td.td_deque_ntasks) != 0) {
        KA_TRACE(20, ("__kmp_execute_tasks_template: T#%d stolen task spawned "
                      "other tasks, restart\n",
                      gtid));
        use_own_tasks = 1;
        new_victim = 0;
      }
    }

    // The task source has been exhausted. If in final spin loop of barrier,
    // check if termination condition is satisfied. The work queue may be empty
    // but there might be proxy tasks still executing.
    if (final_spin &&
        KMP_ATOMIC_LD_ACQ(&current_task->td_incomplete_child_tasks) == 0) {
      // First, decrement the #unfinished threads, if that has not already been
      // done.  This decrement might be to the spin location, and result in the
      // termination condition being satisfied.
      if (!*thread_finished) {
#if KMP_DEBUG
        kmp_int32 count = -1 +
#endif
            KMP_ATOMIC_DEC(unfinished_threads);
        KA_TRACE(20, ("__kmp_execute_tasks_template: T#%d dec "
                      "unfinished_threads to %d task_team=%p\n",
                      gtid, count, task_team));
        *thread_finished = TRUE;
      }

      // It is now unsafe to reference thread->th.th_team !!!
      // Decrementing task_team->tt.tt_unfinished_threads can allow the primary
      // thread to pass through the barrier, where it might reset each thread's
      // th.th_team field for the next parallel region. If we can steal more
      // work, we know that this has not happened yet.
      if (flag != NULL && flag->done_check()) {
        KA_TRACE(
            15,
            ("__kmp_execute_tasks_template: T#%d spin condition satisfied\n",
             gtid));
        return TRUE;
      }
    }

    // If this thread's task team is NULL, primary thread has recognized that
    // there are no more tasks; bail out
    if (thread->th.th_task_team == NULL) {
      KA_TRACE(15,
               ("__kmp_execute_tasks_template: T#%d no more tasks\n", gtid));
      return FALSE;
    }

    // Check the flag again to see if it has already done in case to be trapped
    // into infinite loop when a if0 task depends on a hidden helper task
    // outside any parallel region. Detached tasks are not impacted in this case
    // because the only thread executing this function has to execute the proxy
    // task so it is in another code path that has the same check.
    if (flag == NULL || (!final_spin && flag->done_check())) {
      KA_TRACE(15,
               ("__kmp_execute_tasks_template: T#%d spin condition satisfied\n",
                gtid));
      return TRUE;
    }

    // We could be getting tasks from target constructs; if this is the only
    // thread, keep trying to execute tasks from own queue
    if (nthreads == 1 &&
        KMP_ATOMIC_LD_ACQ(&current_task->td_incomplete_child_tasks))
      use_own_tasks = 1;
    else {
      KA_TRACE(15,
               ("__kmp_execute_tasks_template: T#%d can't find work\n", gtid));
      return FALSE;
    }
  }
}

template <bool C, bool S>
int __kmp_execute_tasks_32(
    kmp_info_t *thread, kmp_int32 gtid, kmp_flag_32<C, S> *flag, int final_spin,
    int *thread_finished USE_ITT_BUILD_ARG(void *itt_sync_obj),
    kmp_int32 is_constrained) {
  return __kmp_execute_tasks_template(
      thread, gtid, flag, final_spin,
      thread_finished USE_ITT_BUILD_ARG(itt_sync_obj), is_constrained);
}

template <bool C, bool S>
int __kmp_execute_tasks_64(
    kmp_info_t *thread, kmp_int32 gtid, kmp_flag_64<C, S> *flag, int final_spin,
    int *thread_finished USE_ITT_BUILD_ARG(void *itt_sync_obj),
    kmp_int32 is_constrained) {
  return __kmp_execute_tasks_template(
      thread, gtid, flag, final_spin,
      thread_finished USE_ITT_BUILD_ARG(itt_sync_obj), is_constrained);
}

template <bool C, bool S>
int __kmp_atomic_execute_tasks_64(
    kmp_info_t *thread, kmp_int32 gtid, kmp_atomic_flag_64<C, S> *flag,
    int final_spin, int *thread_finished USE_ITT_BUILD_ARG(void *itt_sync_obj),
    kmp_int32 is_constrained) {
  return __kmp_execute_tasks_template(
      thread, gtid, flag, final_spin,
      thread_finished USE_ITT_BUILD_ARG(itt_sync_obj), is_constrained);
}

int __kmp_execute_tasks_oncore(
    kmp_info_t *thread, kmp_int32 gtid, kmp_flag_oncore *flag, int final_spin,
    int *thread_finished USE_ITT_BUILD_ARG(void *itt_sync_obj),
    kmp_int32 is_constrained) {
  return __kmp_execute_tasks_template(
      thread, gtid, flag, final_spin,
      thread_finished USE_ITT_BUILD_ARG(itt_sync_obj), is_constrained);
}

template int
__kmp_execute_tasks_32<false, false>(kmp_info_t *, kmp_int32,
                                     kmp_flag_32<false, false> *, int,
                                     int *USE_ITT_BUILD_ARG(void *), kmp_int32);

template int __kmp_execute_tasks_64<false, true>(kmp_info_t *, kmp_int32,
                                                 kmp_flag_64<false, true> *,
                                                 int,
                                                 int *USE_ITT_BUILD_ARG(void *),
                                                 kmp_int32);

template int __kmp_execute_tasks_64<true, false>(kmp_info_t *, kmp_int32,
                                                 kmp_flag_64<true, false> *,
                                                 int,
                                                 int *USE_ITT_BUILD_ARG(void *),
                                                 kmp_int32);

template int __kmp_atomic_execute_tasks_64<false, true>(
    kmp_info_t *, kmp_int32, kmp_atomic_flag_64<false, true> *, int,
    int *USE_ITT_BUILD_ARG(void *), kmp_int32);

template int __kmp_atomic_execute_tasks_64<true, false>(
    kmp_info_t *, kmp_int32, kmp_atomic_flag_64<true, false> *, int,
    int *USE_ITT_BUILD_ARG(void *), kmp_int32);

// __kmp_enable_tasking: Allocate task team and resume threads sleeping at the
// next barrier so they can assist in executing enqueued tasks.
// First thread in allocates the task team atomically.
static void __kmp_enable_tasking(kmp_task_team_t *task_team,
                                 kmp_info_t *this_thr) {
  kmp_thread_data_t *threads_data;
  int nthreads, i, is_init_thread;

  KA_TRACE(10, ("__kmp_enable_tasking(enter): T#%d\n",
                __kmp_gtid_from_thread(this_thr)));

  KMP_DEBUG_ASSERT(task_team != NULL);
  KMP_DEBUG_ASSERT(this_thr->th.th_team != NULL);

  nthreads = task_team->tt.tt_nproc;
  KMP_DEBUG_ASSERT(nthreads > 0);
  KMP_DEBUG_ASSERT(nthreads == this_thr->th.th_team->t.t_nproc);

  // Allocate or increase the size of threads_data if necessary
  is_init_thread = __kmp_realloc_task_threads_data(this_thr, task_team);

  if (!is_init_thread) {
    // Some other thread already set up the array.
    KA_TRACE(
        20,
        ("__kmp_enable_tasking(exit): T#%d: threads array already set up.\n",
         __kmp_gtid_from_thread(this_thr)));
    return;
  }
  threads_data = (kmp_thread_data_t *)TCR_PTR(task_team->tt.tt_threads_data);
  KMP_DEBUG_ASSERT(threads_data != NULL);

  if (__kmp_tasking_mode == tskm_task_teams &&
      (__kmp_dflt_blocktime != KMP_MAX_BLOCKTIME)) {
    // Release any threads sleeping at the barrier, so that they can steal
    // tasks and execute them.  In extra barrier mode, tasks do not sleep
    // at the separate tasking barrier, so this isn't a problem.
    for (i = 0; i < nthreads; i++) {
      void *sleep_loc;
      kmp_info_t *thread = threads_data[i].td.td_thr;

      if (i == this_thr->th.th_info.ds.ds_tid) {
        continue;
      }
      // Since we haven't locked the thread's suspend mutex lock at this
      // point, there is a small window where a thread might be putting
      // itself to sleep, but hasn't set the th_sleep_loc field yet.
      // To work around this, __kmp_execute_tasks_template() periodically checks
      // see if other threads are sleeping (using the same random mechanism that
      // is used for task stealing) and awakens them if they are.
      if ((sleep_loc = TCR_PTR(CCAST(void *, thread->th.th_sleep_loc))) !=
          NULL) {
        KF_TRACE(50, ("__kmp_enable_tasking: T#%d waking up thread T#%d\n",
                      __kmp_gtid_from_thread(this_thr),
                      __kmp_gtid_from_thread(thread)));
        __kmp_null_resume_wrapper(thread);
      } else {
        KF_TRACE(50, ("__kmp_enable_tasking: T#%d don't wake up thread T#%d\n",
                      __kmp_gtid_from_thread(this_thr),
                      __kmp_gtid_from_thread(thread)));
      }
    }
  }

  KA_TRACE(10, ("__kmp_enable_tasking(exit): T#%d\n",
                __kmp_gtid_from_thread(this_thr)));
}

/* // TODO: Check the comment consistency
 * Utility routines for "task teams".  A task team (kmp_task_t) is kind of
 * like a shadow of the kmp_team_t data struct, with a different lifetime.
 * After a child * thread checks into a barrier and calls __kmp_release() from
 * the particular variant of __kmp_<barrier_kind>_barrier_gather(), it can no
 * longer assume that the kmp_team_t structure is intact (at any moment, the
 * primary thread may exit the barrier code and free the team data structure,
 * and return the threads to the thread pool).
 *
 * This does not work with the tasking code, as the thread is still
 * expected to participate in the execution of any tasks that may have been
 * spawned my a member of the team, and the thread still needs access to all
 * to each thread in the team, so that it can steal work from it.
 *
 * Enter the existence of the kmp_task_team_t struct.  It employs a reference
 * counting mechanism, and is allocated by the primary thread before calling
 * __kmp_<barrier_kind>_release, and then is release by the last thread to
 * exit __kmp_<barrier_kind>_release at the next barrier.  I.e. the lifetimes
 * of the kmp_task_team_t structs for consecutive barriers can overlap
 * (and will, unless the primary thread is the last thread to exit the barrier
 * release phase, which is not typical). The existence of such a struct is
 * useful outside the context of tasking.
 *
 * We currently use the existence of the threads array as an indicator that
 * tasks were spawned since the last barrier.  If the structure is to be
 * useful outside the context of tasking, then this will have to change, but
 * not setting the field minimizes the performance impact of tasking on
 * barriers, when no explicit tasks were spawned (pushed, actually).
 */

static kmp_task_team_t *__kmp_free_task_teams =
    NULL; // Free list for task_team data structures
// Lock for task team data structures
kmp_bootstrap_lock_t __kmp_task_team_lock =
    KMP_BOOTSTRAP_LOCK_INITIALIZER(__kmp_task_team_lock);

// __kmp_alloc_task_deque:
// Allocates a task deque for a particular thread, and initialize the necessary
// data structures relating to the deque.  This only happens once per thread
// per task team since task teams are recycled. No lock is needed during
// allocation since each thread allocates its own deque.
static void __kmp_alloc_task_deque(kmp_info_t *thread,
                                   kmp_thread_data_t *thread_data) {
  __kmp_init_bootstrap_lock(&thread_data->td.td_deque_lock);
  KMP_DEBUG_ASSERT(thread_data->td.td_deque == NULL);

  // Initialize last stolen task field to "none"
  thread_data->td.td_deque_last_stolen = -1;

  KMP_DEBUG_ASSERT(TCR_4(thread_data->td.td_deque_ntasks) == 0);
  KMP_DEBUG_ASSERT(thread_data->td.td_deque_head == 0);
  KMP_DEBUG_ASSERT(thread_data->td.td_deque_tail == 0);

  KE_TRACE(
      10,
      ("__kmp_alloc_task_deque: T#%d allocating deque[%d] for thread_data %p\n",
       __kmp_gtid_from_thread(thread), INITIAL_TASK_DEQUE_SIZE, thread_data));
  // Allocate space for task deque, and zero the deque
  // Cannot use __kmp_thread_calloc() because threads not around for
  // kmp_reap_task_team( ).
  thread_data->td.td_deque = (kmp_taskdata_t **)__kmp_allocate(
      INITIAL_TASK_DEQUE_SIZE * sizeof(kmp_taskdata_t *));
  thread_data->td.td_deque_size = INITIAL_TASK_DEQUE_SIZE;
}

// __kmp_free_task_deque:
// Deallocates a task deque for a particular thread. Happens at library
// deallocation so don't need to reset all thread data fields.
static void __kmp_free_task_deque(kmp_thread_data_t *thread_data) {
  if (thread_data->td.td_deque != NULL) {
    __kmp_acquire_bootstrap_lock(&thread_data->td.td_deque_lock);
    TCW_4(thread_data->td.td_deque_ntasks, 0);
    __kmp_free(thread_data->td.td_deque);
    thread_data->td.td_deque = NULL;
    __kmp_release_bootstrap_lock(&thread_data->td.td_deque_lock);
  }

#ifdef BUILD_TIED_TASK_STACK
  // GEH: Figure out what to do here for td_susp_tied_tasks
  if (thread_data->td.td_susp_tied_tasks.ts_entries != TASK_STACK_EMPTY) {
    __kmp_free_task_stack(__kmp_thread_from_gtid(gtid), thread_data);
  }
#endif // BUILD_TIED_TASK_STACK
}

// __kmp_realloc_task_threads_data:
// Allocates a threads_data array for a task team, either by allocating an
// initial array or enlarging an existing array.  Only the first thread to get
// the lock allocs or enlarges the array and re-initializes the array elements.
// That thread returns "TRUE", the rest return "FALSE".
// Assumes that the new array size is given by task_team -> tt.tt_nproc.
// The current size is given by task_team -> tt.tt_max_threads.
static int __kmp_realloc_task_threads_data(kmp_info_t *thread,
                                           kmp_task_team_t *task_team) {
  kmp_thread_data_t **threads_data_p;
  kmp_int32 nthreads, maxthreads;
  int is_init_thread = FALSE;

  if (TCR_4(task_team->tt.tt_found_tasks)) {
    // Already reallocated and initialized.
    return FALSE;
  }

  threads_data_p = &task_team->tt.tt_threads_data;
  nthreads = task_team->tt.tt_nproc;
  maxthreads = task_team->tt.tt_max_threads;

  // All threads must lock when they encounter the first task of the implicit
  // task region to make sure threads_data fields are (re)initialized before
  // used.
  __kmp_acquire_bootstrap_lock(&task_team->tt.tt_threads_lock);

  if (!TCR_4(task_team->tt.tt_found_tasks)) {
    // first thread to enable tasking
    kmp_team_t *team = thread->th.th_team;
    int i;

    is_init_thread = TRUE;
    if (maxthreads < nthreads) {

      if (*threads_data_p != NULL) {
        kmp_thread_data_t *old_data = *threads_data_p;
        kmp_thread_data_t *new_data = NULL;

        KE_TRACE(
            10,
            ("__kmp_realloc_task_threads_data: T#%d reallocating "
             "threads data for task_team %p, new_size = %d, old_size = %d\n",
             __kmp_gtid_from_thread(thread), task_team, nthreads, maxthreads));
        // Reallocate threads_data to have more elements than current array
        // Cannot use __kmp_thread_realloc() because threads not around for
        // kmp_reap_task_team( ).  Note all new array entries are initialized
        // to zero by __kmp_allocate().
        new_data = (kmp_thread_data_t *)__kmp_allocate(
            nthreads * sizeof(kmp_thread_data_t));
        // copy old data to new data
        KMP_MEMCPY_S((void *)new_data, nthreads * sizeof(kmp_thread_data_t),
                     (void *)old_data, maxthreads * sizeof(kmp_thread_data_t));

#ifdef BUILD_TIED_TASK_STACK
        // GEH: Figure out if this is the right thing to do
        for (i = maxthreads; i < nthreads; i++) {
          kmp_thread_data_t *thread_data = &(*threads_data_p)[i];
          __kmp_init_task_stack(__kmp_gtid_from_thread(thread), thread_data);
        }
#endif // BUILD_TIED_TASK_STACK
       // Install the new data and free the old data
        (*threads_data_p) = new_data;
        __kmp_free(old_data);
      } else {
        KE_TRACE(10, ("__kmp_realloc_task_threads_data: T#%d allocating "
                      "threads data for task_team %p, size = %d\n",
                      __kmp_gtid_from_thread(thread), task_team, nthreads));
        // Make the initial allocate for threads_data array, and zero entries
        // Cannot use __kmp_thread_calloc() because threads not around for
        // kmp_reap_task_team( ).
        *threads_data_p = (kmp_thread_data_t *)__kmp_allocate(
            nthreads * sizeof(kmp_thread_data_t));
#ifdef BUILD_TIED_TASK_STACK
        // GEH: Figure out if this is the right thing to do
        for (i = 0; i < nthreads; i++) {
          kmp_thread_data_t *thread_data = &(*threads_data_p)[i];
          __kmp_init_task_stack(__kmp_gtid_from_thread(thread), thread_data);
        }
#endif // BUILD_TIED_TASK_STACK
      }
      task_team->tt.tt_max_threads = nthreads;
    } else {
      // If array has (more than) enough elements, go ahead and use it
      KMP_DEBUG_ASSERT(*threads_data_p != NULL);
    }

    //ME1
    // Initialize ERASE scheduler
    __kmp_scheduler_init(thread);
    __kmp_performance_model_init();

    //ME2

    // initialize threads_data pointers back to thread_info structures
    for (i = 0; i < nthreads; i++) {
      kmp_thread_data_t *thread_data = &(*threads_data_p)[i];
      thread_data->td.td_thr = team->t.t_threads[i];


      //ME1
      kmp_info_t *thread = __kmp_threads[i];

#if DEBUG_PRINT_THREAD_INFO
      kmp_team_t *team = __kmp_get_team();
      int gtid_test = __kmp_gtid_from_tid(i, team);
      printf("gtid = %d, i=%d\n", gtid_test,i);
#endif
      if (thread_data->td.td_deque == NULL) {
        __kmp_alloc_task_deque(thread, thread_data);
      }
      //ME2

      if (thread_data->td.td_deque_last_stolen >= nthreads) {
        // The last stolen field survives across teams / barrier, and the number
        // of threads may have changed.  It's possible (likely?) that a new
        // parallel region will exhibit the same behavior as previous region.
        thread_data->td.td_deque_last_stolen = -1;
      }
    }

    KMP_MB();
    TCW_SYNC_4(task_team->tt.tt_found_tasks, TRUE);
  }

  __kmp_release_bootstrap_lock(&task_team->tt.tt_threads_lock);
  return is_init_thread;
}

// __kmp_free_task_threads_data:
// Deallocates a threads_data array for a task team, including any attached
// tasking deques.  Only occurs at library shutdown.
static void __kmp_free_task_threads_data(kmp_task_team_t *task_team) {
  __kmp_acquire_bootstrap_lock(&task_team->tt.tt_threads_lock);
  if (task_team->tt.tt_threads_data != NULL) {
    int i;
    for (i = 0; i < task_team->tt.tt_max_threads; i++) {
      __kmp_free_task_deque(&task_team->tt.tt_threads_data[i]);
    }
    __kmp_free(task_team->tt.tt_threads_data);
    task_team->tt.tt_threads_data = NULL;
  }
  __kmp_release_bootstrap_lock(&task_team->tt.tt_threads_lock);
}

// __kmp_allocate_task_team:
// Allocates a task team associated with a specific team, taking it from
// the global task team free list if possible.  Also initializes data
// structures.
static kmp_task_team_t *__kmp_allocate_task_team(kmp_info_t *thread,
                                                 kmp_team_t *team) {
  kmp_task_team_t *task_team = NULL;
  int nthreads;

  KA_TRACE(20, ("__kmp_allocate_task_team: T#%d entering; team = %p\n",
                (thread ? __kmp_gtid_from_thread(thread) : -1), team));

  if (TCR_PTR(__kmp_free_task_teams) != NULL) {
    // Take a task team from the task team pool
    __kmp_acquire_bootstrap_lock(&__kmp_task_team_lock);
    if (__kmp_free_task_teams != NULL) {
      task_team = __kmp_free_task_teams;
      TCW_PTR(__kmp_free_task_teams, task_team->tt.tt_next);
      task_team->tt.tt_next = NULL;
    }
    __kmp_release_bootstrap_lock(&__kmp_task_team_lock);
  }

  if (task_team == NULL) {
    KE_TRACE(10, ("__kmp_allocate_task_team: T#%d allocating "
                  "task team for team %p\n",
                  __kmp_gtid_from_thread(thread), team));
    // Allocate a new task team if one is not available. Cannot use
    // __kmp_thread_malloc because threads not around for kmp_reap_task_team.
    task_team = (kmp_task_team_t *)__kmp_allocate(sizeof(kmp_task_team_t));
    __kmp_init_bootstrap_lock(&task_team->tt.tt_threads_lock);
#if USE_ITT_BUILD && USE_ITT_NOTIFY && KMP_DEBUG
    // suppress race conditions detection on synchronization flags in debug mode
    // this helps to analyze library internals eliminating false positives
    __itt_suppress_mark_range(
        __itt_suppress_range, __itt_suppress_threading_errors,
        &task_team->tt.tt_found_tasks, sizeof(task_team->tt.tt_found_tasks));
    __itt_suppress_mark_range(__itt_suppress_range,
                              __itt_suppress_threading_errors,
                              CCAST(kmp_uint32 *, &task_team->tt.tt_active),
                              sizeof(task_team->tt.tt_active));
#endif /* USE_ITT_BUILD && USE_ITT_NOTIFY && KMP_DEBUG */
    // Note: __kmp_allocate zeroes returned memory, othewise we would need:
    // task_team->tt.tt_threads_data = NULL;
    // task_team->tt.tt_max_threads = 0;
    // task_team->tt.tt_next = NULL;
  }

  TCW_4(task_team->tt.tt_found_tasks, FALSE);
  TCW_4(task_team->tt.tt_found_proxy_tasks, FALSE);
  TCW_4(task_team->tt.tt_hidden_helper_task_encountered, FALSE);
  task_team->tt.tt_nproc = nthreads = team->t.t_nproc;

  KMP_ATOMIC_ST_REL(&task_team->tt.tt_unfinished_threads, nthreads);
  TCW_4(task_team->tt.tt_hidden_helper_task_encountered, FALSE);
  TCW_4(task_team->tt.tt_active, TRUE);

  KA_TRACE(20, ("__kmp_allocate_task_team: T#%d exiting; task_team = %p "
                "unfinished_threads init'd to %d\n",
                (thread ? __kmp_gtid_from_thread(thread) : -1), task_team,
                KMP_ATOMIC_LD_RLX(&task_team->tt.tt_unfinished_threads)));
  return task_team;
}

// __kmp_free_task_team:
// Frees the task team associated with a specific thread, and adds it
// to the global task team free list.
void __kmp_free_task_team(kmp_info_t *thread, kmp_task_team_t *task_team) {
  KA_TRACE(20, ("__kmp_free_task_team: T#%d task_team = %p\n",
                thread ? __kmp_gtid_from_thread(thread) : -1, task_team));

  // Put task team back on free list
  __kmp_acquire_bootstrap_lock(&__kmp_task_team_lock);

  KMP_DEBUG_ASSERT(task_team->tt.tt_next == NULL);
  task_team->tt.tt_next = __kmp_free_task_teams;
  TCW_PTR(__kmp_free_task_teams, task_team);

  __kmp_release_bootstrap_lock(&__kmp_task_team_lock);
}

// __kmp_reap_task_teams:
// Free all the task teams on the task team free list.
// Should only be done during library shutdown.
// Cannot do anything that needs a thread structure or gtid since they are
// already gone.
void __kmp_reap_task_teams(void) {
  kmp_task_team_t *task_team;

  if (TCR_PTR(__kmp_free_task_teams) != NULL) {
    // Free all task_teams on the free list
    __kmp_acquire_bootstrap_lock(&__kmp_task_team_lock);
    while ((task_team = __kmp_free_task_teams) != NULL) {
      __kmp_free_task_teams = task_team->tt.tt_next;
      task_team->tt.tt_next = NULL;

      // Free threads_data if necessary
      if (task_team->tt.tt_threads_data != NULL) {
        __kmp_free_task_threads_data(task_team);
      }
      __kmp_free(task_team);
    }
    __kmp_release_bootstrap_lock(&__kmp_task_team_lock);
  }
}

// __kmp_wait_to_unref_task_teams:
// Some threads could still be in the fork barrier release code, possibly
// trying to steal tasks.  Wait for each thread to unreference its task team.
void __kmp_wait_to_unref_task_teams(void) {
  kmp_info_t *thread;
  kmp_uint32 spins;
  kmp_uint64 time;
  int done;

  KMP_INIT_YIELD(spins);
  KMP_INIT_BACKOFF(time);

  for (;;) {
    done = TRUE;

    // TODO: GEH - this may be is wrong because some sync would be necessary
    // in case threads are added to the pool during the traversal. Need to
    // verify that lock for thread pool is held when calling this routine.
    for (thread = CCAST(kmp_info_t *, __kmp_thread_pool); thread != NULL;
         thread = thread->th.th_next_pool) {
#if KMP_OS_WINDOWS
      DWORD exit_val;
#endif
      if (TCR_PTR(thread->th.th_task_team) == NULL) {
        KA_TRACE(10, ("__kmp_wait_to_unref_task_team: T#%d task_team == NULL\n",
                      __kmp_gtid_from_thread(thread)));
        continue;
      }
#if KMP_OS_WINDOWS
      // TODO: GEH - add this check for Linux* OS / OS X* as well?
      if (!__kmp_is_thread_alive(thread, &exit_val)) {
        thread->th.th_task_team = NULL;
        continue;
      }
#endif

      done = FALSE; // Because th_task_team pointer is not NULL for this thread

      KA_TRACE(10, ("__kmp_wait_to_unref_task_team: Waiting for T#%d to "
                    "unreference task_team\n",
                    __kmp_gtid_from_thread(thread)));

      if (__kmp_dflt_blocktime != KMP_MAX_BLOCKTIME) {
        void *sleep_loc;
        // If the thread is sleeping, awaken it.
        if ((sleep_loc = TCR_PTR(CCAST(void *, thread->th.th_sleep_loc))) !=
            NULL) {
          KA_TRACE(
              10,
              ("__kmp_wait_to_unref_task_team: T#%d waking up thread T#%d\n",
               __kmp_gtid_from_thread(thread), __kmp_gtid_from_thread(thread)));
          __kmp_null_resume_wrapper(thread);
        }
      }
    }
    if (done) {
      break;
    }

    // If oversubscribed or have waited a bit, yield.
    KMP_YIELD_OVERSUB_ELSE_SPIN(spins, time);
  }
}

// __kmp_task_team_setup:  Create a task_team for the current team, but use
// an already created, unused one if it already exists.
void __kmp_task_team_setup(kmp_info_t *this_thr, kmp_team_t *team, int always) {
  KMP_DEBUG_ASSERT(__kmp_tasking_mode != tskm_immediate_exec);

  // If this task_team hasn't been created yet, allocate it. It will be used in
  // the region after the next.
  // If it exists, it is the current task team and shouldn't be touched yet as
  // it may still be in use.
  if (team->t.t_task_team[this_thr->th.th_task_state] == NULL &&
      (always || team->t.t_nproc > 1)) {
    team->t.t_task_team[this_thr->th.th_task_state] =
        __kmp_allocate_task_team(this_thr, team);
    KA_TRACE(20, ("__kmp_task_team_setup: Primary T#%d created new task_team %p"
                  " for team %d at parity=%d\n",
                  __kmp_gtid_from_thread(this_thr),
                  team->t.t_task_team[this_thr->th.th_task_state], team->t.t_id,
                  this_thr->th.th_task_state));
  }

  // After threads exit the release, they will call sync, and then point to this
  // other task_team; make sure it is allocated and properly initialized. As
  // threads spin in the barrier release phase, they will continue to use the
  // previous task_team struct(above), until they receive the signal to stop
  // checking for tasks (they can't safely reference the kmp_team_t struct,
  // which could be reallocated by the primary thread). No task teams are formed
  // for serialized teams.
  if (team->t.t_nproc > 1) {
    int other_team = 1 - this_thr->th.th_task_state;
    KMP_DEBUG_ASSERT(other_team >= 0 && other_team < 2);
    if (team->t.t_task_team[other_team] == NULL) { // setup other team as well
      team->t.t_task_team[other_team] =
          __kmp_allocate_task_team(this_thr, team);
      KA_TRACE(20, ("__kmp_task_team_setup: Primary T#%d created second new "
                    "task_team %p for team %d at parity=%d\n",
                    __kmp_gtid_from_thread(this_thr),
                    team->t.t_task_team[other_team], team->t.t_id, other_team));
    } else { // Leave the old task team struct in place for the upcoming region;
      // adjust as needed
      kmp_task_team_t *task_team = team->t.t_task_team[other_team];
      if (!task_team->tt.tt_active ||
          team->t.t_nproc != task_team->tt.tt_nproc) {
        TCW_4(task_team->tt.tt_nproc, team->t.t_nproc);
        TCW_4(task_team->tt.tt_found_tasks, FALSE);
        TCW_4(task_team->tt.tt_found_proxy_tasks, FALSE);
        TCW_4(task_team->tt.tt_hidden_helper_task_encountered, FALSE);
        KMP_ATOMIC_ST_REL(&task_team->tt.tt_unfinished_threads,
                          team->t.t_nproc);
        TCW_4(task_team->tt.tt_active, TRUE);
      }
      // if team size has changed, the first thread to enable tasking will
      // realloc threads_data if necessary
      KA_TRACE(20, ("__kmp_task_team_setup: Primary T#%d reset next task_team "
                    "%p for team %d at parity=%d\n",
                    __kmp_gtid_from_thread(this_thr),
                    team->t.t_task_team[other_team], team->t.t_id, other_team));
    }
  }

  // For regular thread, task enabling should be called when the task is going
  // to be pushed to a dequeue. However, for the hidden helper thread, we need
  // it ahead of time so that some operations can be performed without race
  // condition.
  if (this_thr == __kmp_hidden_helper_main_thread) {
    for (int i = 0; i < 2; ++i) {
      kmp_task_team_t *task_team = team->t.t_task_team[i];
      if (KMP_TASKING_ENABLED(task_team)) {
        continue;
      }
      __kmp_enable_tasking(task_team, this_thr);
      for (int j = 0; j < task_team->tt.tt_nproc; ++j) {
        kmp_thread_data_t *thread_data = &task_team->tt.tt_threads_data[j];
        if (thread_data->td.td_deque == NULL) {
          __kmp_alloc_task_deque(__kmp_hidden_helper_threads[j], thread_data);
        }
      }
    }
  }
}

// __kmp_task_team_sync: Propagation of task team data from team to threads
// which happens just after the release phase of a team barrier.  This may be
// called by any thread, but only for teams with # threads > 1.
void __kmp_task_team_sync(kmp_info_t *this_thr, kmp_team_t *team) {
  KMP_DEBUG_ASSERT(__kmp_tasking_mode != tskm_immediate_exec);

  // Toggle the th_task_state field, to switch which task_team this thread
  // refers to
  this_thr->th.th_task_state = (kmp_uint8)(1 - this_thr->th.th_task_state);

  // It is now safe to propagate the task team pointer from the team struct to
  // the current thread.
  TCW_PTR(this_thr->th.th_task_team,
          team->t.t_task_team[this_thr->th.th_task_state]);
  KA_TRACE(20,
           ("__kmp_task_team_sync: Thread T#%d task team switched to task_team "
            "%p from Team #%d (parity=%d)\n",
            __kmp_gtid_from_thread(this_thr), this_thr->th.th_task_team,
            team->t.t_id, this_thr->th.th_task_state));
}

// __kmp_task_team_wait: Primary thread waits for outstanding tasks after the
// barrier gather phase. Only called by primary thread if #threads in team > 1
// or if proxy tasks were created.
//
// wait is a flag that defaults to 1 (see kmp.h), but waiting can be turned off
// by passing in 0 optionally as the last argument. When wait is zero, primary
// thread does not wait for unfinished_threads to reach 0.
void __kmp_task_team_wait(
    kmp_info_t *this_thr,
    kmp_team_t *team USE_ITT_BUILD_ARG(void *itt_sync_obj), int wait) {
  kmp_task_team_t *task_team = team->t.t_task_team[this_thr->th.th_task_state];

  KMP_DEBUG_ASSERT(__kmp_tasking_mode != tskm_immediate_exec);
  KMP_DEBUG_ASSERT(task_team == this_thr->th.th_task_team);

  if ((task_team != NULL) && KMP_TASKING_ENABLED(task_team)) {
    if (wait) {
      KA_TRACE(20, ("__kmp_task_team_wait: Primary T#%d waiting for all tasks "
                    "(for unfinished_threads to reach 0) on task_team = %p\n",
                    __kmp_gtid_from_thread(this_thr), task_team));
      // Worker threads may have dropped through to release phase, but could
      // still be executing tasks. Wait here for tasks to complete. To avoid
      // memory contention, only primary thread checks termination condition.
      kmp_flag_32<false, false> flag(
          RCAST(std::atomic<kmp_uint32> *,
                &task_team->tt.tt_unfinished_threads),
          0U);
      flag.wait(this_thr, TRUE USE_ITT_BUILD_ARG(itt_sync_obj));
    }
    // Deactivate the old task team, so that the worker threads will stop
    // referencing it while spinning.
    KA_TRACE(
        20,
        ("__kmp_task_team_wait: Primary T#%d deactivating task_team %p: "
         "setting active to false, setting local and team's pointer to NULL\n",
         __kmp_gtid_from_thread(this_thr), task_team));
    KMP_DEBUG_ASSERT(task_team->tt.tt_nproc > 1 ||
                     task_team->tt.tt_found_proxy_tasks == TRUE ||
                     task_team->tt.tt_hidden_helper_task_encountered == TRUE);
    TCW_SYNC_4(task_team->tt.tt_found_proxy_tasks, FALSE);
    TCW_SYNC_4(task_team->tt.tt_hidden_helper_task_encountered, FALSE);
    KMP_CHECK_UPDATE(task_team->tt.tt_untied_task_encountered, 0);
    TCW_SYNC_4(task_team->tt.tt_active, FALSE);
    KMP_MB();

    TCW_PTR(this_thr->th.th_task_team, NULL);
  }
}

// __kmp_tasking_barrier:
// This routine is called only when __kmp_tasking_mode == tskm_extra_barrier.
// Internal function to execute all tasks prior to a regular barrier or a join
// barrier. It is a full barrier itself, which unfortunately turns regular
// barriers into double barriers and join barriers into 1 1/2 barriers.
void __kmp_tasking_barrier(kmp_team_t *team, kmp_info_t *thread, int gtid) {
  std::atomic<kmp_uint32> *spin = RCAST(
      std::atomic<kmp_uint32> *,
      &team->t.t_task_team[thread->th.th_task_state]->tt.tt_unfinished_threads);
  int flag = FALSE;
  KMP_DEBUG_ASSERT(__kmp_tasking_mode == tskm_extra_barrier);

#if USE_ITT_BUILD
  KMP_FSYNC_SPIN_INIT(spin, NULL);
#endif /* USE_ITT_BUILD */
  kmp_flag_32<false, false> spin_flag(spin, 0U);
  while (!spin_flag.execute_tasks(thread, gtid, TRUE,
                                  &flag USE_ITT_BUILD_ARG(NULL), 0)) {
#if USE_ITT_BUILD
    // TODO: What about itt_sync_obj??
    KMP_FSYNC_SPIN_PREPARE(RCAST(void *, spin));
#endif /* USE_ITT_BUILD */

    if (TCR_4(__kmp_global.g.g_done)) {
      if (__kmp_global.g.g_abort)
        __kmp_abort_thread();
      break;
    }
    KMP_YIELD(TRUE);
  }
#if USE_ITT_BUILD
  KMP_FSYNC_SPIN_ACQUIRED(RCAST(void *, spin));
#endif /* USE_ITT_BUILD */
}

// __kmp_give_task puts a task into a given thread queue if:
//  - the queue for that thread was created
//  - there's space in that queue
// Because of this, __kmp_push_task needs to check if there's space after
// getting the lock
static bool __kmp_give_task(kmp_info_t *thread, kmp_int32 tid, kmp_task_t *task,
                            kmp_int32 pass) {
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  kmp_task_team_t *task_team = taskdata->td_task_team;

  KA_TRACE(20, ("__kmp_give_task: trying to give task %p to thread %d.\n",
                taskdata, tid));

  // If task_team is NULL something went really bad...
  KMP_DEBUG_ASSERT(task_team != NULL);

  bool result = false;
  kmp_thread_data_t *thread_data = &task_team->tt.tt_threads_data[tid];

  if (thread_data->td.td_deque == NULL) {
    // There's no queue in this thread, go find another one
    // We're guaranteed that at least one thread has a queue
    KA_TRACE(30,
             ("__kmp_give_task: thread %d has no queue while giving task %p.\n",
              tid, taskdata));
    return result;
  }

  if (TCR_4(thread_data->td.td_deque_ntasks) >=
      TASK_DEQUE_SIZE(thread_data->td)) {
    KA_TRACE(
        30,
        ("__kmp_give_task: queue is full while giving task %p to thread %d.\n",
         taskdata, tid));

    // if this deque is bigger than the pass ratio give a chance to another
    // thread
    if (TASK_DEQUE_SIZE(thread_data->td) / INITIAL_TASK_DEQUE_SIZE >= pass)
      return result;

    __kmp_acquire_bootstrap_lock(&thread_data->td.td_deque_lock);
    if (TCR_4(thread_data->td.td_deque_ntasks) >=
        TASK_DEQUE_SIZE(thread_data->td)) {
      // expand deque to push the task which is not allowed to execute
      __kmp_realloc_task_deque(thread, thread_data);
    }

  } else {

    __kmp_acquire_bootstrap_lock(&thread_data->td.td_deque_lock);

    if (TCR_4(thread_data->td.td_deque_ntasks) >=
        TASK_DEQUE_SIZE(thread_data->td)) {
      KA_TRACE(30, ("__kmp_give_task: queue is full while giving task %p to "
                    "thread %d.\n",
                    taskdata, tid));

      // if this deque is bigger than the pass ratio give a chance to another
      // thread
      if (TASK_DEQUE_SIZE(thread_data->td) / INITIAL_TASK_DEQUE_SIZE >= pass)
        goto release_and_exit;

      __kmp_realloc_task_deque(thread, thread_data);
    }
  }

  // lock is held here, and there is space in the deque

  thread_data->td.td_deque[thread_data->td.td_deque_tail] = taskdata;
  // Wrap index.
  thread_data->td.td_deque_tail =
      (thread_data->td.td_deque_tail + 1) & TASK_DEQUE_MASK(thread_data->td);
  TCW_4(thread_data->td.td_deque_ntasks,
        TCR_4(thread_data->td.td_deque_ntasks) + 1);

  result = true;
  KA_TRACE(30, ("__kmp_give_task: successfully gave task %p to thread %d.\n",
                taskdata, tid));

release_and_exit:
  __kmp_release_bootstrap_lock(&thread_data->td.td_deque_lock);

  return result;
}

//ME1
#if PERF_DYNAMIC_ON
// Enables and resets perf counters
static void __kmp_perf_enable(kmp_info_t *thread){

    ioctl(thread->th.th_counter_cycles, PERF_EVENT_IOC_RESET, 0);
    ioctl(thread->th.th_counter_cycles, PERF_EVENT_IOC_ENABLE, 0);

    #if DEBUG_PRINT_TASK_INFO
    ioctl(thread->th.th_counter_instructions, PERF_EVENT_IOC_RESET, 0);
    ioctl(thread->th.th_counter_instructions, PERF_EVENT_IOC_ENABLE, 0);
    #endif

    ioctl(thread->th.th_counter_cachemiss, PERF_EVENT_IOC_RESET, 0);
    ioctl(thread->th.th_counter_cachemiss, PERF_EVENT_IOC_ENABLE, 0);

    thread->th.th_counters_active = 1;
    return;
}

// Disables the perf counters
static void __kmp_perf_disable(kmp_info_t *thread){
    // 
    ioctl(thread->th.th_counter_cycles, PERF_EVENT_IOC_DISABLE, 0);
    ioctl(thread->th.th_counter_cachemiss, PERF_EVENT_IOC_DISABLE, 0);

    #if DEBUG_PRINT_TASK_INFO
    ioctl(thread->th.th_counter_instructions, PERF_EVENT_IOC_DISABLE, 0);
    #endif

    thread->th.th_counters_active = 0;
    return;
}
#endif

// Initialized the performance model struct
static void __kmp_performance_model_init(){
    std::lock_guard<std::mutex> lock(performance_model_m);
    for(int i = 0; i < CLUSTER_AMOUNT; i++){
        for (int j = 0; j < CLUSTER_SIZE; j++){
            //TODO currently not using frequency, may wanna remove this
            kmp_perf_p->frequencies[i][j] = 0;
        }
        for (int j = 0; j < TASK_TYPES; j++){
            for (int k = 0; k < CLUSTER_SIZE; k++) {
                kmp_perf_p->execution_times[i][j][k] = 0;
            }
        }
    }
}
/*
// Resets the performance model struct for given cluster
// To be used when the frequency for the cluster is changed
// Currently unused
static void __kmp_performance_model_reset(kmp_uint8 cluster){
    for (int i = 0; i < TASK_TYPES; i++){
        for (int j = 0; j < CLUSTER_SIZE; j++){
            kmp_perf_p->execution_times[cluster][i][j] = 0;
        }
    }
}
*/

// Add a new execution time sample for the performance model
// TODO if the width is more then one, we need to gather the other execution times also
static void __kmp_performance_model_add(kmp_uint8 cluster, kmp_uint8 tasktype, kmp_uint32 execution_time, kmp_uint8 width) {
    std::lock_guard<std::mutex> lock(performance_model_m);
    //Check if frequency differs with some error margin with current, if so reset the performance model...
    //TODO Removed frequency since counters are expensive, make assumption based on the execution time instead..
    /*
     if (freq != kmp_perf_p->frequencies[cluster]) {
        __kmp_performance_model_reset(cluster);
     }
     */
    int width_index = width - 1;
    //printf("Number of entries in cluster %d = %d, with width = %d\n", cluster, kmp_sched_p->cluster_tid_entries[cluster], width);
    if (kmp_sched_p->cluster_tid_entries[cluster] < width) return;
    // TODO fix width associated variables, for example if the width is 4, await 4 results before adding etc.

    /*
    printf("Adding performance model, cluster = %hhu, tasktype = %hhu, execution time = %u, width = %hhu\n"
           ,cluster, tasktype, execution_time, width);
    */

    // No previous record, just add the value
    if (kmp_perf_p->execution_times[cluster][tasktype][width_index] == 0){
        kmp_perf_p->execution_times[cluster][tasktype][width_index] = execution_time;
    }
    // Previous record exist, calculate a weighted value
    else{
        kmp_perf_p->execution_times[cluster][tasktype][width_index] =
                (4 * kmp_perf_p->execution_times[cluster][tasktype][width_index] + execution_time) / 5;
    }
}
#if (INTERPOLATION == POLYNOMIAL) || (INTERPOLATION == PMNF)
static double calculate_scale(const kmp_uint8 cluster, const kmp_uint8 tasktype, const int norm_values[]){
    int values = 0;
    double sum = 0, scale;
    for (int i = 0; i<CLUSTER_A_SIZE; i++){
        if (kmp_perf_p->execution_times[cluster][tasktype][i] == 0) continue;

        values++;
        sum += double(kmp_perf_p->execution_times[cluster][tasktype][i])/double(norm_values[i]);
    }
    if (values > 0)
        return scale = sum/values;
    else return 1;
}
#endif

// Get the execution time for given cluster and task type
static kmp_uint32 __kmp_performance_model_get(const kmp_uint8 cluster, const kmp_uint8 tasktype, const kmp_uint8 width){
#if (INTERPOLATION == POLYNOMIAL) || (INTERPOLATION == PMNF)
    if (kmp_perf_p->execution_times[cluster][tasktype][width - 1] != 0)
        return kmp_perf_p->execution_times[cluster][tasktype][width - 1];
    else{
        double exec_time = 0, scale = 1;
        if (tasktype == TASK_CPU){

#if INTERPOLATION == POLYNOMIAL
            int norm_values[] = {92,60,48,39,33,31,25,23};
#elif INTERPOLATION == PMNF
            int norm_values[] = {100,60,49,40,34,32,26,24};
#endif
            exec_time = cpu_interpolation(width);
            scale = calculate_scale(cluster, tasktype, norm_values);

        } else if (tasktype == TASK_MEMORY){
#if INTERPOLATION == POLYNOMIAL
            int norm_values[] = {88,66,55,51,48,46,46,42};
#elif INTERPOLATION == PMNF
            int norm_values[] = {100,66,56,51,49,47,46,42};
#endif
            exec_time = memory_interpolation(width);
            scale = calculate_scale(cluster, tasktype, norm_values);

        }else if (tasktype == TASK_CACHE){
#if INTERPOLATION == POLYNOMIAL
            int norm_values[] = {76,60,47,46,53,43,32,31};
#elif INTERPOLATION == PMNF
            int norm_values[] = {100,61,48,46,53,43,32,31};
#endif
            exec_time = cache_interpolation(width);
            scale = calculate_scale(cluster, tasktype, norm_values);
        }
        printf("Exec_time = %f\n", exec_time);

        printf("Interpolated value at %d = %d\n", width, int (exec_time * scale));
        return exec_time * scale;
    }
#else //end (INTERPOLATION == POLYNOMIAL) || (INTERPOLATION == PMNF)
    return kmp_perf_p->execution_times[cluster][tasktype][width - 1];
#endif
}

// Update activity status, status 1 = active, status 0 = sleeping
static void __kmp_thread_active_status(kmp_uint8 cluster, kmp_uint8 pos, kmp_uint8 status){
    kmp_sched_p->thread_active[cluster][pos] = status;
}

// Initialize the kmp_scheduler struct
static void __kmp_scheduler_init(kmp_info_t *thread){

    kmp_sched_p->idle_power[CLUSTER_A] = CLUSTER_A_POWER_IDLE;

#ifdef CLUSTER_B_ACTIVE
    kmp_sched_p->idle_power[CLUSTER_B] = CLUSTER_B_POWER_IDLE;
#endif
}

// Adds the calling threads cluster and tid to the scheduler
static kmp_uint8 __kmp_scheduler_add_thread(kmp_uint8 cluster, kmp_int32 tid){
    std::lock_guard<std::mutex> lock(scheduler_m); // shared construct, requires a lock
    kmp_uint8 cluster_counter = kmp_sched_p->cluster_tid_entries[cluster];
    kmp_sched_p->cluster_tids[cluster][cluster_counter] = tid; // Adds tid to the schedule struct
    ++kmp_sched_p->cluster_tid_entries[cluster]; // increment the cluster counter
    kmp_sched_p->thread_active[cluster][cluster_counter] = THREAD_SLEEP; // Set thread as ready for tasks
    return cluster_counter;
}

// returns the best thread candidate on given cluster
static kmp_uint8 __kmp_schedule_thread(kmp_task_t *task, kmp_info_t *thread, kmp_uint8 cluster, kmp_int32 tid){
    // We now have the optimal cluster
    // schedule task on optimal thread right now picks the first sleeping thread it finds
    // If unable to find a sleeping thread, schedule task on yourself
    for (int i = 0; i < kmp_sched_p->cluster_tid_entries[cluster]; i++){
        if (kmp_sched_p->thread_active[cluster][i] == THREAD_SLEEP){
            KMP_TASK_TO_TASKDATA(task)->td_no_steal = 1;
            return kmp_sched_p->cluster_tids[cluster][i]; // return first available thread
        }
    }
    // All threads in optimal cluster are busy.

    // You are in the optimal cluster, schedule on yourself
    if(cluster == thread->th.th_cluster) return tid;

    // You are not in the optimal cluster, schedule on first thread in the optimal cluster
    return kmp_sched_p->cluster_tids[cluster][0];
}

// TODO TEMP debug, fix and remove
#if DEBUG_PRINT_THREAD_INFO | DEBUG_PRINT_TASK_INFO | DEBUG_PRINT_TASKLOOP_PERFORMANCE_MODEL_INFO | DEBUG_PRINT_TASKLOOP_SPLIT_INFO | EXPORT_DATA
int unique_taskloop_id = 0;
#endif

// Task mapping algorithm for taskloop constructs
static void __kmp_taskloop_mapping(kmp_info_t *thread, kmp_task_t *task, kmp_int32 tid, kmp_uint64 max_split){
    //debug
#if DEBUG_PRINT_TASKLOOP_PERFORMANCE_MODEL_INFO
    //if (global_taskcounter){
        printf("Performance model update after %d tasks\nExecution times:  | CPU | CACHE | MEMORY\n",
               global_taskcounter);
        for (int cluster = 0; cluster < CLUSTER_AMOUNT; cluster++) {
            for (int width = 0; width < CLUSTER_SIZE; width++) {
                printf("cluster %d width %d | %d  | %d    | %d\n",
                       cluster, width+1,
                       kmp_perf_p->execution_times[cluster][TASK_CPU][width],
                       kmp_perf_p->execution_times[cluster][TASK_CACHE][width],
                       kmp_perf_p->execution_times[cluster][TASK_MEMORY][width]);
            }
        }
    //}
#endif

#if DEBUG_PRINT_POWER_VALUES
    static int test = 0;
    test++;
    if(test == 1){
      for(int i = 0; i < CLUSTER_AMOUNT; i++){
          for(int j = 0; j < TASK_TYPES; j++){
            for(int k = 0; k < CLUSTER_POWER_RUNTIME_SAMPLES; k++){
              for(int l = 0; l < CLUSTER_SIZE; l++){
                printf("cluster = %d, Tasktype = %d, Frequency = %d, Width = %d, Energy = %d\n", i, j, k, l+1, kmp_sched_p->runtime_power[i][j][k][l]);
              }
            }
          }
      }
    }
#endif

    kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
    kmp_uint8 optimal_cluster;
    kmp_uint8 optimal_cluster_width;
    kmp_uint32 minimum_energy = UINT_MAX;

#if INTERPOLATION == ONLINE
    alglib::spline1dinterpolant online_model;
    bool created_model;
#endif

    // Temporary task type variable, to prevent pattern task classification from change
    kmp_uint8 task_type;
    // Get and set task type classification
    if(__kmp_contains_def(task->routine)) task_type = __kmp_get_def(task->routine);
    else task_type = TASK_UNDEFINED;


    //TODO Debug remove
    #if DEBUG_PRINT_THREAD_INFO | DEBUG_PRINT_TASK_INFO | DEBUG_PRINT_TASKLOOP_PERFORMANCE_MODEL_INFO | DEBUG_PRINT_TASKLOOP_SPLIT_INFO | EXPORT_DATA
    ++unique_taskloop_id;
    taskdata->td_unique_tid = unique_taskloop_id;
    taskdata->td_task_id = unique_taskloop_id;
    #endif

    if (task_type != TASK_UNDEFINED) {
        // TODO For each execution place (cluster...) should be randomly selected
        for (kmp_int32 curr_cluster = 0; curr_cluster < kmp_sched_p->num_clusters; curr_cluster++) {

#if INTERPOLATION == ONLINE
            created_model = false;
#endif
            // Max allowed width for current task depending on cluster size and taskloop size
            kmp_uint8 cluster_width_max = (kmp_sched_p->cluster_tid_entries[curr_cluster]) < max_split ?
                                          kmp_sched_p->cluster_tid_entries[curr_cluster] : max_split;

            ++cluster_width_max;
            for(kmp_int32 cluster_width = 1; cluster_width < cluster_width_max; cluster_width++) {
                // the idle power consumption is shared between current cluster and the idle clusters.
                kmp_uint32 idle_power = kmp_sched_p->idle_power[curr_cluster];
                kmp_uint8 number_active_cores[CLUSTER_AMOUNT] = {0};

                for (kmp_int32 cluster = 0; cluster < kmp_sched_p->num_clusters; cluster++) {

                    // first check if all threads per cluster are executing tasks or are sleeping
                    for (kmp_int32 curr_thread = 0;
                         curr_thread < kmp_sched_p->cluster_tid_entries[cluster]; curr_thread++) {
                        if (kmp_sched_p->thread_active[cluster][curr_thread] == THREAD_AWAKE) {
                            number_active_cores[curr_cluster] += 1;
                        }
                    }
                    // Idle power from curr_cluster is already added
                    if (curr_cluster == cluster) continue;

                    // Get idle power consumption if all cores of an entire cluster are sleeping
                    // if one thread on other cluster is awake, ignore its idle power
                    idle_power += number_active_cores[curr_cluster] ? 0 : kmp_sched_p->idle_power[cluster];
                }
                // Get the number of active cores + width of task.
                // If the number is higher than the max value, set it to max
                number_active_cores[curr_cluster] =
                        number_active_cores[curr_cluster] + cluster_width > cluster_width_max ?
                        cluster_width_max : number_active_cores[curr_cluster] + cluster_width;

                // Resource occupation , scaled by 10 to avoid floats
                kmp_uint32 resource_occupation = (10*cluster_width) / number_active_cores[curr_cluster];

                // Idle power based on resource occupation
                idle_power = (idle_power * resource_occupation) / 10;

                // Get the running power consumption from the power profiler, depending on frequency etc
                // TODO currently frequency independent, create function that gets the freq and calculates...
                // Need the frequency for the performance model also
                kmp_uint32 total_power =
                        idle_power + kmp_sched_p->runtime_power[curr_cluster][task_type][HIGH_FREQ_POWER][cluster_width - 1];

                // Get the execution time from the performance model
                kmp_uint32 exec_time = __kmp_performance_model_get(curr_cluster,task_type, cluster_width);
#if INTERPOLATION == ONLINE
                if (exec_time <= 0) {
                    if (!created_model) {

                        std::vector<double> x_in, y_in;
                        alglib::real_1d_array x, y;

                        for (int i = 0; i < CLUSTER_A_SIZE; i++) {
                            if (kmp_perf_p->execution_times[curr_cluster][task_type][i] == 0) continue;

                            y_in.push_back(kmp_perf_p->execution_times[curr_cluster][task_type][i]);
                            x_in.push_back(i + 1);
                        }

                        if (x_in.size() >= 2) {
                            x.attach_to_ptr(x_in.size(), x_in.data());
                            y.attach_to_ptr(y_in.size(), y_in.data());

                            alglib::spline1dbuildlinear(x, y, online_model);
                            created_model = TRUE;
                            exec_time = int (spline1dcalc(online_model, cluster_width));
                        }
                    }else{
                        exec_time = int (spline1dcalc(online_model, cluster_width));
                    }
                    if (exec_time <= 0) exec_time = 0;
                }

#endif

                // Calculate the energy based on the execution time and idle + runtime power consumption
                // Depending on how we store the execution time we may need to change the scale of exec time here
                kmp_uint32 energy = exec_time * total_power;


                // If the energy is less than the current minimum, set this as the current minimum
                if (energy < minimum_energy) {
#if MEASURE_ACCURACY
                    taskdata->td_predicted_exectime = exec_time;
#endif
                    optimal_cluster = curr_cluster;
                    minimum_energy = energy;
                    optimal_cluster_width = cluster_width;
                }
            }
        }
    }
    else{
        // TODO If tasktype is unknown, select fastest? cluster for identification
        // TODO Change to max number of clusters...
        optimal_cluster = CLUSTER_B;
        optimal_cluster_width = 2;
    }
    //printf("optimal_cluster_width = %d\n", optimal_cluster_width);
    //TODO Move this out to its own function, should return the optimal cluster and width instead
    taskdata->td_taskwidth = optimal_cluster_width;
    taskdata->td_cluster = optimal_cluster;

#if TEST_DIFFERENT_WIDTH
    static int width = 1;
    static int width_it = 0;
    taskdata->td_taskwidth = width;
    width_it = width_it % ((__kmp_get_team_num_threads(__kmp_get_gtid())  * 100) - 1) + 1;
    width = (width_it / 100) + 1;
    //printf("Current width iteration = %d, Suggested width = %d\n", width_it, width);
#endif

#if EXPORT_DATA
    if(unique_taskloop_id == 1000){
        struct passwd *pw = getpwuid(getuid());
        std::string homedir = pw->pw_dir;
        std::string filename;
        switch(task_type){
            case(TASK_CPU):
                filename = "CPU.txt";
                break;
            case(TASK_CACHE):
                filename = "CACHE.txt";
                break;
            case (TASK_MEMORY):
                filename = "MEMORY.txt";
                break;
            default:
                return;
        }

        std::string full = homedir + std::string("/") + filename;
        std::cout << full << std::endl;
        std::ofstream myfile (full, std::ios::out | std::ios::app );
        if (myfile.is_open()){
            for (int width = 0; width < CLUSTER_SIZE; width++) {
                int data = kmp_perf_p->execution_times[0][task_type][width];
                myfile << data;
                myfile << " ";
                }
            myfile << "\n";
            myfile.close();
        }
        else printf("Unable to open file");
    }


#endif
}

static kmp_int32 __kmp_task_mapping(kmp_info_t *thread, kmp_task_t *task, kmp_int32 tid) {
    //debug
#if DEBUG_PRINT_TASK_PERFORMANCE_MODEL_INFO
    if (global_taskcounter % 10 == 0){
        printf("Performance model update after %d tasks\nExecution times : CPU | CACHE | MEMORY\n",
               global_taskcounter);
        for (int cluster = 0; cluster < CLUSTER_AMOUNT; cluster++) {
            printf("                | %d  | %d    | %d\n", kmp_perf_p->execution_times[cluster][TASK_CPU][0],
                   kmp_perf_p->execution_times[cluster][TASK_CACHE][0],
                   kmp_perf_p->execution_times[cluster][TASK_MEMORY][0]);
        }
    }
#endif
    kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
    kmp_uint8 optimal_cluster;
    kmp_uint32 minimum_energy = UINT_MAX;

    if (taskdata->td_task_type != TASK_UNDEFINED) {
        // TODO For each execution place (cluster...) should be randomly selected, don't think this is necessary?
        for (kmp_int32 curr_cluster = 0; curr_cluster < kmp_sched_p->num_clusters; curr_cluster++) {
            // first check if all threads per curr_cluster are executing tasks or are sleeping
            // Get idle power consumption, if cores of an entire cluster are sleeping
            kmp_uint32 idle_power = kmp_sched_p->idle_power[curr_cluster];
            for (kmp_int32 cluster = 0; cluster < kmp_sched_p->num_clusters;cluster++) {
                if (curr_cluster == cluster) continue;

                // the idle power consumption is shared between current cluster and the idle clusters.
                kmp_uint8 cluster_awake = 0;
                for (kmp_int32 curr_thread = 0; curr_thread < kmp_sched_p->cluster_tid_entries[cluster]; curr_thread++) {
                    if (kmp_sched_p->thread_active[cluster][curr_thread] == THREAD_AWAKE) {
                        cluster_awake = 1;
                        break;
                    }
                }
                // if one thread on other cluster is awake, ignore its idle power
                idle_power += cluster_awake ? 0 : kmp_sched_p->idle_power[cluster];
            }
            // Get the running power consumption from the power profiler, depending on frequency etc
            // TODO currently frequency independent, create function that gets the freq and calculates...
            // Need the frequency for the performance model also
            kmp_uint32 total_power = idle_power + kmp_sched_p->runtime_power[curr_cluster][taskdata->td_task_type][HIGH_FREQ_POWER][0];

            // Get the execution time from the performance model
            kmp_uint32 exec_time = __kmp_performance_model_get(curr_cluster, taskdata->td_task_type, 1);
            // Calculate the energy based on the execution time and idle + runtime power consumption
            // Depending on how we store the execution time we may need to change the scale of exec time here
            kmp_uint32 energy = exec_time * total_power;


            // If the energy is less than the current minimum, set this as the current minimum
            if (energy < minimum_energy) {
#if MEASURE_ACCURACY
                taskdata->td_predicted_exectime = exec_time;
#endif
                optimal_cluster = curr_cluster;
                minimum_energy = energy;
            }
        }
    }
    else{
        // TODO If tasktype is unknown, select fastest? cluster for identification
        optimal_cluster = CLUSTER_B;
    }
    // We now have the optimal cluster
    // schedule task on optimal thread right now picks the first sleeping thread it finds
    // If unable to find a sleeping thread, schedule task on yourself
    for (int i = 0; i < kmp_sched_p->cluster_tid_entries[optimal_cluster]; i++){
        if (kmp_sched_p->thread_active[optimal_cluster][i] == THREAD_SLEEP){
            return kmp_sched_p->cluster_tids[optimal_cluster][i]; // return first available thread
        }
    }
    // All threads in optimal cluster are busy.

    // You are in the optimal cluster, schedule on yourself
    if(optimal_cluster == thread->th.th_cluster) return tid;

    // You are not in the optimal cluster, schedule on first thread in the optimal cluster
    return kmp_sched_p->cluster_tids[optimal_cluster][0];

}
//ME2

// ME1
// function to perform the scheduling algorithm and
// give the task to the best thread
// returns true/false depening on if the task was successfully scheduled
static kmp_int32 __kmp_schedule_task(kmp_info_t *thread, kmp_task_t *task,
                                kmp_int32 tid) {

  kmp_taskdata_t *taskdata, *current;

  kmp_team_t *team = __kmp_get_team();
  taskdata = KMP_TASK_TO_TASKDATA(task);


  // If the task has run before, get the task type
  if (__kmp_contains_def(task->routine)){
      taskdata->td_task_type= __kmp_get_def(task->routine);
  }
  // Otherwise mark it as undefined
  else{
      taskdata->td_task_type = TASK_UNDEFINED;
  }

  // If the task has run before, get the task type

    kmp_int32 tid_sched;
  if(taskdata->td_cluster == CLUSTER_UNASSIGNED){
      // Task mapping algorithm, returns the tid of preferable thread to schedule
      tid_sched = __kmp_task_mapping(thread, task, tid);
  }
    // Taskloop task, find thread to schedule on
  else{
      tid_sched = __kmp_schedule_thread(task, thread, taskdata->td_cluster, tid);
      #if DEBUG_PRINT_TASK_INFO
      printf("Taskloop task, recommend tid to schedule on: %d\n", tid_sched);
      #endif
  }

  if (tid_sched == tid) {
      kmp_task_team_t *task_team = thread->th.th_task_team;
      kmp_thread_data_t *thread_data;
      thread_data = &task_team->tt.tt_threads_data[tid];
      if (taskdata->td_task_type == TASK_UNDEFINED &&
              TCR_4(thread_data->td.td_deque_ntasks) > 2*__kmp_get_team_num_threads(__kmp_get_gtid())){
#if DEBUG_PRINT_THREAD_INFO
          // Debug message
           printf("Many undefined tasks, invoking task %d now on thread %d\n", taskdata->td_task_id, tid);
#endif
          return INVOKE_NOW;
      }

      return TASK_SCHEDULE_SELF;
  }

  int sched_gtid = __kmp_gtid_from_tid(tid_sched, team);
  kmp_info_t *thread_sched =  __kmp_thread_from_gtid(sched_gtid);

  current = thread_sched->th.th_current_task;

  if (!__kmp_task_is_allowed(sched_gtid, 1, taskdata, current)){
      taskdata->td_no_steal = 0;
#if DEBUG_PRINT_THREAD_INFO
      printf("Task not allowed for thread %d\n", tid_sched);
#endif
      return TASK_SCHEDULE_SELF;
  }


  if (__kmp_give_task(thread_sched, tid_sched, task, 1)) {
    // Putting target thread as awake, otherwise if there are more tasks incoming
    // They will be given to the thread before waking up
    __kmp_thread_active_status(thread_sched->th.th_cluster, thread_sched->th.th_sched_pos, THREAD_AWAKE);
#if DEBUG_PRINT_THREAD_INFO
    printf("Task %d scheduled by thread %d on thread %d \n", taskdata->td_task_id, tid, tid_sched);
#endif
    thread_sched->th.th_cv.notify_all();

    return  TASK_SUCCESSFULLY_SCHEDULED;

  }
  taskdata->td_no_steal = 0;
  return TASK_SCHEDULE_SELF;
}
// ME2


#define PROXY_TASK_FLAG 0x40000000
/* The finish of the proxy tasks is divided in two pieces:
    - the top half is the one that can be done from a thread outside the team
    - the bottom half must be run from a thread within the team

   In order to run the bottom half the task gets queued back into one of the
   threads of the team. Once the td_incomplete_child_task counter of the parent
   is decremented the threads can leave the barriers. So, the bottom half needs
   to be queued before the counter is decremented. The top half is therefore
   divided in two parts:
    - things that can be run before queuing the bottom half
    - things that must be run after queuing the bottom half

   This creates a second race as the bottom half can free the task before the
   second top half is executed. To avoid this we use the
   td_incomplete_child_task of the proxy task to synchronize the top and bottom
   half. */
static void __kmp_first_top_half_finish_proxy(kmp_taskdata_t *taskdata) {
  KMP_DEBUG_ASSERT(taskdata->td_flags.tasktype == TASK_EXPLICIT);
  KMP_DEBUG_ASSERT(taskdata->td_flags.proxy == TASK_PROXY);
  KMP_DEBUG_ASSERT(taskdata->td_flags.complete == 0);
  KMP_DEBUG_ASSERT(taskdata->td_flags.freed == 0);

  taskdata->td_flags.complete = 1; // mark the task as completed

  if (taskdata->td_taskgroup)
    KMP_ATOMIC_DEC(&taskdata->td_taskgroup->count);

  // Create an imaginary children for this task so the bottom half cannot
  // release the task before we have completed the second top half
  KMP_ATOMIC_OR(&taskdata->td_incomplete_child_tasks, PROXY_TASK_FLAG);
}

static void __kmp_second_top_half_finish_proxy(kmp_taskdata_t *taskdata) {
#if KMP_DEBUG
  kmp_int32 children = 0;
  // Predecrement simulated by "- 1" calculation
  children = -1 +
#endif
      KMP_ATOMIC_DEC(&taskdata->td_parent->td_incomplete_child_tasks);
  KMP_DEBUG_ASSERT(children >= 0);

  // Remove the imaginary children
  KMP_ATOMIC_AND(&taskdata->td_incomplete_child_tasks, ~PROXY_TASK_FLAG);
}

static void __kmp_bottom_half_finish_proxy(kmp_int32 gtid, kmp_task_t *ptask) {
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(ptask);
  kmp_info_t *thread = __kmp_threads[gtid];

  KMP_DEBUG_ASSERT(taskdata->td_flags.proxy == TASK_PROXY);
  KMP_DEBUG_ASSERT(taskdata->td_flags.complete ==
                   1); // top half must run before bottom half

  // We need to wait to make sure the top half is finished
  // Spinning here should be ok as this should happen quickly
  while ((KMP_ATOMIC_LD_ACQ(&taskdata->td_incomplete_child_tasks) &
          PROXY_TASK_FLAG) > 0)
    ;

  __kmp_release_deps(gtid, taskdata);
  __kmp_free_task_and_ancestors(gtid, taskdata, thread);
}

/*!
@ingroup TASKING
@param gtid Global Thread ID of encountering thread
@param ptask Task which execution is completed

Execute the completion of a proxy task from a thread of that is part of the
team. Run first and bottom halves directly.
*/
void __kmpc_proxy_task_completed(kmp_int32 gtid, kmp_task_t *ptask) {
  KMP_DEBUG_ASSERT(ptask != NULL);
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(ptask);
  KA_TRACE(
      10, ("__kmp_proxy_task_completed(enter): T#%d proxy task %p completing\n",
           gtid, taskdata));
  __kmp_assert_valid_gtid(gtid);
  KMP_DEBUG_ASSERT(taskdata->td_flags.proxy == TASK_PROXY);

  __kmp_first_top_half_finish_proxy(taskdata);
  __kmp_second_top_half_finish_proxy(taskdata);
  __kmp_bottom_half_finish_proxy(gtid, ptask);

  KA_TRACE(10,
           ("__kmp_proxy_task_completed(exit): T#%d proxy task %p completing\n",
            gtid, taskdata));
}

void __kmpc_give_task(kmp_task_t *ptask, kmp_int32 start = 0) {
  KMP_DEBUG_ASSERT(ptask != NULL);
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(ptask);

  // Enqueue task to complete bottom half completion from a thread within the
  // corresponding team
  kmp_team_t *team = taskdata->td_team;
  kmp_int32 nthreads = team->t.t_nproc;
  kmp_info_t *thread;

  // This should be similar to start_k = __kmp_get_random( thread ) % nthreads
  // but we cannot use __kmp_get_random here
  kmp_int32 start_k = start % nthreads;
  kmp_int32 pass = 1;
  kmp_int32 k = start_k;

  do {
    // For now we're just linearly trying to find a thread
    thread = team->t.t_threads[k];
    k = (k + 1) % nthreads;

    // we did a full pass through all the threads
    if (k == start_k)
      pass = pass << 1;

  } while (!__kmp_give_task(thread, k, ptask, pass));
}

/*!
@ingroup TASKING
@param ptask Task which execution is completed

Execute the completion of a proxy task from a thread that could not belong to
the team.
*/
void __kmpc_proxy_task_completed_ooo(kmp_task_t *ptask) {
  KMP_DEBUG_ASSERT(ptask != NULL);
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(ptask);

  KA_TRACE(
      10,
      ("__kmp_proxy_task_completed_ooo(enter): proxy task completing ooo %p\n",
       taskdata));

  KMP_DEBUG_ASSERT(taskdata->td_flags.proxy == TASK_PROXY);

  __kmp_first_top_half_finish_proxy(taskdata);

  __kmpc_give_task(ptask);

  __kmp_second_top_half_finish_proxy(taskdata);

  KA_TRACE(
      10,
      ("__kmp_proxy_task_completed_ooo(exit): proxy task completing ooo %p\n",
       taskdata));
}

kmp_event_t *__kmpc_task_allow_completion_event(ident_t *loc_ref, int gtid,
                                                kmp_task_t *task) {
  kmp_taskdata_t *td = KMP_TASK_TO_TASKDATA(task);
  if (td->td_allow_completion_event.type == KMP_EVENT_UNINITIALIZED) {
    td->td_allow_completion_event.type = KMP_EVENT_ALLOW_COMPLETION;
    td->td_allow_completion_event.ed.task = task;
    __kmp_init_tas_lock(&td->td_allow_completion_event.lock);
  }
  return &td->td_allow_completion_event;
}

void __kmp_fulfill_event(kmp_event_t *event) {
  if (event->type == KMP_EVENT_ALLOW_COMPLETION) {
    kmp_task_t *ptask = event->ed.task;
    kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(ptask);
    bool detached = false;
    int gtid = __kmp_get_gtid();

    // The associated task might have completed or could be completing at this
    // point.
    // We need to take the lock to avoid races
    __kmp_acquire_tas_lock(&event->lock, gtid);
    if (taskdata->td_flags.proxy == TASK_PROXY) {
      detached = true;
    } else {
#if OMPT_SUPPORT
      // The OMPT event must occur under mutual exclusion,
      // otherwise the tool might access ptask after free
      if (UNLIKELY(ompt_enabled.enabled))
        __ompt_task_finish(ptask, NULL, ompt_task_early_fulfill);
#endif
    }
    event->type = KMP_EVENT_UNINITIALIZED;
    __kmp_release_tas_lock(&event->lock, gtid);

    if (detached) {
#if OMPT_SUPPORT
      // We free ptask afterwards and know the task is finished,
      // so locking is not necessary
      if (UNLIKELY(ompt_enabled.enabled))
        __ompt_task_finish(ptask, NULL, ompt_task_late_fulfill);
#endif
      // If the task detached complete the proxy task
      if (gtid >= 0) {
        kmp_team_t *team = taskdata->td_team;
        kmp_info_t *thread = __kmp_get_thread();
        if (thread->th.th_team == team) {
          __kmpc_proxy_task_completed(gtid, ptask);
          return;
        }
      }

      // fallback
      __kmpc_proxy_task_completed_ooo(ptask);
    }
  }
}

// __kmp_task_dup_alloc: Allocate the taskdata and make a copy of source task
// for taskloop
//
// thread:   allocating thread
// task_src: pointer to source task to be duplicated
// returns:  a pointer to the allocated kmp_task_t structure (task).
kmp_task_t *__kmp_task_dup_alloc(kmp_info_t *thread, kmp_task_t *task_src) {
  kmp_task_t *task;
  kmp_taskdata_t *taskdata;
  kmp_taskdata_t *taskdata_src = KMP_TASK_TO_TASKDATA(task_src);
  kmp_taskdata_t *parent_task = taskdata_src->td_parent; // same parent task
  size_t shareds_offset;
  size_t task_size;

  KA_TRACE(10, ("__kmp_task_dup_alloc(enter): Th %p, source task %p\n", thread,
                task_src));
  KMP_DEBUG_ASSERT(taskdata_src->td_flags.proxy ==
                   TASK_FULL); // it should not be proxy task
  KMP_DEBUG_ASSERT(taskdata_src->td_flags.tasktype == TASK_EXPLICIT);
  task_size = taskdata_src->td_size_alloc;

  // Allocate a kmp_taskdata_t block and a kmp_task_t block.
  KA_TRACE(30, ("__kmp_task_dup_alloc: Th %p, malloc size %ld\n", thread,
                task_size));
#if USE_FAST_MEMORY
  taskdata = (kmp_taskdata_t *)__kmp_fast_allocate(thread, task_size);
#else
  taskdata = (kmp_taskdata_t *)__kmp_thread_malloc(thread, task_size);
#endif /* USE_FAST_MEMORY */
  KMP_MEMCPY(taskdata, taskdata_src, task_size);

  task = KMP_TASKDATA_TO_TASK(taskdata);

  // Initialize new task (only specific fields not affected by memcpy)
  taskdata->td_task_id = KMP_GEN_TASK_ID();
  if (task->shareds != NULL) { // need setup shareds pointer
    shareds_offset = (char *)task_src->shareds - (char *)taskdata_src;
    task->shareds = &((char *)taskdata)[shareds_offset];
    KMP_DEBUG_ASSERT((((kmp_uintptr_t)task->shareds) & (sizeof(void *) - 1)) ==
                     0);
  }
  taskdata->td_alloc_thread = thread;
  taskdata->td_parent = parent_task;
  // task inherits the taskgroup from the parent task
  taskdata->td_taskgroup = parent_task->td_taskgroup;
  // tied task needs to initialize the td_last_tied at creation,
  // untied one does this when it is scheduled for execution
  if (taskdata->td_flags.tiedness == TASK_TIED)
    taskdata->td_last_tied = taskdata;

  // Only need to keep track of child task counts if team parallel and tasking
  // not serialized
  if (!(taskdata->td_flags.team_serial || taskdata->td_flags.tasking_ser)) {
    KMP_ATOMIC_INC(&parent_task->td_incomplete_child_tasks);
    if (parent_task->td_taskgroup)
      KMP_ATOMIC_INC(&parent_task->td_taskgroup->count);
    // Only need to keep track of allocated child tasks for explicit tasks since
    // implicit not deallocated
    if (taskdata->td_parent->td_flags.tasktype == TASK_EXPLICIT)
      KMP_ATOMIC_INC(&taskdata->td_parent->td_allocated_child_tasks);
  }

  KA_TRACE(20,
           ("__kmp_task_dup_alloc(exit): Th %p, created task %p, parent=%p\n",
            thread, taskdata, taskdata->td_parent));
#if OMPT_SUPPORT
  if (UNLIKELY(ompt_enabled.enabled))
    __ompt_task_init(taskdata, thread->th.th_info.ds.ds_gtid);
#endif
  return task;
}

// Routine optionally generated by the compiler for setting the lastprivate flag
// and calling needed constructors for private/firstprivate objects
// (used to form taskloop tasks from pattern task)
// Parameters: dest task, src task, lastprivate flag.
typedef void (*p_task_dup_t)(kmp_task_t *, kmp_task_t *, kmp_int32);

KMP_BUILD_ASSERT(sizeof(long) == 4 || sizeof(long) == 8);

// class to encapsulate manipulating loop bounds in a taskloop task.
// this abstracts away the Intel vs GOMP taskloop interface for setting/getting
// the loop bound variables.
class kmp_taskloop_bounds_t {
  kmp_task_t *task;
  const kmp_taskdata_t *taskdata;
  size_t lower_offset;
  size_t upper_offset;

public:
  kmp_taskloop_bounds_t(kmp_task_t *_task, kmp_uint64 *lb, kmp_uint64 *ub)
      : task(_task), taskdata(KMP_TASK_TO_TASKDATA(task)),
        lower_offset((char *)lb - (char *)task),
        upper_offset((char *)ub - (char *)task) {
    KMP_DEBUG_ASSERT((char *)lb > (char *)_task);
    KMP_DEBUG_ASSERT((char *)ub > (char *)_task);
  }
  kmp_taskloop_bounds_t(kmp_task_t *_task, const kmp_taskloop_bounds_t &bounds)
      : task(_task), taskdata(KMP_TASK_TO_TASKDATA(_task)),
        lower_offset(bounds.lower_offset), upper_offset(bounds.upper_offset) {}
  size_t get_lower_offset() const { return lower_offset; }
  size_t get_upper_offset() const { return upper_offset; }
  kmp_uint64 get_lb() const {
    kmp_int64 retval;
#if defined(KMP_GOMP_COMPAT)
    // Intel task just returns the lower bound normally
    if (!taskdata->td_flags.native) {
      retval = *(kmp_int64 *)((char *)task + lower_offset);
    } else {
      // GOMP task has to take into account the sizeof(long)
      if (taskdata->td_size_loop_bounds == 4) {
        kmp_int32 *lb = RCAST(kmp_int32 *, task->shareds);
        retval = (kmp_int64)*lb;
      } else {
        kmp_int64 *lb = RCAST(kmp_int64 *, task->shareds);
        retval = (kmp_int64)*lb;
      }
    }
#else
    (void)taskdata;
    retval = *(kmp_int64 *)((char *)task + lower_offset);
#endif // defined(KMP_GOMP_COMPAT)
    return retval;
  }
  kmp_uint64 get_ub() const {
    kmp_int64 retval;
#if defined(KMP_GOMP_COMPAT)
    // Intel task just returns the upper bound normally
    if (!taskdata->td_flags.native) {
      retval = *(kmp_int64 *)((char *)task + upper_offset);
    } else {
      // GOMP task has to take into account the sizeof(long)
      if (taskdata->td_size_loop_bounds == 4) {
        kmp_int32 *ub = RCAST(kmp_int32 *, task->shareds) + 1;
        retval = (kmp_int64)*ub;
      } else {
        kmp_int64 *ub = RCAST(kmp_int64 *, task->shareds) + 1;
        retval = (kmp_int64)*ub;
      }
    }
#else
    retval = *(kmp_int64 *)((char *)task + upper_offset);
#endif // defined(KMP_GOMP_COMPAT)
    return retval;
  }
  void set_lb(kmp_uint64 lb) {
#if defined(KMP_GOMP_COMPAT)
    // Intel task just sets the lower bound normally
    if (!taskdata->td_flags.native) {
      *(kmp_uint64 *)((char *)task + lower_offset) = lb;
    } else {
      // GOMP task has to take into account the sizeof(long)
      if (taskdata->td_size_loop_bounds == 4) {
        kmp_uint32 *lower = RCAST(kmp_uint32 *, task->shareds);
        *lower = (kmp_uint32)lb;
      } else {
        kmp_uint64 *lower = RCAST(kmp_uint64 *, task->shareds);
        *lower = (kmp_uint64)lb;
      }
    }
#else
    *(kmp_uint64 *)((char *)task + lower_offset) = lb;
#endif // defined(KMP_GOMP_COMPAT)
  }
  void set_ub(kmp_uint64 ub) {
#if defined(KMP_GOMP_COMPAT)
    // Intel task just sets the upper bound normally
    if (!taskdata->td_flags.native) {
      *(kmp_uint64 *)((char *)task + upper_offset) = ub;
    } else {
      // GOMP task has to take into account the sizeof(long)
      if (taskdata->td_size_loop_bounds == 4) {
        kmp_uint32 *upper = RCAST(kmp_uint32 *, task->shareds) + 1;
        *upper = (kmp_uint32)ub;
      } else {
        kmp_uint64 *upper = RCAST(kmp_uint64 *, task->shareds) + 1;
        *upper = (kmp_uint64)ub;
      }
    }
#else
    *(kmp_uint64 *)((char *)task + upper_offset) = ub;
#endif // defined(KMP_GOMP_COMPAT)
  }
};

// __kmp_taskloop_linear: Start tasks of the taskloop linearly
//
// loc        Source location information
// gtid       Global thread ID
// task       Pattern task, exposes the loop iteration range
// lb         Pointer to loop lower bound in task structure
// ub         Pointer to loop upper bound in task structure
// st         Loop stride
// ub_glob    Global upper bound (used for lastprivate check)
// num_tasks  Number of tasks to execute
// grainsize  Number of loop iterations per task
// extras     Number of chunks with grainsize+1 iterations
// last_chunk Reduction of grainsize for last task
// tc         Iterations count
// task_dup   Tasks duplication routine
// codeptr_ra Return address for OMPT events
void __kmp_taskloop_linear(ident_t *loc, int gtid, kmp_task_t *task,
                           kmp_uint64 *lb, kmp_uint64 *ub, kmp_int64 st,
                           kmp_uint64 ub_glob, kmp_uint64 num_tasks,
                           kmp_uint64 grainsize, kmp_uint64 extras,
                           kmp_int64 last_chunk, kmp_uint64 tc,
#if OMPT_SUPPORT
                           void *codeptr_ra,
#endif
                           void *task_dup) {
  KMP_COUNT_BLOCK(OMP_TASKLOOP);
  KMP_TIME_PARTITIONED_BLOCK(OMP_taskloop_scheduling);
  p_task_dup_t ptask_dup = (p_task_dup_t)task_dup;
  // compiler provides global bounds here
  kmp_taskloop_bounds_t task_bounds(task, lb, ub);
  kmp_uint64 lower = task_bounds.get_lb();
  kmp_uint64 upper = task_bounds.get_ub();
  kmp_uint64 i;
  kmp_info_t *thread = __kmp_threads[gtid];
  kmp_taskdata_t *current_task = thread->th.th_current_task;
  kmp_task_t *next_task;
  kmp_int32 lastpriv = 0;

  KMP_DEBUG_ASSERT(tc == num_tasks * grainsize +
                             (last_chunk < 0 ? last_chunk : extras));
  KMP_DEBUG_ASSERT(num_tasks > extras);
  KMP_DEBUG_ASSERT(num_tasks > 0);
  KA_TRACE(20, ("__kmp_taskloop_linear: T#%d: %lld tasks, grainsize %lld, "
                "extras %lld, last_chunk %lld, i=%lld,%lld(%d)%lld, dup %p\n",
                gtid, num_tasks, grainsize, extras, last_chunk, lower, upper,
                ub_glob, st, task_dup));

  // Launch num_tasks tasks, assign grainsize iterations each task
  for (i = 0; i < num_tasks; ++i) {
    kmp_uint64 chunk_minus_1;
    if (extras == 0) {
      chunk_minus_1 = grainsize - 1;
    } else {
      chunk_minus_1 = grainsize;
      --extras; // first extras iterations get bigger chunk (grainsize+1)
    }
    upper = lower + st * chunk_minus_1;
    if (upper > *ub) {
      upper = *ub;
    }
    if (i == num_tasks - 1) {
      // schedule the last task, set lastprivate flag if needed
      if (st == 1) { // most common case
        KMP_DEBUG_ASSERT(upper == *ub);
        if (upper == ub_glob)
          lastpriv = 1;
      } else if (st > 0) { // positive loop stride
        KMP_DEBUG_ASSERT((kmp_uint64)st > *ub - upper);
        if ((kmp_uint64)st > ub_glob - upper)
          lastpriv = 1;
      } else { // negative loop stride
        KMP_DEBUG_ASSERT(upper + st < *ub);
        if (upper - ub_glob < (kmp_uint64)(-st))
          lastpriv = 1;
      }
    }
    next_task = __kmp_task_dup_alloc(thread, task); // allocate new task
    kmp_taskdata_t *next_taskdata = KMP_TASK_TO_TASKDATA(next_task);
    kmp_taskloop_bounds_t next_task_bounds =
        kmp_taskloop_bounds_t(next_task, task_bounds);

    // adjust task-specific bounds
    next_task_bounds.set_lb(lower);
    if (next_taskdata->td_flags.native) {
      next_task_bounds.set_ub(upper + (st > 0 ? 1 : -1));
    } else {
      next_task_bounds.set_ub(upper);
    }
    if (ptask_dup != NULL) // set lastprivate flag, construct firstprivates,
                           // etc.
      ptask_dup(next_task, task, lastpriv);
    KA_TRACE(40,
             ("__kmp_taskloop_linear: T#%d; task #%llu: task %p: lower %lld, "
              "upper %lld stride %lld, (offsets %p %p)\n",
              gtid, i, next_task, lower, upper, st,
              next_task_bounds.get_lower_offset(),
              next_task_bounds.get_upper_offset()));
#if OMPT_SUPPORT
    __kmp_omp_taskloop_task(NULL, gtid, next_task,
                            codeptr_ra); // schedule new task
#else
    __kmp_omp_task(gtid, next_task, true); // schedule new task
#endif
    lower = upper + st; // adjust lower bound for the next iteration
  }
  // free the pattern task and exit
  __kmp_task_start(gtid, task, current_task); // make internal bookkeeping
  // do not execute the pattern task, just do internal bookkeeping
  __kmp_task_finish<false>(gtid, task, current_task);
}

// Structure to keep taskloop parameters for auxiliary task
// kept in the shareds of the task structure.
typedef struct __taskloop_params {
  kmp_task_t *task;
  kmp_uint64 *lb;
  kmp_uint64 *ub;
  void *task_dup;
  kmp_int64 st;
  kmp_uint64 ub_glob;
  kmp_uint64 num_tasks;
  kmp_uint64 grainsize;
  kmp_uint64 extras;
  kmp_int64 last_chunk;
  kmp_uint64 tc;
  kmp_uint64 num_t_min;
#if OMPT_SUPPORT
  void *codeptr_ra;
#endif
} __taskloop_params_t;

void __kmp_taskloop_recur(ident_t *, int, kmp_task_t *, kmp_uint64 *,
                          kmp_uint64 *, kmp_int64, kmp_uint64, kmp_uint64,
                          kmp_uint64, kmp_uint64, kmp_int64, kmp_uint64,
                          kmp_uint64,
#if OMPT_SUPPORT
                          void *,
#endif
                          void *);

// Execute part of the taskloop submitted as a task.
int __kmp_taskloop_task(int gtid, void *ptask) {
  __taskloop_params_t *p =
      (__taskloop_params_t *)((kmp_task_t *)ptask)->shareds;
  kmp_task_t *task = p->task;
  kmp_uint64 *lb = p->lb;
  kmp_uint64 *ub = p->ub;
  void *task_dup = p->task_dup;
  //  p_task_dup_t ptask_dup = (p_task_dup_t)task_dup;
  kmp_int64 st = p->st;
  kmp_uint64 ub_glob = p->ub_glob;
  kmp_uint64 num_tasks = p->num_tasks;
  kmp_uint64 grainsize = p->grainsize;
  kmp_uint64 extras = p->extras;
  kmp_int64 last_chunk = p->last_chunk;
  kmp_uint64 tc = p->tc;
  kmp_uint64 num_t_min = p->num_t_min;
#if OMPT_SUPPORT
  void *codeptr_ra = p->codeptr_ra;
#endif
#if KMP_DEBUG
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  KMP_DEBUG_ASSERT(task != NULL);
  KA_TRACE(20,
           ("__kmp_taskloop_task: T#%d, task %p: %lld tasks, grainsize"
            " %lld, extras %lld, last_chunk %lld, i=%lld,%lld(%d), dup %p\n",
            gtid, taskdata, num_tasks, grainsize, extras, last_chunk, *lb, *ub,
            st, task_dup));
#endif
  KMP_DEBUG_ASSERT(num_tasks * 2 + 1 > num_t_min);
  if (num_tasks > num_t_min)
    __kmp_taskloop_recur(NULL, gtid, task, lb, ub, st, ub_glob, num_tasks,
                         grainsize, extras, last_chunk, tc, num_t_min,
#if OMPT_SUPPORT
                         codeptr_ra,
#endif
                         task_dup);
  else
    __kmp_taskloop_linear(NULL, gtid, task, lb, ub, st, ub_glob, num_tasks,
                          grainsize, extras, last_chunk, tc,
#if OMPT_SUPPORT
                          codeptr_ra,
#endif
                          task_dup);

  KA_TRACE(40, ("__kmp_taskloop_task(exit): T#%d\n", gtid));
  return 0;
}

// Schedule part of the taskloop as a task,
// execute the rest of the taskloop.
//
// loc        Source location information
// gtid       Global thread ID
// task       Pattern task, exposes the loop iteration range
// lb         Pointer to loop lower bound in task structure
// ub         Pointer to loop upper bound in task structure
// st         Loop stride
// ub_glob    Global upper bound (used for lastprivate check)
// num_tasks  Number of tasks to execute
// grainsize  Number of loop iterations per task
// extras     Number of chunks with grainsize+1 iterations
// last_chunk Reduction of grainsize for last task
// tc         Iterations count
// num_t_min  Threshold to launch tasks recursively
// task_dup   Tasks duplication routine
// codeptr_ra Return address for OMPT events
void __kmp_taskloop_recur(ident_t *loc, int gtid, kmp_task_t *task,
                          kmp_uint64 *lb, kmp_uint64 *ub, kmp_int64 st,
                          kmp_uint64 ub_glob, kmp_uint64 num_tasks,
                          kmp_uint64 grainsize, kmp_uint64 extras,
                          kmp_int64 last_chunk, kmp_uint64 tc,
                          kmp_uint64 num_t_min,
#if OMPT_SUPPORT
                          void *codeptr_ra,
#endif
                          void *task_dup) {
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  KMP_DEBUG_ASSERT(task != NULL);
  KMP_DEBUG_ASSERT(num_tasks > num_t_min);
  KA_TRACE(20,
           ("__kmp_taskloop_recur: T#%d, task %p: %lld tasks, grainsize"
            " %lld, extras %lld, last_chunk %lld, i=%lld,%lld(%d), dup %p\n",
            gtid, taskdata, num_tasks, grainsize, extras, last_chunk, *lb, *ub,
            st, task_dup));
  p_task_dup_t ptask_dup = (p_task_dup_t)task_dup;
  kmp_uint64 lower = *lb;
  kmp_info_t *thread = __kmp_threads[gtid];
  //  kmp_taskdata_t *current_task = thread->th.th_current_task;
  kmp_task_t *next_task;
  size_t lower_offset =
      (char *)lb - (char *)task; // remember offset of lb in the task structure
  size_t upper_offset =
      (char *)ub - (char *)task; // remember offset of ub in the task structure

  KMP_DEBUG_ASSERT(tc == num_tasks * grainsize +
                             (last_chunk < 0 ? last_chunk : extras));
  KMP_DEBUG_ASSERT(num_tasks > extras);
  KMP_DEBUG_ASSERT(num_tasks > 0);

  // split the loop in two halves
  kmp_uint64 lb1, ub0, tc0, tc1, ext0, ext1;
  kmp_int64 last_chunk0 = 0, last_chunk1 = 0;
  kmp_uint64 gr_size0 = grainsize;
  kmp_uint64 n_tsk0 = num_tasks >> 1; // num_tasks/2 to execute
  kmp_uint64 n_tsk1 = num_tasks - n_tsk0; // to schedule as a task
  if (last_chunk < 0) {
    ext0 = ext1 = 0;
    last_chunk1 = last_chunk;
    tc0 = grainsize * n_tsk0;
    tc1 = tc - tc0;
  } else if (n_tsk0 <= extras) {
    gr_size0++; // integrate extras into grainsize
    ext0 = 0; // no extra iters in 1st half
    ext1 = extras - n_tsk0; // remaining extras
    tc0 = gr_size0 * n_tsk0;
    tc1 = tc - tc0;
  } else { // n_tsk0 > extras
    ext1 = 0; // no extra iters in 2nd half
    ext0 = extras;
    tc1 = grainsize * n_tsk1;
    tc0 = tc - tc1;
  }
  ub0 = lower + st * (tc0 - 1);
  lb1 = ub0 + st;

  // create pattern task for 2nd half of the loop
  next_task = __kmp_task_dup_alloc(thread, task); // duplicate the task
  // adjust lower bound (upper bound is not changed) for the 2nd half
  *(kmp_uint64 *)((char *)next_task + lower_offset) = lb1;
  if (ptask_dup != NULL) // construct firstprivates, etc.
    ptask_dup(next_task, task, 0);
  *ub = ub0; // adjust upper bound for the 1st half

  // create auxiliary task for 2nd half of the loop
  // make sure new task has same parent task as the pattern task
  kmp_taskdata_t *current_task = thread->th.th_current_task;
  thread->th.th_current_task = taskdata->td_parent;
  kmp_task_t *new_task =
      __kmpc_omp_task_alloc(loc, gtid, 1, 3 * sizeof(void *),
                            sizeof(__taskloop_params_t), &__kmp_taskloop_task);
  // restore current task
  thread->th.th_current_task = current_task;
  __taskloop_params_t *p = (__taskloop_params_t *)new_task->shareds;
  p->task = next_task;
  p->lb = (kmp_uint64 *)((char *)next_task + lower_offset);
  p->ub = (kmp_uint64 *)((char *)next_task + upper_offset);
  p->task_dup = task_dup;
  p->st = st;
  p->ub_glob = ub_glob;
  p->num_tasks = n_tsk1;
  p->grainsize = grainsize;
  p->extras = ext1;
  p->last_chunk = last_chunk1;
  p->tc = tc1;
  p->num_t_min = num_t_min;
#if OMPT_SUPPORT
  p->codeptr_ra = codeptr_ra;
#endif

#if OMPT_SUPPORT
  // schedule new task with correct return address for OMPT events
  __kmp_omp_taskloop_task(NULL, gtid, new_task, codeptr_ra);
#else
  __kmp_omp_task(gtid, new_task, true); // schedule new task
#endif

  // execute the 1st half of current subrange
  if (n_tsk0 > num_t_min)
    __kmp_taskloop_recur(loc, gtid, task, lb, ub, st, ub_glob, n_tsk0, gr_size0,
                         ext0, last_chunk0, tc0, num_t_min,
#if OMPT_SUPPORT
                         codeptr_ra,
#endif
                         task_dup);
  else
    __kmp_taskloop_linear(loc, gtid, task, lb, ub, st, ub_glob, n_tsk0,
                          gr_size0, ext0, last_chunk0, tc0,
#if OMPT_SUPPORT
                          codeptr_ra,
#endif
                          task_dup);

  KA_TRACE(40, ("__kmp_taskloop_recur(exit): T#%d\n", gtid));
}

static void __kmp_taskloop(ident_t *loc, int gtid, kmp_task_t *task, int if_val,
                           kmp_uint64 *lb, kmp_uint64 *ub, kmp_int64 st,
                           int nogroup, int sched, kmp_uint64 grainsize,
                           int modifier, void *task_dup) {
  kmp_taskdata_t *taskdata = KMP_TASK_TO_TASKDATA(task);
  KMP_DEBUG_ASSERT(task != NULL);
  if (nogroup == 0) {
#if OMPT_SUPPORT && OMPT_OPTIONAL
    OMPT_STORE_RETURN_ADDRESS(gtid);
#endif
    __kmpc_taskgroup(loc, gtid);
  }
  // =========================================================================
  // calculate loop parameters
  kmp_taskloop_bounds_t task_bounds(task, lb, ub);
  kmp_uint64 tc;
  // compiler provides global bounds here
  kmp_uint64 lower = task_bounds.get_lb();
  kmp_uint64 upper = task_bounds.get_ub();
  kmp_uint64 ub_glob = upper; // global upper used to calc lastprivate flag
  kmp_uint64 num_tasks = 0, extras = 0;
  kmp_int64 last_chunk =
      0; // reduce grainsize of last task by last_chunk in strict mode
  kmp_uint64 num_tasks_min = __kmp_taskloop_min_tasks;
  kmp_info_t *thread = __kmp_threads[gtid];
  kmp_taskdata_t *current_task = thread->th.th_current_task;

  KA_TRACE(20, ("__kmp_taskloop: T#%d, task %p, lb %lld, ub %lld, st %lld, "
                "grain %llu(%d, %d), dup %p\n",
                gtid, taskdata, lower, upper, st, grainsize, sched, modifier,
                task_dup));
  // compute trip count
  if (st == 1) { // most common case
    tc = upper - lower + 1;
  } else if (st < 0) {
    tc = (lower - upper) / (-st) + 1;
  } else { // st > 0
    tc = (upper - lower) / st + 1;
  }

//ME1
  kmp_int32 tid = __kmp_tid_from_gtid(gtid);
#if DEBUG_PRINT_TASKLOOP_SPLIT_INFO
  printf("lowerbound = %llu\nupperbound = %llu\nnum_tasks_min = %llu\ngrainsize = %llu\ntc = %llu\n"
         , lower, upper, num_tasks_min, grainsize, tc);
#endif

//ME2

  if (tc == 0) {
    KA_TRACE(20, ("__kmp_taskloop(exit): T#%d zero-trip loop\n", gtid));
    // free the pattern task and exit
    __kmp_task_start(gtid, task, current_task);
    // do not execute anything for zero-trip loop
    __kmp_task_finish<false>(gtid, task, current_task);
    return;
  }

#if OMPT_SUPPORT && OMPT_OPTIONAL
  ompt_team_info_t *team_info = __ompt_get_teaminfo(0, NULL);
  ompt_task_info_t *task_info = __ompt_get_task_info_object(0);
  if (ompt_enabled.ompt_callback_work) {
    ompt_callbacks.ompt_callback(ompt_callback_work)(
        ompt_work_taskloop, ompt_scope_begin, &(team_info->parallel_data),
        &(task_info->task_data), tc, OMPT_GET_RETURN_ADDRESS(0));
  }
#endif

  if (num_tasks_min == 0)
    // TODO: can we choose better default heuristic?
    num_tasks_min =
        KMP_MIN(thread->th.th_team_nproc * 10, INITIAL_TASK_DEQUE_SIZE);

  // compute num_tasks/grainsize based on the input provided
  switch (sched) {
  case 0: // no schedule clause specified, we can choose the default
    // let's try to schedule (team_size*10) tasks

    //ME1
    taskdata->td_task_type = TASK_PATTERN;
    __kmp_taskloop_mapping(thread, task, tid, tc + 1);
#if DEBUG_PRINT_TASKLOOP_SPLIT_INFO
      printf("Taskloop number = %d, Cluster = %d, Number of tasks recommended = %hhu\n",
       taskdata->td_task_id, taskdata->td_cluster, taskdata->td_taskwidth);
#endif
    grainsize = taskdata->td_taskwidth;
    //TODO need to calculate max allowed grainsize here
    //ME2

    //grainsize = thread->th.th_team_nproc * 10;
    KMP_FALLTHROUGH();
    //break;
    
  case 2: // num_tasks provided
    if (grainsize > tc) {
      num_tasks = tc; // too big num_tasks requested, adjust values
      grainsize = 1;
      extras = 0;
    } else {
      num_tasks = grainsize;
      grainsize = tc / num_tasks;
      extras = tc % num_tasks;
    }
    break;
  case 1: // grainsize provided
    if (grainsize > tc) {
      num_tasks = 1;
      grainsize = tc; // too big grainsize requested, adjust values
      extras = 0;
    } else {
      if (modifier) {
        num_tasks = (tc + grainsize - 1) / grainsize;
        last_chunk = tc - (num_tasks * grainsize);
        extras = 0;
      } else {
        num_tasks = tc / grainsize;
        // adjust grainsize for balanced distribution of iterations
        grainsize = tc / num_tasks;
        extras = tc % num_tasks;
      }
    }
    break;
  default:
    KMP_ASSERT2(0, "unknown scheduling of taskloop");
  }

  KMP_DEBUG_ASSERT(tc == num_tasks * grainsize +
                             (last_chunk < 0 ? last_chunk : extras));
  KMP_DEBUG_ASSERT(num_tasks > extras);
  KMP_DEBUG_ASSERT(num_tasks > 0);
  // =========================================================================

  // check if clause value first
  // Also require GOMP_taskloop to reduce to linear (taskdata->td_flags.native)
  if (if_val == 0) { // if(0) specified, mark task as serial
    taskdata->td_flags.task_serial = 1;
    taskdata->td_flags.tiedness = TASK_TIED; // AC: serial task cannot be untied
    // always start serial tasks linearly
    __kmp_taskloop_linear(loc, gtid, task, lb, ub, st, ub_glob, num_tasks,
                          grainsize, extras, last_chunk, tc,
#if OMPT_SUPPORT
                          OMPT_GET_RETURN_ADDRESS(0),
#endif
                          task_dup);
    // !taskdata->td_flags.native => currently force linear spawning of tasks
    // for GOMP_taskloop
  } else if (num_tasks > num_tasks_min && !taskdata->td_flags.native) {
    KA_TRACE(20, ("__kmp_taskloop: T#%d, go recursive: tc %llu, #tasks %llu"
                  "(%lld), grain %llu, extras %llu, last_chunk %lld\n",
                  gtid, tc, num_tasks, num_tasks_min, grainsize, extras,
                  last_chunk));
    __kmp_taskloop_recur(loc, gtid, task, lb, ub, st, ub_glob, num_tasks,
                         grainsize, extras, last_chunk, tc, num_tasks_min,
#if OMPT_SUPPORT
                         OMPT_GET_RETURN_ADDRESS(0),
#endif
                         task_dup);
  } else {
    KA_TRACE(20, ("__kmp_taskloop: T#%d, go linear: tc %llu, #tasks %llu"
                  "(%lld), grain %llu, extras %llu, last_chunk %lld\n",
                  gtid, tc, num_tasks, num_tasks_min, grainsize, extras,
                  last_chunk));
    __kmp_taskloop_linear(loc, gtid, task, lb, ub, st, ub_glob, num_tasks,
                          grainsize, extras, last_chunk, tc,
#if OMPT_SUPPORT
                          OMPT_GET_RETURN_ADDRESS(0),
#endif
                          task_dup);
  }

#if OMPT_SUPPORT && OMPT_OPTIONAL
  if (ompt_enabled.ompt_callback_work) {
    ompt_callbacks.ompt_callback(ompt_callback_work)(
        ompt_work_taskloop, ompt_scope_end, &(team_info->parallel_data),
        &(task_info->task_data), tc, OMPT_GET_RETURN_ADDRESS(0));
  }
#endif

  if (nogroup == 0) {
#if OMPT_SUPPORT && OMPT_OPTIONAL
    OMPT_STORE_RETURN_ADDRESS(gtid);
#endif
    __kmpc_end_taskgroup(loc, gtid);
  }
  KA_TRACE(20, ("__kmp_taskloop(exit): T#%d\n", gtid));
}

/*!
@ingroup TASKING
@param loc       Source location information
@param gtid      Global thread ID
@param task      Task structure
@param if_val    Value of the if clause
@param lb        Pointer to loop lower bound in task structure
@param ub        Pointer to loop upper bound in task structure
@param st        Loop stride
@param nogroup   Flag, 1 if nogroup clause specified, 0 otherwise
@param sched     Schedule specified 0/1/2 for none/grainsize/num_tasks
@param grainsize Schedule value if specified
@param task_dup  Tasks duplication routine

Execute the taskloop construct.
*/
void __kmpc_taskloop(ident_t *loc, int gtid, kmp_task_t *task, int if_val,
                     kmp_uint64 *lb, kmp_uint64 *ub, kmp_int64 st, int nogroup,
                     int sched, kmp_uint64 grainsize, void *task_dup) {
  __kmp_assert_valid_gtid(gtid);
  KA_TRACE(20, ("__kmpc_taskloop(enter): T#%d\n", gtid));
  __kmp_taskloop(loc, gtid, task, if_val, lb, ub, st, nogroup, sched, grainsize,
                 0, task_dup);
  KA_TRACE(20, ("__kmpc_taskloop(exit): T#%d\n", gtid));
}

/*!
@ingroup TASKING
@param loc       Source location information
@param gtid      Global thread ID
@param task      Task structure
@param if_val    Value of the if clause
@param lb        Pointer to loop lower bound in task structure
@param ub        Pointer to loop upper bound in task structure
@param st        Loop stride
@param nogroup   Flag, 1 if nogroup clause specified, 0 otherwise
@param sched     Schedule specified 0/1/2 for none/grainsize/num_tasks
@param grainsize Schedule value if specified
@param modifer   Modifier 'strict' for sched, 1 if present, 0 otherwise
@param task_dup  Tasks duplication routine

Execute the taskloop construct.
*/
void __kmpc_taskloop_5(ident_t *loc, int gtid, kmp_task_t *task, int if_val,
                       kmp_uint64 *lb, kmp_uint64 *ub, kmp_int64 st,
                       int nogroup, int sched, kmp_uint64 grainsize,
                       int modifier, void *task_dup) {
  __kmp_assert_valid_gtid(gtid);
  KA_TRACE(20, ("__kmpc_taskloop_5(enter): T#%d\n", gtid));
  __kmp_taskloop(loc, gtid, task, if_val, lb, ub, st, nogroup, sched, grainsize,
                 modifier, task_dup);
  KA_TRACE(20, ("__kmpc_taskloop_5(exit): T#%d\n", gtid));
}
