/* SPDX-License-Identifier: MIT */
/* Mixed-criticality scheduling domain extensions.
 *
 * Two criticality levels: CRIT_HI (hard-RT control) and CRIT_LO
 * (web/telemetry). HI domains get guaranteed minimum bandwidth
 * (CONFIG_MIXED_CRIT_HI_PCT, default 70%). LO domains get the remainder.
 *
 * Escalation state machine:
 *   NORMAL -> ESCALATED: HI domain overruns its budget (consumed > quantum).
 *                        LO peer is force-depleted.
 *   ESCALATED -> NORMAL: HI domain refill fires.  LO peer re-enabled.
 *
 * Integration: hooks into the existing sched_domain refill callout path.
 * No new locks -- uses atomic operations on domain state fields.
 */

#include <mazu/assert.h>
#include <mazu/eventlog.h>
#include <mazu/sched.h>
#include <mazu/time.h>

void sched_mc_init_pair(struct sched_domain *hi_dom,
                        struct sched_domain *lo_dom)
{
    assert(hi_dom && lo_dom);
    hi_dom->criticality = SCHED_DOMAIN_CRIT_HI;
    lo_dom->criticality = SCHED_DOMAIN_CRIT_LO;
    hi_dom->mc_state = MC_NORMAL;
    lo_dom->mc_state = MC_NORMAL;
    hi_dom->mc_peer = lo_dom;
    lo_dom->mc_peer = hi_dom;
}

void sched_mc_check_escalation(struct sched_domain *hi_dom)
{
    assert(hi_dom->criticality == SCHED_DOMAIN_CRIT_HI);

    if (hi_dom->mc_state != MC_NORMAL)
        return;

    /* Escalation trigger: HI domain consumed more than its budget. */
    u64 consumed = __atomic_load_n(&hi_dom->consumed_ticks, __ATOMIC_RELAXED);
    if (consumed <= hi_dom->quantum_ticks)
        return;

    /* Escalate: suspend LO peer by force-depleting it. */
    struct sched_domain *lo = hi_dom->mc_peer;
    if (!lo)
        return;

    hi_dom->mc_state = MC_ESCALATED;
    lo->mc_state = MC_ESCALATED;

    __atomic_store_n(&lo->state, DOMAIN_DEPLETED, __ATOMIC_RELEASE);

    KTRACE("event=mc_escalate hi_consumed=%lu hi_quantum=%lu", consumed,
           hi_dom->quantum_ticks);
}

void sched_mc_check_recovery(struct sched_domain *hi_dom)
{
    assert(hi_dom->criticality == SCHED_DOMAIN_CRIT_HI);

    if (hi_dom->mc_state != MC_ESCALATED)
        return;

    struct sched_domain *lo = hi_dom->mc_peer;
    if (!lo)
        return;

    /* Re-enable LO domain and reset budget. */
    __atomic_store_n(&lo->state, DOMAIN_ACTIVE, __ATOMIC_RELEASE);
    __atomic_store_n(&lo->consumed_ticks, 0, __ATOMIC_RELAXED);

    hi_dom->mc_state = MC_NORMAL;
    lo->mc_state = MC_NORMAL;

    KTRACE("event=mc_recovery");
}

#include __INC_TEST(mixed_crit)
