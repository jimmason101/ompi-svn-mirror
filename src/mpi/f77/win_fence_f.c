/*
 * $HEADER$
 */

#include "ompi_config.h"

#include <stdio.h>

#include "mpi.h"
#include "mpi/f77/bindings.h"

#if OMPI_HAVE_WEAK_SYMBOLS && OMPI_PROFILE_LAYER
#pragma weak PMPI_WIN_FENCE = mpi_win_fence_f
#pragma weak pmpi_win_fence = mpi_win_fence_f
#pragma weak pmpi_win_fence_ = mpi_win_fence_f
#pragma weak pmpi_win_fence__ = mpi_win_fence_f
#elif OMPI_PROFILE_LAYER
OMPI_GENERATE_F77_BINDINGS (PMPI_WIN_FENCE,
                           pmpi_win_fence,
                           pmpi_win_fence_,
                           pmpi_win_fence__,
                           pmpi_win_fence_f,
                           (MPI_Fint *assert, MPI_Fint *win, MPI_Fint *ierr),
                           (assert, win, ierr) )
#endif

#if OMPI_HAVE_WEAK_SYMBOLS
#pragma weak MPI_WIN_FENCE = mpi_win_fence_f
#pragma weak mpi_win_fence = mpi_win_fence_f
#pragma weak mpi_win_fence_ = mpi_win_fence_f
#pragma weak mpi_win_fence__ = mpi_win_fence_f
#endif

#if ! OMPI_HAVE_WEAK_SYMBOLS && ! OMPI_PROFILE_LAYER
OMPI_GENERATE_F77_BINDINGS (MPI_WIN_FENCE,
                           mpi_win_fence,
                           mpi_win_fence_,
                           mpi_win_fence__,
                           mpi_win_fence_f,
                           (MPI_Fint *assert, MPI_Fint *win, MPI_Fint *ierr),
                           (assert, win, ierr) )
#endif


#if OMPI_PROFILE_LAYER && ! OMPI_HAVE_WEAK_SYMBOLS
#include "mpi/f77/profile/defines.h"
#endif

OMPI_EXPORT
void mpi_win_fence_f(MPI_Fint *assert, MPI_Fint *win, MPI_Fint *ierr)
{
    MPI_Win c_win = MPI_Win_f2c(*win);
    
    *ierr = OMPI_INT_2_FINT(MPI_Win_fence(OMPI_FINT_2_INT(*assert), c_win));
}
