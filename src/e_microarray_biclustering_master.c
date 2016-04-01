#include <e-lib.h>
#include "common.h"
#include "e_microarray_biclustering_utils.h"
#include "static_buffers.h"

int main(void) {
	unsigned *dest, *xt, *all_done_flag, *total_inf_clks, *total_up_clks, *sample_no, *slave_ready_flag, *slave_done_flag, *slave_inf_clks, *slave_up_clks, *masternode_clks, *p;
	unsigned slave_core_addr, all_done_flag_addr, sample_no_addr, total_inf_clks_addr, total_up_clks_addr, masternode_clks_addr, src_addr, xt_addr;
	int i, j, k, all_ready, all_done, inf_clks, up_clks;

	all_done_flag_addr = (unsigned)SHMEM_ADDR + (IN_ROWS*IN_COLS*sizeof(float));
	all_done_flag = (unsigned *)all_done_flag_addr;
	sample_no_addr = (unsigned)SHMEM_ADDR + (IN_ROWS*IN_COLS*sizeof(float) + sizeof(int));
    sample_no = (unsigned *)sample_no_addr;
	total_inf_clks_addr = (unsigned)SHMEM_ADDR + (IN_ROWS*IN_COLS*sizeof(float) + (2*sizeof(int)));
    total_inf_clks = (unsigned *)total_inf_clks_addr;
    total_up_clks_addr = (unsigned)SHMEM_ADDR + (IN_ROWS*IN_COLS*sizeof(float) + (3*sizeof(int)));
    total_up_clks = (unsigned *)total_up_clks_addr;
    masternode_clks_addr = (unsigned)SHMEM_ADDR + (IN_ROWS*IN_COLS*sizeof(float) + (4*sizeof(int)));
    masternode_clks = (unsigned *)masternode_clks_addr;

    src_addr = (unsigned)XT_MEM_ADDR;
    p = CLEAR_FLAG;

    // Initialise benchmark results
    (*(sample_no)) = -1;
    (*(total_inf_clks)) = 0;
    (*(total_up_clks)) = 0;
    (*(masternode_clks)) = 0;
    (*(all_done_flag)) = 0;

    while (1) {
        all_ready = 0;

        for (i = 0; i < N; ++i) {
            slave_ready_flag = (unsigned *)((unsigned)READY_MEM_ADDR + i*sizeof(int));
            all_ready += *slave_ready_flag;
        }

        if (all_ready == N) {
            break;
        }
    }

	for (i = 0; i < IN_COLS; ++i) {
        e_ctimer_set(E_CTIMER_0, E_CTIMER_MAX);
        e_ctimer_start(E_CTIMER_0, E_CTIMER_CLK);

		xt_addr = (unsigned)SHMEM_ADDR + (i*IN_ROWS*sizeof(float));
		xt = (unsigned *)xt_addr;

        for (j = 0; j < M; ++j) {
            for (k = 0; k < N; ++k) {
                slave_core_addr = (unsigned)e_get_global_address_on_chip(j, k, p);
                dest = (unsigned *)((unsigned)slave_core_addr + (unsigned)src_addr);
                e_memcopy(dest, xt, IN_ROWS*sizeof(float));
                e_global_address_irq_set(j, k, E_SYNC);
            }
        }

		while (1) {
            all_done = 0;

            for (j = 0; j < N; ++j) {
                slave_done_flag = (unsigned *)((unsigned)DONE_MEM_ADDR + j*sizeof(int));
                all_done += *slave_done_flag;
            }

            if (all_done == N) {
                break;
            }
        }

        inf_clks = 0;
        up_clks = 0;

        for (j = 0; j < N; ++j) {
            slave_inf_clks = (unsigned *)((unsigned)INF_CLKS_MEM_ADDR + j*sizeof(int));
            inf_clks += *slave_inf_clks;
            *slave_inf_clks = 0;

            slave_up_clks = (unsigned *)((unsigned)UP_CLKS_MEM_ADDR + j*sizeof(int));
            up_clks += *slave_up_clks;
            *slave_up_clks = 0;

            slave_done_flag = (unsigned *)((unsigned)DONE_MEM_ADDR + j*sizeof(int));
            *slave_done_flag = 0;
        }

        // Write benchmark results
        (*(sample_no)) = i;
        (*(total_inf_clks)) = inf_clks;
        (*(total_up_clks)) = up_clks;
        (*(masternode_clks)) = E_CTIMER_MAX - e_ctimer_stop(E_CTIMER_0);
	}

	// Raising "done" flag for host
    (*(all_done_flag)) = SET_FLAG;

	// Put core in idle state
    __asm__ __volatile__("idle");

    return EXIT_SUCCESS;
}