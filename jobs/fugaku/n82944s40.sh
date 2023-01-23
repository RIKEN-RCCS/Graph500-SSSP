#!/bin/bash
#PJM -L "node=48x36x48:strict"
#PJM -L "rscgrp=huge"
#PJM -L "elapse=03:30:00"
#PJM --mpi "max-proc-per-node=4"
#PJM -L "freq=2200,eco_state=2"
#PJM --llio localtmp-size=10Gi
#PJM --llio sharedtmp-size=10Gi
#PJM --mpi "assign-online-node"
#PJM -s
N=82944
S=40
export TOFU_6D=xbc
############################
P=$((4 * $N))
DIR=n${N}s${S}
export OMP_NUM_THREADS=12
export PROCS_PER_NODE=4
MPI_PARAMS="-mca btl_tofu_eager_limit 512000 -mca mpi_print_stats 3 -stdout-proc ./${DIR}.%j/%/1000r/stdout -stderr-proc ./${DIR}.%j/%/1000r/stderr"
###
export PLE_MPI_STD_EMPTYFILE=off
export FLIB_BARRIER=HARD
llio_transfer ../build/bin/sssp-parallel
export DELTA_STEP=0.0002
stdbuf -i0 -o0 -e0 mpiexec $MPI_PARAMS -n $P ../../build/bin/sssp-parallel $S

echo $SECONDS
