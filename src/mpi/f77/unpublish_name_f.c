/*
 * $HEADER$
 */

#include "ompi_config.h"

#include <stdio.h>

#include "mpi.h"
#include "mpi/f77/bindings.h"

#if OMPI_HAVE_WEAK_SYMBOLS && OMPI_PROFILE_LAYER
#pragma weak PMPI_UNPUBLISH_NAME = mpi_unpublish_name_f
#pragma weak pmpi_unpublish_name = mpi_unpublish_name_f
#pragma weak pmpi_unpublish_name_ = mpi_unpublish_name_f
#pragma weak pmpi_unpublish_name__ = mpi_unpublish_name_f
#elif OMPI_PROFILE_LAYER
OMPI_GENERATE_F77_BINDINGS (PMPI_UNPUBLISH_NAME,
                           pmpi_unpublish_name,
                           pmpi_unpublish_name_,
                           pmpi_unpublish_name__,
                           pmpi_unpublish_name_f,
                           (char *service_name, MPI_Fint *info, char *port_name, MPI_Fint *ierr),
                           (service_name, info, port_name, ierr) )
#endif

#if OMPI_HAVE_WEAK_SYMBOLS
#pragma weak MPI_UNPUBLISH_NAME = mpi_unpublish_name_f
#pragma weak mpi_unpublish_name = mpi_unpublish_name_f
#pragma weak mpi_unpublish_name_ = mpi_unpublish_name_f
#pragma weak mpi_unpublish_name__ = mpi_unpublish_name_f
#endif

#if ! OMPI_HAVE_WEAK_SYMBOLS && ! OMPI_PROFILE_LAYER
OMPI_GENERATE_F77_BINDINGS (MPI_UNPUBLISH_NAME,
                           mpi_unpublish_name,
                           mpi_unpublish_name_,
                           mpi_unpublish_name__,
                           mpi_unpublish_name_f,
                           (char *service_name, MPI_Fint *info, char *port_name, MPI_Fint *ierr),
                           (service_name, info, port_name, ierr) )
#endif


#if OMPI_PROFILE_LAYER && ! OMPI_HAVE_WEAK_SYMBOLS
#include "mpi/f77/profile/defines.h"
#endif

OMPI_EXPORT
void mpi_unpublish_name_f(char *service_name, MPI_Fint *info,
			  char *port_name, MPI_Fint *ierr)
{
    MPI_Info c_info;

    c_info = MPI_Info_f2c(*info);

    *ierr = OMPI_INT_2_FINT(MPI_Unpublish_name(service_name, c_info, 
						port_name));
}
