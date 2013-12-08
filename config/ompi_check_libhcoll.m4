# -*- shell-script -*-
#
# Copyright (c) 2011 Mellanox Technologies. All rights reserved.

# Copyright (c) 2013 Cisco Systems, Inc.  All rights reserved.
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# OMPI_CHECK_HCOLL(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
# check if hcoll support can be found.  sets prefix_{CPPFLAGS, 
# LDFLAGS, LIBS} as needed and runs action-if-found if there is
# support, otherwise executes action-if-not-found
AC_DEFUN([OMPI_CHECK_HCOLL],[
dnl To be checked/fixed by Jeff and Alexander in Chicago at the OMPI
dnl dev meeting in Dec 2013
dnl    OPAL_VAR_SCOPE_PUSH([ompi_check_hcoll_dir ompi_hcoll_libdir ompi_check_hcoll_incdir ompi_check_hcoll_libs ompi_check_hcoll_happy CPPFLAGS_save LDFLAGS_save LIBS_save])
    OPAL_VAR_SCOPE_PUSH([CPPFLAGS_save LDFLAGS_save LIBS_save])

    AC_ARG_WITH([hcoll],
        [AC_HELP_STRING([--with-hcoll(=DIR)],
             [Build hcoll (Mellanox Hierarchical Collectives) support, searching for libraries in DIR])])
    OMPI_CHECK_WITHDIR([hcoll], [$with_hcoll], [lib/libhcoll.so])

    AS_IF([test "$with_hcoll" != "no"],
          [AS_IF([test ! -z "$with_hcoll" -a "$with_hcoll" != "yes"],
			  [ompi_check_hcoll_dir=$with_hcoll
			   ompi_check_hcoll_libdir="$ompi_check_hcoll_dir/lib"
			   ompi_check_hcoll_incdir="$ompi_check_hcoll_dir/include"
			   ompi_check_hcoll_libs=hcoll

			   coll_hcoll_extra_CPPFLAGS="-I$ompi_check_hcoll_incdir/hcoll -I$ompi_check_hcoll_incdir/hcoll/api"

			   AC_SUBST([coll_hcoll_extra_CPPFLAGS])
			   AC_SUBST([coll_hcoll_HOME], "$ompi_check_hcoll_dir")


			   CPPFLAGS_save=$CPPFLAGS
			   LDFLAGS_save=$LDFLAGS
			   LIBS_save=$LIBS
			   CPPFLAGS="$CPPFLAGS $coll_hcoll_extra_CPPFLAGS"

			   OPAL_LOG_MSG([$1_CPPFLAGS : $$1_CPPFLAGS], 1)
			   OPAL_LOG_MSG([$1_LDFLAGS  : $$1_LDFLAGS], 1)
			   OPAL_LOG_MSG([$1_LIBS     : $$1_LIBS], 1)

			   OMPI_CHECK_PACKAGE([$1],
				   [hcoll_api.h],
				   [$ompi_check_hcoll_libs],
				   [hcoll_get_version],
				   [],
				   [$ompi_check_hcoll_dir],
				   [$ompi_check_hcoll_libdir],
				   [ompi_check_hcoll_happy="yes"],
				   [ompi_check_hcoll_happy="no"])

			   CPPFLAGS=$CPPFLAGS_save
			   LDFLAGS=$LDFLAGS_save
			   LIBS=$LIBS_save],
			   [ompi_check_hcoll_happy="no"])
          ])



    AS_IF([test "$ompi_check_hcoll_happy" = "yes" -a "$enable_progress_threads" = "yes"],
          [AC_MSG_WARN([hcoll driver does not currently support progress threads.  Disabling HCOLL.])
           ompi_check_hcoll_happy="no"])

    AS_IF([test "$ompi_check_hcoll_happy" = "yes"],
          [$2],
          [AS_IF([test ! -z "$with_hcoll" -a "$with_hcoll" != "no"],
                 [AC_MSG_ERROR([HCOLL support requested but not found.  Aborting])])
           $3])

    OPAL_VAR_SCOPE_POP
])

