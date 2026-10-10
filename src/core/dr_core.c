#include "dr_core.h"
#include <math.h>
#include <stddef.h>
#include <string.h>
#include <float.h>

#define PI 3.14159265358979323846264338327950288
#define NS_PER_S 1000000000.0
static double minimum(double a, double b) { return a < b ? a : b; }
static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static int finite_value(double x) { return isfinite(x); }
static int context_valid(mx5_dr_context a) {
    return a.source_epoch && a.session_epoch && a.generation;
}
static int context_equal(mx5_dr_context a, mx5_dr_context b) {
    return a.source_epoch == b.source_epoch && a.session_epoch == b.session_epoch &&
           a.generation == b.generation;
}
static double wrap(double x) {
    x = fmod(x, 2.0 * PI);
    return x < 0.0 ? x + 2.0 * PI : x;
}
static double lon_wrap(double x) {
    x = fmod(x + 180.0, 360.0);
    if (x < 0.0) x += 360.0;
    return x - 180.0;
}
static mx5_dr_result fail(mx5_dr_core *c, mx5_dr_result why) {
    c->estimate.valid = 0;
    c->estimate.model_valid = 0;
    c->estimate.reason = why;
    c->estimate.state = why == MX5_DR_E_LIMIT ? MX5_DR_LIMIT_REACHED : MX5_DR_INVALID;
    c->seeded = 0;
    c->stop_dwell_s = 0.0;
    return why;
}
mx5_dr_config mx5_dr_default_config(void) {
    mx5_dr_config p;
    memset(&p, 0, sizeof p);
    p.duration_max_s = 60.0; p.distance_max_m = 1500.0; p.error_max_m = 100.0;
    p.extended_limits = 0;
    p.integration_step_s = 0.05; p.speed_error_mps = 0.3; p.yaw_error_rad_s = 0.002;
    p.stop_enter_mps = 0.2; p.stop_exit_mps = 0.5; p.stop_hold_s = 1.5;
    p.stop_yaw_max_rad_s = 0.02;
    p.physical_speed_max_mps = 100.0; p.physical_yaw_max_rad_s = 2.0;
    p.interval_max_ns = 250000000; p.sample_age_max_ns = 250000000;
    p.snapshot_age_max_ns = 150000000; p.time_uncertainty_max_ns = 100000000;
    return p;
}
/* Extended caps (BETA tunnel mode, MODEL domain only): numeric sanity bounds,
 * not behavioural limits - 6 h, 1000 km, 1000 km of budget. */
#define EXT_DURATION_MAX_S 21600.0
#define EXT_DISTANCE_MAX_M 1000000.0
#define EXT_ERROR_MAX_M 1000000.0
static int valid_config(const mx5_dr_config *p, int allow_extended) {
    const int ext = p && p->extended_limits == 1;
    if (p && (p->extended_limits > 1 || p->reserved_zero)) return 0;
    if (p && (p->hold_stopped_yaw > 1 || (p->hold_stopped_yaw && !allow_extended))) return 0;
    if (ext && !allow_extended) return 0;
    return p && finite_value(p->duration_max_s) && p->duration_max_s > 0.0 && p->duration_max_s <= (ext ? EXT_DURATION_MAX_S : 60.0) &&
        finite_value(p->distance_max_m) && p->distance_max_m > 0.0 && p->distance_max_m <= (ext ? EXT_DISTANCE_MAX_M : 1500.0) &&
        finite_value(p->error_max_m) && p->error_max_m > 0.0 && p->error_max_m <= (ext ? EXT_ERROR_MAX_M : 100.0) &&
        finite_value(p->integration_step_s) && p->integration_step_s >= 0.001 && p->integration_step_s <= 0.05 &&
        finite_value(p->speed_error_mps) && p->speed_error_mps >= 0.0 &&
        finite_value(p->yaw_error_rad_s) && p->yaw_error_rad_s >= 0.0 &&
        finite_value(p->stop_enter_mps) && p->stop_enter_mps >= 0.0 &&
        finite_value(p->stop_exit_mps) && p->stop_exit_mps > p->stop_enter_mps &&
        finite_value(p->stop_hold_s) && p->stop_hold_s > 0.0 &&
        finite_value(p->stop_yaw_max_rad_s) && p->stop_yaw_max_rad_s >= 0.0 &&
        finite_value(p->physical_speed_max_mps) && p->physical_speed_max_mps > p->stop_exit_mps &&
        finite_value(p->physical_yaw_max_rad_s) && p->physical_yaw_max_rad_s > p->stop_yaw_max_rad_s &&
        p->interval_max_ns > 0 && p->interval_max_ns <= 250000000 &&
        p->sample_age_max_ns > 0 && p->sample_age_max_ns <= 250000000 &&
        p->snapshot_age_max_ns > 0 && p->snapshot_age_max_ns <= 150000000 &&
        p->time_uncertainty_max_ns <= p->sample_age_max_ns;
}
static mx5_dr_result init_common(mx5_dr_core *c, const mx5_dr_config *p, mx5_dr_context x, int allow_extended) {
    if (!c) return MX5_DR_E_CONFIG;
    memset(c, 0, sizeof(*c));
    if (!valid_config(p, allow_extended) || !context_valid(x)) return fail(c, MX5_DR_E_CONFIG);
    c->config = *p; c->configured = 1;
    c->estimate.context = x; c->estimate.state = MX5_DR_UNSEEDED;
    c->estimate.reason = MX5_DR_E_NO_SEED;
    return MX5_DR_OK;
}
mx5_dr_result mx5_dr_init(mx5_dr_core *c, const mx5_dr_config *p, mx5_dr_context x) {
    return init_common(c, p, x, 0);
}
mx5_dr_result mx5_dr_init_model(mx5_dr_core *c, const mx5_dr_config *p, mx5_dr_context x) {
    mx5_dr_result r=init_common(c,p,x,1);
    if (c) { c->domain=MX5_DR_MODEL_DOMAIN; c->estimate.domain=MX5_DR_MODEL_DOMAIN; }
    return r;
}
mx5_dr_result mx5_dr_reset(mx5_dr_core *c, mx5_dr_context x) {
    mx5_dr_config p;
    if (!c || !c->configured) return MX5_DR_E_CONFIG;
    p = c->config;
    return c->domain==MX5_DR_MODEL_DOMAIN ? mx5_dr_init_model(c,&p,x) : mx5_dr_init(c, &p, x);
}
mx5_dr_result mx5_dr_seed(mx5_dr_core *c, const mx5_dr_anchor *a) {
    mx5_dr_snapshot s;
    if (!c || !c->configured || !a) return MX5_DR_E_CONFIG;
    if (!context_equal(a->context, c->estimate.context)) return MX5_DR_E_CONTEXT;
    if (c->estimate.state == MX5_DR_ACTIVE || c->estimate.state == MX5_DR_NATIVE)
        return MX5_DR_E_STATE;
    if (c->domain==MX5_DR_MODEL_DOMAIN) {
        if (a->quality!=MX5_DR_MODEL || a->validated || a->heading_valid || a->calibration_verified)
            return fail(c,MX5_DR_E_QUALITY);
    } else if (!a->validated || !a->heading_valid || !a->calibration_verified || a->quality != MX5_DR_VALID)
        return fail(c, MX5_DR_E_QUALITY);
    if (!a->anchor_id || !a->position_seq || !a->measured_ns || !a->utc_ns ||
        a->position_seq <= c->highest_position_seq || a->position_seq <= c->last_control_seq ||
        (c->highest_position_seq && a->measured_ns <= c->anchor.measured_ns))
        return fail(c, MX5_DR_E_SEQUENCE);
    if (!finite_value(a->latitude_deg) || fabs(a->latitude_deg) >= 85.0 ||
        !finite_value(a->longitude_deg) || a->longitude_deg < -180.0 || a->longitude_deg > 180.0 ||
        !finite_value(a->body_heading_rad) || a->body_heading_rad < 0.0 || a->body_heading_rad >= 2.0*PI ||
        !finite_value(a->position_error_m) || a->position_error_m < 0.0 ||
        !finite_value(a->heading_error_rad) || a->heading_error_rad < 0.0)
        return fail(c, MX5_DR_E_NUMERIC);
    if (a->position_error_m > c->config.error_max_m) return fail(c, MX5_DR_E_LIMIT);
    memset(&s, 0, sizeof(s));
    s.domain = c->domain;
    s.context = a->context; s.anchor_id = a->anchor_id;
    s.processed_position_seq = a->position_seq;
    s.frontier_ns = a->measured_ns; s.derived_utc_ns = a->utc_ns;
    s.latitude_deg = a->latitude_deg; s.longitude_deg = lon_wrap(a->longitude_deg);
    s.body_heading_rad = a->body_heading_rad;
    s.error_budget_m = a->position_error_m; s.heading_budget_rad = a->heading_error_rad;
    s.state = MX5_DR_READY; s.reason = MX5_DR_OK;
    c->estimate = s; c->anchor = *a; c->seeded = 1; c->have_interval = 0;
    c->highest_position_seq = a->position_seq; c->stop_dwell_s = 0.0;
    return MX5_DR_OK;
}
mx5_dr_result mx5_dr_control(mx5_dr_core *c, mx5_dr_control_kind kind,
                            mx5_dr_context x, uint64_t seq) {
    if (!c || !c->configured) return MX5_DR_E_CONFIG;
    if (x.source_epoch != c->estimate.context.source_epoch ||
        x.session_epoch != c->estimate.context.session_epoch ||
        x.generation <= c->estimate.context.generation || !seq || seq <= c->last_control_seq || seq <= c->highest_position_seq)
        return MX5_DR_E_CONTEXT;
    if (kind < MX5_DR_GAP || kind > MX5_DR_DISABLE) return fail(c, MX5_DR_E_CONFIG);
    c->estimate.context = x; c->estimate.processed_position_seq = seq;
    c->last_control_seq = seq; c->estimate.valid = 0; c->estimate.model_valid = 0;
    if (kind == MX5_DR_GAP) {
        if (c->seeded && c->estimate.state != MX5_DR_READY) return fail(c,MX5_DR_E_STATE);
        if (!c->seeded) {
            c->estimate.reason = MX5_DR_E_NO_SEED;
            return MX5_DR_E_NO_SEED;
        }
        c->estimate.state = MX5_DR_ACTIVE;
        c->estimate.reason = MX5_DR_OK;
        return MX5_DR_OK;
    }
    c->seeded = 0; c->have_interval = 0; c->stop_dwell_s = 0.0;
    c->estimate.state = kind == MX5_DR_GPS_RETURN ? MX5_DR_REACQUIRING :
                        kind == MX5_DR_NATIVE_POSITION ? MX5_DR_NATIVE : MX5_DR_UNSEEDED;
    c->estimate.reason = MX5_DR_E_NO_SEED;
    return MX5_DR_OK;
}
static int same_evidence(const mx5_dr_evidence *a, const mx5_dr_evidence *b) {
    return a->source_id == b->source_id && a->source_epoch == b->source_epoch &&
        a->producer_seq == b->producer_seq && a->measured_ns == b->measured_ns &&
        a->received_ns == b->received_ns && a->lease_until_ns == b->lease_until_ns &&
        a->time_uncertainty_ns == b->time_uncertainty_ns && a->quality == b->quality &&
        a->freshness == b->freshness;
}
static int same_interval(const mx5_dr_interval *a, const mx5_dr_interval *b) {
    return context_equal(a->context,b->context) && a->interval_seq == b->interval_seq &&
        a->start_ns == b->start_ns && a->end_ns == b->end_ns && a->received_ns == b->received_ns &&
        a->speed_mps == b->speed_mps && a->yaw_rad_s == b->yaw_rad_s &&
        a->reverse_active == b->reverse_active && a->raw_yaw == b->raw_yaw && a->yaw_count == b->yaw_count &&
        a->yaw_is_mean == b->yaw_is_mean && a->yaw_window_start_ns == b->yaw_window_start_ns &&
        a->yaw_window_end_ns == b->yaw_window_end_ns &&
        same_evidence(&a->speed,&b->speed) && same_evidence(&a->yaw,&b->yaw) &&
        same_evidence(&a->reverse,&b->reverse);
}
static mx5_dr_result evidence_check(const mx5_dr_core *c, const mx5_dr_evidence *e,
                                    const mx5_dr_interval *i, int mean_yaw) {
    if (c->domain==MX5_DR_MODEL_DOMAIN) {
        if (e->quality!=MX5_DR_MODEL || e->freshness!=MX5_DR_MODEL_TIME)
            return MX5_DR_E_QUALITY;
    } else if (e->quality != MX5_DR_VALID || (e->freshness != MX5_DR_PRODUCER_TIME &&
        e->freshness != MX5_DR_SEQUENCE_WITH_BOUND)) return MX5_DR_E_QUALITY;
    if (!e->source_id || !e->source_epoch || !e->producer_seq) return MX5_DR_E_QUALITY;
    if (!e->measured_ns || e->measured_ns > (mean_yaw ? i->yaw_window_end_ns : i->start_ns) ||
        e->received_ns < e->measured_ns || e->received_ns > i->received_ns ||
        e->lease_until_ns < i->end_ns || e->time_uncertainty_ns > c->config.time_uncertainty_max_ns ||
        (i->end_ns >= e->measured_ns && i->end_ns - e->measured_ns > c->config.sample_age_max_ns) ||
        e->received_ns - e->measured_ns > c->config.sample_age_max_ns)
        return MX5_DR_E_TIME;
    return MX5_DR_OK;
}
static uint64_t capped_lease(const mx5_dr_evidence *e, uint64_t max_age) {
    uint64_t expiry = UINT64_MAX-e->measured_ns < max_age ? UINT64_MAX : e->measured_ns+max_age;
    return min_u64(e->lease_until_ns,expiry);
}
static mx5_dr_result evidence_progress(const mx5_dr_evidence *e, const mx5_dr_evidence *old) {
    if (e->source_id != old->source_id || e->source_epoch != old->source_epoch) return MX5_DR_E_CONTEXT;
    if (e->producer_seq < old->producer_seq || e->measured_ns < old->measured_ns) return MX5_DR_E_SEQUENCE;
    if (e->producer_seq == old->producer_seq && !same_evidence(e,old)) return MX5_DR_E_SEQUENCE;
    return MX5_DR_OK;
}
static int move_wgs84(mx5_dr_snapshot *s, double east, double north) {
    const double a = 6378137.0, f = 1.0/298.257223563, e2 = f*(2.0-f);
    double phi = s->latitude_deg * PI / 180.0;
    double q = 1.0-e2*sin(phi)*sin(phi);
    double m = a*(1.0-e2)/(q*sqrt(q));
    double mid = phi + north/(2.0*m), n;
    q = 1.0-e2*sin(mid)*sin(mid);
    m = a*(1.0-e2)/(q*sqrt(q)); n = a/sqrt(q);
    s->latitude_deg += north/m*180.0/PI;
    s->longitude_deg = lon_wrap(s->longitude_deg + east/(n*cos(mid))*180.0/PI);
    s->accumulated_east_m += east; s->accumulated_north_m += north;
    return finite_value(s->latitude_deg) && fabs(s->latitude_deg) < 85.0 && finite_value(s->longitude_deg);
}
mx5_dr_result mx5_dr_step(mx5_dr_core *c, const mx5_dr_interval *i) {
    mx5_dr_result r; mx5_dr_snapshot s; double remaining, dwell;
    uint64_t elapsed_ns;
    if (!c || !c->configured || !i) return MX5_DR_E_CONFIG;
    if (!context_equal(i->context,c->estimate.context)) return MX5_DR_E_CONTEXT;
    if (!c->seeded || (c->estimate.state != MX5_DR_READY && c->estimate.state != MX5_DR_ACTIVE)) return MX5_DR_E_NO_SEED;
    if (c->have_interval && same_interval(i,&c->last_interval)) return MX5_DR_DUPLICATE;
    if (!i->interval_seq || (c->have_interval && i->interval_seq <= c->last_interval.interval_seq))
        return fail(c,MX5_DR_E_SEQUENCE);
    if (i->start_ns != c->estimate.frontier_ns || i->end_ns <= i->start_ns ||
        i->end_ns-i->start_ns > c->config.interval_max_ns || i->received_ns < i->end_ns ||
        i->received_ns-i->end_ns > c->config.sample_age_max_ns ||
        (c->have_interval && i->received_ns < c->last_interval.received_ns)) return fail(c,MX5_DR_E_TIME);
    if (i->raw_yaw >= 4094 || !i->yaw_count) return fail(c,MX5_DR_E_QUALITY);
    if (i->reverse_active != 0 && i->reverse_active != 1) return fail(c,MX5_DR_E_FRAME);
    if (i->yaw_is_mean != 0 && i->yaw_is_mean != 1) return fail(c,MX5_DR_E_TIME);
    if (i->yaw_is_mean && (i->yaw_window_start_ns >= i->yaw_window_end_ns ||
        i->start_ns < i->yaw_window_start_ns || i->end_ns > i->yaw_window_end_ns ||
        i->received_ns < i->yaw_window_end_ns ||
        i->yaw_window_end_ns-i->yaw_window_start_ns > c->config.sample_age_max_ns)) return fail(c,MX5_DR_E_TIME);
    if (!finite_value(i->speed_mps) || i->speed_mps < 0.0 || i->speed_mps > c->config.physical_speed_max_mps ||
        !finite_value(i->yaw_rad_s) || fabs(i->yaw_rad_s) > c->config.physical_yaw_max_rad_s)
        return fail(c,MX5_DR_E_NUMERIC);
    r = evidence_check(c,&i->speed,i,0); if (r != MX5_DR_OK) return fail(c,r);
    r = evidence_check(c,&i->yaw,i,i->yaw_is_mean); if (r != MX5_DR_OK) return fail(c,r);
    r = evidence_check(c,&i->reverse,i,0); if (r != MX5_DR_OK) return fail(c,r);
    if (c->have_interval) {
        const mx5_dr_interval *old=&c->last_interval;
        r=evidence_progress(&i->speed,&old->speed); if(r!=MX5_DR_OK) return fail(c,r);
        r=evidence_progress(&i->yaw,&old->yaw); if(r!=MX5_DR_OK) return fail(c,r);
        r=evidence_progress(&i->reverse,&old->reverse); if(r!=MX5_DR_OK) return fail(c,r);
        if ((i->speed.producer_seq==old->speed.producer_seq && i->speed_mps!=old->speed_mps) ||
            (i->yaw.producer_seq==old->yaw.producer_seq && (i->yaw_rad_s!=old->yaw_rad_s ||
                i->raw_yaw!=old->raw_yaw || i->yaw_count!=old->yaw_count || i->yaw_is_mean!=old->yaw_is_mean ||
                i->yaw_window_start_ns!=old->yaw_window_start_ns || i->yaw_window_end_ns!=old->yaw_window_end_ns)) ||
            (i->reverse.producer_seq==old->reverse.producer_seq && i->reverse_active!=old->reverse_active))
            return fail(c,MX5_DR_E_SEQUENCE);
    }
    elapsed_ns=i->end_ns-c->anchor.measured_ns;
    if ((double)elapsed_ns/NS_PER_S > c->config.duration_max_s || UINT64_MAX-c->anchor.utc_ns < elapsed_ns)
        return fail(c,MX5_DR_E_LIMIT);
    s=c->estimate; dwell=c->stop_dwell_s;
    remaining=(double)(i->end_ns-i->start_ns)/NS_PER_S;
    s.distance_m += i->speed_mps*remaining;
    if (s.distance_m > c->config.distance_max_m) return fail(c,MX5_DR_E_LIMIT);
    if (s.stopped && i->speed_mps >= c->config.stop_exit_mps) { s.stopped=0; dwell=0.0; }
    /* Tunnel BETA can see a closed yaw window before the first nonzero
     * wheel event. Keep the established stop and charge the held rotation
     * to the existing uncertainty budget below. Never mask moving input. */
    if (s.stopped && fabs(i->yaw_rad_s)>c->config.stop_yaw_max_rad_s &&
        !(c->config.hold_stopped_yaw && c->domain==MX5_DR_MODEL_DOMAIN && i->speed_mps==0.0))
        return fail(c,MX5_DR_E_FRAME);
    while (remaining > 1e-12) {
        double dt=minimum(remaining,c->config.integration_step_s), signed_v, angle, sinc_value, east, north;
        int candidate=i->speed_mps <= c->config.stop_enter_mps && fabs(i->yaw_rad_s)<=c->config.stop_yaw_max_rad_s;
        if (!s.stopped && !candidate) dwell=0.0;
        if (!s.stopped && candidate) dt=minimum(dt,c->config.stop_hold_s-dwell);
        if (dt < 1e-12) { s.stopped=1; continue; }
        s.heading_budget_rad += c->config.yaw_error_rad_s*dt;
        s.error_budget_m += c->config.speed_error_mps*dt;
        if (s.stopped) {
            s.heading_budget_rad += fabs(i->yaw_rad_s)*dt;
            s.error_budget_m += i->speed_mps*dt;
        } else {
            signed_v=i->reverse_active ? -i->speed_mps : i->speed_mps;
            angle=i->yaw_rad_s*dt;
            if (fabs(angle)<1e-5) { double z=angle*0.5; sinc_value=1.0-z*z/6.0+z*z*z*z/120.0; }
            else sinc_value=sin(angle*0.5)/(angle*0.5);
            east=signed_v*dt*sinc_value*sin(s.body_heading_rad+angle*0.5);
            north=signed_v*dt*sinc_value*cos(s.body_heading_rad+angle*0.5);
            if (!move_wgs84(&s,east,north)) return fail(c,MX5_DR_E_NUMERIC);
            s.body_heading_rad=wrap(s.body_heading_rad+angle);
            if (candidate) { dwell += dt; if (dwell>=c->config.stop_hold_s-1e-12) s.stopped=1; }
        }
        s.error_budget_m += i->speed_mps*2.0*sin(minimum(s.heading_budget_rad,PI)*0.5)*dt;
        if (!finite_value(s.error_budget_m) || !finite_value(s.heading_budget_rad)) return fail(c,MX5_DR_E_NUMERIC);
        if (s.error_budget_m>c->config.error_max_m) return fail(c,MX5_DR_E_LIMIT);
        remaining -= dt;
    }
    s.frontier_ns=i->end_ns; s.derived_utc_ns=c->anchor.utc_ns+elapsed_ns;
    s.elapsed_s=(double)elapsed_ns/NS_PER_S;
    s.sensor_lease_until_ns=min_u64(capped_lease(&i->speed,c->config.sample_age_max_ns),
        min_u64(capped_lease(&i->yaw,c->config.sample_age_max_ns),capped_lease(&i->reverse,c->config.sample_age_max_ns)));
    s.speed_mps=s.stopped ? 0.0 : i->speed_mps;
    s.has_bearing=!s.stopped && i->speed_mps>0.0;
    s.travel_bearing_rad=s.has_bearing ? wrap(s.body_heading_rad+(i->reverse_active ? PI : 0.0)) : 0.0;
    s.solution_seq=i->interval_seq;
    s.valid=s.state==MX5_DR_ACTIVE && c->domain==MX5_DR_QUALIFIED_DOMAIN;
    s.model_valid=s.state==MX5_DR_ACTIVE && c->domain==MX5_DR_MODEL_DOMAIN;
    s.reason=MX5_DR_OK;
    c->estimate=s; c->stop_dwell_s=dwell; c->last_interval=*i; c->have_interval=1;
    return MX5_DR_OK;
}
static mx5_dr_result get_snapshot(const mx5_dr_core *c, uint64_t now,
                                 mx5_dr_context expected, mx5_dr_snapshot *out, mx5_dr_domain domain) {
    mx5_dr_result r=MX5_DR_OK;
    if (!out) return MX5_DR_E_CONFIG;
    memset(out,0,sizeof(*out));
    if (!c || !c->configured) { out->reason=MX5_DR_E_CONFIG; return out->reason; }
    *out=c->estimate;
    if (c->domain!=domain) r=MX5_DR_E_QUALITY;
    else if (!context_equal(expected,c->estimate.context)) r=MX5_DR_E_CONTEXT;
    else if (!c->seeded || c->estimate.state!=MX5_DR_ACTIVE || !c->have_interval) r=MX5_DR_E_NO_SEED;
    else if (now<c->estimate.frontier_ns || now<c->last_interval.received_ns) r=MX5_DR_E_TIME;
    else if (now-c->estimate.frontier_ns>c->config.snapshot_age_max_ns || now>c->estimate.sensor_lease_until_ns) r=MX5_DR_E_STALE;
    else {
        double age=(double)(now-c->estimate.frontier_ns)/NS_PER_S;
        out->error_budget_m += (c->last_interval.speed_mps+c->config.speed_error_mps)*age;
        if ((double)(now-c->anchor.measured_ns)/NS_PER_S>c->config.duration_max_s ||
            out->error_budget_m>c->config.error_max_m) r=MX5_DR_E_LIMIT;
    }
    out->valid=r==MX5_DR_OK && domain==MX5_DR_QUALIFIED_DOMAIN;
    out->model_valid=r==MX5_DR_OK && domain==MX5_DR_MODEL_DOMAIN; out->reason=r;
    return r;
}
mx5_dr_result mx5_dr_get_snapshot(const mx5_dr_core *c, uint64_t now,
                                 mx5_dr_context expected, mx5_dr_snapshot *out) {
    return get_snapshot(c,now,expected,out,MX5_DR_QUALIFIED_DOMAIN);
}
mx5_dr_result mx5_dr_get_model_snapshot(const mx5_dr_core *c, uint64_t now,
                                       mx5_dr_context expected, mx5_dr_snapshot *out) {
    return get_snapshot(c,now,expected,out,MX5_DR_MODEL_DOMAIN);
}
const char *mx5_dr_result_name(mx5_dr_result r) {
    static const char *const names[]={"OK","DUPLICATE","E_CONFIG","E_NO_SEED","E_CONTEXT",
        "E_QUALITY","E_TIME","E_SEQUENCE","E_FRAME","E_LIMIT","E_STALE","E_NUMERIC","E_STATE"};
    return (unsigned)r < sizeof(names)/sizeof(names[0]) ? names[r] : "E_UNKNOWN";
}
