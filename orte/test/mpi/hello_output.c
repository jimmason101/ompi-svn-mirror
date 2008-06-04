/* -*- C -*-
 *
 * $HEADER$
 *
 * The most basic of MPI applications
 */

#include <stdio.h>
#include "mpi.h"

#include "orte/util/output.h"

int main(int argc, char* argv[])
{
    int rank, size;
    int stream, stream2;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    stream = orte_output_open(NULL);
    orte_output(stream, "(stream) Hello, World, I am %d of %d\n", rank, size);
    printf("(printf) Hello, World, I am %d of %d\n", rank, size);
    
    orte_output_set_verbosity(stream, 10);
    orte_output(stream, "this is an orte_output on the verbose stream");
    
    stream2 = opal_output_open(NULL);
    opal_output(stream2, "opal_output stream2");
    opal_output_set_verbosity(stream2, 10);
    opal_output(stream2, "this is an opal_output on the same verbose stream2");

    MPI_Finalize();
    return 0;
}
