/*
 * Distributed under the terms of the GNU GPL version 2.
 * Copyright (c) 2007, 2008 The Board of Trustees of The Leland 
 * Stanford Junior University
 */

#include "table.h"
#include "flow.h"
#include "datapath.h"

#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/list.h>

struct sw_table_linear {
	struct sw_table swt;

	spinlock_t lock;
	unsigned int max_flows;
	atomic_t n_flows;
	struct list_head flows;
	struct list_head iter_flows;
	unsigned long int next_serial;
};

static struct sw_flow *table_linear_lookup(struct sw_table *swt,
					 const struct sw_flow_key *key)
{
	struct sw_table_linear *tl = (struct sw_table_linear *) swt;
	struct sw_flow *flow;
	list_for_each_entry_rcu (flow, &tl->flows, node) {
		if (flow_matches(&flow->key, key))
			return flow;
	}
	return NULL;
}

static int table_linear_insert(struct sw_table *swt, struct sw_flow *flow)
{
	struct sw_table_linear *tl = (struct sw_table_linear *) swt;
	unsigned long int flags;
	struct sw_flow *f;


	/* Loop through the existing list of entries.  New entries will
	 * always be placed behind those with equal priority.  Just replace 
	 * any flows that match exactly.
	 */
	spin_lock_irqsave(&tl->lock, flags);
	list_for_each_entry_rcu (f, &tl->flows, node) {
		if (f->priority == flow->priority
				&& f->key.wildcards == flow->key.wildcards
				&& flow_matches(&f->key, &flow->key)
				&& flow_del(f)) {
			flow->serial = f->serial;
			list_replace_rcu(&f->node, &flow->node);
			list_replace_rcu(&f->iter_node, &flow->iter_node);
			spin_unlock_irqrestore(&tl->lock, flags);
			flow_deferred_free(f);
			return 1;
		}

		if (f->priority < flow->priority)
			break;
	}

	/* Make sure there's room in the table. */
	if (atomic_read(&tl->n_flows) >= tl->max_flows) {
		spin_unlock_irqrestore(&tl->lock, flags);
		return 0;
	}
	atomic_inc(&tl->n_flows);

	/* Insert the entry immediately in front of where we're pointing. */
	list_add_tail_rcu(&flow->node, &f->node);
	list_add_rcu(&flow->iter_node, &tl->iter_flows);
	spin_unlock_irqrestore(&tl->lock, flags);
	return 1;
}

static int do_delete(struct sw_table *swt, struct sw_flow *flow) 
{
	if (flow_del(flow)) {
		list_del_rcu(&flow->node);
		list_del_rcu(&flow->iter_node);
		flow_deferred_free(flow);
		return 1;
	}
	return 0;
}

static int table_linear_delete(struct sw_table *swt,
				const struct sw_flow_key *key, uint16_t priority, int strict)
{
	struct sw_table_linear *tl = (struct sw_table_linear *) swt;
	struct list_head *pos, *n;
	unsigned int count = 0;

	list_for_each_safe_rcu (pos, n, &tl->flows) {
		struct sw_flow *flow = list_entry(pos, struct sw_flow, node);
		if (flow_del_matches(&flow->key, key, strict)
				&& (strict && (flow->priority == priority)))
			count += do_delete(swt, flow);
	}
	if (count)
		atomic_sub(count, &tl->n_flows);
	return count;
}

static int table_linear_timeout(struct datapath *dp, struct sw_table *swt)
{
	struct sw_table_linear *tl = (struct sw_table_linear *) swt;
	struct list_head *pos, *n;
	int count = 0;

	list_for_each_safe_rcu (pos, n, &tl->flows) {
		struct sw_flow *flow = list_entry(pos, struct sw_flow, node);
		if (flow_timeout(flow)) {
			count += do_delete(swt, flow);
			if (dp->flags & OFPC_SEND_FLOW_EXP)
				dp_send_flow_expired(dp, flow);
		}
	}
	if (count)
		atomic_sub(count, &tl->n_flows);
	return count;
}

static void table_linear_destroy(struct sw_table *swt)
{
	struct sw_table_linear *tl = (struct sw_table_linear *) swt;

	while (!list_empty(&tl->flows)) {
		struct sw_flow *flow = list_entry(tl->flows.next,
						  struct sw_flow, node);
		list_del(&flow->node);
		flow_free(flow);
	}
	kfree(tl);
}

static int table_linear_iterate(struct sw_table *swt,
				const struct sw_flow_key *key,
				struct sw_table_position *position,
				int (*callback)(struct sw_flow *, void *),
				void *private)
{
	struct sw_table_linear *tl = (struct sw_table_linear *) swt;
	struct sw_flow *flow;
	unsigned long start;

	start = ~position->private[0];
	list_for_each_entry_rcu (flow, &tl->iter_flows, iter_node) {
		if (flow->serial <= start && flow_matches(key, &flow->key)) {
			int error = callback(flow, private);
			if (error) {
				position->private[0] = ~(flow->serial - 1);
				return error;
			}
		}
	}
	return 0;
}

static void table_linear_stats(struct sw_table *swt,
				struct sw_table_stats *stats)
{
	struct sw_table_linear *tl = (struct sw_table_linear *) swt;
	stats->name = "linear";
	stats->n_flows = atomic_read(&tl->n_flows);
	stats->max_flows = tl->max_flows;
}


struct sw_table *table_linear_create(unsigned int max_flows)
{
	struct sw_table_linear *tl;
	struct sw_table *swt;

	tl = kzalloc(sizeof *tl, GFP_KERNEL);
	if (tl == NULL)
		return NULL;

	swt = &tl->swt;
	swt->lookup = table_linear_lookup;
	swt->insert = table_linear_insert;
	swt->delete = table_linear_delete;
	swt->timeout = table_linear_timeout;
	swt->destroy = table_linear_destroy;
	swt->iterate = table_linear_iterate;
	swt->stats = table_linear_stats;

	tl->max_flows = max_flows;
	atomic_set(&tl->n_flows, 0);
	INIT_LIST_HEAD(&tl->flows);
	INIT_LIST_HEAD(&tl->iter_flows);
	spin_lock_init(&tl->lock);
	tl->next_serial = 0;

	return swt;
}
