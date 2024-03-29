#include <e-lib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "_e_lib_extended.h"
#include "common.h"
#include "e_sync.h"

float adjust_scaling(float scaling);
float sign(float value);
void sync_isr(int x);

int main(void) {
    unsigned  *done_flag, *inf_clks, *up_clks, *p, i, j, reps, slave_core_addr, out_mem_offset, timer_value_0, timer_value_1;
    float *wk, *update_wk, *nu_opt, *nu_k, *dest, *scaling_incomplete, *rms_wk_incomplete, *scaling_val, subgrad[WK_ROWS], minus_mu_2, minus_mu_2_over_n, beta_mu_w, nu_k_size, scaling, rms_wk;
    volatile float *xt, *nu_k0, *nu_k1, *nu_k2, *scaling_incomplete_k, *rms_wk_incomplete_k;
#ifdef USE_MASTER_NODE
    unsigned *ready_flag, master_node_addr, done_flag_counter;
    e_mutex_t *mutex;
#endif

    xt = (float *)XT_MEM_ADDR;	            // Address of xt (WK_ROWS x 1)
    wk = (float *)WK_MEM_ADDR;	            // Address of dictionary atom (WK_ROWS x 1)
    update_wk = (float *)UP_WK_MEM_ADDR;	// Address of update atom (WK_ROWS x 1)
    nu_k0 = (float *)NU_K0_MEM_ADDR;	    // Address of node 0 dual variable estimate (WK_ROWS x 1)
    nu_k1 = (float *)NU_K1_MEM_ADDR;        // Address of node 1 dual variable estimate (WK_ROWS x 1)
    nu_k2 = (float *)NU_K2_MEM_ADDR;        // Address of node 2 dual variable estimate (WK_ROWS x 1)
    nu_opt = (float *)NU_OPT_K0_MEM_ADDR;   // Address of optimal dual variable (WK_ROWS x 1)

    p = CLEAR_FLAG;
    out_mem_offset = (unsigned)((e_group_config.core_row * e_group_config.group_cols + e_group_config.core_col)*sizeof(unsigned));

#ifdef USE_MASTER_NODE
    master_node_addr = (unsigned)_e_get_global_address_on_chip(MASTER_NODE_ROW, MASTER_NODE_COL, p);
    done_flag = (unsigned *)(master_node_addr + DONE_MEM_ADDR);
    ready_flag = (unsigned *)(master_node_addr + READY_MEM_ADDR + out_mem_offset);
    inf_clks = (unsigned *)(master_node_addr + INF_CLKS_MEM_ADDR + out_mem_offset);
    up_clks = (unsigned *)(master_node_addr + UP_CLKS_MEM_ADDR + out_mem_offset);
    scaling_val = (float *)(master_node_addr + SCAL_MEM_ADDR + out_mem_offset);

    mutex = (int *)DONE_MUTEX_MEM_ADDR;
#else
    done_flag = (unsigned *)(SHMEM_ADDR + out_mem_offset);
    inf_clks = (unsigned *)(SHMEM_ADDR + M_N*sizeof(unsigned) + out_mem_offset);
    up_clks = (unsigned *)(SHMEM_ADDR + 2*M_N*sizeof(unsigned) + out_mem_offset);
    scaling_val = (float *)(SHMEM_ADDR + 3*M_N*sizeof(unsigned) + out_mem_offset);
#endif

    // Address of this cores dual variable estimate
    nu_k = (float *)(NU_K0_MEM_ADDR + e_group_config.core_col * NU_MEM_OFFSET);
    // Address of this cores incomplete scaling value
    scaling_incomplete = (float *)(INC_SCAL_MEM_ADDR + e_group_config.core_row * sizeof(float));
    // Address of this cores incomplete rms value
    rms_wk_incomplete = (float *)(INC_RMS_MEM_ADDR + e_group_config.core_row * sizeof(float));

    minus_mu_2 = MU_2 * -1.0f;
    minus_mu_2_over_n = minus_mu_2 / N;
    beta_mu_w = BETA * MU_W;
    nu_k_size = WK_ROWS*sizeof(float);

    // Re-enable interrupts
    e_irq_attach(E_SYNC, sync_isr);
    e_irq_mask(E_SYNC, E_FALSE);
    e_irq_global_mask(E_FALSE);

    // Initialise barriers
    e_barrier_init(barriers, tgt_bars);

#ifdef USE_MASTER_NODE
    (*(ready_flag)) = SET_FLAG;
#endif

    while (1) {
#ifdef USE_MASTER_NODE
        // Put core in idle state
        __asm__ __volatile__("idle");
#endif

        timer_value_0 = 0;
        timer_value_1 = 0;

        // Set timers for benchmarking
        e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);
        e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);

	for (reps = 0; reps < NUM_ITER; ++reps) {
            scaling = 0.0f;
            *scaling_incomplete = 0.0f;

	    for (i = 0; i < WK_ROWS; ++i) {
	        /* subgrad = (nu-xt)*minus_mu_over_N */
		subgrad[i] = (nu_opt[i] - xt[i]) * minus_mu_2_over_n;
		/* scaling = (my_W_transpose*nu) */
		*scaling_incomplete += wk[i] * nu_opt[i];
	    }

	    // Exchange incomplete scaling values along column
            for (j = 0; j < e_group_config.group_rows; ++j) {
                if (j != e_group_config.core_row) {
                    slave_core_addr = (unsigned)e_get_global_address(j, e_group_config.core_col, p);
                    dest = (float *)(slave_core_addr + scaling_incomplete);
                    e_memcopy(dest, &scaling_incomplete, sizeof(float));
                }
            }

            timer_value_0 += E_CTIMER_MAX - e_ctimer_stop(E_CTIMER_0);
            e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);

            // Synch with all other cores
            e_barrier(barriers, tgt_bars);
            e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);

            for (j = 0; j < e_group_config.group_rows; ++j) {
                scaling_incomplete_k = (float *)(INC_SCAL_MEM_ADDR + j*sizeof(float));
                scaling += *scaling_incomplete_k;
            }

	    scaling = adjust_scaling(scaling);

	    for (i = 0; i < WK_ROWS; ++i) {
		/* D * diagmat(scaling*my_minus_mu) */
		nu_k[i] = wk[i] * scaling * minus_mu_2;
	    }

	    // Exchange dual variable estimates along row
            for (j = 0; j < e_group_config.group_cols; ++j) {
                if (j != e_group_config.core_col) {
                    slave_core_addr = (unsigned)e_get_global_address(e_group_config.core_row, j, p);
                    dest = (float *)(slave_core_addr + nu_k);
                    e_memcopy(dest, nu_k, nu_k_size);
                }
            }

            timer_value_0 += E_CTIMER_MAX - e_ctimer_stop(E_CTIMER_0);
            e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);

	    	// Synch with all other cores
            e_barrier(barriers, tgt_bars);
            e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);

	    // Average dual variable estimates
	    for (i = 0; i < WK_ROWS; ++i) {
	        nu_opt[i] += subgrad[i] + ((nu_k0[i] + nu_k1[i] + nu_k2[i]) / N);
	    }
        }

	timer_value_0 += E_CTIMER_MAX - e_ctimer_stop(E_CTIMER_0);
	e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);
        e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);

        scaling = 0.0f;
        *scaling_incomplete = 0.0f;

	for (i = 0; i < WK_ROWS; ++i) {
	    /* scaling = (my_W_transpose*nu); */
	    *scaling_incomplete += wk[i] * nu_opt[i];
	}

	// Exchange incomplete scaling values along column
        for (j = 0; j < e_group_config.group_rows; ++j) {
            if (j != e_group_config.core_row) {
                slave_core_addr = (unsigned)e_get_global_address(j, e_group_config.core_col, p);
                dest = (float *)(slave_core_addr + scaling_incomplete);
                e_memcopy(dest, &scaling_incomplete, sizeof(float));
            }
        }

        timer_value_1 += E_CTIMER_MAX - e_ctimer_stop(E_CTIMER_0);
	e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);

        // Synch with all other cores
        e_barrier(barriers, tgt_bars);

        e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);

        for (j = 0; j < e_group_config.group_rows; ++j) {
            scaling_incomplete_k = (float *)(INC_SCAL_MEM_ADDR + j*sizeof(float));
            scaling += *scaling_incomplete_k;
        }

	scaling = adjust_scaling(scaling);

	// Update dictionary atom
	rms_wk = 0.0f;
	*rms_wk_incomplete = 0.0f;

	// Create update atom (Y_opt)
	for (i = 0; i < WK_ROWS; ++i) {
	    update_wk[i] =  MU_W * nu_opt[i] * scaling;
	    wk[i] += update_wk[i];
	    wk[i] = fmax(fabsf(wk[i])-beta_mu_w, 0.0f) * sign(wk[i]);
	    *rms_wk_incomplete += wk[i] * wk[i];

	    // Resetting/initialising the dual variable and update atom
	    update_wk[i] = 0.0f;
	    nu_opt[i] = 0.0f;
	}

        // Exchange incomplete rms values along column
        for (j = 0; j < e_group_config.group_rows; ++j) {
            if (j != e_group_config.core_row) {
                slave_core_addr = (unsigned)e_get_global_address(j, e_group_config.core_col, p);
                dest = (float *)(slave_core_addr + rms_wk_incomplete);
                e_memcopy(dest, &rms_wk_incomplete, sizeof(float));
            }
        }

        timer_value_1 += E_CTIMER_MAX - e_ctimer_stop(E_CTIMER_0);
	e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);

	// Synch with all other cores
        e_barrier(barriers, tgt_bars);

        e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);

        for (j = 0; j < e_group_config.group_rows; ++j) {
            rms_wk_incomplete_k = (float *)(INC_RMS_MEM_ADDR + j*sizeof(float));
            rms_wk += *rms_wk_incomplete_k;
        }

	rms_wk = sqrtf(rms_wk);

	if (rms_wk > 1.0f) {
	    for (i = 0; i < WK_ROWS; ++i) {
		wk[i] = wk[i] / rms_wk;
	    }
	}

	timer_value_1 += E_CTIMER_MAX - e_ctimer_stop(E_CTIMER_0);

#ifdef USE_MASTER_NODE
        // Aqcuire the mutex lock
	_e_global_mutex_lock(MASTER_NODE_ROW, MASTER_NODE_COL, mutex);
        // Write benchmark values
	(*(inf_clks)) = timer_value_0;
	(*(up_clks)) = timer_value_1;
	(*(scaling_val)) = scaling;
	// Increment "done" flag for master node
	done_flag_counter = (*(done_flag));
	done_flag_counter = done_flag_counter + 1;
	(*(done_flag)) = done_flag_counter;

	// Release the mutex lock
	_e_global_mutex_unlock(MASTER_NODE_ROW, MASTER_NODE_COL, mutex);
#else
        // Write benchmark values
        (*(inf_clks)) = timer_value_0;
	(*(up_clks)) = timer_value_1;
	(*(scaling_val)) = scaling;
	// Raising "done" flag
        (*(done_flag)) = SET_FLAG;
        // Put core in idle state
        __asm__ __volatile__("idle");

#endif
    }

    return EXIT_SUCCESS;
}

/*
* Function: adjust_scaling
* ------------------------
* Adjusts the value of scaling
*
* scaling: the value to adjust
*
*/

inline float adjust_scaling(float scaling) {
    if (scaling > GAMMA) {
        return ((scaling - GAMMA) * ONE_OVER_DELTA);
    } else if (scaling < -GAMMA) {
        return ((scaling + GAMMA) * ONE_OVER_DELTA);
    } else {
        return 0.0f;
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
* Function: sync_isr
* ------------------
* Override sync function
*
* x: arbitrary value
*
*/

void __attribute__((interrupt)) sync_isr(int x) {
    return;
}
