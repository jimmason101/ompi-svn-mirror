/*
 * $HEADERS$
 */
#include "ompi_config.h"
#include <stdio.h>

#include "mpi.h"
#include "mpi/c/bindings.h"
#include "mca/topo/topo.h"
#include "communicator/communicator.h"
#include "errhandler/errhandler.h"

#if OMPI_HAVE_WEAK_SYMBOLS && OMPI_PROFILING_DEFINES
#pragma weak MPI_Cart_rank = PMPI_Cart_rank
#endif

#if OMPI_PROFILING_DEFINES
#include "mpi/c/profile/defines.h"
#endif

static const char FUNC_NAME[] = "MPI_Cart_rank";


int MPI_Cart_rank(MPI_Comm comm, int *coords, int *rank) 
{
    int err;
    mca_topo_base_module_cart_rank_fn_t func;

    /* check the arguments */
    if (MPI_PARAM_CHECK) {
        OMPI_ERR_INIT_FINALIZE(FUNC_NAME);
        if (MPI_COMM_NULL == comm) {
            return OMPI_ERRHANDLER_INVOKE (MPI_COMM_WORLD, MPI_ERR_COMM,
                                          FUNC_NAME);
        }
        if (OMPI_COMM_IS_INTER(comm)) { 
            return OMPI_ERRHANDLER_INVOKE (MPI_COMM_WORLD, MPI_ERR_COMM,
                                          FUNC_NAME);
        }
        if (!OMPI_COMM_IS_CART(comm)) {
            return OMPI_ERRHANDLER_INVOKE (MPI_COMM_WORLD, MPI_ERR_TOPOLOGY,
                                          FUNC_NAME);
        }
        if ((NULL == coords) || (NULL == rank)){
            return OMPI_ERRHANDLER_INVOKE (MPI_COMM_WORLD, MPI_ERR_ARG,
                                          FUNC_NAME);
        }
    }

    /* get the function pointer on this communicator */
    func = comm->c_topo->topo_cart_rank;

    /* call the function */
    if ( MPI_SUCCESS != 
            (err = func(comm, coords, rank))) {
        return OMPI_ERRHANDLER_INVOKE(MPI_COMM_WORLD, err, FUNC_NAME);
    }

    return MPI_SUCCESS;
}
