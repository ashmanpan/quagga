/*
  PIM for Quagga
  Copyright (C) 2008  Everton da Silva Marques

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING; if not, write to the
  Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
  MA 02110-1301 USA
  
*/

#include <zebra.h>

#include "zebra/rib.h"

#include "log.h"
#include "zclient.h"
#include "memory.h"
#include "thread.h"
#include "linklist.h"
#include "vty.h"
#include "plist.h"
#include "hash.h"
#include "jhash.h"
#include "wheel.h"

#include "pimd.h"
#include "pim_pim.h"
#include "pim_str.h"
#include "pim_time.h"
#include "pim_iface.h"
#include "pim_join.h"
#include "pim_zlookup.h"
#include "pim_upstream.h"
#include "pim_ifchannel.h"
#include "pim_neighbor.h"
#include "pim_rpf.h"
#include "pim_zebra.h"
#include "pim_oil.h"
#include "pim_macro.h"
#include "pim_rp.h"
#include "pim_br.h"
#include "pim_register.h"
#include "pim_msdp.h"
#include "pim_jp_agg.h"
#include "pim_nht.h"

struct hash *pim_upstream_hash = NULL;
struct list *pim_upstream_list = NULL;
struct timer_wheel *pim_upstream_sg_wheel = NULL;

static void join_timer_stop(struct pim_upstream *up);
static void pim_upstream_update_assert_tracking_desired(struct pim_upstream *up);

/*
 * A (*,G) or a (*,*) is going away
 * remove the parent pointer from
 * those pointing at us
 */
static void
pim_upstream_remove_children (struct pim_upstream *up)
{
  struct pim_upstream *child;

  if (!up->sources)
    return;

  while (!list_isempty (up->sources))
    {
      child = listnode_head (up->sources);
      child->parent = NULL;
      listnode_delete (up->sources, child);
    }
}

/*
 * A (*,G) or a (*,*) is being created
 * Find the children that would point
 * at us.
 */
static void
pim_upstream_find_new_children (struct pim_upstream *up)
{
  struct pim_upstream *child;
  struct listnode *ch_node;

  if ((up->sg.src.s_addr != INADDR_ANY) &&
      (up->sg.grp.s_addr != INADDR_ANY))
    return;

  if ((up->sg.src.s_addr == INADDR_ANY) &&
      (up->sg.grp.s_addr == INADDR_ANY))
    return;

  for (ALL_LIST_ELEMENTS_RO (pim_upstream_list, ch_node, child))
    {
      if ((up->sg.grp.s_addr != INADDR_ANY) &&
	  (child->sg.grp.s_addr == up->sg.grp.s_addr) &&
	  (child != up))
	{
	  child->parent = up;
	  listnode_add_sort (up->sources, child);
	}
    }
}

/*
 * If we have a (*,*) || (S,*) there is no parent
 * If we have a (S,G), find the (*,G)
 * If we have a (*,G), find the (*,*)
 */
static struct pim_upstream *
pim_upstream_find_parent (struct pim_upstream *child)
{
  struct prefix_sg any = child->sg;
  struct pim_upstream *up = NULL;

  // (S,G)
  if ((child->sg.src.s_addr != INADDR_ANY) &&
      (child->sg.grp.s_addr != INADDR_ANY))
    {
      any.src.s_addr = INADDR_ANY;
      up = pim_upstream_find (&any);

      if (up)
	listnode_add (up->sources, child);

      return up;
    }

  return NULL;
}

void pim_upstream_free(struct pim_upstream *up)
{
  XFREE(MTYPE_PIM_UPSTREAM, up);
  up = NULL;
}

static void upstream_channel_oil_detach(struct pim_upstream *up)
{
  if (up->channel_oil) {
    pim_channel_oil_del(up->channel_oil);
    up->channel_oil = NULL;
  }
}

void
pim_upstream_del(struct pim_upstream *up, const char *name)
{
  bool notify_msdp = false;
  struct prefix nht_p;

  if (PIM_DEBUG_TRACE)
    zlog_debug ("%s(%s): Delete %s ref count: %d",
		__PRETTY_FUNCTION__, name, up->sg_str, up->ref_count);

  --up->ref_count;

  if (up->ref_count >= 1)
    return;

  THREAD_OFF(up->t_ka_timer);
  THREAD_OFF(up->t_rs_timer);
  THREAD_OFF(up->t_msdp_reg_timer);

  if (up->join_state == PIM_UPSTREAM_JOINED) {
    pim_jp_agg_single_upstream_send (&up->rpf, up, 0);

    if (up->sg.src.s_addr == INADDR_ANY) {
        /* if a (*, G) entry in the joined state is being deleted we
         * need to notify MSDP */
        notify_msdp = true;
    }
  }

  join_timer_stop(up);
  up->rpf.source_nexthop.interface = NULL;

  if (up->sg.src.s_addr != INADDR_ANY) {
    wheel_remove_item (pim_upstream_sg_wheel, up);
    notify_msdp = true;
  }

  pim_upstream_remove_children (up);
  pim_mroute_del (up->channel_oil, __PRETTY_FUNCTION__);
  upstream_channel_oil_detach(up);

  if (up->sources)
    list_delete (up->sources);
  up->sources = NULL;

  /*
    notice that listnode_delete() can't be moved
    into pim_upstream_free() because the later is
    called by list_delete_all_node()
  */
  if (up->parent)
    {
      listnode_delete (up->parent->sources, up);
      up->parent = NULL;
    }
  listnode_delete (pim_upstream_list, up);
  hash_release (pim_upstream_hash, up);

  if (notify_msdp)
    {
      pim_msdp_up_del (&up->sg);
    }

  /* Deregister addr with Zebra NHT */
  nht_p.family = AF_INET;
  nht_p.prefixlen = IPV4_MAX_BITLEN;
  nht_p.u.prefix4 = up->upstream_addr;
  if (PIM_DEBUG_TRACE)
    {
      char buf[PREFIX2STR_BUFFER];
      prefix2str (&nht_p, buf, sizeof (buf));
      zlog_debug ("%s: Deregister upstream %s upstream addr %s with NHT ",
                __PRETTY_FUNCTION__, up->sg_str, buf);
    }
  pim_delete_tracked_nexthop (&nht_p, up, NULL);

  pim_upstream_free (up);
}

void
pim_upstream_send_join (struct pim_upstream *up)
{
  if (PIM_DEBUG_TRACE) {
    char rpf_str[PREFIX_STRLEN];
    pim_addr_dump("<rpf?>", &up->rpf.rpf_addr, rpf_str, sizeof(rpf_str));
    zlog_debug ("%s: RPF'%s=%s(%s) for Interface %s", __PRETTY_FUNCTION__,
		up->sg_str, rpf_str, pim_upstream_state2str (up->join_state),
		up->rpf.source_nexthop.interface->name);
    if (pim_rpf_addr_is_inaddr_any(&up->rpf)) {
      zlog_debug("%s: can't send join upstream: RPF'%s=%s",
		 __PRETTY_FUNCTION__,
		 up->sg_str, rpf_str);
      /* warning only */
    }
  }

  /* send Join(S,G) to the current upstream neighbor */
  pim_jp_agg_single_upstream_send(&up->rpf, up, 1 /* join */);
}

static int on_join_timer(struct thread *t)
{
  struct pim_upstream *up;

  up = THREAD_ARG(t);

  up->t_join_timer = NULL;

  /*
   * In the case of a HFR we will not ahve anyone to send this to.
   */
  if (PIM_UPSTREAM_FLAG_TEST_FHR(up->flags))
    return 0;

  /*
   * Don't send the join if the outgoing interface is a loopback
   * But since this might change leave the join timer running
   */
  if (up->rpf.source_nexthop.interface &&
      !if_is_loopback (up->rpf.source_nexthop.interface))
    pim_upstream_send_join (up);

  join_timer_start(up);

  return 0;
}

static void join_timer_stop(struct pim_upstream *up)
{
  struct pim_neighbor *nbr;

  nbr = pim_neighbor_find (up->rpf.source_nexthop.interface,
                           up->rpf.rpf_addr.u.prefix4);

  if (nbr)
    pim_jp_agg_remove_group (nbr->upstream_jp_agg, up);

  THREAD_OFF (up->t_join_timer);
}

void
join_timer_start(struct pim_upstream *up)
{
  struct pim_neighbor *nbr = NULL;

  if (up->rpf.source_nexthop.interface)
    {
      nbr = pim_neighbor_find (up->rpf.source_nexthop.interface,
                               up->rpf.rpf_addr.u.prefix4);

      if (PIM_DEBUG_PIM_EVENTS) {
        zlog_debug("%s: starting %d sec timer for upstream (S,G)=%s",
                   __PRETTY_FUNCTION__,
                   qpim_t_periodic,
                   up->sg_str);
      }
    }

  if (nbr)
    pim_jp_agg_add_group (nbr->upstream_jp_agg, up, 1);
  else
    {
      THREAD_OFF (up->t_join_timer);
      THREAD_TIMER_ON(master, up->t_join_timer,
                      on_join_timer,
                      up, qpim_t_periodic);
    }
}

/*
 * This is only called when we are switching the upstream
 * J/P from one neighbor to another
 *
 * As such we need to remove from the old list and
 * add to the new list.
 */
void pim_upstream_join_timer_restart(struct pim_upstream *up, struct pim_rpf *old)
{
  struct pim_neighbor *nbr;

  nbr = pim_neighbor_find (old->source_nexthop.interface,
                           old->rpf_addr.u.prefix4);
  if (nbr)
    pim_jp_agg_remove_group (nbr->upstream_jp_agg, up);

  //THREAD_OFF(up->t_join_timer);
  join_timer_start(up);
}

static void pim_upstream_join_timer_restart_msec(struct pim_upstream *up,
						 int interval_msec)
{
  if (PIM_DEBUG_PIM_EVENTS) {
    zlog_debug("%s: restarting %d msec timer for upstream (S,G)=%s",
	       __PRETTY_FUNCTION__,
	       interval_msec,
	       up->sg_str);
  }

  THREAD_OFF(up->t_join_timer);
  THREAD_TIMER_MSEC_ON(master, up->t_join_timer,
		       on_join_timer,
		       up, interval_msec);
}

void pim_upstream_join_suppress(struct pim_upstream *up,
				struct in_addr rpf_addr,
				int holdtime)
{
  long t_joinsuppress_msec;
  long join_timer_remain_msec;

  t_joinsuppress_msec = MIN(pim_if_t_suppressed_msec(up->rpf.source_nexthop.interface),
			    1000 * holdtime);

  join_timer_remain_msec = pim_time_timer_remain_msec(up->t_join_timer);

  if (PIM_DEBUG_TRACE) {
    char rpf_str[INET_ADDRSTRLEN];
    pim_inet4_dump("<rpf?>", rpf_addr, rpf_str, sizeof(rpf_str));
    zlog_debug("%s %s: detected Join%s to RPF'(S,G)=%s: join_timer=%ld msec t_joinsuppress=%ld msec",
	       __FILE__, __PRETTY_FUNCTION__, 
	       up->sg_str,
	       rpf_str,
	       join_timer_remain_msec, t_joinsuppress_msec);
  }

  if (join_timer_remain_msec < t_joinsuppress_msec) {
    if (PIM_DEBUG_TRACE) {
      zlog_debug("%s %s: suppressing Join(S,G)=%s for %ld msec",
		 __FILE__, __PRETTY_FUNCTION__, 
		 up->sg_str, t_joinsuppress_msec);
    }

    pim_upstream_join_timer_restart_msec(up, t_joinsuppress_msec);
  }
}

void pim_upstream_join_timer_decrease_to_t_override(const char *debug_label,
                                                    struct pim_upstream *up)
{
  long join_timer_remain_msec;
  int t_override_msec;

  join_timer_remain_msec = pim_time_timer_remain_msec(up->t_join_timer);
  t_override_msec = pim_if_t_override_msec(up->rpf.source_nexthop.interface);

  if (PIM_DEBUG_TRACE) {
    char rpf_str[INET_ADDRSTRLEN];
    pim_inet4_dump("<rpf?>", up->rpf.rpf_addr.u.prefix4, rpf_str, sizeof(rpf_str));
    zlog_debug("%s: to RPF'%s=%s: join_timer=%ld msec t_override=%d msec",
	       debug_label,
	       up->sg_str, rpf_str,
	       join_timer_remain_msec, t_override_msec);
  }
    
  if (join_timer_remain_msec > t_override_msec) {
    if (PIM_DEBUG_TRACE) {
      zlog_debug("%s: decreasing (S,G)=%s join timer to t_override=%d msec",
		 debug_label,
		 up->sg_str,
		 t_override_msec);
    }

    pim_upstream_join_timer_restart_msec(up, t_override_msec);
  }
}

static void forward_on(struct pim_upstream *up)
{
  struct listnode      *chnode;
  struct listnode      *chnextnode;
  struct pim_interface *pim_ifp;
  struct pim_ifchannel *ch;

  /* scan (S,G) state */
  for (ALL_LIST_ELEMENTS(pim_ifchannel_list, chnode, chnextnode, ch)) {
    pim_ifp = ch->interface->info;
    if (!pim_ifp)
      continue;

    if (ch->upstream != up)
      continue;

    if (pim_macro_chisin_oiflist(ch))
      pim_forward_start(ch);

  } /* scan iface channel list */
}

static void forward_off(struct pim_upstream *up)
{
  struct listnode      *chnode;
  struct listnode      *chnextnode;
  struct pim_interface *pim_ifp;
  struct pim_ifchannel *ch;

  /* scan per-interface (S,G) state */
  for (ALL_LIST_ELEMENTS(pim_ifchannel_list, chnode, chnextnode, ch)) {
    pim_ifp = ch->interface->info;
    if (!pim_ifp)
      continue;

    if (ch->upstream != up)
      continue;

    pim_forward_stop(ch);

  } /* scan iface channel list */
}

static int
pim_upstream_could_register (struct pim_upstream *up)
{
  struct pim_interface *pim_ifp = up->rpf.source_nexthop.interface->info;

  if (pim_ifp && PIM_I_am_DR (pim_ifp) &&
      pim_if_connected_to_source (up->rpf.source_nexthop.interface, up->sg.src))
    return 1;

  return 0;
}

void
pim_upstream_switch(struct pim_upstream *up,
		    enum pim_upstream_state new_state)
{
  enum pim_upstream_state old_state = up->join_state;

  if (PIM_DEBUG_PIM_EVENTS) {
    zlog_debug("%s: PIM_UPSTREAM_%s: (S,G) old: %s new: %s",
	       __PRETTY_FUNCTION__,
	       up->sg_str,
	       pim_upstream_state2str (up->join_state),
	       pim_upstream_state2str (new_state));
  }

  up->join_state = new_state;
  if (old_state != new_state)
    up->state_transition = pim_time_monotonic_sec();

  pim_upstream_update_assert_tracking_desired(up);

  if (new_state == PIM_UPSTREAM_JOINED) {
    if (old_state != PIM_UPSTREAM_JOINED)
      {
        int old_fhr = PIM_UPSTREAM_FLAG_TEST_FHR(up->flags);
        forward_on(up);
        pim_msdp_up_join_state_changed(up);
	if (pim_upstream_could_register (up))
	  {
            PIM_UPSTREAM_FLAG_SET_FHR(up->flags);
            if (!old_fhr && PIM_UPSTREAM_FLAG_TEST_SRC_STREAM(up->flags))
              {
                up->reg_state = PIM_REG_JOIN;
                pim_upstream_keep_alive_timer_start (up, qpim_keep_alive_time);
	        pim_channel_add_oif (up->channel_oil, pim_regiface, PIM_OIF_FLAG_PROTO_PIM);
              }
	  }
	else
          {
	    pim_upstream_send_join (up);
	    join_timer_start (up);
	  }
      }
    else
      {
        forward_on (up);
      }
  }
  else {

    forward_off(up);
    if (old_state == PIM_UPSTREAM_JOINED)
      pim_msdp_up_join_state_changed(up);

    pim_jp_agg_single_upstream_send(&up->rpf, up, 0 /* prune */);
    join_timer_stop(up);
  }
}

int
pim_upstream_compare (void *arg1, void *arg2)
{
  const struct pim_upstream *up1 = (const struct pim_upstream *)arg1;
  const struct pim_upstream *up2 = (const struct pim_upstream *)arg2;

  if (ntohl(up1->sg.grp.s_addr) < ntohl(up2->sg.grp.s_addr))
    return -1;

  if (ntohl(up1->sg.grp.s_addr) > ntohl(up2->sg.grp.s_addr))
    return 1;

  if (ntohl(up1->sg.src.s_addr) < ntohl(up2->sg.src.s_addr))
    return -1;

  if (ntohl(up1->sg.src.s_addr) > ntohl(up2->sg.src.s_addr))
    return 1;

  return 0;
}

static struct pim_upstream *
pim_upstream_new (struct prefix_sg *sg,
		  struct interface *incoming,
		  int flags)
{
  enum pim_rpf_result rpf_result;
  struct pim_interface *pim_ifp;
  struct pim_upstream *up;

  up = XCALLOC(MTYPE_PIM_UPSTREAM, sizeof(*up));
  if (!up)
    {
      zlog_err("%s: PIM XCALLOC(%zu) failure",
	     __PRETTY_FUNCTION__, sizeof(*up));
      return NULL;
    }
  
  up->sg                          = *sg;
  pim_str_sg_set (sg, up->sg_str);
  up = hash_get (pim_upstream_hash, up, hash_alloc_intern);
  if (!pim_rp_set_upstream_addr (&up->upstream_addr, sg->src, sg->grp))
    {
      if (PIM_DEBUG_TRACE)
	zlog_debug("%s: Received a (*,G) with no RP configured", __PRETTY_FUNCTION__);

      hash_release (pim_upstream_hash, up);
      XFREE (MTYPE_PIM_UPSTREAM, up);
      return NULL;
    }

  up->parent                     = pim_upstream_find_parent (up);
  if (up->sg.src.s_addr == INADDR_ANY)
    {
      up->sources = list_new ();
      up->sources->cmp = pim_upstream_compare;
    }
  else
    up->sources = NULL;

  pim_upstream_find_new_children (up);
  up->flags                      = flags;
  up->ref_count                  = 1;
  up->t_join_timer               = NULL;
  up->t_ka_timer                 = NULL;
  up->t_rs_timer                 = NULL;
  up->t_msdp_reg_timer           = NULL;
  up->join_state                 = PIM_UPSTREAM_NOTJOINED;
  up->reg_state                  = PIM_REG_NOINFO;
  up->state_transition           = pim_time_monotonic_sec();
  up->channel_oil                = NULL;
  up->sptbit                     = PIM_UPSTREAM_SPTBIT_FALSE;

  up->rpf.source_nexthop.interface                = NULL;
  up->rpf.source_nexthop.mrib_nexthop_addr.family = AF_INET;
  up->rpf.source_nexthop.mrib_nexthop_addr.u.prefix4.s_addr = PIM_NET_INADDR_ANY;
  up->rpf.source_nexthop.mrib_metric_preference   = qpim_infinite_assert_metric.metric_preference;
  up->rpf.source_nexthop.mrib_route_metric        = qpim_infinite_assert_metric.route_metric;
  up->rpf.rpf_addr.family                         = AF_INET;
  up->rpf.rpf_addr.u.prefix4.s_addr               = PIM_NET_INADDR_ANY;

  if (up->sg.src.s_addr != INADDR_ANY)
    wheel_add_item (pim_upstream_sg_wheel, up);

  rpf_result = pim_rpf_update(up, NULL, 1);
  if (rpf_result == PIM_RPF_FAILURE) {
    struct prefix nht_p;

    if (PIM_DEBUG_TRACE)
      zlog_debug ("%s: Attempting to create upstream(%s), Unable to RPF for source", __PRETTY_FUNCTION__,
                  up->sg_str);

    nht_p.family = AF_INET;
    nht_p.prefixlen = IPV4_MAX_BITLEN;
    nht_p.u.prefix4 = up->upstream_addr;
    pim_delete_tracked_nexthop (&nht_p, up, NULL);

    if (up->parent)
      {
	listnode_delete (up->parent->sources, up);
	up->parent = NULL;
      }

    if (up->sg.src.s_addr != INADDR_ANY)
      wheel_remove_item (pim_upstream_sg_wheel, up);

    pim_upstream_remove_children (up);
    if (up->sources)
      list_delete (up->sources);

    hash_release (pim_upstream_hash, up);
    XFREE(MTYPE_PIM_UPSTREAM, up);
    return NULL;
  }

  pim_ifp = up->rpf.source_nexthop.interface->info;
  if (pim_ifp)
    up->channel_oil = pim_channel_oil_add(&up->sg, pim_ifp->mroute_vif_index);

  listnode_add_sort(pim_upstream_list, up);

  if (PIM_DEBUG_TRACE)
    {
      zlog_debug ("%s: Created Upstream %s upstream_addr %s",
            __PRETTY_FUNCTION__, up->sg_str,
            inet_ntoa (up->upstream_addr));
    }

  return up;
}

struct pim_upstream *pim_upstream_find(struct prefix_sg *sg)
{
  struct pim_upstream lookup;
  struct pim_upstream *up = NULL;

  lookup.sg = *sg;
  up = hash_lookup (pim_upstream_hash, &lookup);
  return up;
}

struct pim_upstream *
pim_upstream_find_or_add(struct prefix_sg *sg,
                         struct interface *incoming,
                         int flags, const char *name)
{
  struct pim_upstream *up;

  up = pim_upstream_find(sg);

  if (up)
    {
      if (!(up->flags & flags))
        {
          up->flags |= flags;
          up->ref_count++;
        }
    }
  else
    up = pim_upstream_add (sg, incoming, flags, name);

  return up;
}

static void pim_upstream_ref(struct pim_upstream *up, int flags)
{
  up->flags |= flags;
  ++up->ref_count;
}

struct pim_upstream *pim_upstream_add(struct prefix_sg *sg,
				      struct interface *incoming,
				      int flags, const char *name)
{
  struct pim_upstream *up = NULL;
  int found = 0;
  up = pim_upstream_find(sg);
  if (up) {
    pim_upstream_ref(up, flags);
    found = 1;
  }
  else {
    up = pim_upstream_new(sg, incoming, flags);
  }

  if (PIM_DEBUG_TRACE)
    {
      if (up)
	zlog_debug("%s(%s): %s, found: %d: ref_count: %d",
		   __PRETTY_FUNCTION__, name,
		   up->sg_str, found,
		   up->ref_count);
      else
	zlog_debug("%s(%s): (%s) failure to create",
		   __PRETTY_FUNCTION__, name,
		   pim_str_sg_dump (sg));
    }

  return up;
}

int
pim_upstream_evaluate_join_desired_interface (struct pim_upstream *up,
					      struct pim_ifchannel *ch)
{
  struct pim_upstream *parent = up->parent;

  if (ch->upstream == up)
    {
      if (!pim_macro_ch_lost_assert(ch) && pim_macro_chisin_joins_or_include(ch))
	return 1;

      if (PIM_IF_FLAG_TEST_S_G_RPT(ch->flags))
	return 0;
    }

  /*
   * joins (*,G)
   */
  if (parent && ch->upstream == parent)
    {
      if (!pim_macro_ch_lost_assert (ch) && pim_macro_chisin_joins_or_include (ch))
	return 1;
    }

  return 0;
}

/*
  Evaluate JoinDesired(S,G):

  JoinDesired(S,G) is true if there is a downstream (S,G) interface I
  in the set:

  inherited_olist(S,G) =
  joins(S,G) (+) pim_include(S,G) (-) lost_assert(S,G)

  JoinDesired(S,G) may be affected by changes in the following:

  pim_ifp->primary_address
  pim_ifp->pim_dr_addr
  ch->ifassert_winner_metric
  ch->ifassert_winner
  ch->local_ifmembership 
  ch->ifjoin_state
  ch->upstream->rpf.source_nexthop.mrib_metric_preference
  ch->upstream->rpf.source_nexthop.mrib_route_metric
  ch->upstream->rpf.source_nexthop.interface

  See also pim_upstream_update_join_desired() below.
 */
int pim_upstream_evaluate_join_desired(struct pim_upstream *up)
{
  struct listnode      *chnode;
  struct listnode      *chnextnode;
  struct pim_interface *pim_ifp;
  struct pim_ifchannel *ch;
  int                  ret = 0;

  /* scan per-interface (S,G) state */
  for (ALL_LIST_ELEMENTS(pim_ifchannel_list, chnode, chnextnode, ch))
    {
      pim_ifp = ch->interface->info;
      if (!pim_ifp)
	continue;

      ret += pim_upstream_evaluate_join_desired_interface (up, ch);
    } /* scan iface channel list */

  return ret; /* false */
}

/*
  See also pim_upstream_evaluate_join_desired() above.
*/
void pim_upstream_update_join_desired(struct pim_upstream *up)
{
  int was_join_desired; /* boolean */
  int is_join_desired; /* boolean */

  was_join_desired = PIM_UPSTREAM_FLAG_TEST_DR_JOIN_DESIRED(up->flags);

  is_join_desired = pim_upstream_evaluate_join_desired(up);
  if (is_join_desired)
    PIM_UPSTREAM_FLAG_SET_DR_JOIN_DESIRED(up->flags);
  else
    PIM_UPSTREAM_FLAG_UNSET_DR_JOIN_DESIRED(up->flags);

  /* switched from false to true */
  if (is_join_desired && !was_join_desired) {
    pim_upstream_switch(up, PIM_UPSTREAM_JOINED);
    return;
  }
      
  /* switched from true to false */
  if (!is_join_desired && was_join_desired) {
    pim_upstream_switch(up, PIM_UPSTREAM_NOTJOINED);
    return;
  }
}

/*
  RFC 4601 4.5.7. Sending (S,G) Join/Prune Messages
  Transitions from Joined State
  RPF'(S,G) GenID changes

  The upstream (S,G) state machine remains in Joined state.  If the
  Join Timer is set to expire in more than t_override seconds, reset
  it so that it expires after t_override seconds.
*/
void pim_upstream_rpf_genid_changed(struct in_addr neigh_addr)
{
  struct listnode     *up_node;
  struct listnode     *up_nextnode;
  struct pim_upstream *up;

  /*
   * Scan all (S,G) upstreams searching for RPF'(S,G)=neigh_addr
   */
  for (ALL_LIST_ELEMENTS(pim_upstream_list, up_node, up_nextnode, up)) {

    if (PIM_DEBUG_TRACE) {
      char neigh_str[INET_ADDRSTRLEN];
      char rpf_addr_str[PREFIX_STRLEN];
      pim_inet4_dump("<neigh?>", neigh_addr, neigh_str, sizeof(neigh_str));
      pim_addr_dump("<rpf?>", &up->rpf.rpf_addr, rpf_addr_str, sizeof(rpf_addr_str));
      zlog_debug("%s: matching neigh=%s against upstream (S,G)=%s joined=%d rpf_addr=%s",
		 __PRETTY_FUNCTION__,
		 neigh_str, up->sg_str,
		 up->join_state == PIM_UPSTREAM_JOINED,
		 rpf_addr_str);
    }

    /* consider only (S,G) upstream in Joined state */
    if (up->join_state != PIM_UPSTREAM_JOINED)
      continue;

    /* match RPF'(S,G)=neigh_addr */
    if (up->rpf.rpf_addr.u.prefix4.s_addr != neigh_addr.s_addr)
      continue;

    pim_upstream_join_timer_decrease_to_t_override("RPF'(S,G) GenID change",
                                                   up);
  }
}


void pim_upstream_rpf_interface_changed(struct pim_upstream *up,
					struct interface *old_rpf_ifp)
{
  struct listnode      *chnode;
  struct listnode      *chnextnode;
  struct pim_ifchannel *ch;
  struct pim_interface *pim_ifp;

  /* search all ifchannels */
  for (ALL_LIST_ELEMENTS(pim_ifchannel_list, chnode, chnextnode, ch)) {

    pim_ifp = ch->interface->info;
    if (!pim_ifp)
      continue;

    if (ch->upstream != up)
      continue;

    if (ch->ifassert_state == PIM_IFASSERT_I_AM_LOSER) {
      if (
	  /* RPF_interface(S) was NOT I */
	  (old_rpf_ifp == ch->interface)
	  &&
	  /* RPF_interface(S) stopped being I */
	  (ch->upstream->rpf.source_nexthop.interface != ch->interface)
	  ) {
	assert_action_a5(ch);
      }
    } /* PIM_IFASSERT_I_AM_LOSER */

    pim_ifchannel_update_assert_tracking_desired(ch);
  }
}

void pim_upstream_update_could_assert(struct pim_upstream *up)
{
  struct listnode      *chnode;
  struct listnode      *chnextnode;
  struct pim_interface *pim_ifp;
  struct pim_ifchannel *ch;

  /* scan per-interface (S,G) state */
  for (ALL_LIST_ELEMENTS(pim_ifchannel_list, chnode, chnextnode, ch)) {
    pim_ifp = ch->interface->info;
    if (!pim_ifp)
      continue;

    if (ch->upstream != up)
      continue;

    pim_ifchannel_update_could_assert(ch);
  } /* scan iface channel list */
}

void pim_upstream_update_my_assert_metric(struct pim_upstream *up)
{
  struct listnode      *chnode;
  struct listnode      *chnextnode;
  struct pim_interface *pim_ifp;
  struct pim_ifchannel *ch;

  /* scan per-interface (S,G) state */
  for (ALL_LIST_ELEMENTS(pim_ifchannel_list, chnode, chnextnode, ch)) {
    pim_ifp = ch->interface->info;
    if (!pim_ifp)
      continue;

    if (ch->upstream != up)
      continue;

    pim_ifchannel_update_my_assert_metric(ch);

  } /* scan iface channel list */
}

static void pim_upstream_update_assert_tracking_desired(struct pim_upstream *up)
{
  struct listnode      *chnode;
  struct listnode      *chnextnode;
  struct pim_interface *pim_ifp;
  struct pim_ifchannel *ch;

  /* scan per-interface (S,G) state */
  for (ALL_LIST_ELEMENTS(pim_ifchannel_list, chnode, chnextnode, ch)) {
    pim_ifp = ch->interface->info;
    if (!pim_ifp)
      continue;

    if (ch->upstream != up)
      continue;

    pim_ifchannel_update_assert_tracking_desired(ch);

  } /* scan iface channel list */
}

/* When kat is stopped CouldRegister goes to false so we need to
 * transition  the (S, G) on FHR to NI state and remove reg tunnel
 * from the OIL */
static void pim_upstream_fhr_kat_expiry(struct pim_upstream *up)
{
  if (!PIM_UPSTREAM_FLAG_TEST_FHR(up->flags))
    return;

  if (PIM_DEBUG_TRACE)
    zlog_debug ("kat expired on %s; clear fhr reg state", up->sg_str);

  /* stop reg-stop timer */
  THREAD_OFF(up->t_rs_timer);
  /* remove regiface from the OIL if it is there*/
  pim_channel_del_oif (up->channel_oil, pim_regiface, PIM_OIF_FLAG_PROTO_PIM);
  /* clear the register state */
  up->reg_state = PIM_REG_NOINFO;
  PIM_UPSTREAM_FLAG_UNSET_FHR(up->flags);
}

/* When kat is started CouldRegister can go to true. And if it does we
 * need to transition  the (S, G) on FHR to JOINED state and add reg tunnel
 * to the OIL */
static void pim_upstream_fhr_kat_start(struct pim_upstream *up)
{
  if (pim_upstream_could_register(up)) {
    if (PIM_DEBUG_TRACE)
      zlog_debug ("kat started on %s; set fhr reg state to joined", up->sg_str);

    PIM_UPSTREAM_FLAG_SET_FHR(up->flags);
    if (up->reg_state == PIM_REG_NOINFO) {
      pim_channel_add_oif (up->channel_oil, pim_regiface, PIM_OIF_FLAG_PROTO_PIM);
      up->reg_state = PIM_REG_JOIN;
    }
  }
}

/*
 * On an RP, the PMBR value must be cleared when the
 * Keepalive Timer expires
 * KAT expiry indicates that flow is inactive. If the flow was created or
 * maintained by activity now is the time to deref it.
 */
static int
pim_upstream_keep_alive_timer (struct thread *t)
{
  struct pim_upstream *up;

  up = THREAD_ARG(t);
  up->t_ka_timer = NULL;

  if (I_am_RP (up->sg.grp))
  {
    pim_br_clear_pmbr (&up->sg);
    /*
     * We need to do more here :)
     * But this is the start.
     */
  }

  /* source is no longer active - pull the SA from MSDP's cache */
  pim_msdp_sa_local_del(&up->sg);

  /* if entry was created because of activity we need to deref it */
  if (PIM_UPSTREAM_FLAG_TEST_SRC_STREAM(up->flags))
  {
    pim_upstream_fhr_kat_expiry(up);
    if (PIM_DEBUG_TRACE)
      zlog_debug ("kat expired on %s; remove stream reference", up->sg_str);
    PIM_UPSTREAM_FLAG_UNSET_SRC_STREAM(up->flags);
    pim_upstream_del(up, __PRETTY_FUNCTION__);
  }

  return 0;
}

void
pim_upstream_keep_alive_timer_start (struct pim_upstream *up,
				     uint32_t time)
{
  if (!PIM_UPSTREAM_FLAG_TEST_SRC_STREAM(up->flags)) {
    if (PIM_DEBUG_TRACE)
      zlog_debug ("kat start on %s with no stream reference", up->sg_str);
  }
  THREAD_OFF (up->t_ka_timer);
  THREAD_TIMER_ON (master,
		   up->t_ka_timer,
		   pim_upstream_keep_alive_timer,
		   up, time);

  /* any time keepalive is started against a SG we will have to
   * re-evaluate our active source database */
  pim_msdp_sa_local_update(up);
}

/* MSDP on RP needs to know if a source is registerable to this RP */
static int
pim_upstream_msdp_reg_timer(struct thread *t)
{
  struct pim_upstream *up;

  up = THREAD_ARG(t);
  up->t_msdp_reg_timer = NULL;

  /* source is no longer active - pull the SA from MSDP's cache */
  pim_msdp_sa_local_del(&up->sg);
  return 1;
}
void
pim_upstream_msdp_reg_timer_start(struct pim_upstream *up)
{
  THREAD_OFF(up->t_msdp_reg_timer);
  THREAD_TIMER_ON(master, up->t_msdp_reg_timer,
      pim_upstream_msdp_reg_timer, up, PIM_MSDP_REG_RXED_PERIOD);

  pim_msdp_sa_local_update(up);
}

/*
 * 4.2.1 Last-Hop Switchover to the SPT
 *
 *  In Sparse-Mode PIM, last-hop routers join the shared tree towards the
 *  RP.  Once traffic from sources to joined groups arrives at a last-hop
 *  router, it has the option of switching to receive the traffic on a
 *  shortest path tree (SPT).
 *
 *  The decision for a router to switch to the SPT is controlled as
 *  follows:
 *
 *    void
 *    CheckSwitchToSpt(S,G) {
 *      if ( ( pim_include(*,G) (-) pim_exclude(S,G)
 *             (+) pim_include(S,G) != NULL )
 *           AND SwitchToSptDesired(S,G) ) {
 *             # Note: Restarting the KAT will result in the SPT switch
 *             set KeepaliveTimer(S,G) to Keepalive_Period
 *      }
 *    }
 *
 *  SwitchToSptDesired(S,G) is a policy function that is implementation
 *  defined.  An "infinite threshold" policy can be implemented by making
 *  SwitchToSptDesired(S,G) return false all the time.  A "switch on
 *  first packet" policy can be implemented by making
 *  SwitchToSptDesired(S,G) return true once a single packet has been
 *  received for the source and group.
 */
int
pim_upstream_switch_to_spt_desired (struct prefix_sg *sg)
{
  if (I_am_RP (sg->grp))
    return 1;

  return 0;
}

int
pim_upstream_is_sg_rpt (struct pim_upstream *up)
{
  struct listnode *chnode;
  struct pim_ifchannel *ch;

  for (ALL_LIST_ELEMENTS_RO(pim_ifchannel_list, chnode, ch))
    {
      if ((ch->upstream == up) &&
	  (PIM_IF_FLAG_TEST_S_G_RPT(ch->flags)))
	return 1;
    }

  return 0;
}
/*
 *  After receiving a packet set SPTbit:
 *   void
 *   Update_SPTbit(S,G,iif) {
 *     if ( iif == RPF_interface(S)
 *           AND JoinDesired(S,G) == TRUE
 *           AND ( DirectlyConnected(S) == TRUE
 *                 OR RPF_interface(S) != RPF_interface(RP(G))
 *                 OR inherited_olist(S,G,rpt) == NULL
 *                 OR ( ( RPF'(S,G) == RPF'(*,G) ) AND
 *                      ( RPF'(S,G) != NULL ) )
 *                 OR ( I_Am_Assert_Loser(S,G,iif) ) {
 *        Set SPTbit(S,G) to TRUE
 *     }
 *   }
 */
void
pim_upstream_set_sptbit (struct pim_upstream *up, struct interface *incoming)
{
  struct pim_rpf *grpf = NULL;

  // iif == RPF_interfvace(S)
  if (up->rpf.source_nexthop.interface != incoming)
    {
      if (PIM_DEBUG_TRACE)
	zlog_debug ("%s: Incoming Interface: %s is different than RPF_interface(S) %s",
		    __PRETTY_FUNCTION__, incoming->name, up->rpf.source_nexthop.interface->name);
      return;
    }

  // AND JoinDesired(S,G) == TRUE
  // FIXME

  // DirectlyConnected(S) == TRUE
  if (pim_if_connected_to_source (up->rpf.source_nexthop.interface, up->sg.src))
    {
      if (PIM_DEBUG_TRACE)
	zlog_debug ("%s: %s is directly connected to the source", __PRETTY_FUNCTION__,
		    up->sg_str);
      up->sptbit = PIM_UPSTREAM_SPTBIT_TRUE;
      return;
     }

  // OR RPF_interface(S) != RPF_interface(RP(G))
  grpf = RP(up->sg.grp);
  if (!grpf || up->rpf.source_nexthop.interface != grpf->source_nexthop.interface)
    {
      if (PIM_DEBUG_TRACE)
	zlog_debug ("%s: %s RPF_interface(S) != RPF_interface(RP(G))",
		    __PRETTY_FUNCTION__, up->sg_str);
      up->sptbit = PIM_UPSTREAM_SPTBIT_TRUE;
      return;
    }

  // OR inherited_olist(S,G,rpt) == NULL
  if (pim_upstream_is_sg_rpt(up) && pim_upstream_empty_inherited_olist(up))
    {
      if (PIM_DEBUG_TRACE)
	zlog_debug ("%s: %s OR inherited_olist(S,G,rpt) == NULL", __PRETTY_FUNCTION__,
		    up->sg_str);
      up->sptbit = PIM_UPSTREAM_SPTBIT_TRUE;
      return;
    }

  // OR ( ( RPF'(S,G) == RPF'(*,G) ) AND
  //      ( RPF'(S,G) != NULL ) )
  if (up->parent && pim_rpf_is_same (&up->rpf, &up->parent->rpf))
    {
      if (PIM_DEBUG_TRACE)
	zlog_debug ("%s: %s RPF'(S,G) is the same as RPF'(*,G)", __PRETTY_FUNCTION__,
		    up->sg_str);
      up->sptbit = PIM_UPSTREAM_SPTBIT_TRUE;
      return;
    }

  return;
}

const char *
pim_upstream_state2str (enum pim_upstream_state join_state)
{
  switch (join_state)
    {
    case PIM_UPSTREAM_NOTJOINED:
      return "NotJoined";
      break;
    case PIM_UPSTREAM_JOINED:
      return "Joined";
      break;
    }
  return "Unknown";
}

const char *
pim_reg_state2str (enum pim_reg_state reg_state, char *state_str)
{
  switch (reg_state)
    {
    case PIM_REG_NOINFO:
      strcpy (state_str, "RegNoInfo");
      break;
    case PIM_REG_JOIN:
      strcpy (state_str, "RegJoined");
      break;
    case PIM_REG_JOIN_PENDING:
      strcpy (state_str, "RegJoinPend");
      break;
    case PIM_REG_PRUNE:
      strcpy (state_str, "RegPrune");
      break;
    default:
      strcpy (state_str, "RegUnknown");
    }
  return state_str;
}

static int
pim_upstream_register_stop_timer (struct thread *t)
{
  struct pim_interface *pim_ifp;
  struct pim_upstream *up;
  struct pim_rpf *rpg;
  struct ip ip_hdr;
  up = THREAD_ARG (t);

  up->t_rs_timer = NULL;

  if (PIM_DEBUG_TRACE)
    {
      char state_str[PIM_REG_STATE_STR_LEN];
      zlog_debug ("%s: (S,G)=%s upstream register stop timer %s",
		  __PRETTY_FUNCTION__, up->sg_str,
                  pim_reg_state2str(up->reg_state, state_str));
    }

  switch (up->reg_state)
    {
    case PIM_REG_JOIN_PENDING:
      up->reg_state = PIM_REG_JOIN;
      pim_channel_add_oif (up->channel_oil, pim_regiface, PIM_OIF_FLAG_PROTO_PIM);
      break;
    case PIM_REG_JOIN:
      break;
    case PIM_REG_PRUNE:
      pim_ifp = up->rpf.source_nexthop.interface->info;
      if (!pim_ifp)
        {
         if (PIM_DEBUG_TRACE)
           zlog_debug ("%s: Interface: %s is not configured for pim",
                       __PRETTY_FUNCTION__, up->rpf.source_nexthop.interface->name);
         return 0;
       }
      up->reg_state = PIM_REG_JOIN_PENDING;
      pim_upstream_start_register_stop_timer (up, 1);

      if (((up->channel_oil->cc.lastused/100) > PIM_KEEPALIVE_PERIOD) &&
	  (I_am_RP (up->sg.grp)))
	{
	  if (PIM_DEBUG_TRACE)
	    zlog_debug ("%s: Stop sending the register, because I am the RP and we haven't seen a packet in a while", __PRETTY_FUNCTION__);
	  return 0;
	}
      rpg = RP (up->sg.grp);
      memset (&ip_hdr, 0, sizeof (struct ip));
      ip_hdr.ip_p = PIM_IP_PROTO_PIM;
      ip_hdr.ip_hl = 5;
      ip_hdr.ip_v = 4;
      ip_hdr.ip_src = up->sg.src;
      ip_hdr.ip_dst = up->sg.grp;
      ip_hdr.ip_len = htons (20);
      // checksum is broken
      pim_register_send ((uint8_t *)&ip_hdr, sizeof (struct ip),
			 pim_ifp->primary_address, rpg, 1, up);
      break;
    default:
      break;
    }

  return 0;
}

void
pim_upstream_start_register_stop_timer (struct pim_upstream *up, int null_register)
{
  uint32_t time;

  if (up->t_rs_timer)
    {
      THREAD_TIMER_OFF (up->t_rs_timer);
      up->t_rs_timer = NULL;
    }

  if (!null_register)
    {
      uint32_t lower = (0.5 * PIM_REGISTER_SUPPRESSION_PERIOD);
      uint32_t upper = (1.5 * PIM_REGISTER_SUPPRESSION_PERIOD);
      time = lower + (random () % (upper - lower + 1)) - PIM_REGISTER_PROBE_PERIOD;
    }
  else
    time = PIM_REGISTER_PROBE_PERIOD;

  if (PIM_DEBUG_TRACE)
    {
      zlog_debug ("%s: (S,G)=%s Starting upstream register stop timer %d",
		  __PRETTY_FUNCTION__, up->sg_str, time);
    }
  THREAD_TIMER_ON (master, up->t_rs_timer,
		   pim_upstream_register_stop_timer,
		   up, time);
}

int
pim_upstream_inherited_olist_decide (struct pim_upstream *up)
{
  struct pim_interface *pim_ifp;
  struct listnode *chnextnode;
  struct pim_ifchannel *ch;
  struct listnode *chnode;
  int output_intf = 0;

  pim_ifp = up->rpf.source_nexthop.interface->info;
  if (pim_ifp && !up->channel_oil)
    up->channel_oil = pim_channel_oil_add (&up->sg, pim_ifp->mroute_vif_index);

  for (ALL_LIST_ELEMENTS (pim_ifchannel_list, chnode, chnextnode, ch))
    {
      pim_ifp = ch->interface->info;
      if (!pim_ifp)
	continue;

      if (pim_upstream_evaluate_join_desired_interface (up, ch))
	{
          int flag = PIM_OIF_FLAG_PROTO_PIM;

          if (ch->sg.src.s_addr == INADDR_ANY && ch->upstream != up)
            flag = PIM_OIF_FLAG_PROTO_STAR;
          pim_channel_add_oif (up->channel_oil, ch->interface, flag);
	  output_intf++;
	}
    }

  return output_intf;
}

/*
 * For a given upstream, determine the inherited_olist
 * and apply it.
 *
 * inherited_olist(S,G,rpt) =
 *           ( joins(*,*,RP(G)) (+) joins(*,G) (-) prunes(S,G,rpt) )
 *      (+) ( pim_include(*,G) (-) pim_exclude(S,G))
 *      (-) ( lost_assert(*,G) (+) lost_assert(S,G,rpt) )
 *
 *  inherited_olist(S,G) =
 *      inherited_olist(S,G,rpt) (+)
 *      joins(S,G) (+) pim_include(S,G) (-) lost_assert(S,G)
 *
 * return 1 if there are any output interfaces
 * return 0 if there are not any output interfaces
 */
int
pim_upstream_inherited_olist (struct pim_upstream *up)
{
  int output_intf =  pim_upstream_inherited_olist_decide (up);

  /*
   * If we have output_intf switch state to Join and work like normal
   * If we don't have an output_intf that means we are probably a
   * switch on a stick so turn on forwarding to just accept the
   * incoming packets so we don't bother the other stuff!
   */
  if (output_intf)
    pim_upstream_switch (up, PIM_UPSTREAM_JOINED);
  else
    forward_on (up);

  return output_intf;
}

int
pim_upstream_empty_inherited_olist (struct pim_upstream *up)
{
  return pim_channel_oil_empty (up->channel_oil);
}

/*
 * When we have a new neighbor,
 * find upstreams that don't have their rpf_addr
 * set and see if the new neighbor allows
 * the join to be sent
 */
void
pim_upstream_find_new_rpf (void)
{
  struct listnode     *up_node;
  struct listnode     *up_nextnode;
  struct pim_upstream *up;

  /*
   * Scan all (S,G) upstreams searching for RPF'(S,G)=neigh_addr
   */
  for (ALL_LIST_ELEMENTS(pim_upstream_list, up_node, up_nextnode, up))
    {
      if (pim_rpf_addr_is_inaddr_any(&up->rpf))
	{
	  if (PIM_DEBUG_TRACE)
	    zlog_debug ("Upstream %s without a path to send join, checking",
			up->sg_str);
	  pim_rpf_update (up, NULL, 1);
	}
    }
}

static unsigned int
pim_upstream_hash_key (void *arg)
{
  struct pim_upstream *up = (struct pim_upstream *)arg;

  return jhash_2words (up->sg.src.s_addr, up->sg.grp.s_addr, 0);
}

void pim_upstream_terminate (void)
{
  if (pim_upstream_list)
    list_delete (pim_upstream_list);
  pim_upstream_list = NULL;

  if (pim_upstream_hash)
    hash_free (pim_upstream_hash);
  pim_upstream_hash = NULL;
}

static int
pim_upstream_equal (const void *arg1, const void *arg2)
{
  const struct pim_upstream *up1 = (const struct pim_upstream *)arg1;
  const struct pim_upstream *up2 = (const struct pim_upstream *)arg2;

  if ((up1->sg.grp.s_addr == up2->sg.grp.s_addr) &&
      (up1->sg.src.s_addr == up2->sg.src.s_addr))
    return 1;

  return 0;
}

/* rfc4601:section-4.2:"Data Packet Forwarding Rules" defines
 * the cases where kat has to be restarted on rxing traffic -
 *
 * if( DirectlyConnected(S) == TRUE AND iif == RPF_interface(S) ) {
 * set KeepaliveTimer(S,G) to Keepalive_Period
 * # Note: a register state transition or UpstreamJPState(S,G)
 * # transition may happen as a result of restarting
 * # KeepaliveTimer, and must be dealt with here.
 * }
 * if( iif == RPF_interface(S) AND UpstreamJPState(S,G) == Joined AND
 * inherited_olist(S,G) != NULL ) {
 * set KeepaliveTimer(S,G) to Keepalive_Period
 * }
 */
static bool pim_upstream_kat_start_ok(struct pim_upstream *up)
{
  /* "iif == RPF_interface(S)" check has to be done by the kernel or hw
   * so we will skip that here */
  if (pim_if_connected_to_source(up->rpf.source_nexthop.interface,
        up->sg.src)) {
    return true;
  }

  if ((up->join_state == PIM_UPSTREAM_JOINED) &&
            !pim_upstream_empty_inherited_olist(up)) {
    /* XXX: I have added this RP check just for 3.2 and it's a digression from
     * what rfc-4601 says. Till now we were only running KAT on FHR and RP and
     * there is some angst around making the change to run it all routers that
     * maintain the (S, G) state. This is tracked via CM-13601 and MUST be
     * removed to handle spt turn-arounds correctly in a 3-tier clos */
    if (I_am_RP (up->sg.grp))
      return true;
  }

  return false;
}

/*
 * Code to check and see if we've received packets on a S,G mroute
 * and if so to set the SPT bit appropriately
 */
static void
pim_upstream_sg_running (void *arg)
{
  struct pim_upstream *up = (struct pim_upstream *)arg;

  // No packet can have arrived here if this is the case
  if (!up->channel_oil || !up->channel_oil->installed)
    {
      if (PIM_DEBUG_TRACE)
	zlog_debug ("%s: %s is not installed in mroute",
		    __PRETTY_FUNCTION__, up->sg_str);
      return;
    }

  /*
   * This is a bit of a hack
   * We've noted that we should rescan but
   * we've missed the window for doing so in
   * pim_zebra.c for some reason.  I am
   * only doing this at this point in time
   * to get us up and working for the moment
   */
  if (up->channel_oil->oil_inherited_rescan)
    {
      if (PIM_DEBUG_TRACE)
        zlog_debug ("%s: Handling unscanned inherited_olist for %s", __PRETTY_FUNCTION__, up->sg_str);
      pim_upstream_inherited_olist_decide (up);
      up->channel_oil->oil_inherited_rescan = 0;
    }
  pim_mroute_update_counters (up->channel_oil);

  // Have we seen packets?
  if ((up->channel_oil->cc.oldpktcnt >= up->channel_oil->cc.pktcnt) &&
      (up->channel_oil->cc.lastused/100 > 30))
    {
      if (PIM_DEBUG_TRACE)
	{
	  zlog_debug ("%s: %s old packet count is equal or lastused is greater than 30, (%ld,%ld,%lld)",
		      __PRETTY_FUNCTION__, up->sg_str,
		      up->channel_oil->cc.oldpktcnt,
		      up->channel_oil->cc.pktcnt,
		      up->channel_oil->cc.lastused/100);
	}
      return;
    }

  if (pim_upstream_kat_start_ok(up)) {
    /* Add a source reference to the stream if
     * one doesn't already exist */
    if (!PIM_UPSTREAM_FLAG_TEST_SRC_STREAM(up->flags))
    {
      if (PIM_DEBUG_TRACE)
        zlog_debug ("source reference created on kat restart %s", up->sg_str);

      pim_upstream_ref(up, PIM_UPSTREAM_FLAG_MASK_SRC_STREAM);
      PIM_UPSTREAM_FLAG_SET_SRC_STREAM(up->flags);
      pim_upstream_fhr_kat_start(up);
    }
    pim_upstream_keep_alive_timer_start(up, qpim_keep_alive_time);
  }

  if (up->sptbit != PIM_UPSTREAM_SPTBIT_TRUE)
  {
    pim_upstream_set_sptbit(up, up->rpf.source_nexthop.interface);
  }
  return;
}

void
pim_upstream_init (void)
{
  pim_upstream_sg_wheel = wheel_init (master, 31000, 100,
				      pim_upstream_hash_key,
				      pim_upstream_sg_running);
  pim_upstream_hash = hash_create_size (8192, pim_upstream_hash_key,
					pim_upstream_equal);

  pim_upstream_list = list_new ();
  pim_upstream_list->del = (void (*)(void *)) pim_upstream_free;
  pim_upstream_list->cmp = pim_upstream_compare;

}
