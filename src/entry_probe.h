/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ENTRY_PROBE_H
#define ENTRY_PROBE_H
/* Execution, return validation and FDT publication are independent outcomes.
 * The caller, not the P3 adapter, owns experiment selection/handoff policy. */
struct entry_probe_outcome {
    int called, valid, context_ok, publication_error;
};
int entry_probe_run(void *dt, struct entry_probe_outcome *outcome);
#endif
