/*
 * Copyright (c) 2008, 2009, 2010, 2011, 2012 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "ofp-actions.h"
#include "autopath.h"
#include "bundle.h"
#include "byte-order.h"
#include "compiler.h"
#include "dynamic-string.h"
#include "learn.h"
#include "meta-flow.h"
#include "multipath.h"
#include "nx-match.h"
#include "ofp-util.h"
#include "ofpbuf.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(ofp_actions);

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

/* Converting OpenFlow 1.0 to ofpacts. */

static enum ofperr
output_from_openflow10(const struct ofp10_action_output *oao,
                       struct ofpbuf *out)
{
    struct ofpact_output *output;

    output = ofpact_put_OUTPUT(out);
    output->port = ntohs(oao->port);
    output->max_len = ntohs(oao->max_len);

    return ofputil_check_output_port(output->port, OFPP_MAX);
}

static enum ofperr
enqueue_from_openflow10(const struct ofp_action_enqueue *oae,
                        struct ofpbuf *out)
{
    struct ofpact_enqueue *enqueue;

    enqueue = ofpact_put_ENQUEUE(out);
    enqueue->port = ntohs(oae->port);
    enqueue->queue = ntohl(oae->queue_id);
    if (enqueue->port >= OFPP_MAX && enqueue->port != OFPP_IN_PORT
        && enqueue->port != OFPP_LOCAL) {
        return OFPERR_OFPBAC_BAD_OUT_PORT;
    }
    return 0;
}

static void
resubmit_from_openflow(const struct nx_action_resubmit *nar,
                       struct ofpbuf *out)
{
    struct ofpact_resubmit *resubmit;

    resubmit = ofpact_put_RESUBMIT(out);
    resubmit->ofpact.compat = OFPUTIL_NXAST_RESUBMIT;
    resubmit->in_port = ntohs(nar->in_port);
    resubmit->table_id = 0xff;
}

static enum ofperr
resubmit_table_from_openflow(const struct nx_action_resubmit *nar,
                             struct ofpbuf *out)
{
    struct ofpact_resubmit *resubmit;

    if (nar->pad[0] || nar->pad[1] || nar->pad[2]) {
        return OFPERR_OFPBAC_BAD_ARGUMENT;
    }

    resubmit = ofpact_put_RESUBMIT(out);
    resubmit->ofpact.compat = OFPUTIL_NXAST_RESUBMIT_TABLE;
    resubmit->in_port = ntohs(nar->in_port);
    resubmit->table_id = nar->table;
    return 0;
}

static enum ofperr
output_reg_from_openflow(const struct nx_action_output_reg *naor,
                         struct ofpbuf *out)
{
    struct ofpact_output_reg *output_reg;

    if (!is_all_zeros(naor->zero, sizeof naor->zero)) {
        return OFPERR_OFPBAC_BAD_ARGUMENT;
    }

    output_reg = ofpact_put_OUTPUT_REG(out);
    output_reg->src.field = mf_from_nxm_header(ntohl(naor->src));
    output_reg->src.ofs = nxm_decode_ofs(naor->ofs_nbits);
    output_reg->src.n_bits = nxm_decode_n_bits(naor->ofs_nbits);
    output_reg->max_len = ntohs(naor->max_len);

    return mf_check_src(&output_reg->src, NULL);
}

static void
fin_timeout_from_openflow(const struct nx_action_fin_timeout *naft,
                          struct ofpbuf *out)
{
    struct ofpact_fin_timeout *oft;

    oft = ofpact_put_FIN_TIMEOUT(out);
    oft->fin_idle_timeout = ntohs(naft->fin_idle_timeout);
    oft->fin_hard_timeout = ntohs(naft->fin_hard_timeout);
}

static void
controller_from_openflow(const struct nx_action_controller *nac,
                         struct ofpbuf *out)
{
    struct ofpact_controller *oc;

    oc = ofpact_put_CONTROLLER(out);
    oc->max_len = ntohs(nac->max_len);
    oc->controller_id = ntohs(nac->controller_id);
    oc->reason = nac->reason;
}

static void
note_from_openflow(const struct nx_action_note *nan, struct ofpbuf *out)
{
    struct ofpact_note *note;
    unsigned int length;

    length = ntohs(nan->len) - offsetof(struct nx_action_note, note);
    note = ofpact_put(out, OFPACT_NOTE,
                      offsetof(struct ofpact_note, data) + length);
    note->length = length;
    memcpy(note->data, nan->note, length);
}

static enum ofperr
decode_nxast_action(const union ofp_action *a, enum ofputil_action_code *code)
{
    const struct nx_action_header *nah = (const struct nx_action_header *) a;
    uint16_t len = ntohs(a->header.len);

    if (len < sizeof(struct nx_action_header)) {
        return OFPERR_OFPBAC_BAD_LEN;
    } else if (a->vendor.vendor != CONSTANT_HTONL(NX_VENDOR_ID)) {
        return OFPERR_OFPBAC_BAD_VENDOR;
    }

    switch (nah->subtype) {
#define NXAST_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME)    \
        case CONSTANT_HTONS(ENUM):                      \
            if (EXTENSIBLE                              \
                ? len >= sizeof(struct STRUCT)          \
                : len == sizeof(struct STRUCT)) {       \
                *code = OFPUTIL_##ENUM;                 \
                return 0;                               \
            } else {                                    \
                return OFPERR_OFPBAC_BAD_LEN;           \
            }                                           \
            NOT_REACHED();
#include "ofp-util.def"

    case CONSTANT_HTONS(NXAST_SNAT__OBSOLETE):
    case CONSTANT_HTONS(NXAST_DROP_SPOOFED_ARP__OBSOLETE):
    default:
        return OFPERR_OFPBAC_BAD_TYPE;
    }
}

/* Parses 'a' to determine its type.  On success stores the correct type into
 * '*code' and returns 0.  On failure returns an OFPERR_* error code and
 * '*code' is indeterminate.
 *
 * The caller must have already verified that 'a''s length is potentially
 * correct (that is, a->header.len is nonzero and a multiple of sizeof(union
 * ofp_action) and no longer than the amount of space allocated to 'a').
 *
 * This function verifies that 'a''s length is correct for the type of action
 * that it represents. */
static enum ofperr
decode_openflow10_action(const union ofp_action *a,
                         enum ofputil_action_code *code)
{
    switch (a->type) {
    case CONSTANT_HTONS(OFPAT10_VENDOR):
        return decode_nxast_action(a, code);

#define OFPAT10_ACTION(ENUM, STRUCT, NAME)                          \
        case CONSTANT_HTONS(ENUM):                                  \
            if (a->header.len == htons(sizeof(struct STRUCT))) {    \
                *code = OFPUTIL_##ENUM;                             \
                return 0;                                           \
            } else {                                                \
                return OFPERR_OFPBAC_BAD_LEN;                       \
            }                                                       \
            break;
#include "ofp-util.def"

    default:
        return OFPERR_OFPBAC_BAD_TYPE;
    }
}

static enum ofperr
ofpact_from_nxast(const union ofp_action *a, enum ofputil_action_code code,
                  struct ofpbuf *out)
{
    const struct nx_action_resubmit *nar;
    const struct nx_action_set_tunnel *nast;
    const struct nx_action_set_queue *nasq;
    const struct nx_action_note *nan;
    const struct nx_action_set_tunnel64 *nast64;
    struct ofpact_tunnel *tunnel;
    enum ofperr error = 0;

    switch (code) {
    case OFPUTIL_ACTION_INVALID:
#define OFPAT10_ACTION(ENUM, STRUCT, NAME) case OFPUTIL_##ENUM:
#define OFPAT11_ACTION(ENUM, STRUCT, NAME) case OFPUTIL_##ENUM:
#define OFPAT12_ACTION(ENUM, STRUCT, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
        NOT_REACHED();

    case OFPUTIL_NXAST_RESUBMIT:
        resubmit_from_openflow((const struct nx_action_resubmit *) a, out);
        break;

    case OFPUTIL_NXAST_SET_TUNNEL:
        nast = (const struct nx_action_set_tunnel *) a;
        tunnel = ofpact_put_SET_TUNNEL(out);
        tunnel->ofpact.compat = code;
        tunnel->tun_id = ntohl(nast->tun_id);
        break;

    case OFPUTIL_NXAST_SET_QUEUE:
        nasq = (const struct nx_action_set_queue *) a;
        ofpact_put_SET_QUEUE(out)->queue_id = ntohl(nasq->queue_id);
        break;

    case OFPUTIL_NXAST_POP_QUEUE:
        ofpact_put_POP_QUEUE(out);
        break;

    case OFPUTIL_NXAST_REG_MOVE:
        error = nxm_reg_move_from_openflow(
            (const struct nx_action_reg_move *) a, out);
        break;

    case OFPUTIL_NXAST_REG_LOAD:
        error = nxm_reg_load_from_openflow(
            (const struct nx_action_reg_load *) a, out);
        break;

    case OFPUTIL_NXAST_NOTE:
        nan = (const struct nx_action_note *) a;
        note_from_openflow(nan, out);
        break;

    case OFPUTIL_NXAST_SET_TUNNEL64:
        nast64 = (const struct nx_action_set_tunnel64 *) a;
        tunnel = ofpact_put_SET_TUNNEL(out);
        tunnel->ofpact.compat = code;
        tunnel->tun_id = ntohll(nast64->tun_id);
        break;

    case OFPUTIL_NXAST_MULTIPATH:
        error = multipath_from_openflow((const struct nx_action_multipath *) a,
                                        ofpact_put_MULTIPATH(out));
        break;

    case OFPUTIL_NXAST_AUTOPATH:
        error = autopath_from_openflow((const struct nx_action_autopath *) a,
                                       ofpact_put_AUTOPATH(out));
        break;

    case OFPUTIL_NXAST_BUNDLE:
    case OFPUTIL_NXAST_BUNDLE_LOAD:
        error = bundle_from_openflow((const struct nx_action_bundle *) a, out);
        break;

    case OFPUTIL_NXAST_OUTPUT_REG:
        error = output_reg_from_openflow(
            (const struct nx_action_output_reg *) a, out);
        break;

    case OFPUTIL_NXAST_RESUBMIT_TABLE:
        nar = (const struct nx_action_resubmit *) a;
        error = resubmit_table_from_openflow(nar, out);
        break;

    case OFPUTIL_NXAST_LEARN:
        error = learn_from_openflow((const struct nx_action_learn *) a, out);
        break;

    case OFPUTIL_NXAST_EXIT:
        ofpact_put_EXIT(out);
        break;

    case OFPUTIL_NXAST_DEC_TTL:
        ofpact_put_DEC_TTL(out);
        break;

    case OFPUTIL_NXAST_FIN_TIMEOUT:
        fin_timeout_from_openflow(
            (const struct nx_action_fin_timeout *) a, out);
        break;

    case OFPUTIL_NXAST_CONTROLLER:
        controller_from_openflow((const struct nx_action_controller *) a, out);
        break;
    }

    return error;
}

static enum ofperr
ofpact_from_openflow10(const union ofp_action *a, struct ofpbuf *out)
{
    enum ofputil_action_code code;
    enum ofperr error;

    error = decode_openflow10_action(a, &code);
    if (error) {
        return error;
    }

    switch (code) {
    case OFPUTIL_ACTION_INVALID:
#define OFPAT11_ACTION(ENUM, STRUCT, NAME) case OFPUTIL_##ENUM:
#define OFPAT12_ACTION(ENUM, STRUCT, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
        NOT_REACHED();

    case OFPUTIL_OFPAT10_OUTPUT:
        return output_from_openflow10(&a->output10, out);

    case OFPUTIL_OFPAT10_SET_VLAN_VID:
        if (a->vlan_vid.vlan_vid & ~htons(0xfff)) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_VLAN_VID(out)->vlan_vid = ntohs(a->vlan_vid.vlan_vid);
        break;

    case OFPUTIL_OFPAT10_SET_VLAN_PCP:
        if (a->vlan_pcp.vlan_pcp & ~7) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_VLAN_PCP(out)->vlan_pcp = a->vlan_pcp.vlan_pcp;
        break;

    case OFPUTIL_OFPAT10_STRIP_VLAN:
        ofpact_put_STRIP_VLAN(out);
        break;

    case OFPUTIL_OFPAT10_SET_DL_SRC:
        memcpy(ofpact_put_SET_ETH_SRC(out)->mac,
               ((const struct ofp_action_dl_addr *) a)->dl_addr, ETH_ADDR_LEN);
        break;

    case OFPUTIL_OFPAT10_SET_DL_DST:
        memcpy(ofpact_put_SET_ETH_DST(out)->mac,
               ((const struct ofp_action_dl_addr *) a)->dl_addr, ETH_ADDR_LEN);
        break;

    case OFPUTIL_OFPAT10_SET_NW_SRC:
        ofpact_put_SET_IPV4_SRC(out)->ipv4 = a->nw_addr.nw_addr;
        break;

    case OFPUTIL_OFPAT10_SET_NW_DST:
        ofpact_put_SET_IPV4_DST(out)->ipv4 = a->nw_addr.nw_addr;
        break;

    case OFPUTIL_OFPAT10_SET_NW_TOS:
        if (a->nw_tos.nw_tos & ~IP_DSCP_MASK) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_IPV4_DSCP(out)->dscp = a->nw_tos.nw_tos;
        break;

    case OFPUTIL_OFPAT10_SET_TP_SRC:
        ofpact_put_SET_L4_SRC_PORT(out)->port = ntohs(a->tp_port.tp_port);
        break;

    case OFPUTIL_OFPAT10_SET_TP_DST:
        ofpact_put_SET_L4_DST_PORT(out)->port = ntohs(a->tp_port.tp_port);

        break;

    case OFPUTIL_OFPAT10_ENQUEUE:
        error = enqueue_from_openflow10((const struct ofp_action_enqueue *) a,
                                        out);
        break;

#define NXAST_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
	return ofpact_from_nxast(a, code, out);
    }

    return error;
}

static inline union ofp_action *
action_next(const union ofp_action *a)
{
    return ((union ofp_action *) (void *)
            ((uint8_t *) a + ntohs(a->header.len)));
}

static inline bool
action_is_valid(const union ofp_action *a, size_t n_actions)
{
    uint16_t len = ntohs(a->header.len);
    return (!(len % OFP_ACTION_ALIGN)
            && len >= sizeof *a
            && len / sizeof *a <= n_actions);
}

/* This macro is careful to check for actions with bad lengths. */
#define ACTION_FOR_EACH(ITER, LEFT, ACTIONS, N_ACTIONS)                 \
    for ((ITER) = (ACTIONS), (LEFT) = (N_ACTIONS);                      \
         (LEFT) > 0 && action_is_valid(ITER, LEFT);                     \
         ((LEFT) -= ntohs((ITER)->header.len) / sizeof(union ofp_action), \
          (ITER) = action_next(ITER)))

static enum ofperr
ofpacts_from_openflow(const union ofp_action *in, size_t n_in,
                      struct ofpbuf *out,
                      enum ofperr (ofpact_from_openflow)(
                          const union ofp_action *a, struct ofpbuf *out))
{
    const union ofp_action *a;
    size_t left;

    ACTION_FOR_EACH (a, left, in, n_in) {
        enum ofperr error = ofpact_from_openflow(a, out);
        if (error) {
            VLOG_WARN_RL(&rl, "bad action at offset %td (%s)",
                         (a - in) * sizeof *a, ofperr_get_name(error));
            return error;
        }
    }
    if (left) {
        VLOG_WARN_RL(&rl, "bad action format at offset %zu",
                     (n_in - left) * sizeof *a);
        return OFPERR_OFPBAC_BAD_LEN;
    }

    ofpact_put_END(out);

    return 0;
}

static enum ofperr
ofpacts_from_openflow10(const union ofp_action *in, size_t n_in,
                        struct ofpbuf *out)
{
    return ofpacts_from_openflow(in, n_in, out, ofpact_from_openflow10);
}

/* Attempts to convert 'actions_len' bytes of OpenFlow actions from the front
 * of 'openflow' into ofpacts.  On success, replaces any existing content in
 * 'ofpacts' by the converted ofpacts; on failure, clears 'ofpacts'.  Returns 0
 * if successful, otherwise an OpenFlow error.
 *
 * This function does not check that the actions are valid in a given context.
 * The caller should do so, with ofpacts_check(). */
enum ofperr
ofpacts_pull_openflow10(struct ofpbuf *openflow, unsigned int actions_len,
                        struct ofpbuf *ofpacts)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
    const union ofp_action *actions;
    enum ofperr error;

    ofpbuf_clear(ofpacts);

    if (actions_len % OFP_ACTION_ALIGN != 0) {
        VLOG_WARN_RL(&rl, "OpenFlow message actions length %u is not a "
                     "multiple of %d", actions_len, OFP_ACTION_ALIGN);
        return OFPERR_OFPBRC_BAD_LEN;
    }

    actions = ofpbuf_try_pull(openflow, actions_len);
    if (actions == NULL) {
        VLOG_WARN_RL(&rl, "OpenFlow message actions length %u exceeds "
                     "remaining message length (%zu)",
                     actions_len, openflow->size);
        return OFPERR_OFPBRC_BAD_LEN;
    }

    error = ofpacts_from_openflow10(actions, actions_len / OFP_ACTION_ALIGN,
                                    ofpacts);
    if (error) {
        ofpbuf_clear(ofpacts);
    }
    return 0;
}

/* OpenFlow 1.1 actions. */

/* Parses 'a' to determine its type.  On success stores the correct type into
 * '*code' and returns 0.  On failure returns an OFPERR_* error code and
 * '*code' is indeterminate.
 *
 * The caller must have already verified that 'a''s length is potentially
 * correct (that is, a->header.len is nonzero and a multiple of sizeof(union
 * ofp_action) and no longer than the amount of space allocated to 'a').
 *
 * This function verifies that 'a''s length is correct for the type of action
 * that it represents. */
static enum ofperr
decode_openflow11_action(const union ofp_action *a,
                         enum ofputil_action_code *code)
{
    switch (a->type) {
    case CONSTANT_HTONS(OFPAT11_EXPERIMENTER):
        return decode_nxast_action(a, code);

#define OFPAT11_ACTION(ENUM, STRUCT, NAME)                          \
        case CONSTANT_HTONS(ENUM):                                  \
            if (a->header.len == htons(sizeof(struct STRUCT))) {    \
                *code = OFPUTIL_##ENUM;                             \
                return 0;                                           \
            } else {                                                \
                return OFPERR_OFPBAC_BAD_LEN;                       \
            }                                                       \
            break;
#include "ofp-util.def"

    default:
        return OFPERR_OFPBAC_BAD_TYPE;
    }
}

static enum ofperr
output_from_openflow11(const struct ofp11_action_output *oao,
                       struct ofpbuf *out)
{
    struct ofpact_output *output;
    enum ofperr error;

    output = ofpact_put_OUTPUT(out);
    output->max_len = ntohs(oao->max_len);

    error = ofputil_port_from_ofp11(oao->port, &output->port);
    if (error) {
        return error;
    }

    return ofputil_check_output_port(output->port, OFPP_MAX);
}

static enum ofperr
ofpact_from_openflow11(const union ofp_action *a, struct ofpbuf *out)
{
    enum ofputil_action_code code;
    enum ofperr error;

    error = decode_openflow11_action(a, &code);
    if (error) {
        return error;
    }

    switch (code) {
    case OFPUTIL_ACTION_INVALID:
#define OFPAT10_ACTION(ENUM, STRUCT, NAME) case OFPUTIL_##ENUM:
#define OFPAT12_ACTION(ENUM, STRUCT, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
        NOT_REACHED();

    case OFPUTIL_OFPAT11_OUTPUT:
        return output_from_openflow11((const struct ofp11_action_output *) a,
                                      out);

    case OFPUTIL_OFPAT11_SET_VLAN_VID:
        if (a->vlan_vid.vlan_vid & ~htons(0xfff)) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_VLAN_VID(out)->vlan_vid = ntohs(a->vlan_vid.vlan_vid);
        break;

    case OFPUTIL_OFPAT11_SET_VLAN_PCP:
        if (a->vlan_pcp.vlan_pcp & ~7) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_VLAN_PCP(out)->vlan_pcp = a->vlan_pcp.vlan_pcp;
        break;

    case OFPUTIL_OFPAT11_SET_DL_SRC:
        memcpy(ofpact_put_SET_ETH_SRC(out)->mac,
               ((const struct ofp_action_dl_addr *) a)->dl_addr, ETH_ADDR_LEN);
        break;

    case OFPUTIL_OFPAT11_SET_DL_DST:
        memcpy(ofpact_put_SET_ETH_DST(out)->mac,
               ((const struct ofp_action_dl_addr *) a)->dl_addr, ETH_ADDR_LEN);
        break;

    case OFPUTIL_OFPAT11_SET_NW_SRC:
        ofpact_put_SET_IPV4_SRC(out)->ipv4 = a->nw_addr.nw_addr;
        break;

    case OFPUTIL_OFPAT11_SET_NW_DST:
        ofpact_put_SET_IPV4_DST(out)->ipv4 = a->nw_addr.nw_addr;
        break;

    case OFPUTIL_OFPAT11_SET_NW_TOS:
        if (a->nw_tos.nw_tos & ~IP_DSCP_MASK) {
            return OFPERR_OFPBAC_BAD_ARGUMENT;
        }
        ofpact_put_SET_IPV4_DSCP(out)->dscp = a->nw_tos.nw_tos;
        break;

    case OFPUTIL_OFPAT11_SET_TP_SRC:
        ofpact_put_SET_L4_SRC_PORT(out)->port = ntohs(a->tp_port.tp_port);
        break;

    case OFPUTIL_OFPAT11_SET_TP_DST:
        ofpact_put_SET_L4_DST_PORT(out)->port = ntohs(a->tp_port.tp_port);
        break;

#define NXAST_ACTION(ENUM, STRUCT, EXTENSIBLE, NAME) case OFPUTIL_##ENUM:
#include "ofp-util.def"
        return ofpact_from_nxast(a, code, out);
    }

    return error;
}

static enum ofperr
ofpacts_from_openflow11(const union ofp_action *in, size_t n_in,
                        struct ofpbuf *out)
{
    return ofpacts_from_openflow(in, n_in, out, ofpact_from_openflow11);
}

/* OpenFlow 1.1 instructions. */

#define OVS_INSTRUCTIONS                                    \
    DEFINE_INST(OFPIT11_GOTO_TABLE,                         \
                ofp11_instruction_goto_table,     false,    \
                "goto_table")                               \
                                                            \
    DEFINE_INST(OFPIT11_WRITE_METADATA,                     \
                ofp11_instruction_write_metadata, false,    \
                "write_metadata")                           \
                                                            \
    DEFINE_INST(OFPIT11_WRITE_ACTIONS,                      \
                ofp11_instruction_actions,        true,     \
                "write_actions")                            \
                                                            \
    DEFINE_INST(OFPIT11_APPLY_ACTIONS,                      \
                ofp11_instruction_actions,        true,     \
                "apply_actions")                            \
                                                            \
    DEFINE_INST(OFPIT11_CLEAR_ACTIONS,                      \
                ofp11_instruction,                false,    \
                "clear_actions")

enum ovs_instruction_type {
#define DEFINE_INST(ENUM, STRUCT, NAME, EXTENSIBLE) OVSINST_##ENUM,
    OVS_INSTRUCTIONS
#undef DEFINE_INST
};

enum {
#define DEFINE_INST(ENUM, STRUCT, NAME, EXTENSIBLE) + 1
    N_OVS_INSTRUCTIONS = OVS_INSTRUCTIONS
#undef DEFINE_INST
};

static inline struct ofp11_instruction *
instruction_next(const struct ofp11_instruction *inst)
{
    return ((struct ofp11_instruction *) (void *)
            ((uint8_t *) inst + ntohs(inst->len)));
}

static inline bool
instruction_is_valid(const struct ofp11_instruction *inst,
                     size_t n_instructions)
{
    uint16_t len = ntohs(inst->len);
    return (!(len % OFP11_INSTRUCTION_ALIGN)
            && len >= sizeof *inst
            && len / sizeof *inst <= n_instructions);
}

/* This macro is careful to check for instructions with bad lengths. */
#define INSTRUCTION_FOR_EACH(ITER, LEFT, INSTRUCTIONS, N_INSTRUCTIONS)  \
    for ((ITER) = (INSTRUCTIONS), (LEFT) = (N_INSTRUCTIONS);            \
         (LEFT) > 0 && instruction_is_valid(ITER, LEFT);                \
         ((LEFT) -= (ntohs((ITER)->len)                                 \
                     / sizeof(struct ofp11_instruction)),               \
          (ITER) = instruction_next(ITER)))

static enum ofperr
decode_openflow11_instruction(const struct ofp11_instruction *inst,
                              enum ovs_instruction_type *type)
{
    uint16_t len = ntohs(inst->len);

    switch (inst->type) {
    case CONSTANT_HTONS(OFPIT11_EXPERIMENTER):
        return OFPERR_OFPBIC_BAD_EXPERIMENTER;

#define DEFINE_INST(ENUM, STRUCT, NAME, EXTENSIBLE)     \
        case CONSTANT_HTONS(ENUM):                      \
            if (EXTENSIBLE                              \
                ? len >= sizeof(struct STRUCT)          \
                : len == sizeof(struct STRUCT)) {       \
                *type = OVSINST_##ENUM;                 \
                return 0;                               \
            } else {                                    \
                return OFPERR_OFPBAC_BAD_LEN;           \
            }
OVS_INSTRUCTIONS
#undef DEFINE_INST

    default:
        return OFPERR_OFPBIC_UNKNOWN_INST;
    }
}

static enum ofperr
decode_openflow11_instructions(const struct ofp11_instruction insts[],
                               size_t n_insts,
                               const struct ofp11_instruction *out[])
{
    const struct ofp11_instruction *inst;
    size_t left;

    memset(out, 0, N_OVS_INSTRUCTIONS * sizeof *out);
    INSTRUCTION_FOR_EACH (inst, left, insts, n_insts) {
        enum ovs_instruction_type type;
        enum ofperr error;

        error = decode_openflow11_instruction(inst, &type);
        if (error) {
            return error;
        }

        if (out[type]) {
            return OFPERR_NXBIC_DUP_TYPE;
        }
        out[type] = inst;
    }

    if (left) {
        VLOG_WARN_RL(&rl, "bad instruction format at offset %zu",
                     (n_insts - left) * sizeof *inst);
        return OFPERR_OFPBIC_BAD_LEN;
    }
    return 0;
}

static void
get_actions_from_instruction(const struct ofp11_instruction *inst,
                         const union ofp_action **actions,
                         size_t *n_actions)
{
    *actions = (const union ofp_action *) (inst + 1);
    *n_actions = (ntohs(inst->len) - sizeof *inst) / OFP11_INSTRUCTION_ALIGN;
}

enum ofperr
ofpacts_pull_openflow11_instructions(struct ofpbuf *openflow,
                                     unsigned int instructions_len,
                                     struct ofpbuf *ofpacts)
{
    static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
    const struct ofp11_instruction *instructions;
    const struct ofp11_instruction *insts[N_OVS_INSTRUCTIONS];
    enum ofperr error;

    ofpbuf_clear(ofpacts);

    if (instructions_len % OFP11_INSTRUCTION_ALIGN != 0) {
        VLOG_WARN_RL(&rl, "OpenFlow message instructions length %u is not a "
                     "multiple of %d",
                     instructions_len, OFP11_INSTRUCTION_ALIGN);
        error = OFPERR_OFPBRC_BAD_LEN;
        goto exit;
    }

    instructions = ofpbuf_try_pull(openflow, instructions_len);
    if (instructions == NULL) {
        VLOG_WARN_RL(&rl, "OpenFlow message instructions length %u exceeds "
                     "remaining message length (%zu)",
                     instructions_len, openflow->size);
        error = OFPERR_OFPBRC_BAD_LEN;
        goto exit;
    }

    error = decode_openflow11_instructions(
        instructions, instructions_len / OFP11_INSTRUCTION_ALIGN,
        insts);
    if (error) {
        goto exit;
    }

    if (insts[OVSINST_OFPIT11_APPLY_ACTIONS]) {
        const union ofp_action *actions;
        size_t n_actions;

        get_actions_from_instruction(insts[OVSINST_OFPIT11_APPLY_ACTIONS],
                                     &actions, &n_actions);
        error = ofpacts_from_openflow11(actions, n_actions, ofpacts);
        if (error) {
            goto exit;
        }
    }

    ofpact_put_END(ofpacts);

    if (insts[OVSINST_OFPIT11_GOTO_TABLE] ||
        insts[OVSINST_OFPIT11_WRITE_METADATA] ||
        insts[OVSINST_OFPIT11_WRITE_ACTIONS] ||
        insts[OVSINST_OFPIT11_CLEAR_ACTIONS]) {
        error = OFPERR_OFPBIC_UNSUP_INST;
        goto exit;
    }

exit:
    if (error) {
        ofpbuf_clear(ofpacts);
    }
    return error;
}

static enum ofperr
ofpact_check__(const struct ofpact *a, const struct flow *flow, int max_ports)
{
    const struct ofpact_enqueue *enqueue;

    switch (a->type) {
    case OFPACT_END:
        return 0;

    case OFPACT_OUTPUT:
        return ofputil_check_output_port(ofpact_get_OUTPUT(a)->port,
                                         max_ports);

    case OFPACT_CONTROLLER:
        return 0;

    case OFPACT_ENQUEUE:
        enqueue = ofpact_get_ENQUEUE(a);
        if (enqueue->port >= max_ports && enqueue->port != OFPP_IN_PORT
            && enqueue->port != OFPP_LOCAL) {
            return OFPERR_OFPBAC_BAD_OUT_PORT;
        }
        return 0;

    case OFPACT_OUTPUT_REG:
        return mf_check_src(&ofpact_get_OUTPUT_REG(a)->src, flow);

    case OFPACT_BUNDLE:
        return bundle_check(ofpact_get_BUNDLE(a), max_ports, flow);

    case OFPACT_SET_VLAN_VID:
    case OFPACT_SET_VLAN_PCP:
    case OFPACT_STRIP_VLAN:
    case OFPACT_SET_ETH_SRC:
    case OFPACT_SET_ETH_DST:
    case OFPACT_SET_IPV4_SRC:
    case OFPACT_SET_IPV4_DST:
    case OFPACT_SET_IPV4_DSCP:
    case OFPACT_SET_L4_SRC_PORT:
    case OFPACT_SET_L4_DST_PORT:
        return 0;

    case OFPACT_REG_MOVE:
        return nxm_reg_move_check(ofpact_get_REG_MOVE(a), flow);

    case OFPACT_REG_LOAD:
        return nxm_reg_load_check(ofpact_get_REG_LOAD(a), flow);

    case OFPACT_DEC_TTL:
    case OFPACT_SET_TUNNEL:
    case OFPACT_SET_QUEUE:
    case OFPACT_POP_QUEUE:
    case OFPACT_FIN_TIMEOUT:
    case OFPACT_RESUBMIT:
        return 0;

    case OFPACT_LEARN:
        return learn_check(ofpact_get_LEARN(a), flow);

    case OFPACT_MULTIPATH:
        return multipath_check(ofpact_get_MULTIPATH(a), flow);

    case OFPACT_AUTOPATH:
        return autopath_check(ofpact_get_AUTOPATH(a), flow);

    case OFPACT_NOTE:
    case OFPACT_EXIT:
        return 0;

    default:
        NOT_REACHED();
    }
}

/* Checks that the actions in 'ofpacts' (terminated by OFPACT_END) are
 * appropriate for a packet with the prerequisites satisfied by 'flow' in a
 * switch with no more than 'max_ports' ports. */
enum ofperr
ofpacts_check(const struct ofpact ofpacts[],
              const struct flow *flow, int max_ports)
{
    const struct ofpact *a;

    OFPACT_FOR_EACH (a, ofpacts) {
        enum ofperr error = ofpact_check__(a, flow, max_ports);
        if (error) {
            return error;
        }
    }

    return 0;
}

/* Converting ofpacts to Nicira OpenFlow extensions. */

static void
ofpact_output_reg_to_nxast(const struct ofpact_output_reg *output_reg,
                                struct ofpbuf *out)
{
    struct nx_action_output_reg *naor = ofputil_put_NXAST_OUTPUT_REG(out);

    naor->ofs_nbits = nxm_encode_ofs_nbits(output_reg->src.ofs,
                                           output_reg->src.n_bits);
    naor->src = htonl(output_reg->src.field->nxm_header);
    naor->max_len = htons(output_reg->max_len);
}

static void
ofpact_resubmit_to_nxast(const struct ofpact_resubmit *resubmit,
                         struct ofpbuf *out)
{
    struct nx_action_resubmit *nar;

    if (resubmit->table_id == 0xff
        && resubmit->ofpact.compat != OFPUTIL_NXAST_RESUBMIT_TABLE) {
        nar = ofputil_put_NXAST_RESUBMIT(out);
    } else {
        nar = ofputil_put_NXAST_RESUBMIT_TABLE(out);
        nar->table = resubmit->table_id;
    }
    nar->in_port = htons(resubmit->in_port);
}

static void
ofpact_set_tunnel_to_nxast(const struct ofpact_tunnel *tunnel,
                           struct ofpbuf *out)
{
    uint64_t tun_id = tunnel->tun_id;

    if (tun_id <= UINT32_MAX
        && tunnel->ofpact.compat != OFPUTIL_NXAST_SET_TUNNEL64) {
        ofputil_put_NXAST_SET_TUNNEL(out)->tun_id = htonl(tun_id);
    } else {
        ofputil_put_NXAST_SET_TUNNEL64(out)->tun_id = htonll(tun_id);
    }
}

static void
ofpact_note_to_nxast(const struct ofpact_note *note, struct ofpbuf *out)
{
    size_t start_ofs = out->size;
    struct nx_action_note *nan;
    unsigned int remainder;
    unsigned int len;

    nan = ofputil_put_NXAST_NOTE(out);
    out->size -= sizeof nan->note;

    ofpbuf_put(out, note->data, note->length);

    len = out->size - start_ofs;
    remainder = len % OFP_ACTION_ALIGN;
    if (remainder) {
        ofpbuf_put_zeros(out, OFP_ACTION_ALIGN - remainder);
    }
    nan = (struct nx_action_note *)((char *)out->data + start_ofs);
    nan->len = htons(out->size - start_ofs);
}

static void
ofpact_controller_to_nxast(const struct ofpact_controller *oc,
                           struct ofpbuf *out)
{
    struct nx_action_controller *nac;

    nac = ofputil_put_NXAST_CONTROLLER(out);
    nac->max_len = htons(oc->max_len);
    nac->controller_id = htons(oc->controller_id);
    nac->reason = oc->reason;
}

static void
ofpact_fin_timeout_to_nxast(const struct ofpact_fin_timeout *fin_timeout,
                            struct ofpbuf *out)
{
    struct nx_action_fin_timeout *naft = ofputil_put_NXAST_FIN_TIMEOUT(out);
    naft->fin_idle_timeout = htons(fin_timeout->fin_idle_timeout);
    naft->fin_hard_timeout = htons(fin_timeout->fin_hard_timeout);
}

static void
ofpact_to_nxast(const struct ofpact *a, struct ofpbuf *out)
{
    switch (a->type) {
    case OFPACT_CONTROLLER:
        ofpact_controller_to_nxast(ofpact_get_CONTROLLER(a), out);
        break;

    case OFPACT_OUTPUT_REG:
        ofpact_output_reg_to_nxast(ofpact_get_OUTPUT_REG(a), out);
        break;

    case OFPACT_BUNDLE:
        bundle_to_openflow(ofpact_get_BUNDLE(a), out);
        break;

    case OFPACT_REG_MOVE:
        nxm_reg_move_to_openflow(ofpact_get_REG_MOVE(a), out);
        break;

    case OFPACT_REG_LOAD:
        nxm_reg_load_to_openflow(ofpact_get_REG_LOAD(a), out);
        break;

    case OFPACT_DEC_TTL:
        ofputil_put_NXAST_DEC_TTL(out);
        break;

    case OFPACT_SET_TUNNEL:
        ofpact_set_tunnel_to_nxast(ofpact_get_SET_TUNNEL(a), out);
        break;

    case OFPACT_SET_QUEUE:
        ofputil_put_NXAST_SET_QUEUE(out)->queue_id
            = htonl(ofpact_get_SET_QUEUE(a)->queue_id);
        break;

    case OFPACT_POP_QUEUE:
        ofputil_put_NXAST_POP_QUEUE(out);
        break;

    case OFPACT_FIN_TIMEOUT:
        ofpact_fin_timeout_to_nxast(ofpact_get_FIN_TIMEOUT(a), out);
        break;

    case OFPACT_RESUBMIT:
        ofpact_resubmit_to_nxast(ofpact_get_RESUBMIT(a), out);
        break;

    case OFPACT_LEARN:
        learn_to_openflow(ofpact_get_LEARN(a), out);
        break;

    case OFPACT_MULTIPATH:
        multipath_to_openflow(ofpact_get_MULTIPATH(a), out);
        break;

    case OFPACT_AUTOPATH:
        autopath_to_openflow(ofpact_get_AUTOPATH(a), out);
        break;

    case OFPACT_NOTE:
        ofpact_note_to_nxast(ofpact_get_NOTE(a), out);
        break;

    case OFPACT_EXIT:
        ofputil_put_NXAST_EXIT(out);
        break;

    case OFPACT_END:
    case OFPACT_OUTPUT:
    case OFPACT_ENQUEUE:
    case OFPACT_SET_VLAN_VID:
    case OFPACT_SET_VLAN_PCP:
    case OFPACT_STRIP_VLAN:
    case OFPACT_SET_ETH_SRC:
    case OFPACT_SET_ETH_DST:
    case OFPACT_SET_IPV4_SRC:
    case OFPACT_SET_IPV4_DST:
    case OFPACT_SET_IPV4_DSCP:
    case OFPACT_SET_L4_SRC_PORT:
    case OFPACT_SET_L4_DST_PORT:
        NOT_REACHED();
    }
}

/* Converting ofpacts to OpenFlow 1.0. */

static void
ofpact_output_to_openflow10(const struct ofpact_output *output,
                            struct ofpbuf *out)
{
    struct ofp10_action_output *oao;

    oao = ofputil_put_OFPAT10_OUTPUT(out);
    oao->port = htons(output->port);
    oao->max_len = htons(output->max_len);
}

static void
ofpact_enqueue_to_openflow10(const struct ofpact_enqueue *enqueue,
                             struct ofpbuf *out)
{
    struct ofp_action_enqueue *oae;

    oae = ofputil_put_OFPAT10_ENQUEUE(out);
    oae->port = htons(enqueue->port);
    oae->queue_id = htonl(enqueue->queue);
}

static void
ofpact_to_openflow10(const struct ofpact *a, struct ofpbuf *out)
{
    switch (a->type) {
    case OFPACT_END:
        NOT_REACHED();

    case OFPACT_OUTPUT:
        ofpact_output_to_openflow10(ofpact_get_OUTPUT(a), out);
        break;

    case OFPACT_ENQUEUE:
        ofpact_enqueue_to_openflow10(ofpact_get_ENQUEUE(a), out);
        break;

    case OFPACT_SET_VLAN_VID:
        ofputil_put_OFPAT10_SET_VLAN_VID(out)->vlan_vid
            = htons(ofpact_get_SET_VLAN_VID(a)->vlan_vid);
        break;

    case OFPACT_SET_VLAN_PCP:
        ofputil_put_OFPAT10_SET_VLAN_PCP(out)->vlan_pcp
            = ofpact_get_SET_VLAN_PCP(a)->vlan_pcp;
        break;

    case OFPACT_STRIP_VLAN:
        ofputil_put_OFPAT10_STRIP_VLAN(out);
        break;

    case OFPACT_SET_ETH_SRC:
        memcpy(ofputil_put_OFPAT10_SET_DL_SRC(out)->dl_addr,
               ofpact_get_SET_ETH_SRC(a)->mac, ETH_ADDR_LEN);
        break;

    case OFPACT_SET_ETH_DST:
        memcpy(ofputil_put_OFPAT10_SET_DL_DST(out)->dl_addr,
               ofpact_get_SET_ETH_DST(a)->mac, ETH_ADDR_LEN);
        break;

    case OFPACT_SET_IPV4_SRC:
        ofputil_put_OFPAT10_SET_NW_SRC(out)->nw_addr
            = ofpact_get_SET_IPV4_SRC(a)->ipv4;
        break;

    case OFPACT_SET_IPV4_DST:
        ofputil_put_OFPAT10_SET_NW_DST(out)->nw_addr
            = ofpact_get_SET_IPV4_DST(a)->ipv4;
        break;

    case OFPACT_SET_IPV4_DSCP:
        ofputil_put_OFPAT10_SET_NW_TOS(out)->nw_tos
            = ofpact_get_SET_IPV4_DSCP(a)->dscp;
        break;

    case OFPACT_SET_L4_SRC_PORT:
        ofputil_put_OFPAT10_SET_TP_SRC(out)->tp_port
            = htons(ofpact_get_SET_L4_SRC_PORT(a)->port);
        break;

    case OFPACT_SET_L4_DST_PORT:
        ofputil_put_OFPAT10_SET_TP_DST(out)->tp_port
            = htons(ofpact_get_SET_L4_DST_PORT(a)->port);
        break;

    case OFPACT_CONTROLLER:
    case OFPACT_OUTPUT_REG:
    case OFPACT_BUNDLE:
    case OFPACT_REG_MOVE:
    case OFPACT_REG_LOAD:
    case OFPACT_DEC_TTL:
    case OFPACT_SET_TUNNEL:
    case OFPACT_SET_QUEUE:
    case OFPACT_POP_QUEUE:
    case OFPACT_FIN_TIMEOUT:
    case OFPACT_RESUBMIT:
    case OFPACT_LEARN:
    case OFPACT_MULTIPATH:
    case OFPACT_AUTOPATH:
    case OFPACT_NOTE:
    case OFPACT_EXIT:
        ofpact_to_nxast(a, out);
        break;
    }
}

/* Converts the ofpacts in 'ofpacts' (terminated by OFPACT_END) into OpenFlow
 * 1.0 actions in 'openflow', appending the actions to any existing data in
 * 'openflow'. */
void
ofpacts_to_openflow10(const struct ofpact ofpacts[], struct ofpbuf *openflow)
{
    const struct ofpact *a;

    OFPACT_FOR_EACH (a, ofpacts) {
        ofpact_to_openflow10(a, openflow);
    }
}

/* Converting ofpacts to OpenFlow 1.1. */

static void
ofpact_output_to_openflow11(const struct ofpact_output *output,
                            struct ofpbuf *out)
{
    struct ofp11_action_output *oao;

    oao = ofputil_put_OFPAT11_OUTPUT(out);
    oao->port = ofputil_port_to_ofp11(output->port);
    oao->max_len = htons(output->max_len);
}

static void
ofpact_to_openflow11(const struct ofpact *a, struct ofpbuf *out)
{
    switch (a->type) {
    case OFPACT_END:
        NOT_REACHED();

    case OFPACT_OUTPUT:
        return ofpact_output_to_openflow11(ofpact_get_OUTPUT(a), out);

    case OFPACT_ENQUEUE:
        /* XXX */
        break;

    case OFPACT_SET_VLAN_VID:
        ofputil_put_OFPAT11_SET_VLAN_VID(out)->vlan_vid
            = htons(ofpact_get_SET_VLAN_VID(a)->vlan_vid);
        break;

    case OFPACT_SET_VLAN_PCP:
        ofputil_put_OFPAT11_SET_VLAN_PCP(out)->vlan_pcp
            = ofpact_get_SET_VLAN_PCP(a)->vlan_pcp;
        break;

    case OFPACT_STRIP_VLAN:
        /* XXX */
        break;

    case OFPACT_SET_ETH_SRC:
        memcpy(ofputil_put_OFPAT11_SET_DL_SRC(out)->dl_addr,
               ofpact_get_SET_ETH_SRC(a)->mac, ETH_ADDR_LEN);
        break;

    case OFPACT_SET_ETH_DST:
        memcpy(ofputil_put_OFPAT11_SET_DL_DST(out)->dl_addr,
               ofpact_get_SET_ETH_DST(a)->mac, ETH_ADDR_LEN);
        break;

    case OFPACT_SET_IPV4_SRC:
        ofputil_put_OFPAT11_SET_NW_SRC(out)->nw_addr
            = ofpact_get_SET_IPV4_SRC(a)->ipv4;
        break;

    case OFPACT_SET_IPV4_DST:
        ofputil_put_OFPAT11_SET_NW_DST(out)->nw_addr
            = ofpact_get_SET_IPV4_DST(a)->ipv4;
        break;

    case OFPACT_SET_IPV4_DSCP:
        ofputil_put_OFPAT11_SET_NW_TOS(out)->nw_tos
            = ofpact_get_SET_IPV4_DSCP(a)->dscp;
        break;

    case OFPACT_SET_L4_SRC_PORT:
        ofputil_put_OFPAT11_SET_TP_SRC(out)->tp_port
            = htons(ofpact_get_SET_L4_SRC_PORT(a)->port);
        break;

    case OFPACT_SET_L4_DST_PORT:
        ofputil_put_OFPAT11_SET_TP_DST(out)->tp_port
            = htons(ofpact_get_SET_L4_DST_PORT(a)->port);
        break;

    case OFPACT_CONTROLLER:
    case OFPACT_OUTPUT_REG:
    case OFPACT_BUNDLE:
    case OFPACT_REG_MOVE:
    case OFPACT_REG_LOAD:
    case OFPACT_DEC_TTL:
    case OFPACT_SET_TUNNEL:
    case OFPACT_SET_QUEUE:
    case OFPACT_POP_QUEUE:
    case OFPACT_FIN_TIMEOUT:
    case OFPACT_RESUBMIT:
    case OFPACT_LEARN:
    case OFPACT_MULTIPATH:
    case OFPACT_AUTOPATH:
    case OFPACT_NOTE:
    case OFPACT_EXIT:
        ofpact_to_nxast(a, out);
        break;
    }
}

/* Converts the ofpacts in 'ofpacts' (terminated by OFPACT_END) into OpenFlow
 * 1.1 actions in 'openflow', appending the actions to any existing data in
 * 'openflow'. */
void
ofpacts_to_openflow11(const struct ofpact ofpacts[], struct ofpbuf *openflow,
                      enum ofp11_instruction_type type)
{
    const struct ofpact *a;

    switch (type) {
    case OFPIT11_GOTO_TABLE:
    case OFPIT11_WRITE_METADATA:
    case OFPIT11_WRITE_ACTIONS:
        /* FIXME: Implementation needed */
        NOT_REACHED();

    case OFPIT11_APPLY_ACTIONS: {
        struct ofp11_instruction_actions *oia;
        size_t start_len = openflow->size;

        ofpbuf_put_uninit(openflow, sizeof *oia);
        OFPACT_FOR_EACH (a, ofpacts) {
            ofpact_to_openflow11(a, openflow);
        }
        oia = (struct ofp11_instruction_actions *)((char *)openflow->data +
                                                   start_len);
        oia->type = htons(type);
        oia->len = htons(openflow->size - start_len);
        memset(oia->pad, 0, sizeof oia->pad);

        break;
    }

    case OFPIT11_CLEAR_ACTIONS:
    case OFPIT11_EXPERIMENTER:
        /* FIXME: Implementation needed */
        NOT_REACHED();

    default:
        NOT_REACHED();
    }
}

/* Returns true if 'action' outputs to 'port', false otherwise. */
static bool
ofpact_outputs_to_port(const struct ofpact *ofpact, uint16_t port)
{
    switch (ofpact->type) {
    case OFPACT_OUTPUT:
        return ofpact_get_OUTPUT(ofpact)->port == port;
    case OFPACT_ENQUEUE:
        return ofpact_get_ENQUEUE(ofpact)->port == port;
    case OFPACT_CONTROLLER:
        return port == OFPP_CONTROLLER;

    case OFPACT_END:
    case OFPACT_OUTPUT_REG:
    case OFPACT_BUNDLE:
    case OFPACT_SET_VLAN_VID:
    case OFPACT_SET_VLAN_PCP:
    case OFPACT_STRIP_VLAN:
    case OFPACT_SET_ETH_SRC:
    case OFPACT_SET_ETH_DST:
    case OFPACT_SET_IPV4_SRC:
    case OFPACT_SET_IPV4_DST:
    case OFPACT_SET_IPV4_DSCP:
    case OFPACT_SET_L4_SRC_PORT:
    case OFPACT_SET_L4_DST_PORT:
    case OFPACT_REG_MOVE:
    case OFPACT_REG_LOAD:
    case OFPACT_DEC_TTL:
    case OFPACT_SET_TUNNEL:
    case OFPACT_SET_QUEUE:
    case OFPACT_POP_QUEUE:
    case OFPACT_FIN_TIMEOUT:
    case OFPACT_RESUBMIT:
    case OFPACT_LEARN:
    case OFPACT_MULTIPATH:
    case OFPACT_AUTOPATH:
    case OFPACT_NOTE:
    case OFPACT_EXIT:
    default:
        return false;
    }
}

/* Returns true if any action in 'ofpacts' outputs to 'port', false
 * otherwise. */
bool
ofpacts_output_to_port(const struct ofpact *ofpacts, uint16_t port)
{
    const struct ofpact *a;

    OFPACT_FOR_EACH (a, ofpacts) {
        if (ofpact_outputs_to_port(a, port)) {
            return true;
        }
    }

    return false;
}

bool
ofpacts_equal(const struct ofpact *a, size_t a_len,
              const struct ofpact *b, size_t b_len)
{
    return a_len == b_len && !memcmp(a, b, a_len);
}

/* Formatting ofpacts. */

static void
print_note(const struct ofpact_note *note, struct ds *string)
{
    size_t i;

    ds_put_cstr(string, "note:");
    for (i = 0; i < note->length; i++) {
        if (i) {
            ds_put_char(string, '.');
        }
        ds_put_format(string, "%02"PRIx8, note->data[i]);
    }
}

static void
print_fin_timeout(const struct ofpact_fin_timeout *fin_timeout,
                  struct ds *s)
{
    ds_put_cstr(s, "fin_timeout(");
    if (fin_timeout->fin_idle_timeout) {
        ds_put_format(s, "idle_timeout=%"PRIu16",",
                      fin_timeout->fin_idle_timeout);
    }
    if (fin_timeout->fin_hard_timeout) {
        ds_put_format(s, "hard_timeout=%"PRIu16",",
                      fin_timeout->fin_hard_timeout);
    }
    ds_chomp(s, ',');
    ds_put_char(s, ')');
}

static void
ofpact_format(const struct ofpact *a, struct ds *s)
{
    const struct ofpact_enqueue *enqueue;
    const struct ofpact_resubmit *resubmit;
    const struct ofpact_autopath *autopath;
    const struct ofpact_controller *controller;
    const struct ofpact_tunnel *tunnel;
    uint16_t port;

    switch (a->type) {
    case OFPACT_END:
        NOT_REACHED();

    case OFPACT_OUTPUT:
        port = ofpact_get_OUTPUT(a)->port;
        if (port < OFPP_MAX) {
            ds_put_format(s, "output:%"PRIu16, port);
        } else {
            ofputil_format_port(port, s);
            if (port == OFPP_CONTROLLER) {
                ds_put_format(s, ":%"PRIu16, ofpact_get_OUTPUT(a)->max_len);
            }
        }
        break;

    case OFPACT_CONTROLLER:
        controller = ofpact_get_CONTROLLER(a);
        if (controller->reason == OFPR_ACTION &&
            controller->controller_id == 0) {
            ds_put_format(s, "CONTROLLER:%"PRIu16,
                          ofpact_get_CONTROLLER(a)->max_len);
        } else {
            enum ofp_packet_in_reason reason = controller->reason;

            ds_put_cstr(s, "controller(");
            if (reason != OFPR_ACTION) {
                ds_put_format(s, "reason=%s,",
                              ofputil_packet_in_reason_to_string(reason));
            }
            if (controller->max_len != UINT16_MAX) {
                ds_put_format(s, "max_len=%"PRIu16",", controller->max_len);
            }
            if (controller->controller_id != 0) {
                ds_put_format(s, "id=%"PRIu16",", controller->controller_id);
            }
            ds_chomp(s, ',');
            ds_put_char(s, ')');
        }
        break;

    case OFPACT_ENQUEUE:
        enqueue = ofpact_get_ENQUEUE(a);
        ds_put_format(s, "enqueue:");
        ofputil_format_port(enqueue->port, s);
        ds_put_format(s, "q%"PRIu32, enqueue->queue);
        break;

    case OFPACT_OUTPUT_REG:
        ds_put_cstr(s, "output:");
        mf_format_subfield(&ofpact_get_OUTPUT_REG(a)->src, s);
        break;

    case OFPACT_BUNDLE:
        bundle_format(ofpact_get_BUNDLE(a), s);
        break;

    case OFPACT_SET_VLAN_VID:
        ds_put_format(s, "mod_vlan_vid:%"PRIu16,
                      ofpact_get_SET_VLAN_VID(a)->vlan_vid);
        break;

    case OFPACT_SET_VLAN_PCP:
        ds_put_format(s, "mod_vlan_pcp:%"PRIu8,
                      ofpact_get_SET_VLAN_PCP(a)->vlan_pcp);
        break;

    case OFPACT_STRIP_VLAN:
        ds_put_cstr(s, "strip_vlan");
        break;

    case OFPACT_SET_ETH_SRC:
        ds_put_format(s, "mod_dl_src:"ETH_ADDR_FMT,
                      ETH_ADDR_ARGS(ofpact_get_SET_ETH_SRC(a)->mac));
        break;

    case OFPACT_SET_ETH_DST:
        ds_put_format(s, "mod_dl_dst:"ETH_ADDR_FMT,
                      ETH_ADDR_ARGS(ofpact_get_SET_ETH_DST(a)->mac));
        break;

    case OFPACT_SET_IPV4_SRC:
        ds_put_format(s, "mod_nw_src:"IP_FMT,
                      IP_ARGS(&ofpact_get_SET_IPV4_SRC(a)->ipv4));
        break;

    case OFPACT_SET_IPV4_DST:
        ds_put_format(s, "mod_nw_dst:"IP_FMT,
                      IP_ARGS(&ofpact_get_SET_IPV4_DST(a)->ipv4));
        break;

    case OFPACT_SET_IPV4_DSCP:
        ds_put_format(s, "mod_nw_tos:%d", ofpact_get_SET_IPV4_DSCP(a)->dscp);
        break;

    case OFPACT_SET_L4_SRC_PORT:
        ds_put_format(s, "mod_tp_src:%d", ofpact_get_SET_L4_SRC_PORT(a)->port);
        break;

    case OFPACT_SET_L4_DST_PORT:
        ds_put_format(s, "mod_tp_dst:%d", ofpact_get_SET_L4_DST_PORT(a)->port);
        break;

    case OFPACT_REG_MOVE:
        nxm_format_reg_move(ofpact_get_REG_MOVE(a), s);
        break;

    case OFPACT_REG_LOAD:
        nxm_format_reg_load(ofpact_get_REG_LOAD(a), s);
        break;

    case OFPACT_DEC_TTL:
        ds_put_cstr(s, "dec_ttl");
        break;

    case OFPACT_SET_TUNNEL:
        tunnel = ofpact_get_SET_TUNNEL(a);
        ds_put_format(s, "set_tunnel%s:%#"PRIx64,
                      (tunnel->tun_id > UINT32_MAX
                       || a->compat == OFPUTIL_NXAST_SET_TUNNEL64 ? "64" : ""),
                      tunnel->tun_id);
        break;

    case OFPACT_SET_QUEUE:
        ds_put_format(s, "set_queue:%"PRIu32,
                      ofpact_get_SET_QUEUE(a)->queue_id);
        break;

    case OFPACT_POP_QUEUE:
        ds_put_cstr(s, "pop_queue");
        break;

    case OFPACT_FIN_TIMEOUT:
        print_fin_timeout(ofpact_get_FIN_TIMEOUT(a), s);
        break;

    case OFPACT_RESUBMIT:
        resubmit = ofpact_get_RESUBMIT(a);
        if (resubmit->in_port != OFPP_IN_PORT && resubmit->table_id == 255) {
            ds_put_format(s, "resubmit:%"PRIu16, resubmit->in_port);
        } else {
            ds_put_format(s, "resubmit(");
            if (resubmit->in_port != OFPP_IN_PORT) {
                ofputil_format_port(resubmit->in_port, s);
            }
            ds_put_char(s, ',');
            if (resubmit->table_id != 255) {
                ds_put_format(s, "%"PRIu8, resubmit->table_id);
            }
            ds_put_char(s, ')');
        }
        break;

    case OFPACT_LEARN:
        learn_format(ofpact_get_LEARN(a), s);
        break;

    case OFPACT_MULTIPATH:
        multipath_format(ofpact_get_MULTIPATH(a), s);
        break;

    case OFPACT_AUTOPATH:
        autopath = ofpact_get_AUTOPATH(a);
        ds_put_format(s, "autopath(%u,", autopath->port);
        mf_format_subfield(&autopath->dst, s);
        ds_put_char(s, ')');
        break;

    case OFPACT_NOTE:
        print_note(ofpact_get_NOTE(a), s);
        break;

    case OFPACT_EXIT:
        ds_put_cstr(s, "exit");
        break;
    }
}

/* Appends a string representing the actions in 'ofpacts' (terminated by
 * OFPACT_END) to 'string'. */
void
ofpacts_format(const struct ofpact *ofpacts, struct ds *string)
{
    ds_put_cstr(string, "actions=");
    if (ofpacts->type == OFPACT_END) {
        ds_put_cstr(string, "drop");
    } else {
        const struct ofpact *a;

        OFPACT_FOR_EACH (a, ofpacts) {
            if (a != ofpacts) {
                ds_put_cstr(string, ",");
            }
            ofpact_format(a, string);
        }
    }
}

/* Internal use by helpers. */

void *
ofpact_put(struct ofpbuf *ofpacts, enum ofpact_type type, size_t len)
{
    struct ofpact *ofpact;
    unsigned int rem;

    rem = ofpacts->size % OFPACT_ALIGNTO;
    if (rem) {
        ofpbuf_put_zeros(ofpacts, OFPACT_ALIGNTO - rem);
    }

    ofpact = ofpacts->l2 = ofpbuf_put_uninit(ofpacts, len);
    ofpact_init(ofpact, type, len);
    return ofpact;
}

void
ofpact_init(struct ofpact *ofpact, enum ofpact_type type, size_t len)
{
    memset(ofpact, 0, len);
    ofpact->type = type;
    ofpact->compat = OFPUTIL_ACTION_INVALID;
    ofpact->len = len;
}

/* Updates 'ofpact->len' to the number of bytes in the tail of 'ofpacts'
 * starting at 'ofpact'.
 *
 * This is the correct way to update a variable-length ofpact's length after
 * adding the variable-length part of the payload.  (See the large comment
 * near the end of ofp-actions.h for more information.) */
void
ofpact_update_len(struct ofpbuf *ofpacts, struct ofpact *ofpact)
{
    assert(ofpact == ofpacts->l2);
    ofpact->len = (char *) ofpbuf_tail(ofpacts) - (char *) ofpact;
}