/* -*- C -*-
 * 
 * $HEADER$
 *
 */

#include "lam_config.h"

#include "lam/constants.h"
#include "lam/types.h"
#include "lam/util/malloc.h"
#include "mca/mca.h"
#include "mca/lam/pcm/pcm.h"
#include "mca/lam/pcm/cofs/src/pcm_cofs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Struct of function pointers and all that to let us be initialized
 */
mca_pcm_base_module_1_0_0_t mca_pcm_cofs_module = {
  {
    MCA_PCM_BASE_VERSION_1_0_0,

    "cofs", /* MCA module name */
    1,  /* MCA module major version */
    0,  /* MCA module minor version */
    0,  /* MCA module release version */
    mca_pcm_cofs_open,  /* module open */
    mca_pcm_cofs_close /* module close */
  },
  {
    false /* checkpoint / restart */
  },
  mca_pcm_cofs_query,  /* module query */
  mca_pcm_cofs_init,    /* module init */
  mca_pcm_cofs_finalize
};


struct mca_pcm_1_0_0_t mca_pcm_cofs_1_0_0 = {
  mca_pcm_cofs_query_get_nodes,

  mca_pcm_cofs_handle_new,
  mca_pcm_cofs_handle_get,
  mca_pcm_cofs_handle_free,

  mca_pcm_cofs_job_can_spawn,
  mca_pcm_cofs_job_set_arguments,
  mca_pcm_cofs_job_launch_procs,
  mca_pcm_cofs_job_rendezvous,
  mca_pcm_cofs_job_wait,
  mca_pcm_cofs_job_running,
  mca_pcm_cofs_job_list_running,

  mca_pcm_cofs_proc_startup,
  mca_pcm_cofs_proc_get_peers,
  mca_pcm_cofs_proc_get_me,
  mca_pcm_cofs_proc_get_parent
};


char mca_pcm_cofs_comm_loc[LAM_PATH_MAX];
int mca_pcm_cofs_my_vpid;
char *mca_pcm_cofs_my_handle = NULL;

int
mca_pcm_cofs_open(lam_cmd_line_t *cmd)
{
  return LAM_SUCCESS;
}


int
mca_pcm_cofs_close(void)
{
  return LAM_SUCCESS;
}


int
mca_pcm_cofs_query(int *priority)
{
  char *test_ret;

  *priority = 0;

  /* BWB - remove printfs once things settle down some... */
  test_ret = getenv("MCA_common_lam_cofs_my_vpid");
  if (test_ret == NULL) {
    printf("COFS PCM will not be running because MCA_common_lam_cofs_my_vpid not set\n");
    return LAM_ERROR;
  }

  test_ret = getenv("MCA_common_lam_cofs_job_handle");
  if (test_ret == NULL) {
    printf("COFS PCM will not be running because MCA_common_lam_cofs_job_handle not set\n");
    return LAM_ERROR;
  }

  test_ret = getenv("MCA_common_lam_cofs_num_procs");
  if (test_ret == NULL) {
    printf("COFS PCM will not be running because MCA_common_lam_cofs_num_procs not set\n");
    return LAM_ERROR;
  }

  return LAM_SUCCESS;
}


struct mca_pcm_1_0_0_t*
mca_pcm_cofs_init(void)
{
  char *tmp;
  FILE *fp;

  /*
   * BWB - fix me, make register the "right" way...
   */
  tmp = getenv("MCA_common_lam_cofs_comm_dir");
  if (tmp == NULL) {
    /* make it $HOME */
    tmp = getenv("HOME");
    if (tmp == NULL) {
      printf("pcm_cofs can not find communication dir\n");
      return NULL;
    }
    snprintf(mca_pcm_cofs_comm_loc, LAM_PATH_MAX, "%s/cofs", tmp);
  } else {
    snprintf(mca_pcm_cofs_comm_loc, LAM_PATH_MAX, "%s", tmp);
  }

  /*
   * See if we can write in our directory...
   */
  tmp = LAM_MALLOC(strlen(mca_pcm_cofs_comm_loc) + 5);
  if (tmp == NULL) return NULL;
  sprintf(tmp, "%s/me", mca_pcm_cofs_comm_loc);
  fp = fopen(tmp, "w");
  if (fp == NULL) {
    printf("pcm_cofs can not write in communication dir\n");
    LAM_FREE(tmp);
    return NULL;
  }
  fclose(fp);
  unlink(tmp);
  LAM_FREE(tmp);

  /*
   * BWB - fix me, make register the "right" way...
   */
  /* find our vpid */
  tmp = getenv("MCA_common_lam_cofs_my_vpid");
  if (tmp == NULL) {
    printf("pcm_cofs can not find vpid\n");
    return NULL;
  }
  mca_pcm_cofs_my_vpid = atoi(tmp);

  mca_pcm_cofs_my_handle = getenv("MCA_common_lam_cofs_job_handle");

  mca_pcm_cofs_procs = NULL;

  tmp = getenv("MCA_common_lam_cofs_num_procs");
  if (tmp == NULL) {
    printf("pcm_cofs can not find nprocs\n");
    return NULL;
  }
  mca_pcm_cofs_nprocs = atoi(tmp);

  return &mca_pcm_cofs_1_0_0;
}


int
mca_pcm_cofs_finalize(void)
{
  if (mca_pcm_cofs_procs != NULL) {
    LAM_FREE(mca_pcm_cofs_procs);
    mca_pcm_cofs_procs = NULL;
    mca_pcm_cofs_nprocs = 0;
  }

  return LAM_SUCCESS;
}

