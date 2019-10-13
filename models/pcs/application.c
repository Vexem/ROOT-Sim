#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <ROOT-Sim.h>

#include "application.h"

bool 	pcs_statistics = false,
	fading_check = false,  // Is the model set up to periodically recompute the fading of all ongoing calls?
	variable_ta = false; // Should the call interarrival frequency change depending on the current time?
unsigned complete_calls = COMPLETE_CALLS,
	channels_per_cell = CHANNELS_PER_CELL; // Total channels per each cell
double 	ref_ta = TA,   // Initial call interarrival frequency (same for all cells)
	ta_duration = TA_DURATION, // Average duration of a call
	ta_change = TA_CHANGE; // Average time after which a call is diverted to another cell

enum {
	OPT_STAT = 128, /// this tells argp to not assign short options
	OPT_TA,
	OPT_TAD,
	OPT_TAC,
	OPT_CPC,
	OPT_CC,
	OPT_FR,
	OPT_VTA,
};

const struct argp_option model_options[] = {
		{"pcs-statistics", OPT_STAT, NULL, 0, NULL, 0},
		{"ta", OPT_TA, "FLOAT", 0, NULL, 0},
		{"ta-duration", OPT_TAD, "FLOAT", 0, NULL, 0},
		{"ta-change", OPT_TAC, "FLOAT", 0, NULL, 0},
		{"channels-per-cell", OPT_CPC, "UINT", 0, NULL, 0},
		{"complete-calls", OPT_CC, "INT", 0, NULL, 0},
		{"fading-recheck", OPT_FR, NULL, 0, NULL, 0},
		{"variable-ta", OPT_VTA, NULL, 0, NULL, 0},
		{0}
};

// this macro abuse looks so elegant though...
#define HANDLE_CASE(label, fmt, var)	\
	case label: \
		if(sscanf(arg, fmt, &var) != 1){ \
			return ARGP_ERR_UNKNOWN; \
		} \
	break

static error_t model_parse (int key, char *arg, struct argp_state *state) {
	(void)state;
	
	switch (key) {
		HANDLE_CASE(OPT_TA, "%lf", ref_ta);
		HANDLE_CASE(OPT_TAD, "%lf", ta_duration);
		HANDLE_CASE(OPT_TAC, "%lf", ta_change);
		HANDLE_CASE(OPT_CPC, "%u", channels_per_cell);
		HANDLE_CASE(OPT_CC, "%u", complete_calls);

		case OPT_STAT:
			pcs_statistics = true;
			break;
		case OPT_FR:
			fading_check = true;
			break;;
		case OPT_VTA:
			variable_ta = true;
			break;

		case ARGP_KEY_SUCCESS:
			printf("CURRENT CONFIGURATION:\ncomplete calls: %d\nTA: %f\nta_duration: %f\nta_change: %f\nchannels_per_cell: %d\nfading_recheck: %d\nvariable_ta: %d\n",
				complete_calls, ref_ta, ta_duration, ta_change, channels_per_cell, fading_check, variable_ta);
			fflush(stdout);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

#undef HANDLE_CASE

struct argp model_argp = {model_options, model_parse, NULL, NULL, NULL, NULL, NULL};

struct _topology_settings_t topology_settings = {.default_geometry = TOPOLOGY_HEXAGON};

void ProcessEvent(unsigned int curr_lp, simtime_t event_ts, int event_type, event_content_type *event_content, unsigned int size, void *ptr) {
	(void)size;
	
	unsigned int w;

	printf("MODEL: LP%d executing event (type %d) with ts %f and call term time: %f\n", curr_lp, event_type, event_ts, event_content->call_term_time);

	event_content_type new_event_content;

	new_event_content.cell = -1;
	new_event_content.channel = -1;
	new_event_content.call_term_time = -1;

	simtime_t handoff_time;
	simtime_t timestamp = 0;

	lp_state_type *state;
	state = (lp_state_type*)ptr;

	if(state != NULL) {
		state->lvt = event_ts;
		state->executed_events++;
	}

	if(event_type == HANDOFF_LEAVE || event_type == HANDOFF_RECV) {
	    if(event_content->call_term_time == 000000) {
            fprintf(stderr,"MODEL: LP%d, type: %d, WARNING: CALL TERM TIME for event_ts %f = %f\n", curr_lp, event_type, event_ts, event_content->call_term_time);
            abort();
        }
    }
	switch(event_type) {

		case INIT:

			// Initialize the LP's state
			state = (lp_state_type *)malloc(sizeof(lp_state_type));
			if (state == NULL){
				printf("Out of memory!\n");
				exit(EXIT_FAILURE);
			}

			SetState(state);

			bzero(state, sizeof(lp_state_type));

			state->channel_counter = channels_per_cell;
			state->ta = ref_ta;

			// Setup channel state
			state->channel_state = malloc(sizeof(unsigned int) * 2 * (CHANNELS_PER_CELL / BITS + 1));
			for (w = 0; w < state->channel_counter / (sizeof(int) * 8) + 1; w++)
				state->channel_state[w] = 0;

			// Start the simulation
			timestamp = (simtime_t) (20 * Random());
            printf("MODEL CASE <%d - INIT>: FIRST START CALL event (receiver: LP%d, type: %d, ts: %f) \n",
                   event_type, curr_lp, START_CALL, timestamp);
			ScheduleNewEvent(curr_lp, timestamp, START_CALL, NULL, 0);

			// If needed, start the first fading recheck
			//if (state->fading_recheck) {
			//	timestamp = (simtime_t) (FADING_RECHECK_FREQUENCY * Random());
			//	ScheduleNewEvent(curr_lp, timestamp, FADING_RECHECK, NULL, 0);
		//	}

			break;


		case START_CALL:

			state->arriving_calls++;

			if (state->channel_counter == 0) {
				state->blocked_on_setup++;
			} else {

				state->channel_counter--;

				new_event_content.channel = allocation(state);
				new_event_content.from = curr_lp;
				new_event_content.sent_at = event_ts;

//				printf("(%d) allocation %d at %f\n", curr_lp, new_event_content.channel, event_ts);

				// Determine call duration
				switch (DURATION_DISTRIBUTION) {

					case UNIFORM:
						new_event_content.call_term_time = event_ts + (simtime_t)(ta_duration * Random());
						break;

					case EXPONENTIAL:
						new_event_content.call_term_time = event_ts + (simtime_t)(Expent(ta_duration));
						break;

					default:
 						new_event_content.call_term_time = event_ts + (simtime_t) (5 * Random() );
				}

				// Determine whether the call will be handed-off or not
				switch (CELL_CHANGE_DISTRIBUTION) {

					case UNIFORM:

						handoff_time  = event_ts + (simtime_t)((ta_change) * Random());
						break;

					case EXPONENTIAL:
						handoff_time = event_ts + (simtime_t)(Expent(ta_change));
						break;

					default:
						handoff_time = event_ts + (simtime_t)(5 * Random());

				}

				if(new_event_content.call_term_time < handoff_time) {
                    printf("MODEL CASE <%d - START_CALL>: new END_CALL event scheduled (receiver: LP%d, type: %d, ts: %f, ctt: %f) \n",
                           event_type, curr_lp, END_CALL, new_event_content.call_term_time, new_event_content.call_term_time);
					ScheduleNewEvent(curr_lp, new_event_content.call_term_time, END_CALL, &new_event_content, sizeof(new_event_content));
				} else {
					new_event_content.cell = FindReceiver();
                    printf("MODEL CASE <%d - START_CALL>: new HANDOFF_LEAVE event scheduled (receiver: LP%d, type: %d, ts: %f, ctt: %f) \n",
                           event_type, curr_lp, HANDOFF_LEAVE, handoff_time, new_event_content.call_term_time);
					ScheduleNewEvent(curr_lp, handoff_time, HANDOFF_LEAVE, &new_event_content, sizeof(new_event_content));
				}
			}


			if (variable_ta)
				state->ta = recompute_ta(ref_ta, event_ts);

			// Determine the time at which a new call will be issued
			switch (DISTRIBUTION) {

				case UNIFORM:
					timestamp= event_ts + (simtime_t)(state->ta * Random());
					break;

				case EXPONENTIAL:
					timestamp= event_ts + (simtime_t)(Expent(state->ta));
					break;

				default:
					timestamp= event_ts + (simtime_t) (5 * Random());

			}
            printf("MODEL CASE <%d - START_CALL>: new START_CALL event scheduled (receiver: LP%d, type: %d, ts: %f) \n",
                   event_type, curr_lp, START_CALL, timestamp);
			ScheduleNewEvent(curr_lp, timestamp, START_CALL, NULL, 0);

			break;

		case END_CALL:

			state->channel_counter++;
			state->complete_calls++;
			deallocation(curr_lp, state, event_content->channel, event_ts);

			break;

		case HANDOFF_LEAVE:

			state->channel_counter++;
			state->leaving_handoffs++;
			deallocation(curr_lp, state, event_content->channel, event_ts);

			new_event_content.call_term_time =  event_content->call_term_time;
			new_event_content.from = curr_lp;
			new_event_content.dummy = &(state->dummy);
            printf("MODEL CASE <%d - HANDOFF_LEAVE>: new HANDOFF_RECV event scheduled (receiver: LP%d, type: %d, ts: %f, ctt: %f) \n",
                   event_type, event_content->cell, HANDOFF_RECV, event_ts, new_event_content.call_term_time);
			ScheduleNewEvent(event_content->cell, event_ts, HANDOFF_RECV, &new_event_content, sizeof(new_event_content));
			break;

		case HANDOFF_RECV:
			state->arriving_handoffs++;
			state->arriving_calls++;

			if(Random() < 0.3 && curr_lp == 1 && event_content->from == 2){//&& state->dummy_flag == false) {
				*(event_content->dummy) = 1;
				state->dummy_flag = true;
			}

			if (state->channel_counter == 0)
				state->blocked_on_handoff++;
			else {
				state->channel_counter--;

				new_event_content.channel = allocation(state);
				new_event_content.call_term_time = event_content->call_term_time;


				switch (CELL_CHANGE_DISTRIBUTION) {
					case UNIFORM:
						handoff_time  = event_ts + (simtime_t)((ta_change) * Random());

						break;
					case EXPONENTIAL:
						handoff_time = event_ts + (simtime_t)(Expent(ta_change));

						break;
					default:
						handoff_time = event_ts +
                                       (simtime_t) (5 * Random());
				}

				if(new_event_content.call_term_time < handoff_time ) {
                    printf("MODEL CASE <%d - HANDOFF_RECV>: new END_CALL event scheduled (receiver: LP%d, type: %d, ts: %f, ctt: %f) \n",
                           event_type, curr_lp, END_CALL, new_event_content.call_term_time, new_event_content.call_term_time);
					ScheduleNewEvent(curr_lp, new_event_content.call_term_time, END_CALL, &new_event_content, sizeof(new_event_content));
				} else {
					new_event_content.cell = FindReceiver();
                    printf("MODEL CASE <%d - HANDOFF_RECV>: new HANDOFF_LEAVE event scheduled (receiver: LP%d, type: %d, ts: %f, ctt: %f) \n",
                           event_type, curr_lp, HANDOFF_LEAVE, handoff_time, new_event_content.call_term_time);
					ScheduleNewEvent(curr_lp, handoff_time, HANDOFF_LEAVE, &new_event_content, sizeof(new_event_content));
				}
			}


			break;


		case FADING_RECHECK:

/*
			if(state->check_fading)
				state->check_fading = false;
			else
				state->check_fading = true;
*/

			fading_recheck(state);

			timestamp = event_ts + (simtime_t) (FADING_RECHECK_FREQUENCY );
            printf("MODEL CASE <%d - FADING_RECHECK>: new FADING_RECHECK event scheduled (receiver: LP%d, type: %d, ts: %f, ctt: %f) \n",
                   event_type, curr_lp, FADING_RECHECK, timestamp, new_event_content.call_term_time);
			ScheduleNewEvent(curr_lp, timestamp, FADING_RECHECK, NULL, 0);

			break;


		default:
			fprintf(stdout, "PCS: Unknown event type! (curr_lp = %d - event type = %d)\n", curr_lp, event_type);
			abort();

	}
}


bool OnGVT(unsigned int me, lp_state_type *snapshot) {
	(void)me;

	fprintf(stdout,"PT%d: %f%%\n", me, (double)snapshot->complete_calls/complete_calls);

	if (snapshot->complete_calls < complete_calls)
		return false;
	return true;
}
