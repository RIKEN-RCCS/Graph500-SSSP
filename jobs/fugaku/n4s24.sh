#!/bin/bash
#PJM -L "node=4"
#PJM -L "rscgrp=small"
#PJM -L "elapse=01:00:00"
#PJM --mpi "max-proc-per-node=4"
#PJM -L "freq=2200,eco_state=2"
#PJM -s
N=4
S=24

############################
P=$((4 * $N))
export OMP_NUM_THREADS=12
export PROCS_PER_NODE=4
export DELTA_STEP=0.001
export FLIB_BARRIER=HARD
export PLE_MPI_STD_EMPTYFILE=off
MPI_PARAMS="-mca btl_tofu_eager_limit 512000 -mca mpi_print_stats 3"
export PRESOL_SECONDS=27000
mpiexec $MPI_PARAMS -n $P ../../build/bin/sssp-parallel $S
echo $SECONDS
