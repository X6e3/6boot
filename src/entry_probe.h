/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ENTRY_PROBE_H
#define ENTRY_PROBE_H

/* Call the 6boot P3 entry probe and record its result under /chosen in dt.
 * Never blocks the boot: a failed call is recorded and the boot goes on. */
int entry_probe_run(void *dt);

#endif
