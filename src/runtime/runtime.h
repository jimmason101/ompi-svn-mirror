/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

/**
 * @file
 *
 * Interface into the Open MPI Run Time Environment
 */
#ifndef OMPI_RUNTIME_H
#define OMPI_RUNTIME_H

#include "ompi_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include "mca/gpr/gpr_types.h"
#include "util/cmd_line.h"

#include "runtime/runtime_types.h"
#include "mca/ns/ns.h"

/* For backwards compatibility.  If you only need MPI stuff, please include
   mpiruntime/mpiruntime.h directly */
#include "mpi/runtime/mpiruntime.h"

/* constants for spawn constraints */

/** Spawn constraint - require multi-cell support.  The selected spawn
    system must be capable of starting across multiple cells.  This
    allows multiple pcms to be used to satisfy a single resource
    allocation request */
#define OMPI_RTE_SPAWN_MULTI_CELL 0x0001
/** Spawn constraint - require ability to launch daemons.  The
    selected spawn system must be capable of starting daemon process.
    Setting this flag will result in a spawn service that does not
    neccessarily provide process monitoring or standard I/O
    forwarding.  The calling process may exit before all children have
    exited. */
#define OMPI_RTE_SPAWN_DAEMON     0x0002
/** Spawn constraint - require quality of service support.  The
    selected spawn system must provide I/O forwarding, quick process
    shutdown, and process status monitoring. */
#define OMPI_RTE_SPAWN_HIGH_QOS   0x0004
/** Spawn constraint - caller is an MPI process.  The caller is an MPI
    application (has called MPI_Init).  This should be used only for
    MPI_COMM_SPAWN and MPI_COMM_SPAWN_MULTIPLE.  The calling process
    will follow the semantics of the MPI_COMM_SPAWN_* functions. */
#define OMPI_RTE_SPAWN_FROM_MPI   0x0008
/** Spawn constraint - require ability to launch either MPMD (hence
    the name) applications or applications with specific placement of
    processes. */
#define OMPI_RTE_SPAWN_MPMD       0x0010

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

    /* globals used by RTE - instanced in orte_init.c */

    OMPI_DECLSPEC extern int orte_debug_flag;

    /* Define the info structure underlying the Open MPI universe system
    * instanced in ompi_rte_init.c */

    struct orte_universe_t {
        char *name;
        char *host;
        bool persistence;
        char *scope;
        bool console;
        char *seed_uri;             /**< OOB contact info for universe seed */
        bool console_connected;     /**< Indicates if console is connected */
        char *scriptfile;           /**< Name of file containing commands to be executed */
    };
    typedef struct orte_universe_t orte_universe_t;

OMPI_DECLSPEC extern orte_universe_t orte_universe_info;

    /**
     * Initialize the Open MPI support code
     *
     * This function initializes the Open MPI support code, including
     * malloc debugging and threads.  It should be called exactly once
     * by every application that utilizes any of the Open MPI support
     * libraries (including MPI applications, mpirun, and mpicc).
     *
     * This function should be called before \code ompi_rte_init, if
     * \code ompi_rte_init is to be called.
     */
OMPI_DECLSPEC    int ompi_init(int argc, char* argv[]);

    /**
     * Finalize the Open MPI support code
     *
     * Finalize the Open MPI support code.  Any function calling \code
     * ompi_init should call \code ompi_finalize.  This function should
     * be called after \code ompi_rte_finalize, if \code
     * ompi_rte_finalize is called.
     */
OMPI_DECLSPEC    int ompi_finalize(void);

    /**
     * Abort the current application with a pretty-print error message
     *
     * Aborts currently running application with \code abort(), pretty
     * printing an error message if possible.  Error message should be
     * specified using the standard \code printf() format.
     */
OMPI_DECLSPEC    int ompi_abort(int status, char *fmt, ...);


    /**
     * Initialize the Open run time environment
     *
     * Initlize the Open MPI run time environment, including process
     * control and out of band messaging.  This function should be
     * called exactly once, after \code ompi_init.  This function should
     * be called by every application using the RTE interface, including
     * MPI applications and mpirun.
     */
OMPI_DECLSPEC    int orte_init(ompi_cmd_line_t *cmd_line, bool *allow_multi_user_threads, bool *have_hidden_threads);

    /**
     * Finalize the Open run time environment
     *
     */
OMPI_DECLSPEC    int orte_finalize(void);

    /**
     * Hold for startup message to arrive, then decode it.
     */

OMPI_DECLSPEC    int orte_wait_startup_msg(void);

    /**
     * Change state as processes complete registration/unregistration
     */

OMPI_DECLSPEC    void orte_all_procs_registered(orte_gpr_notify_message_t* match, void* cbdata);

OMPI_DECLSPEC    void orte_all_procs_unregistered(orte_gpr_notify_message_t* match, void* cbdata);

OMPI_DECLSPEC	 int orte_monitor_procs_registered(void);

OMPI_DECLSPEC    int orte_monitor_procs_unregistered(void);

    /**
     * Setup rte command line options
     *
     * Defines the command line options specific to the rte/seed daemon
     *
     * @param cmd_line Pointer to an ompi_cmd_line_t object
     * @retval None
     */
OMPI_DECLSPEC    void orte_cmd_line_setup(ompi_cmd_line_t *cmd_line);


    /**
     * Parse the rte command line for options
     *
     * Parses the specified command line for rte specific options.
     * Fills the relevant global structures with the information obtained.
     *
     * @param cmd_line Command line to be parsed.
     * @retval None
     */
OMPI_DECLSPEC    void orte_parse_cmd_line(ompi_cmd_line_t *cmd_line);

    /**
     * Parse the rte command line for daemon-specific options
     *
     * Parses the specified command line for rte daemon-specific options.
     * Fills the relevant global structures with the information obtained.
     *
     * @param cmd_line Command line to be parsed.
     * @retval None
     */
OMPI_DECLSPEC    void orte_parse_daemon_cmd_line(ompi_cmd_line_t *cmd_line);

    /**
     * Check for universe existence
     *
     * Checks to see if a specified universe exists. If so, attempts
     * to connect to verify that the universe is accepting connections.
     * If both ns and gpr replicas provided, first checks for those
     * connections. Gets any missing info from the universe contact.
     *
     * @param None Reads everything from the process_info and system_info
     * structures
     *
     * @retval OMPI_SUCCESS Universe found and connection accepted
     * @retval OMPI_NO_CONNECTION_ALLOWED Universe found, but not persistent or
     * restricted to local scope
     * @retval OMPI_CONNECTION_FAILED Universe found, but connection attempt
     * failed. Probably caused by unclean termination of the universe seed
     * daemon.
     * @retval OMPI_CONNECTION_REFUSED Universe found and contact made, but
     * universe refused to allow connection.
     */
OMPI_DECLSPEC    int orte_universe_exists(void);

    /**
     * Parse the RTE environmental variables
     *
     * Checks the environmental variables and passes their info (where
     * set) into the respective info structures. Sets ALL Open MPI
     * default values in universe, process, and system structures.
     *
     * @param None
     *
     * @retval None
     */
OMPI_DECLSPEC    void orte_parse_environ(void);


    /**
     * Startup a job - notify processes that all ready to begin
     */
OMPI_DECLSPEC   int orte_job_startup(orte_jobid_t jobid);

    /**
     * Shutdown a job - notify processes that all ready to stop
     */
OMPI_DECLSPEC   int orte_job_shutdown(orte_jobid_t jobid);

    /**
     * Complete initialization of the RTE
     */
OMPI_DECLSPEC   int orte_init_cleanup(bool *allow_user_threads, bool *have_hidden_threads);

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif /* OMPI_RUNTIME_H */
