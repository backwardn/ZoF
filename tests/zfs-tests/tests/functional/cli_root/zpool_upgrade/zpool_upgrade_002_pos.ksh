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
# Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright 2015 Nexenta Systems, Inc.  All rights reserved.
#

. $STF_SUITE/tests/functional/cli_root/zpool_upgrade/zpool_upgrade.kshlib

#
# DESCRIPTION:
# import pools of all versions - zpool upgrade on each pools works
#
# STRATEGY:
# 1. Execute the command with several invalid options
# 2. Verify a 0 exit status for each
#

verify_runnable "global"

function cleanup
{
	destroy_upgraded_pool $config
}

log_assert "Import pools of all versions - zpool upgrade on each pool works"
log_onexit cleanup

for config in $CONFIGS; do
    create_old_pool $config
    check_upgrade $config
    destroy_upgraded_pool $config
done

log_pass "Import pools of all versions - zpool upgrade on each pool works"
