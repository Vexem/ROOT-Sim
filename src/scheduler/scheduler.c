/**
 * @file scheduler/scheduler.c
 *
 * @brief The ROOT-Sim scheduler main module
 *
 * This module implements the schedule() function, which is the main
 * entry point for all the schedulers implemented in ROOT-Sim, and
 * several support functions which allow to initialize worker threads.
 *
 * Also, the LP_main_loop() function, which is the function where all
 * the User-Level Threads associated with Logical Processes live, is
 * defined here.
 *
 * @copyright
 * Copyright (C) 2008-2019 HPDCS Group
 * https://hpdcs.github.io
 *
 * This file is part of ROOT-Sim (ROme OpTimistic Simulator).
 *
 * ROOT-Sim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; only version 3 of the License applies.
 *
 * ROOT-Sim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ROOT-Sim; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * @author Francesco Quaglia
 * @author Alessandro Pellegrini
 * @author Roberto Vitali
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <datatypes/list.h>
#include <datatypes/msgchannel.h>
#include <core/core.h>
#include <core/timer.h>
#include <arch/atomic.h>
#include <arch/ult.h>
#include <arch/thread.h>
#include <core/init.h>
#include <scheduler/binding.h>
#include <scheduler/process.h>
#include <scheduler/scheduler.h>
#include <scheduler/stf.h>
#include <mm/state.h>
#include <communication/communication.h>

#ifdef HAVE_CROSS_STATE
#include <mm/ecs.h>
#endif

#include <mm/mm.h>
#include <statistics/statistics.h>
#include <arch/thread.h>
#include <communication/communication.h>
#include <gvt/gvt.h>
#include <statistics/statistics.h>
#include <arch/x86/linux/cross_state_manager/cross_state_manager.h>
#include <queues/xxhash.h>

/// This is used to keep track of how many LPs were bound to the current KLT
__thread unsigned int n_prc_per_thread;

/// This is a per-thread variable pointing to the block state of the LP currently scheduled
__thread struct lp_struct *current;

/// This is a list to keep high-priority messages yet to be processed
static __thread list(msg_t) hi_prio_list;

/**
 * This is a per-thread variable telling what is the event that should be executed
 * when activating an LP. It is incorrect to rely on current->bound, as there
 * are cases (such as the silent execution) in which we have a certain bound set,
 * but we execute a different event.
 *
 * @todo We should uniform this behaviour, and drop current_evt, as this might be
 *       misleading when reading the code.
 */
__thread msg_t *current_evt;

// Timer per thread used to gather statistics on execution time for
// controllers and processers in asymmetric executions
static __thread timer timer_local_thread;

// Pointer to an array of longs which are used as an accumulator of time
// spent idle in asym_schedule or asym_process
long *total_idle_microseconds;
/*
* This function initializes the scheduler. In particular, it relies on MPI to broadcast to every simulation kernel process
* which is the actual scheduling algorithm selected.
*
* @author Francesco Quaglia
*
* @param sched The scheduler selected initially, but master can decide to change it, so slaves must rely on what master send to them
*/
void scheduler_init(void)
{
#ifdef HAVE_PREEMPTION
	preempt_init();
#endif
}

/**
* This function finalizes the scheduler
*
* @author Alessandro Pellegrini
*/
void scheduler_fini(void)
{
#ifdef HAVE_PREEMPTION
	preempt_fini();
#endif

	foreach_lp(lp) {
		rsfree(lp->queue_in);
		rsfree(lp->queue_out);
		rsfree(lp->queue_states);
		rsfree(lp->bottom_halves);
		rsfree(lp->rendezvous_queue);

		// Destroy stacks
		rsfree(lp->stack);

		rsfree(lp);
	}

	rsfree(lps_blocks);
	rsfree(lps_bound_blocks);
}

/**
* This is a LP main loop. It s the embodiment of the userspace thread implementing the logic of the LP.
* Whenever an event is to be scheduled, the corresponding metadata are set by the schedule() function,
* which in turns calls activate_LP() to execute the actual context switch.
* This ProcessEvent wrapper explicitly returns control to simulation kernel user thread when an event
* processing is finished. In case the LP tries to access state data which is not belonging to its
* simulation state, a SIGSEGV signal is raised and the LP might be descheduled if it is not safe
* to perform the remote memory access. This is the only case where control is not returned to simulation
* thread explicitly by this wrapper.
*
* @param args arguments passed to the LP main loop. Currently, this is not used.
*/
void LP_main_loop(void *args)
{
#ifdef EXTRA_CHECKS
	unsigned long long hash1, hash2;
	hash1 = hash2 = 0;
#endif

	(void)args;		// this is to make the compiler stop complaining about unused args

	// Save a default context
	context_save(&current->default_context);

	while (true) {

#ifdef EXTRA_CHECKS
		if (current->bound->size > 0) {
			hash1 = XXH64(current_evt->event_content, current_evt->size, current->gid);
		}
#endif

		timer event_timer;
		timer_start(event_timer);

		// Process the event
        if(&abm_settings){
			ProcessEventABM();
		}else if (&topology_settings){
			ProcessEventTopology();
		}else{
			switch_to_application_mode();
			current->ProcessEvent(current->gid.to_int,
				      current_evt->timestamp, current_evt->type,
				      current_evt->event_content,
				      current_evt->size,
				      current->current_base_pointer);
			switch_to_platform_mode();
		}
		int delta_event_timer = timer_value_micro(event_timer);

#ifdef EXTRA_CHECKS
		if (current->bound->size > 0) {
			hash2 =
			    XXH64(current_evt->event_content, current_evt->size,
				  current->gid);
		}

		if (hash1 != hash2) {
			rootsim_error(true,
				      "Error, LP %d has modified the payload of event %d during its processing. Aborting...\n",
				      current->gid, current->bound->type);
		}
#endif

		statistics_post_data(current, STAT_EVENT, 1.0);
		statistics_post_data(current, STAT_EVENT_TIME,
				     delta_event_timer);

		// Give back control to the simulation kernel's user-level thread
		context_switch(&current->context, &kernel_context);
	}
}


void initialize_processing_thread(void) {
    hi_prio_list = new_list(msg_t);
}

void initialize_worker_thread(void)
{
    msg_t *init_event;

	// Divide LPs among worker threads, for the first time here
	rebind_LPs();
	if (master_thread() && master_kernel()) {
		printf("Initializing LPs... ");
		fflush(stdout);
	}

    if(rootsim_config.num_controllers == 0) {
        thread_barrier(&all_thread_barrier);
    } else {
        thread_barrier(&controller_barrier);
    }

    if (master_thread() && master_kernel())
        printf("done\n");

    // Schedule an INIT event to the newly instantiated LP
    // We need two separate foreach_bound_lp here, because
    // in this way we are sure that there is at least one
    // event to be used as the bound and we do not have to make
    // any check on null throughout the scheduler code.

    foreach_bound_lp(lp) {
        pack_msg(&init_event, lp->gid, lp->gid, INIT, 0.0, 0.0, 0, NULL);
        init_event->mark = generate_mark(lp);
        list_insert_head(lp->queue_in, init_event);
        lp->state_log_forced = true;
    }

    // Worker Threads synchronization barrier: they all should start working together
	if(rootsim_config.num_controllers == 0) {
		thread_barrier(&all_thread_barrier);
	} else {
		thread_barrier(&controller_barrier);
	}

    foreach_bound_lp(lp) {
        schedule_on_init(lp);
    }

	if(rootsim_config.num_controllers == 0) {
		thread_barrier(&all_thread_barrier);
	} else {
		thread_barrier(&controller_barrier);
	}


#ifdef HAVE_PREEMPTION
	if (!rootsim_config.disable_preemption)
		enable_preemption();
#endif

}

/**
* This function is the application-level ProcessEvent() callback entry point.
* It allows to specify which lp must be scheduled, specifying its lvt, its event
* to be executed and its simulation state.
* This provides a general entry point to application-level code, to be used
* if the LP is in forward execution, in coasting forward or in initialization.
*
* @author Alessandro Pellegrini
*
* @date November 11, 2013
*
* @param next A pointer to the lp_struct of the LP which has to be activated
* @param evt A pointer to the event to be processed by the LP
*/
void activate_LP(struct lp_struct *next, msg_t * evt)
{

	// Notify the LP main execution loop of the information to be used for actual simulation
	current = next;
	current_evt = evt;

//      #ifdef HAVE_PREEMPTION
//      if(!rootsim_config.disable_preemption)
//              enable_preemption();
//      #endif

#ifdef HAVE_CROSS_STATE
	// Activate memory view for the current LP
	lp_alloc_schedule();
#endif

    if (unlikely(is_blocked_state(next->state))) {
		rootsim_error(true, "Critical condition: LP %d has a wrong state: %d. Aborting...\n",
			      next->gid.to_int, next->state);
	}

	context_switch(&kernel_context, &next->context);

    current->last_processed = evt;

//      #ifdef HAVE_PREEMPTION
//        if(!rootsim_config.disable_preemption)
//                disable_preemption();
//        #endif

#ifdef HAVE_CROSS_STATE
	// Deactivate memory view for the current LP if no conflict has arisen
	if (!is_blocked_state(next->state)) {
//              printf("Deschedule %d\n",lp);
		lp_alloc_deschedule();
	}
#endif

	current = NULL;
}

bool check_rendevouz_request(struct lp_struct *lp)
{
	msg_t *temp_mess;

	if (lp->state != LP_STATE_WAIT_FOR_SYNCH)
		return false;

	if (lp->bound != NULL && list_next(lp->bound) != NULL) {
		temp_mess = list_next(lp->bound);
		return temp_mess->type == RENDEZVOUS_START && lp->wait_on_rendezvous > temp_mess->rendezvous_mark;
	}

	return false;
}

/**
 * This is the core processing routine of PTs
 *
 * @Author: Stefano Conoci
 * @Author: Alessandro Pellegrini
 */
void asym_process(void) {
    msg_t *msg;
    msg_t *msg_hi;
    msg_t *hi_prio_msg;
    struct lp_struct *lp;

    timer_start(timer_local_thread);

    // We initially check for high priority msgs. If one is present,
    // we process it and then return. In this way, the next call to
    // asym_process() will again check for high priority events, making
    // them really high priority.
    msg = pt_get_hi_prio_msg();
    if(msg != NULL) {
        list_insert_tail(hi_prio_list, msg);

        // Never change this return to anything else: we call asym_process()
        // within asym_process() to forcely match a high priority queue when
        // there could be a priority inversion between hi and lo prio ports.
        return;
    }

    // No high priority message. Get an event to process.
    msg = pt_get_lo_prio_msg();

    // My queue might be empty...
    if(msg == NULL){
        //printf("Empty queue for PT %d\n", tid);
        total_idle_microseconds[tid] += timer_value_micro(timer_local_thread);
        return;
    }

    // Check if we picked a stale message
    hi_prio_msg = list_head(hi_prio_list);
    while(hi_prio_msg != NULL) {
        if(gid_equals(msg->receiver, hi_prio_msg->receiver) && !is_control_msg(msg->type) && msg->timestamp > hi_prio_msg->timestamp) {
            // TODO: switch to bool
            if(hi_prio_msg->event_content[0] == 0) {
                hi_prio_msg->event_content[0] = 1;

                //printf("Sending a ROLLBACK_ACK for LP %d at time %f\n", msg->receiver.id, msg->timestamp);
                // atomic_sub(&notice_count, 1);

                // Send back an ack to start processing the actual rollback operation
                pack_msg(&msg, msg->receiver, msg->receiver, ASYM_ROLLBACK_ACK, msg->timestamp, msg->timestamp, 0, NULL);
                msg->message_kind = control;
                pt_put_out_msg(msg);
            }
            return;
        }
        hi_prio_msg = list_next(hi_prio_msg);
    }

    // Match a ROLLBACK_NOTICE with a ROLLBACK_BUBBLE and remove it from the
    // local hi_priority_list.
    if(is_control_msg(msg->type) && msg->type == ASYM_ROLLBACK_BUBBLE) {
    //	asym_process();

        ariprova:
        hi_prio_msg = list_head(hi_prio_list);
        while(hi_prio_msg != NULL) {
            if(msg->mark == hi_prio_msg->mark) {
            //msg_release(msg);

                // TODO: switch to bool
                if(hi_prio_msg->event_content[0] == 0) {
                //printf("Sending a ROLLBACK_ACK (2)  for LP %d at time %f\n", msg->receiver.id, msg->timestamp);
                    // atomic_sub(&notice_count, 1);

                    // Send back an ack to start processing the actual rollback operation
                    pack_msg(&msg, msg->receiver, msg->receiver, ASYM_ROLLBACK_ACK, msg->timestamp, msg->timestamp, 0, NULL);
                    msg->message_kind = control;
                    pt_put_out_msg(msg);
                }

                list_delete_by_content(hi_prio_list, hi_prio_msg);
                //msg_release(hi_prio_msg);
                return;
            }
            hi_prio_msg = list_next(hi_prio_msg);
        }

        msg_hi = pt_get_hi_prio_msg();
        while(msg_hi != NULL) {
            list_insert_tail(hi_prio_list, msg_hi);
            msg_hi = pt_get_hi_prio_msg();
        }
        goto ariprova;

        fprintf(stderr, "Cannot match a bubble!\n");
        abort();
    }

    lp = find_lp_by_gid(msg->receiver);

    // TODO: find a way to set the LP to RUNNING without incurring in a race condition with the CT

    // Process this event
    activate_LP(lp, msg);
    my_processed_events++;
    msg->unprocessed = false;

    // Send back to the controller the (possibly) generated events
    asym_send_outgoing_msgs(lp);

    // Log the state, if needed
    // TODO: we make the PT take a checkpoint. The optimality would be to let the CT
    // take a checkpoint, but we need some sort of synchronization which is out of the
    // scope of the current development phase here.
    LogState(lp);

  //printf("asym_process for PT %d completed in %d millisecond\n", tid, timer_value_milli(timer_local_thread));

}
/**
* This is the asymmetric scheduler. This function extracts a bunch of events
* to be processed by LPs bound to a controller and sends them to processing
* threads for later execution. Rollbacks are executed by the controller, and
* are triggered here in a lazy fashion.
*
* @Author Stefano Conoci
* @Author Alessandro Pellegrini
*/
#define __cmp_ts(a, b) (((a).timestamp > (b).timestamp) - ((b).timestamp > (a).timestamp))

void asym_schedule(void) {
    LID_t lid;
    msg_t *event, *curr_event, extracted_event;
    msg_t *rollback_control;
    struct lp_struct *lp,*curr_lp;
    unsigned int port_events_to_fill[n_cores];
    unsigned int port_current_size[n_cores];
    unsigned int i, j, thread_id_mask;
    unsigned int events_to_fill = 0;
    char first_encountered = 0;
    unsigned long long mark;
    int toAdd = 0, delta_utilization = 0;
    unsigned int sent_events = 0;
    unsigned int sent_notice = 0;

//	printf("NOTICE COUNT: %d\n", atomic_read(&notice_count));

    timer_start(timer_local_thread);

   /** Compute utilization rate of the input ports since the last call to asym_schedule
    *  and resize the ports if necessary */
    for(i = 0; i < Threads[tid]->num_PTs; i++) {
        Thread_State* pt = Threads[tid]->PTs[i];
        int port_size = pt->port_batch_size;
        port_current_size[pt->tid] = get_port_current_size(pt->input_port[PORT_PRIO_LO]);
        delta_utilization = port_size - port_current_size[pt->tid];
        if(delta_utilization < 0){
            delta_utilization = 0;
        }
        double utilization_rate = (double) delta_utilization / (double) port_size;

        /* DEBUG
        printf("Port current size: %u, port size %u, delta_utilization %d\n", port_current_size[pt->tid], port_size, delta_utilization);
        printf("Input port size of PT %u: %d (utilization factor: %f)\n", pt->tid, port_current_size[pt->tid], utilization_rate);
           DEBUG */

        // If utilization rate is too high, the size of the port should be increased
        if(utilization_rate > UPPER_PORT_THRESHOLD){
            if(pt->port_batch_size <= (MAX_PORT_SIZE - BATCH_STEP)){
                pt->port_batch_size+=BATCH_STEP;
            }else if(pt->port_batch_size < MAX_PORT_SIZE){
                pt->port_batch_size++;
            }
            //printf("Increased port size of PT %u to %d\n", pt->tid, pt->port_batch_size);
        }
            // If utilization rate is too low, the size of the port should be decreased
        else if (utilization_rate < LOWER_PORT_THRESHOLD){
            if(pt->port_batch_size > BATCH_STEP){
                pt->port_batch_size-=BATCH_STEP;
            }else if(pt->port_batch_size > 1){
                pt->port_batch_size--;
            }
            //printf("Reduced port size of PT %u to %d\n", pt->tid, pt->port_batch_size);
        }
    }

    //printf("asym_schedule: port resize completed for controller %d at millisecond %d\n", tid, timer_value_milli(timer_local_thread));

    /** Compute the total number of events necessary to fill all
     *  the input ports, considering the current batch value
     *  and the current number of events yet to be processed in
     *  each port */
    for(i = 0; i < Threads[tid]->num_PTs; i++){
        Thread_State* pt = Threads[tid]->PTs[i];
        toAdd = pt->port_batch_size - port_current_size[pt->tid];
        // Might be negative as we reduced the port size, and it might
        // have been already full
        if(toAdd > 0){
            port_events_to_fill[pt->tid] = toAdd;
            events_to_fill += toAdd;
            //printf("Adding toAdd = %d to events_to_fill\n", toAdd);
        } else {
            port_events_to_fill[pt->tid] = 0;
        }
    }

    //printf("asym_schedule: events_to_fill computed for controller %d at millisecond %d\n", tid, timer_value_milli(timer_local_thread));


    /** Create a copy of lps_bound_blocks in asym_lps_mask which will
     * be modified during scheduling in order to jump LPs bound to PT
     * for whom the input port is already filled */
    memcpy(asym_lps_mask, lps_bound_blocks, sizeof(struct lp_struct *) * n_prc_per_thread);
    foreach_bound_mask_lp(lp_a){
        Thread_State *pt = Threads[lp_a->processing_thread];
        if(port_current_size[pt->tid] >= pt->port_batch_size) {
            lp_a = NULL;
        }
    }
    //printf("asym_schedule: asym_lps_mask computed for controller %d at millisecond %d\n", tid, timer_value_milli(timer_local_thread));

     /** Put up to MAX_LP_EVENTS_PER_BATCH events for each LP in the priority
      * queue events_heap */
    if(rootsim_config.scheduler == BATCH_LOWEST_TIMESTAMP){
        // Clean the priority queue
      /*bzero(Threads[tid]->events_heap->nodes, Threads[tid]->events_heap->len*sizeof(node_heap_t));
        Threads[tid]->events_heap->len = 0;*/
        bzero(array_items(Threads[tid]->heap),sizeof(*array_items(Threads[tid]->heap))*array_count(Threads[tid]->heap));
        array_count(Threads[tid]->heap) = 0;

        foreach_bound_mask_lp(lp_b){
            if(lp_b != NULL && !is_blocked_state(lp_b->state)){
                if(lp_b->bound == NULL && !list_empty(lp_b->queue_in)){
                    curr_event = list_head(lp_b->queue_in);
                }
                else{
                    curr_event = list_next(lp_b->bound);
                }

                j = 0;
                while(curr_event != NULL && j < MAX_LP_EVENTS_PER_BATCH){
                    if(curr_event->timestamp >= 0){

                        heap_insert(Threads[tid]->heap,*curr_event,__cmp_ts);

                        //heap_push(Threads[tid]->events_heap, curr_event->timestamp, curr_event);
                        // printf("Pushing to priority queue event from LP %d with timestamp %lf\n",
                        //lid_to_int(lp->lid), curr_event->timestamp);
                        j++;
                        curr_event = curr_event->next;
                    }
                }
            }
        }
    }

    // Set to 0 all the curr_scheduled_events array
    bzero(Threads[tid]->curr_scheduled_events, sizeof(int)*n_prc);

    //printf("events_to_fill=%d\n", events_to_fill);

    for(i = 0; i < events_to_fill; i++) {

     /* printf("SCHED: mask: ");
		int jk;
		for(jk = 0; jk < n_prc_per_thread; jk++) {
			printf("%p, ", asym_lps_mask[jk]);
		}
		puts(""); */

#ifdef HAVE_CROSS_STATE
        bool resume_execution = false;
#endif


        // Find next LP to be executed, depending on the chosen scheduler
        switch (rootsim_config.scheduler) {

            case SCHEDULER_STF:
                lp = asym_smallest_timestamp_first();
                break;

            case BATCH_LOWEST_TIMESTAMP:

                extracted_event = heap_extract(Threads[tid]->heap,__cmp_ts);
                curr_event = &extracted_event;
                //curr_event = heap_pop(Threads[tid]->events_heap);

                //printf("Retrieving from priority queue event from LP %d with timestamp %lf\n",
                //		lid_to_int(GidToLid(curr_event->sender)), curr_event->timestamp);
                int found = 0;
                    lp = NULL;
                while(curr_event != NULL && !found){
                    curr_lp = find_lp_by_gid(curr_event->receiver);
                    if(port_events_to_fill[curr_lp->processing_thread] > 0 &&
                            curr_lp->state != LP_STATE_WAIT_FOR_ROLLBACK_ACK){
                        found = 1;
                        lp = curr_lp;
                        lid = curr_lp->lid;
                    }
                    else{
                        extracted_event = heap_extract(Threads[tid]->heap,__cmp_ts);
                        //curr_event = heap_pop(Threads[tid]->events_heap);
                        lp = NULL;
                    }
                }
                break;

            default:
                lp = asym_smallest_timestamp_first();
        }

        // No logical process found with events to be processed
        if (unlikely(lp == NULL)) {
            statistics_post_data(NULL, STAT_IDLE_CYCLES, 1.0);
            return;
        }

        if(rootsim_config.scheduler == SCHEDULER_STF)
            lid = lp->lid;

        // If we have to rollback
        if(lp->state == LP_STATE_ROLLBACK) {
            // Get a mark for the current batch of control messages
            mark = generate_mark(lp);

            //printf("Sending a ROLLBACK_NOTICE for LP %d at time %f\n", lid.id, lvt(lid));
            sent_notice++;

            // atomic_add(&notice_count, 1);

            // Send rollback notice in the high priority port
            pack_msg(&rollback_control, lp->gid, lp->gid, ASYM_ROLLBACK_NOTICE, lvt(lp), lvt(lp), sizeof(char), &first_encountered);
            rollback_control->message_kind = control;
            rollback_control->mark = mark;
            pt_put_hi_prio_msg(lp->processing_thread, rollback_control);

            // Set the LP to a blocked state
            lp->state = LP_STATE_WAIT_FOR_ROLLBACK_ACK;

            /** Notify the PT in charge of managing this LP that the rollback is complete and
             * events to the LP should not be discarded anymore */
            // printf("Sending a ROLLBACK_BUBBLE for LP %d at time %f\n", lid.id, lvt(lid));

            pack_msg(&rollback_control, lp->gid, lp->gid, ASYM_ROLLBACK_BUBBLE, lvt(lp), lvt(lp), 0, NULL);
            rollback_control->message_kind = control;
            rollback_control->mark = mark;
            pt_put_lo_prio_msg(lp->processing_thread, rollback_control);

            continue;
        }

        if(lp->state == LP_STATE_ROLLBACK_ALLOWED) {
            // Rollback the LP and send antimessages
            lp->state = LP_STATE_ROLLBACK;
            rollback(lp);
            lp->state = LP_STATE_READY;
            //send_outgoing_msgs(lid);

            // Prune the retirement queue for this LP
           while(true) {
                event = list_head(lp->retirement_queue);
                if(event == NULL) {
                    break;
                }
                list_delete_by_content(lp->retirement_queue, event);
                msg_release(event);
            }

            continue;
        }

        if(!is_blocked_state(lp->state) && lp->state != LP_STATE_READY_FOR_SYNCH){
            event = advance_to_next_event(lp);
        } else {
            event = lp->bound;
        }

        /** Sanity check: if we get here, it means that lid is a LP which has
         *  at least one event to be executed. If advance_to_next_event() returns
         *  NULL, it means that lid has no events to be executed. This is
         *  a critical condition and we abort. */
        if(unlikely(event == NULL)) {
            rootsim_error(true, "Critical condition: LP %d seems to have events to be processed, but I cannot find them. Aborting...\n", lp->gid);
        }

        if (unlikely(!process_control_msg(event))) {
            return;
        }

#ifdef HAVE_CROSS_STATE
        // TODO: we should change this by managing the state internally to activate_LP, as this
		// would uniform the code across symmetric/asymmetric implementations.
		// In case we are resuming an interrupted execution, we keep track of this.
		// If at the end of the scheduling the LP is not blocked, we can unblock all the remote objects
		if(is_blocked_state(LPS(lid)->state) || LPS(lid)->state == LP_STATE_READY_FOR_SYNCH) {
			resume_execution = true;
		}
#endif

        thread_id_mask = lp->processing_thread;

        // Put the event in the low prio queue of the associated PT
        event->unprocessed = true;
        pt_put_lo_prio_msg(thread_id_mask, event);
        //printf("asym_scheduler: %d/%d Hi prio: %d tid: %d\n ", atomic_read(&Threads[tid]->input_port[1]->size),  Threads[tid]->port_batch_size, atomic_read(&Threads[tid]->input_port[0]->size), tid);
        sent_events++;

        // Modify port_events_to_fill to reflect last message sent
        port_events_to_fill[thread_id_mask]--;

        unsigned int lp_id = lp->lid.to_int;

        if(rootsim_config.scheduler == SCHEDULER_STF){
            // Increase curr_scheduled_events, and set pointer to
            // respective LP to NULL if exceeded MAX_LP_EVENTS_PER_BATCH
            Threads[tid]->curr_scheduled_events[lp_id] = Threads[tid]->curr_scheduled_events[lp_id]+1;
            //printf("curr_scheduled_events[%u] = %d\n", lp_id, Threads[tid]->curr_scheduled_events[lp_id]);
            if(Threads[tid]->curr_scheduled_events[lp_id] >= MAX_LP_EVENTS_PER_BATCH){
                foreach_bound_mask_lp(lp_c){
                    if(lp_c!= NULL && lid_equals(lp_c->lid,lid)){  //undefined behavior
                        lp_c = NULL;
                        //printf("Setting to NULL pointer to LP %d\n", lp_id);
                        break;
                    }
                }
            }

            //printf("asym_schedule: event sent for controller %d at millisecond %d\n", tid, timer_value_milli(timer_local_thread));

            /** If one port becomes full, should set all pointers to LP
             *  mapped to the PT of the respective port to NULL
             *  printf("thread_id_mask: %u\n", thread_id_mask); */
            if(port_events_to_fill[thread_id_mask] == 0){
                foreach_bound_mask_lp(lp_d){
                    if(lp_d != NULL && lp_d->processing_thread == thread_id_mask)
                        lp_d = NULL;
                }
            }

        }

#ifdef HAVE_CROSS_STATE
        if(resume_execution && !is_blocked_state(LPS(lid)->state)) {
			printf("ECS event is finished mark %llu !!!\n", LPS(lid)->wait_on_rendezvous);
			fflush(stdout);
			unblock_synchronized_objects(lid);

			// This is to avoid domino effect when relying on rendezvous messages
			// TODO: I'm not quite sure if with asynchronous PTs' this way to code ECS-related checkpoints still holds
			force_LP_checkpoint(lid);
		}
#endif
    }

    if(sent_events == 0){
        total_idle_microseconds[tid] += timer_value_micro(timer_local_thread);
    }

 /* printf("Sent events: %u\n", sent_events);
    printf("Sent rollback notice: %u\n", sent_notice);
    printf("asym_schedule for controller %d completed in %d milliseconds\n", tid, timer_value_milli(timer_local_thread)); */
}

/**
* This function checks which LP must be activated (if any),
* and in turn activates it. This is used only to support forward execution.
*
* @author Alessandro Pellegrini
*/
void schedule(void)
{
	struct lp_struct *next;
	msg_t *event;

#ifdef HAVE_CROSS_STATE
	bool resume_execution = false;
#endif

	// Find the next LP to be scheduled
	switch (rootsim_config.scheduler) {

	case SCHEDULER_STF:
		next = smallest_timestamp_first();
		break;

	default:
		rootsim_error(true, "unrecognized scheduler!");
	}

	// No logical process found with events to be processed
	if (next == NULL) {
		statistics_post_data(NULL, STAT_IDLE_CYCLES, 1.0);
		return;
	}

	// If we have to rollback
	if (next->state == LP_STATE_ROLLBACK) {
		rollback(next);
		next->state = LP_STATE_READY;
		send_outgoing_msgs(next);
		return;
	}

	if (!is_blocked_state(next->state)
	    && next->state != LP_STATE_READY_FOR_SYNCH) {
		event = advance_to_next_event(next);
	} else {
		event = next->bound;
	}

	// Sanity check: if we get here, it means that lid is a LP which has
	// at least one event to be executed. If advance_to_next_event() returns
	// NULL, it means that lid has no events to be executed. This is
	// a critical condition and we abort.
	if (unlikely(event == NULL)) {
		rootsim_error(true,
			      "Critical condition: LP %d seems to have events to be processed, but I cannot find them. Aborting...\n",
			      next->gid);
	}

	if (unlikely(!process_control_msg(event))) {
		return;
	}
#ifdef HAVE_CROSS_STATE
	// In case we are resuming an interrupted execution, we keep track of this.
	// If at the end of the scheduling the LP is not blocked, we can unblock all the remote objects
	if (is_blocked_state(next->state) || next->state == LP_STATE_READY_FOR_SYNCH) {
		resume_execution = true;
	}
#endif

	// Schedule the LP user-level thread
	if (next->state == LP_STATE_READY_FOR_SYNCH)
		next->state = LP_STATE_RUNNING_ECS;
	else
		next->state = LP_STATE_RUNNING;
	activate_LP(next, event);

	if (!is_blocked_state(next->state)) {
		next->state = LP_STATE_READY;
		send_outgoing_msgs(next);
	}
#ifdef HAVE_CROSS_STATE
	if (resume_execution && !is_blocked_state(next->state)) {
		//printf("ECS event is finished mark %llu !!!\n", next->wait_on_rendezvous);
		fflush(stdout);
		unblock_synchronized_objects(next);

		// This is to avoid domino effect when relying on rendezvous messages
		force_LP_checkpoint(next);
	}
#endif

	// Log the state, if needed
	LogState(next);
}

void schedule_on_init(struct lp_struct *next)
{
	msg_t *event;

#ifdef HAVE_CROSS_STATE
	bool resume_execution = false;
#endif

	event = list_head(next->queue_in);
	next->bound = event;


	// Sanity check: if we get here, it means that lid is a LP which has
	// at least one event to be executed. If advance_to_next_event() returns
	// NULL, it means that lid has no events to be executed. This is
	// a critical condition and we abort.
	if (unlikely(event == NULL) || event->type != INIT) {
		rootsim_error(true,
			      "Critical condition: LP %d should have an INIT event but I cannot find it. Aborting...\n",
			      next->gid);
	}

#ifdef HAVE_CROSS_STATE
	// In case we are resuming an interrupted execution, we keep track of this.
	// If at the end of the scheduling the LP is not blocked, we can unblock all the remote objects
	if (is_blocked_state(next->state) || next->state == LP_STATE_READY_FOR_SYNCH) {
		resume_execution = true;
	}
#endif

	next->state = LP_STATE_RUNNING;

	activate_LP(next, event);

	if (!is_blocked_state(next->state)) {
		next->state = LP_STATE_READY;
		send_outgoing_msgs(next);
	}
#ifdef HAVE_CROSS_STATE
	if (resume_execution && !is_blocked_state(next->state)) {
		//printf("ECS event is finished mark %llu !!!\n", next->wait_on_rendezvous);
		fflush(stdout);
		unblock_synchronized_objects(next);

		// This is to avoid domino effect when relying on rendezvous messages
		force_LP_checkpoint(next);
	}
#endif

	// Log the state, if needed
	LogState(next);
}
