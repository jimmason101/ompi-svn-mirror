/*
  openmpi.c - main program for spawning persistent universe.

  --------------------------------------------------------------------------

  Authors:	Ralph H. Castain <rhc@lanl.gov>

  --------------------------------------------------------------------------

*/
#include "ompi_config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/param.h>
#include <errno.h>

#include "event/event.h"
#include "include/constants.h"

#include "util/argv.h"
#include "util/output.h"
#include "util/os_path.h"
#include "util/pack.h"
#include "util/sys_info.h"
#include "util/cmd_line.h"
#include "util/proc_info.h"
#include "util/session_dir.h"
#include "util/universe_setup_file_io.h"

#include "mca/base/base.h"
#include "mca/oob/base/base.h"
#include "mca/ns/base/base.h"
#include "mca/pcm/base/base.h"

#include "tools/ompid/ompid.h"

#include "runtime/runtime.h"

extern char** environ;


int main(int argc, char **argv)
{
    ompi_cmd_line_t *cmd_line = NULL;
    char *universe = NULL, *tmp=NULL, **tmp2=NULL;
    char *contact_info=NULL;
    char **seed_argv;
    pid_t pid;
    bool multi_thread = false;
    bool hidden_thread = false;
    int ret, i, seed_argc;
    ompi_rte_node_schedule_t *sched;
    char cwd[MAXPATHLEN];
    ompi_list_t *nodelist = NULL;
    ompi_list_t schedlist;

    /* require tcp oob */
    setenv("OMPI_MCA_oob_base_include", "tcp", 1);

    /*
     * Intialize the Open MPI environment
     */
    if (OMPI_SUCCESS != ompi_init(argc, argv)) {
        /* BWB show_help */
        printf("show_help: ompi_init failed\n");
        return ret;
    }

    /* get the system info and setup defaults */
    ompi_sys_info();
    ompi_universe_info.host = strdup(ompi_system_info.nodename);
    ompi_universe_info.uid = strdup(ompi_system_info.user);

    /* give myself default bootstrap name */
    ompi_process_info.name = ns_base_create_process_name(MCA_NS_BASE_CELLID_MAX,
							 MCA_NS_BASE_JOBID_MAX,
							 MCA_NS_BASE_VPID_MAX);

    /* setup to read common command line options that span all Open MPI programs */
    cmd_line = OBJ_NEW(ompi_cmd_line_t);

    ompi_cmd_line_make_opt(cmd_line, 'v', "version", 0,
			   "Show version of Open MPI and this program");

    ompi_cmd_line_make_opt(cmd_line, 'h', "help", 0,
			   "Show help for this function");


    /* setup rte command line arguments */
    ompi_rte_cmd_line_setup(cmd_line);

    /*
     * setup  mca command line arguments
     */
    if (OMPI_SUCCESS != (ret = mca_base_cmd_line_setup(cmd_line))) {
	/* BWB show_help */
	printf("show_help: mca_base_cmd_line_setup failed\n");
	return ret;
    }

    if (OMPI_SUCCESS != mca_base_cmd_line_process_args(cmd_line)) {
	/* BWB show_help */
	printf("show_help: mca_base_cmd_line_process_args\n");
	return ret;
    }

    /* parse the local commands */
    if (OMPI_SUCCESS != ompi_cmd_line_parse(cmd_line, true, argc, argv)) {
	exit(ret);
    }

    if (ompi_cmd_line_is_taken(cmd_line, "help") || 
        ompi_cmd_line_is_taken(cmd_line, "h")) {
        printf("...showing ompi_info help message...\n");
        exit(1);
    }

    if (ompi_cmd_line_is_taken(cmd_line, "version") ||
	ompi_cmd_line_is_taken(cmd_line, "v")) {
	printf("...showing off my version!\n");
	exit(1);
    }

    /* parse the cmd_line for rte options */
    ompi_rte_parse_cmd_line(cmd_line);


    /* start the initial barebones RTE (just OOB) so we can check universe existence */
    if (OMPI_SUCCESS != (ret = mca_base_open())) {
        /* JMS show_help */
        printf("show_help: mca_base_open failed\n");
        exit(ret);
    }
    ompi_rte_init_stage1(&multi_thread, &hidden_thread);

    /* check for local universe existence */
    if ((0 == strncmp(ompi_universe_info.host, ompi_system_info.nodename, strlen(ompi_system_info.nodename))) &&
	(OMPI_SUCCESS != (ret = ompi_rte_local_universe_exists())) &&
	(OMPI_ERR_NOT_IMPLEMENTED != ret)) {

	if (OMPI_ERR_NOT_FOUND != ret) {
	    /* if not found, then keep current name. otherwise,
	     * define unique name based on current one.
	     * either way, start new universe
	     */
	    universe = strdup(ompi_universe_info.name);
	    free(ompi_universe_info.name);
	    pid = getpid();
	    if (0 < asprintf(&ompi_universe_info.name, "%s-%d", universe, pid)) {
		fprintf(stderr, "error creating unique universe name - please report error to bugs@open-mpi.org\n");
		exit(1);
	    }
	}

	ompi_process_info.my_universe = strdup(ompi_universe_info.name);

	/* startup rest of RTE - needed for handshake with seed */
	ompi_process_info.ns_replica = NULL;
	ompi_process_info.gpr_replica = NULL;
    ompi_rte_init_stage2(&multi_thread, &hidden_thread);

	/* parse command line for rest of seed options */
	ompi_rte_parse_daemon_cmd_line(cmd_line);

	/* does not exist - need to start it locally
	 * using fork/exec process
	 */
	if ((pid = fork()) < 0) {
	    fprintf(stderr, "unable to start universe - please report error to bugs@open-mpi.org\n");
	    exit(-1);
	} else if (pid == 0) { /* child process does the exec */

	    /* build the environment for the seed
	     * including universe name and tmpdir_base
	     */
	    seed_argv = NULL;
	    seed_argc = 0;
	    ompi_argv_append(&seed_argc, &seed_argv, "ompid");
	    ompi_argv_append(&seed_argc, &seed_argv, "-seed");
	    ompi_argv_append(&seed_argc, &seed_argv, "-scope");
	    ompi_argv_append(&seed_argc, &seed_argv, ompi_universe_info.scope);
	    if (ompi_universe_info.persistence) {
		ompi_argv_append(&seed_argc, &seed_argv, "-persistent");
	    }
	    if (ompi_universe_info.web_server) {
		ompi_argv_append(&seed_argc, &seed_argv, "-webserver");
	    }
	    if (NULL != ompi_universe_info.scriptfile) {
		ompi_argv_append(&seed_argc, &seed_argv, "-script");
		ompi_argv_append(&seed_argc, &seed_argv, ompi_universe_info.scriptfile);
	    }
	    if (NULL != ompi_universe_info.hostfile) {
		ompi_argv_append(&seed_argc, &seed_argv, "-hostfile");
		ompi_argv_append(&seed_argc, &seed_argv, ompi_universe_info.hostfile);
	    }
	    /* provide my contact info as the temporary registry replica*/
	    contact_info = mca_oob_get_contact_info();
	    ompi_argv_append(&seed_argc, &seed_argv, "-gprreplica");
	    ompi_argv_append(&seed_argc, &seed_argv, contact_info);
	    /* add options for universe name and tmpdir_base, if provided */
	    ompi_argv_append(&seed_argc, &seed_argv, "-universe");
	    ompi_argv_append(&seed_argc, &seed_argv, ompi_universe_info.name);
	    if (NULL != ompi_process_info.tmpdir_base) {
		ompi_argv_append(&seed_argc, &seed_argv, "-tmpdir");
		ompi_argv_append(&seed_argc, &seed_argv, ompi_process_info.tmpdir_base);
	    }

	    /*
	     * spawn the local seed
	     */
	    if (0 > execvp("ompid", seed_argv)) {
		fprintf(stderr, "unable to exec daemon - please report error to bugs@open-mpi.org\n");
		fprintf(stderr, "errno: %s\n", strerror(errno));
		exit(1);
	    }
	}
    } else {  /* check for remote universe existence */
	/* future implementation: launch probe daemon, providing my contact info, probe
	 * checks for session directory on remote machine and transmits back results
	 * then probe dies
	 */
    }

    /* if console, kickoff console - point comm at universe */

    /* all done, so exit! */
    exit(0);

}

/* 	/\* start the rest of the RTE *\/ */
/* 	ompi_rte_init_stage2(&multi_thread, &hidden_thread); */

/* 	fprintf(stderr, "allocating procesors\n"); */

/* 	/\* allocate a processor to the job *\/ */
/* 	nodelist = ompi_rte_allocate_resources(0, 0, 1); */
/* 	if (NULL == nodelist) { */
/* 	    /\* BWB show_help *\/ */
/* 	    printf("show_help: ompi_rte_allocate_resources failed\n"); */
/* 	    exit(-1); */
/* 	} */

/* 	fprintf(stderr, "scheduling\n"); */

/* 	/\* */
/* 	 * "schedule" seed process */
/* 	 *\/ */
/* 	OBJ_CONSTRUCT(&schedlist,  ompi_list_t); */
/* 	sched = OBJ_NEW(ompi_rte_node_schedule_t); */
/* 	ompi_list_append(&schedlist, (ompi_list_item_t*) sched); */
/* 	ompi_argv_append(&(sched->argc), &(sched->argv), */
/* 			 "/Users/rcastain/Documents/OpenMPI/src/tools/ompid/ompid"); */
/* /\* 	ompi_cmd_line_get_tail(cmd_line, &(sched->argc), &(sched->argv)); *\/ */

/* 	if (OMPI_SUCCESS != mca_pcm.pcm_spawn_procs(0, &schedlist)) { */
/* 	    printf("show_help: woops!  the seed didn't spawn :( \n"); */
/* 	    exit(-1); */
/* 	} */
/* 	    getcwd(cwd, MAXPATHLEN); */
/* 	    sched->cwd = strdup(cwd); */
/* 	    sched->nodelist = nodelist; */

