#include <spacetime.h>
#include "util.h"
#include "inline-util.h"

void *run_worker(void *arg){
	struct thread_params params = *(struct thread_params *) arg;
	uint16_t worker_lid = (uint16_t) params.id;	/* Local ID of this worker thread*/
	uint16_t worker_gid = (uint16_t) (machine_id * WORKERS_PER_MACHINE + params.id);	/* Global ID of this worker thread*/

	int *recv_q_depths, *send_q_depths;
	setup_q_depths(&recv_q_depths, &send_q_depths);
	struct hrd_ctrl_blk *cb = hrd_ctrl_blk_init(worker_gid,	/* local_hid */
												0, -1, /* port_index, numa_node_id */
												0, 0,	/* #conn qps, uc */
												NULL, 0, -1,	/* prealloc conn buf, buf size, key */
												TOTAL_WORKER_UD_QPs, DGRAM_BUFF_SIZE,	/* num_dgram_qps, dgram_buf_size */
												BASE_SHM_KEY + worker_lid, /* key */
												recv_q_depths, send_q_depths); /* Depth of the dgram RECV, SEND Q*/

	/* -----------------------------------------------------
	--------------DECLARATIONS------------------------------
	---------------------------------------------------------*/
	ud_req_inv_t *incoming_invs = (ud_req_inv_t *) cb->dgram_buf;
	ud_req_ack_t *incoming_acks = (ud_req_ack_t *) &cb->dgram_buf[INV_RECV_REQ_SIZE * RECV_INV_Q_DEPTH];
	ud_req_val_t *incoming_vals = (ud_req_val_t *) &cb->dgram_buf[INV_RECV_REQ_SIZE * RECV_INV_Q_DEPTH +
																  ACK_RECV_REQ_SIZE * RECV_ACK_Q_DEPTH];

	///Send declarations
	struct ibv_send_wr send_inv_wr[MAX_SEND_INV_WRS],
					   send_ack_wr[MAX_SEND_ACK_WRS],
					   send_val_wr[MAX_SEND_VAL_WRS],
			  		   send_crd_wr[MAX_SEND_CRD_WRS];

	struct ibv_sge     send_inv_sgl[MAX_PCIE_BCAST_BATCH],
			           send_ack_sgl[MAX_SEND_ACK_WRS],
			           send_val_sgl[MAX_PCIE_BCAST_BATCH], send_crd_sgl;

	uint8_t credits[TOTAL_WORKER_UD_QPs][MACHINE_NUM];

	///Receive declarations
	struct ibv_recv_wr recv_inv_wr[MAX_RECV_INV_WRS],
					   recv_ack_wr[MAX_RECV_ACK_WRS],
					   recv_val_wr[MAX_RECV_VAL_WRS],
			           recv_crd_wr[MAX_RECV_CRD_WRS];

	struct ibv_sge 	   recv_inv_sgl[MAX_RECV_INV_WRS],
			 		   recv_ack_sgl[MAX_RECV_ACK_WRS],
			           recv_val_sgl[MAX_RECV_VAL_WRS], recv_crd_sgl;

	//Only for immediates
	struct ibv_wc      recv_inv_wc[MAX_RECV_INV_WRS],
				       recv_ack_wc[MAX_RECV_ACK_WRS],
			 	       recv_val_wc[MAX_RECV_VAL_WRS],
			           recv_crd_wc[MAX_RECV_CRD_WRS];



	int inv_push_recv_ptr = 0, inv_pull_recv_ptr = -1,
		ack_push_recv_ptr = 0, ack_pull_recv_ptr = -1,
		val_push_recv_ptr = 0, val_pull_recv_ptr = -1,
		crd_push_recv_ptr = 0, crd_pull_recv_ptr = -1;
	int inv_push_send_ptr = 0, ack_push_send_ptr = 0,
		val_push_send_ptr = 0, crd_push_send_ptr = 0;
   	int i;
	//init receiv buffs as empty (not need for CRD since CRD msgs are (immediate) header-only
	for(i = 0; i < RECV_INV_Q_DEPTH; i ++)
        incoming_invs[i].req.opcode = ST_EMPTY;
	for(i = 0; i < RECV_ACK_Q_DEPTH; i ++)
		incoming_acks[i].req.opcode = ST_EMPTY;
	for(i = 0; i < RECV_VAL_Q_DEPTH; i ++)
		incoming_vals[i].req.opcode = ST_EMPTY;

	/* Post receives, we need to do this early */
	if (WRITE_RATIO > 0){
		if(ENABLE_POST_RECV_PRINTS && ENABLE_INV_PRINTS && worker_lid == 0)
			yellow_printf("vvv Post Initial Receives: \033[31mINVs\033[0m %d\n", MAX_RECV_INV_WRS);
		post_receives(cb, MAX_RECV_INV_WRS, ST_INV_BUFF, incoming_invs, &inv_push_recv_ptr);
		if(ENABLE_POST_RECV_PRINTS && ENABLE_VAL_PRINTS && worker_lid == 0)
			yellow_printf("vvv Post Initial Receives: \033[1m\033[32mVALs\033[0m %d\n", MAX_RECV_VAL_WRS);
		post_receives(cb, MAX_RECV_VAL_WRS, ST_VAL_BUFF, incoming_vals, &val_push_recv_ptr);
	}
	setup_qps(worker_gid, cb);

	int inv_ops_i = 0, ack_ops_i = 0, val_ops_i = 0;
	uint16_t outstanding_invs = 0, outstanding_acks = 0, outstanding_vals = 0;

	spacetime_ops_t *ops;
	spacetime_inv_t *inv_recv_ops, *inv_send_ops;
	spacetime_ack_t *ack_recv_ops, *ack_send_ops;
	spacetime_val_t *val_recv_ops, *val_send_ops;

	setup_ops(&ops, &inv_recv_ops, &ack_recv_ops,
			  &val_recv_ops, &inv_send_ops, &ack_send_ops, &val_send_ops);

	///if no inlinig declare & set_up_mrs()
	//struct ibv_mr *inv_mr, *ack_mr, *val_mr, *crd_mr;

	setup_credits(credits, cb, send_crd_wr, &send_crd_sgl, recv_crd_wr, &recv_crd_sgl);
	setup_WRs(send_inv_wr, send_inv_sgl, recv_inv_wr, recv_inv_sgl,
			  send_ack_wr, send_ack_sgl, recv_ack_wr, recv_ack_sgl,
			  send_val_wr, send_val_sgl, recv_val_wr, recv_val_sgl, cb, worker_lid);

	int j, is_update;
	long long rolling_iter = 0; /* For throughput measurement */
	uint32_t trace_iter = 0, credit_debug_cnt = 0;
//	in_progress_write_debug_cnt = 0;
	long long int inv_br_tx = 0, val_br_tx = 0, send_ack_tx = 0, send_crd_tx = 0;

	int issued_ops = 0;
    struct spacetime_trace_command *trace;
	trace_init(&trace, worker_gid);
	//ONLY for dbg
//	spacetime_ack_t last_ack;
//	spacetime_inv_t last_inv;
//	spacetime_object_meta last_meta, last_meta2, last_meta3;
//	spacetime_object_meta last_inv_meta, last_inv_meta2, last_inv_meta3;
	/* -----------------------------------------------------
	--------------Start the main Loop-----------------------
	---------------------------------------------------------*/
	while (true) {
		if (unlikely(credit_debug_cnt > M_1)) {
			red_printf("Worker %d misses credits \n", worker_gid);
			red_printf("Inv Credits %d, Ack credits %d, Val credits %d, CRD credits %d\n",
					   credits[INV_UD_QP_ID][(machine_id + 1) % MACHINE_NUM],
					   credits[ACK_UD_QP_ID][(machine_id + 1) % MACHINE_NUM],
					   credits[VAL_UD_QP_ID][(machine_id + 1) % MACHINE_NUM],
					   credits[CRD_UD_QP_ID][(machine_id + 1) % MACHINE_NUM]);
			credit_debug_cnt = 0;
		}

       	refill_ops(&trace_iter, worker_lid, trace, ops);
        /*
		for (j = 0; j < MAX_BATCH_OPS_SIZE; j++) {
			if (ops[j].opcode != ST_EMPTY) continue;
			ops[j].state = ST_NEW;
			uint32_t key_i = (uint32_t) worker_lid * MAX_BATCH_OPS_SIZE + j;
//			uint32_t key_i = (uint32_t) worker_gid * MAX_BATCH_OPS_SIZE + j;
			uint128 key_hash = CityHash128((char *) &key_i, 4);

			memcpy(&ops[j].key, &key_hash, sizeof(spacetime_key_t));
//			if(issued_ops == MAX_BATCH_OPS_SIZE * 300) exit(0);
			is_update = (issued_ops++ / MAX_BATCH_OPS_SIZE) % 2 == 0 ? 1 : 0;
			ops[j].opcode = (uint8) (is_update == 1 ? ST_OP_PUT : ST_OP_GET);
			if (is_update == 1)
				memset(ops[j].value, ((uint8_t) machine_id % 2 == 0 ? 'x' : 'y'), ST_VALUE_SIZE);
			ops[j].val_len = (uint8) (is_update == 1 ? ST_VALUE_SIZE : -1);
			if(ENABLE_REQ_PRINTS && machine_id == 0)
				red_printf("Key id: %d, op: %s, hash:%" PRIu64 "\n", key_i,
						   code_to_str(ops[j].opcode), ((uint64_t *) &ops[j].key)[1]);
		}
         */

		if(ENABLE_ASSERTIONS)
			for(j = 0; j < MAX_BATCH_OPS_SIZE; j++)
				assert(ops[j].opcode == ST_OP_PUT || ops[j].opcode == ST_OP_GET);
		spacetime_batch_ops(MAX_BATCH_OPS_SIZE, &ops, worker_lid);

		if (WRITE_RATIO > 0) {
			///~~~~~~~~~~~~~~~~~~~~~~INVS~~~~~~~~~~~~~~~~~~~~~~~~~~~
			///TODO remove credits recv etc from bcst_invs
			broadcasts_invs(ops, inv_send_ops, &inv_push_send_ptr,
							send_inv_wr, send_inv_sgl, credits, cb, &inv_br_tx,
							incoming_acks, &ack_push_recv_ptr, worker_lid, &credit_debug_cnt);

			if(ENABLE_ASSERTIONS)
				for(j = 0; j < MAX_BATCH_OPS_SIZE; j++){
					if(!(ops[j].state == ST_BUFFERED_IN_PROGRESS_REPLAY ||
						   ops[j].state == ST_IN_PROGRESS_WRITE ||
						   ops[j].state == ST_PUT_STALL ||
						   ops[j].opcode == ST_OP_GET))
						printf("Opcode: %s, State: %s\n",code_to_str(ops[j].opcode), code_to_str(ops[j].state));
					assert(ops[j].state == ST_BUFFERED_IN_PROGRESS_REPLAY ||
						   ops[j].state == ST_IN_PROGRESS_WRITE ||
						   ops[j].state == ST_PUT_STALL ||
						   ops[j].opcode == ST_OP_GET);
				}

			///Poll for INVs
			poll_buff(incoming_invs, ST_INV_BUFF, &inv_pull_recv_ptr, inv_recv_ops,
					  &inv_ops_i, outstanding_invs, cb->dgram_recv_cq[INV_UD_QP_ID],
					  recv_inv_wc, credits, worker_lid);

			if(inv_ops_i > 0) {
				///TODO fix outstanding_invs
				spacetime_batch_invs(inv_ops_i, &inv_recv_ops, worker_lid);
				///INVS_bookkeeping
				outstanding_invs = 0; //TODO this is only for testing

				///~~~~~~~~~~~~~~~~~~~~~~ACKS~~~~~~~~~~~~~~~~~~~~~~~~~~~
				issue_acks(inv_recv_ops, ack_send_ops, &ack_push_send_ptr,
						   &send_ack_tx, send_ack_wr, send_ack_sgl, credits,
						   cb, incoming_invs, &inv_push_recv_ptr, worker_lid, &credit_debug_cnt);
				inv_ops_i = 0;
			}

			///Poll for Acks
			poll_buff(incoming_acks, ST_ACK_BUFF, &ack_pull_recv_ptr, ack_recv_ops,
					  &ack_ops_i, outstanding_acks, cb->dgram_recv_cq[ACK_UD_QP_ID],
					  recv_ack_wc, credits, worker_lid);
			if(ack_ops_i > 0){
				spacetime_batch_acks(ack_ops_i, &ack_recv_ops, ops, worker_lid);

				///~~~~~~~~~~~~~~~~~~~~~~VALS~~~~~~~~~~~~~~~~~~~~~~~~~~~
				///BC vals and poll for credits
				broadcasts_vals(ack_recv_ops, val_send_ops, &val_push_send_ptr,
								send_val_wr, send_val_sgl, credits, cb, recv_crd_wc,
								&credit_debug_cnt, &val_br_tx, recv_crd_wr, worker_lid);
				ack_ops_i = 0;
			}

            ///TODO outstandig_vals are not really required
			///Poll for Vals
            poll_buff(incoming_vals, ST_VAL_BUFF, &val_pull_recv_ptr, val_recv_ops,
                      &val_ops_i, outstanding_vals, cb->dgram_recv_cq[VAL_UD_QP_ID],
					  recv_val_wc, credits, worker_lid);

            if(val_ops_i > 0){
                spacetime_batch_vals(val_ops_i, &val_recv_ops, worker_lid);

                ///~~~~~~~~~~~~~~~~~~~~~~CREDITS~~~~~~~~~~~~~~~~~~~~~~~~~~~
                issue_credits(val_recv_ops, &send_crd_tx, send_crd_wr, credits,
							  cb, incoming_vals, &val_push_recv_ptr, worker_lid, &credit_debug_cnt);
                val_ops_i = 0;
            }
		}

		for(j = 0; j < MAX_BATCH_OPS_SIZE; j++) {
			///TODO Add reordering / moving of uncompleted requests to first buckets of ops
            if(ENABLE_ASSERTIONS)
				assert(ops[j].state == ST_PUT_SUCCESS ||
					   ops[j].state == ST_GET_SUCCESS ||
				       ops[j].state == ST_IN_PROGRESS_WRITE ||
					   ops[j].state == ST_PUT_STALL ||
				       ops[j].state == ST_GET_STALL);
//				printf("State[%d]: %s\n", j, code_to_str(ops[j].state));

			if(ENABLE_ASSERTIONS)
				assert(ops[j].opcode == ST_OP_PUT || ops[j].opcode == ST_OP_GET);
			if (ops[j].state == ST_PUT_SUCCESS || ops[j].state == ST_GET_SUCCESS) {
				if(ENABLE_REQ_PRINTS && worker_lid < MAX_THREADS_TO_PRINT)
					green_printf("W%d--> Key Hash:%" PRIu64 "\n\t\tType: %s, version %d, tie-b: %d, value(len-%d): %c\n",
								 worker_lid, ((uint64_t *) &ops[j].key)[1],
								 code_to_str(ops[j].state), ops[j].version,
								 ops[j].tie_breaker_id, ops[j].val_len, ops[j].value[0]);
				ops[j].state = ST_EMPTY;
				ops[j].opcode = ST_EMPTY;
				w_stats[worker_lid].cache_hits_per_worker++;
			}
		}

		rolling_iter++;
	}
	return NULL;
}
