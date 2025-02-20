/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "lib/filters/filter.h"
#include "lib/commands/toolcontext.h"

struct rfilter {
	struct dm_pool *mem;
	dm_bitset_t accept;
	struct dm_regex *engine;
	unsigned config_filter:1;
	unsigned config_global_filter:1;
	unsigned warned_filter:1;
	unsigned warned_global_filter:1;
};

static int _extract_pattern(struct dm_pool *mem, const char *pat,
			    char **regex, dm_bitset_t accept, int ix)
{
	char sep, *r, *ptr;

	/*
	 * is this an accept or reject pattern
	 */
	switch (*pat) {
	case 'a':
		dm_bit_set(accept, ix);
		break;

	case 'r':
		dm_bit_clear(accept, ix);
		break;

	default:
		log_error("Pattern must begin with 'a' or 'r'.");
		return 0;
	}
	pat++;

	/*
	 * get the separator
	 */
	switch (*pat) {
	case '(':
		sep = ')';
		break;

	case '[':
		sep = ']';
		break;

	case '{':
		sep = '}';
		break;

	default:
		sep = *pat;
	}
	pat++;

	/*
	 * copy the regex
	 */
	if (!(r = dm_pool_strdup(mem, pat)))
		return_0;

	/*
	 * trim the trailing character, having checked it's sep.
	 */
	ptr = r + strlen(r) - 1;
	if (*ptr != sep) {
		log_error("Invalid separator at end of regex.");
		return 0;
	}
	*ptr = '\0';

	regex[ix] = r;
	return 1;
}

static int _build_matcher(struct rfilter *rf, const struct dm_config_value *val)
{
	struct dm_pool *scratch;
	const struct dm_config_value *v;
	char **regex;
	unsigned count = 0;
	int i, r = 0;

	if (!(scratch = dm_pool_create("filter dm_regex", 1024)))
		return_0;

	/*
	 * count how many patterns we have.
	 */
	for (v = val; v; v = v->next) {
		if (v->type != DM_CFG_STRING) {
			log_error("Filter patterns must be enclosed in quotes.");
			goto out;
		}

		count++;
	}

	/* Allocate space for them */
	if (!(regex = dm_pool_alloc(scratch, sizeof(*regex) * count))) {
		log_error("Failed to allocate regex.");
		goto out;
	}

	/* Create the accept/reject bitset */
	if (!(rf->accept = dm_bitset_create(rf->mem, count))) {
		log_error("Failed to create bitset.");
		goto out;
	}

	/*
	 * fill the array back to front because we
	 * want the opposite precedence to what
	 * the matcher gives.
	 */
	for (v = val, i = count - 1; v; v = v->next, i--)
		if (!_extract_pattern(scratch, v->v.str, regex, rf->accept, i)) {
			log_error("Invalid filter pattern \"%s\".", v->v.str);
			goto out;
		}

	/*
	 * build the matcher.
	 */
	if (!(rf->engine = dm_regex_create(rf->mem, (const char * const*) regex,
					   count)))
		goto_out;
	r = 1;

      out:
	dm_pool_destroy(scratch);
	return r;
}

static int _accept_p(struct cmd_context *cmd, struct dev_filter *f, struct device *dev, const char *use_filter_name)
{
	int m, first = 1, rejected = 0;
	struct rfilter *rf = (struct rfilter *) f->private;
	struct dm_str_list *sl;

	dev->filtered_flags &= ~DEV_FILTERED_REGEX;

	if (cmd->enable_devices_list)
		return 1;

	if (cmd->filter_regex_skip)
		return 1;

	if (cmd->enable_devices_file && !cmd->filter_regex_with_devices_file) {
		/* can't warn in create_filter because enable_devices_file is set later */
		if (rf->config_filter && !rf->warned_filter) {
			log_warn("Please remove the lvm.conf filter, it is ignored with the devices file.");
			rf->warned_filter = 1;
		}
		if (rf->config_global_filter && !rf->warned_global_filter) {
			log_warn("Please remove the lvm.conf global_filter, it is ignored with the devices file.");
			rf->warned_global_filter = 1;
		}
		return 1;
	}

	dm_list_iterate_items(sl, &dev->aliases) {
		m = dm_regex_match(rf->engine, sl->str);

		if (m >= 0) {
			if (dm_bit(rf->accept, m)) {
				if (!first && !cmd->filter_regex_set_preferred_name_disable)
					dev_set_preferred_name(sl, dev);

				return 1;
			}

			rejected = 1;
		}

		first = 0;
	}

	if (rejected) {
		dev->filtered_flags |= DEV_FILTERED_REGEX;
		log_debug_devs("%s: Skipping (regex)", dev_name(dev));
	}

	/*
	 * pass everything that doesn't match
	 * anything.
	 */
	return !rejected;
}

static void _regex_destroy(struct dev_filter *f)
{
	struct rfilter *rf = (struct rfilter *) f->private;

	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying regex filter while in use %u times.", f->use_count);

	dm_pool_destroy(rf->mem);
}

struct dev_filter *regex_filter_create(const struct dm_config_value *patterns, int config_filter, int config_global_filter)
{
	struct dm_pool *mem = dm_pool_create("filter regex", 10 * 1024);
	struct rfilter *rf;
	struct dev_filter *f;

	if (!mem)
		return_NULL;

	if (!(rf = dm_pool_alloc(mem, sizeof(*rf))))
		goto_bad;

	rf->mem = mem;

	rf->config_filter = config_filter;
	rf->config_global_filter = config_global_filter;

	if (!_build_matcher(rf, patterns))
		goto_bad;

	if (!(f = dm_pool_zalloc(mem, sizeof(*f))))
		goto_bad;

	f->passes_filter = _accept_p;
	f->destroy = _regex_destroy;
	f->use_count = 0;
	f->private = rf;
	f->name = "regex";

	log_debug_devs("Regex filter initialised.");

	return f;

      bad:
	dm_pool_destroy(mem);
	return NULL;
}

static int _filter_contains_symlink(struct cmd_context *cmd, int filter_cfg)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	const char *fname;
		
	if ((cn = find_config_tree_array(cmd, filter_cfg, NULL))) {
		for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING)
				continue;
			if (!cv->v.str)
				continue;
			
			fname = cv->v.str;
		
			if (fname[0] != 'a')
				continue;
	
			if (strstr(fname, "/dev/disk/"))
				return 1;
			if (strstr(fname, "/dev/mapper/"))
				return 1;
		
			/* In case /dev/disk/by was omitted */
			if (strstr(fname, "lvm-pv-uuid"))
				return 1;
			if (strstr(fname, "dm-uuid"))
				return 1;
			if (strstr(fname, "wwn-"))
				return 1;
			if (strstr(fname, "pci-"))
				return 1;
		}
	}

	return 0;
}

int regex_filter_contains_symlink(struct cmd_context *cmd)
{
	return _filter_contains_symlink(cmd, devices_filter_CFG) ||
	       _filter_contains_symlink(cmd, devices_global_filter_CFG);
}

