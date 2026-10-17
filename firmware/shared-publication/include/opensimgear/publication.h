#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { OSG_PUBLICATION_MAX_TARGETS=32, OSG_PUBLICATION_MIN_TTL_MS=250,
       OSG_PUBLICATION_MAX_TTL_MS=5000, OSG_PUBLICATION_DISABLED_ORDER=255 };
typedef struct {
    uint8_t publisher[16];
    uint32_t boot;
    /* Authoritative product generation changes on every route/connection change.
     * It pins the native epoch/node/lease tuple, not a browser supplied revision. */
    uint64_t route_generation;
} osg_publication_scope_t;
typedef struct {
    uint8_t identity[16];
    uint32_t boot,configuration_generation;
    uint64_t profile_revision;
    bool input_capable,enabled;
    uint8_t report_id,order;
} osg_publication_target_t;
typedef struct {
    uint8_t count;
    osg_publication_target_t targets[OSG_PUBLICATION_MAX_TARGETS];
} osg_publication_view_t;
typedef struct { uint32_t lease_id,expires_at; uint64_t transition; } osg_publication_ticket_t;
typedef enum {
    OSG_PUBLICATION_IDLE, OSG_PUBLICATION_OWNED,
    OSG_PUBLICATION_RESTORING, OSG_PUBLICATION_ATTENTION
} osg_publication_phase_t;
typedef enum {
    OSG_PUBLICATION_OK, OSG_PUBLICATION_NO_CHANGE, OSG_PUBLICATION_RESTORE,
    OSG_PUBLICATION_PRESERVED, OSG_PUBLICATION_INVALID,
    OSG_PUBLICATION_BUSY, OSG_PUBLICATION_NOT_OWNED, OSG_PUBLICATION_EXPIRED,
    OSG_PUBLICATION_STALE_TIME, OSG_PUBLICATION_EXHAUSTED,
    OSG_PUBLICATION_UNVERIFIED
} osg_publication_result_t;
/* Private product state. Never serialize owner or the complete lease to a host.
 * One instance controls one bounded publisher transaction; no flash/config IO.
 * All calls and actual descriptor changes share one serialized native owner. */
typedef struct {
    osg_publication_scope_t scope;
    uint8_t owner[16];
    osg_publication_view_t original,desired,last_owned;
    uint32_t lease_counter,now;
    uint64_t transition;
    osg_publication_ticket_t ticket;
    osg_publication_phase_t phase;
    osg_publication_result_t outcome;
    bool clock_started,apply_started,apply_confirmed;
} osg_publication_t;
void osg_publication_init(osg_publication_t *);
bool osg_publication_view_valid(const osg_publication_view_t *);
/* actual is an atomic, settled firmware-local read, never a host fallback.
 * desired may alter enabled/order only; identity/config/profile/report IDs stay
 * pinned. A repeated exact claim returns the same ticket and never recaptures
 * baseline, reschedules a transition or implicitly extends expiry. Use renew. */
osg_publication_result_t osg_publication_claim_current(osg_publication_t *,
    const osg_publication_scope_t *,const uint8_t owner[16],
    const osg_publication_view_t *actual,const osg_publication_view_t *desired,
    uint32_t now,uint32_t ttl,osg_publication_ticket_t *);
osg_publication_result_t osg_publication_renew(osg_publication_t *,
    const osg_publication_scope_t *,const uint8_t owner[16],uint32_t lease,
    uint32_t now,uint32_t ttl,osg_publication_ticket_t *);
/* apply_allowed is a prospective read-only check, never IO admission.
 * Immediately before native descriptor IO, begin_apply records the one-shot
 * admitted intent in the SAME non-yielding owner context as activation. It can
 * succeed only once per lease. Actual readback confirmation is separate. If
 * confirmation is lost, expiry still recognizes the exact admitted desired
 * state as owned for comparison/restoration. No method acknowledges inputs. */
bool osg_publication_apply_allowed(const osg_publication_t *,
    const osg_publication_scope_t *,const uint8_t owner[16],
    const osg_publication_ticket_t *,const osg_publication_view_t *actual,uint32_t now);
bool osg_publication_begin_apply(osg_publication_t *,
    const osg_publication_scope_t *,const uint8_t owner[16],
    const osg_publication_ticket_t *,const osg_publication_view_t *actual,uint32_t now);
bool osg_publication_confirm_applied(osg_publication_t *,
    const osg_publication_scope_t *,const uint8_t owner[16],
    const osg_publication_ticket_t *,const osg_publication_view_t *actual,uint32_t now);
/* Revokes delivery BEFORE returning. RESTORE is permission to compare and
 * restore the captured original, not evidence that a driver write completed.
 * Unknown actual state retains an attention record and blocks new acquisition.
 * Duplicate release is read-only/idempotent for this exact owner and lease. */
osg_publication_result_t osg_publication_release(osg_publication_t *,
    const osg_publication_scope_t *,const uint8_t owner[16],uint32_t lease,
    const osg_publication_view_t *actual,uint32_t now);
osg_publication_result_t osg_publication_tick(osg_publication_t *,
    const osg_publication_view_t *actual,uint32_t now);
osg_publication_result_t osg_publication_invalidate(osg_publication_t *,
    const osg_publication_scope_t *,const osg_publication_view_t *actual,uint32_t now);
/* Recheck immediately before restoration; changed state is preserved and
 * cancels the pending restore. Failed/unknown reads retain attention. Product
 * must cancel old scheduled transitions after any revocation/generation change.
 * Recovery may inspect/release THIS lease; it must never claim or resume it. */
bool osg_publication_restore_allowed(osg_publication_t *,
    const osg_publication_ticket_t *,const osg_publication_view_t *actual);
bool osg_publication_confirm_restored(osg_publication_t *,
    const osg_publication_ticket_t *,const osg_publication_view_t *actual);
