#include <e-lib.h>
#include <math.h>
#include <string.h>
#include "common.h"
#include "static_buffers.h"

void adjustScaling(float scaling);
float sign(float value);
void syncISR(int x);
#ifdef USE_MASTER_NODE
void usrISR(int x);
volatile int write_msg_cnt;
#endif

int main(void) {
	unsigned *xt, *wk, *update_wk, *nu_opt, *nu_k, *ready_flag, *done_flag, *inf_clks, *up_clks, *dest, *p;
	unsigned volatile *nu_k0, *nu_k1, *nu_k2;
	unsigned slave_core, j, k, nu_src_addr, ready_flag_addr, done_flag_addr, inf_clks_addr, up_clks_addr, out_mem_offset, timer_value_0, timer_value_1;
	float subgrad[IN_ROWS], scaling, rms_wk, rms_wk_reciprocol;
	int i, reps;

	xt = (unsigned *)XT_MEM_ADDR;	            // Address of xt (56 x 1)
	wk = (unsigned *)WK_MEM_ADDR;	            // Address of dictionary atom (56 x 1)
	update_wk = (unsigned *)UP_WK_MEM_ADDR;	// Address of update atom (56 x 1)
	nu_opt = (unsigned *)NU_OPT_MEM_ADDR;      // Address of optimal dual variable (56 x 1)
	nu_k0 = (unsigned *)NU_K0_MEM_ADDR;	    // Address of node 0 dual variable estimate (56 x 1)
	nu_k1 = (unsigned *)NU_K1_MEM_ADDR;        // Address of node 1 dual variable estimate (56 x 1)
	nu_k2 = (unsigned *)NU_K2_MEM_ADDR;        // Address of node 2 dual variable estimate (56 x 1)

    p = CLEAR_FLAG;
    out_mem_offset = (unsigned)(e_group_config.core_col*sizeof(int));

#ifdef USE_MASTER_NODE
	unsigned master_node_addr = (unsigned)e_get_global_address(0, N, p);
	inf_clks_addr = master_node_addr + (unsigned)INF_CLKS_MEM_ADDR + out_mem_offset;
	up_clks_addr = master_node_addr + (unsigned)UP_CLKS_MEM_ADDR + out_mem_offset;
	done_flag_addr = master_node_addr + (unsigned)DONE_MEM_ADDR + out_mem_offset;
	ready_flag_addr = master_node_addr + (unsigned)READY_MEM_ADDR + out_mem_offset;
	ready_flag = (unsigned *)ready_flag_addr;
#else
    //inf_clks_addr = master_node_addr + (unsigned)INF_CLKS_MEM_ADDR + out_mem_offset;
	//up_clks_addr = master_node_addr + (unsigned)UP_CLKS_MEM_ADDR + out_mem_offset;
    done_flag_addr = (unsigned)SHMEM_ADDR + out_mem_offset;
#endif

    inf_clks = (unsigned *)inf_clks_addr;
    up_clks = (unsigned *)up_clks_addr;
    done_flag = (unsigned *)done_flag_addr;	 // "Done" flag (1 x 1)
    nu_src_addr = (unsigned)NU_K0_MEM_ADDR;

    for (i = 0; i < e_group_config.core_col; ++i) {
        nu_src_addr = nu_src_addr + (unsigned)NU_MEM_OFFSET;
    }

    nu_k = (unsigned *)nu_src_addr;    // Address of this cores dual variable estimate

    // Re-enable interrupts
    e_irq_attach(E_SYNC, syncISR);
    e_irq_mask(E_SYNC, E_FALSE);
    e_irq_attach(E_USER_INT, usrISR);
    e_irq_mask(E_USER_INT, E_FALSE);
    e_irq_global_mask(E_FALSE);

    write_msg_cnt = 0;

#ifdef USE_MASTER_NODE
    (*(ready_flag)) = SET_FLAG;
#else
#ifdef USE_BARRIER
    // Initialise barriers
    e_barrier_init(barriers, tgt_bars);
#endif
#endif

    while (1) {
#ifdef USE_MASTER_NODE
        // Put core in idle state
        __asm__ __volatile__("idle");
#endif
        scaling = 0.0f;

        // Set timers for benchmarking
        e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);
        e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);

		for (reps = 0; reps < NUM_ITER; ++reps) {
			for (i = 0; i < IN_ROWS; ++i) {
				/* subgrad = (nu-xt)*minus_mu_over_N */
				subgrad[i] = nu_opt[i] - xt[i];
				subgrad[i] = subgrad[i] * (-MU_2 * ONE_OVER_N);
				/* scaling = (my_W_transpose*nu) */
				scaling = scaling + wk[i] * nu_opt[i];
			}

			adjustScaling(scaling);

			for (i = 0; i < IN_ROWS; ++i) {
				/* D * diagmat(scaling*my_minus_mu) */
				nu_k[i] = wk[i] * (scaling * -MU_2);
			}

	        // Exchange dual variable estimates
			for (j = 0; j < e_group_config.group_rows; ++j) {
	            for (k = 0; k < N; ++k) {
	                if ((j != e_group_config.core_row) | (k != e_group_config.core_col)) {
                        slave_core = (unsigned)e_get_global_address(j, k, p);
	                    dest = (unsigned *)(slave_core + (unsigned)nu_src_addr);
	                    e_memcopy(dest, nu_k, IN_ROWS*sizeof(float));
	                    //nu_flag = (unsigned *)(slave_core + (unsigned)NU_K_FLAG_ADDR + k*sizeof(int));
	                    //(*(nu_flag)) = 0x00000001;
	                    e_irq_set((unsigned)j, (unsigned)k, E_USER_INT);
	                }
	            }
			}

#ifdef USE_BARRIER
	    	// Synch with all other cores
	    	e_barrier(barriers, tgt_bars);
#else
            while (write_msg_cnt < 1);
            write_msg_cnt = 0;
#endif

	    	// Average dual variable estimates
			for (i = 0; i < IN_ROWS; ++i) {
	            nu_opt[i] = nu_opt[i] + subgrad[i] + ((nu_k0[i] + nu_k1[i] + nu_k2[i]) * ONE_OVER_N);
			}
		}

		timer_value_0 = E_CTIMER_MAX - e_ctimer_stop(E_CTIMER_0);
		e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);
		e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);

		for (i = 0; i < IN_ROWS; ++i) {
			/* scaling = (my_W_transpose*nu); */
			scaling = scaling + wk[i] * nu_opt[i];
		}

		adjustScaling(scaling);

		// Update dictionary atom
		rms_wk = 0.0f;

		// Create update atom (Y_opt)
		for (i = 0; i < IN_ROWS; ++i) {
			update_wk[i] =  MU_W * (nu_opt[i] * scaling);
			wk[i] = wk[i] + update_wk[i];
			wk[i] = fmax(abs(wk[i])-BETA*MU_W, 0.0f) * sign(wk[i]);
	        rms_wk = rms_wk + wk[i] * wk[i];

	        // Resetting/initialising the dual variable and update atom
			update_wk[i] = 0.0f;
			nu_opt[i] = 0.0f;
		}

		rms_wk = sqrt(rms_wk);

		if (rms_wk > 1.0f) {
            rms_wk_reciprocol = 1.0f / rms_wk;

			for (i = 0; i < IN_ROWS; ++i) {
				wk[i] = wk[i] * rms_wk_reciprocol;
			}
		}

		timer_value_1 = E_CTIMER_MAX - e_ctimer_stop(E_CTIMER_0);

		// Write benchmark values
		(*(inf_clks)) = timer_value_0;
		(*(up_clks)) = timer_value_1;
		// Raising "done" flag for master node
	   	(*(done_flag)) = SET_FLAG;

#ifndef USE_MASTER_NODE
        // Put core in idle state
        __asm__ __volatile__("idle");
#endif
	}

    return EXIT_SUCCESS;
}

/*
* Function: adjustScaling
* -----------------------
* Adjusts the value of scaling
*
* scaling: the value to adjust
*
*/

inline void adjustScaling(float scaling) {
    if (scaling > GAMMA) {
        scaling = (scaling - GAMMA) * ONE_OVER_DELTA;
    } else if (scaling < -GAMMA) {
        scaling = (scaling + GAMMA) * ONE_OVER_DELTA;
    } else {
        scaling = 0.0f;
    }
}

/*
* Function: sign
* --------------
* Returns a float (-1.0, 0.0, 1.0) depending on
* the sign of value
*
* value: the value to check
*
*/

inline float sign(float value) {
	if (value > 0.0f) {
		return 1.0f;
	} else if (value < 0.0f) {
		return -1.0f;
	} else {
		return 0.0f;
	}
}

/*
* Function: syncISR
* -----------------
* Override sync function
*
* x: arbitrary value
*
*/

void __attribute__((interrupt)) syncISR(int x) {
    return;
}

/*
* Function: syncISR
* -----------------
* Override sync function
*
* x: arbitrary value
*
*/

void __attribute__((interrupt)) usrISR(int x) {
    write_msg_cnt = write_msg_cnt + 1;
    return;
}
