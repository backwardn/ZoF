#!/usr/bin/env ksh -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2013, 2016 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib

#
# DESCRIPTION:
# Verify that 'zpool iostat' can be executed as non-root.
#
# STRATEGY:
# 1. Create an array of options.
# 2. Execute each element of the array.
# 3. Verify that a success is returned.
#

verify_runnable "both"

typeset testpool
if is_global_zone ; then
	testpool=$TESTPOOL
else
	testpool=${TESTPOOL%%/*}
fi

set -A args "iostat" "iostat $testpool"

log_assert "zpool iostat [pool_name ...] [interval]"

typeset -i i=0
while [[ $i -lt ${#args[*]} ]]; do
	log_must zpool ${args[i]}
	((i = i + 1))
done

log_pass "The sub-command 'iostat' succeeds as non-root."
