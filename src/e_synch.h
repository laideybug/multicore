#ifndef E_SYNCH_H_INCLUDED
#define E_SYNCH_H_INCLUDED

#include "common.h"
#include "e-lib.h"

extern volatile e_barrier_t barriers[N];
extern          e_barrier_t *tgt_bars[N];

#endif // E_SYNCH_H_INCLUDED