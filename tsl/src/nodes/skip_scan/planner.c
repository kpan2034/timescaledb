/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#include <postgres.h>
#include <access/sysattr.h>
#include <nodes/extensible.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/pathnodes.h>
#include <optimizer/clauses.h>
#include <optimizer/cost.h>
#include <optimizer/optimizer.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <optimizer/planmain.h>
#include <optimizer/prep.h>
#include <optimizer/restrictinfo.h>
#include <optimizer/tlist.h>
#include <parser/parse_coerce.h>
#include <parser/parsetree.h>
#include <rewrite/rewriteManip.h>
#include <utils/syscache.h>
#include <utils/typcache.h>

#include "compat/compat.h"
#include "guc.h"
#include "nodes/chunk_append/chunk_append.h"
#include "nodes/constraint_aware_append/constraint_aware_append.h"
#include "nodes/skip_scan/skip_scan.h"
#include <import/planner.h>

#include <math.h>

typedef struct SkipScanPath
{
	CustomPath cpath;
	IndexPath *index_path;

	/* Index clause which we'll use to skip past elements we've already seen */
	RestrictInfo *skip_clause;
	/* attribute number of the distinct column on the table/chunk */
	AttrNumber distinct_attno;
	/* The column offset on the index we are calling DISTINCT on */
	AttrNumber scankey_attno;
	int distinct_typ_len;
	bool distinct_by_val;
	/* Var referencing the distinct column on the relation */
	Var *distinct_var;
} SkipScanPath;

typedef struct DistinctPathInfo
{
	UpperRelationKind stage; /* What kind of Upper distinct path we are dealing with */
	Path *unique_path;		 /* If not NULL, valid Upper distinct path */
	Expr *distinct_expr;	 /* If not NULL, valid distinct expression for Upper distinct path */
} DistinctPathInfo;

static int get_idx_key(IndexOptInfo *idxinfo, AttrNumber attno);
static List *sort_indexquals(IndexOptInfo *indexinfo, List *quals);
static OpExpr *fix_indexqual(IndexOptInfo *index, RestrictInfo *rinfo, AttrNumber scankey_attno);
static bool build_skip_qual(PlannerInfo *root, SkipScanPath *skip_scan_path, IndexPath *index_path,
							Var *var);
static List *build_subpath(PlannerInfo *root, List *subpaths, DistinctPathInfo *dpinfo,
						   List *top_pathkeys);
static Var *get_distinct_var(PlannerInfo *root, DistinctPathInfo *dpinfo, IndexPath *index_path,
							 SkipScanPath *skip_scan_path);
static TargetEntry *tlist_member_match_var(Var *var, List *targetlist);

/**************************
 * SkipScan Plan Creation *
 **************************/

static CustomScanMethods skip_scan_plan_methods = {
	.CustomName = "SkipScan",
	.CreateCustomScanState = tsl_skip_scan_state_create,
};

void
_skip_scan_init(void)
{
	TryRegisterCustomScanMethods(&skip_scan_plan_methods);
}

static Plan *
skip_scan_plan_create(PlannerInfo *root, RelOptInfo *relopt, CustomPath *best_path, List *tlist,
					  List *clauses, List *custom_plans)
{
	SkipScanPath *path = (SkipScanPath *) best_path;
	CustomScan *skip_plan = makeNode(CustomScan);
	IndexPath *index_path = path->index_path;

	OpExpr *op = fix_indexqual(index_path->indexinfo, path->skip_clause, path->scankey_attno);

	Plan *plan = linitial(custom_plans);
	if (IsA(plan, IndexScan))
	{
		IndexScan *idx_plan = castNode(IndexScan, plan);
		skip_plan->scan = idx_plan->scan;

		/* we prepend skip qual here so sort_indexquals will put it as first qual for that column */
		idx_plan->indexqual =
			sort_indexquals(index_path->indexinfo, lcons(op, idx_plan->indexqual));
	}
	else if (IsA(plan, IndexOnlyScan))
	{
		IndexOnlyScan *idx_plan = castNode(IndexOnlyScan, plan);
		skip_plan->scan = idx_plan->scan;
		/* we prepend skip qual here so sort_indexquals will put it as first qual for that column */
		idx_plan->indexqual =
			sort_indexquals(index_path->indexinfo, lcons(op, idx_plan->indexqual));
	}
	else
		elog(ERROR, "unsupported subplan type for SkipScan: %s", ts_get_node_name((Node *) plan));

	skip_plan->scan.plan.targetlist = tlist;
	skip_plan->custom_scan_tlist = list_copy(tlist);
	skip_plan->scan.plan.qual = NIL;
	skip_plan->scan.plan.type = T_CustomScan;
	skip_plan->methods = &skip_scan_plan_methods;
	skip_plan->custom_plans = custom_plans;
	/* get position of skipped column in tuples produced by child scan */
	TargetEntry *tle = tlist_member_match_var(path->distinct_var, plan->targetlist);

	bool nulls_first = index_path->indexinfo->nulls_first[path->scankey_attno - 1];
	if (index_path->indexscandir == BackwardScanDirection)
		nulls_first = !nulls_first;

	skip_plan->custom_private = list_make5_int(tle->resno,
											   path->distinct_by_val,
											   path->distinct_typ_len,
											   nulls_first,
											   path->scankey_attno);
	return &skip_plan->scan.plan;
}

/*************************
 * SkipScanPath Creation *
 *************************/
static CustomPathMethods skip_scan_path_methods = {
	.CustomName = "SkipScanPath",
	.PlanCustomPath = skip_scan_plan_create,
};

#if PG16_GE
typedef struct FindAggrefsContext
{
	List *aggrefs; /* all non-nested Aggrefs found in a node */
} FindAggrefsContext;

static bool
find_aggrefs_walker(Node *node, FindAggrefsContext *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		context->aggrefs = lappend(context->aggrefs, node);
		/* don't recurse inside Aggrefs */
		return false;
	}

	return expression_tree_walker(node, find_aggrefs_walker, context);
}
#endif
/* We can get upper path Distinct expression once for upper path,
 * rather than repeat this check for each child path of an upper path input
 */
static Expr *
get_upper_distinct_expr(PlannerInfo *root, UpperRelationKind stage)
{
	ListCell *lc;
	int num_vars = 0;
	Expr *tlexpr = NULL;

	if (stage == UPPERREL_DISTINCT && root->parse->distinctClause)
	{
		foreach (lc, root->parse->distinctClause)
		{
			SortGroupClause *clause = lfirst_node(SortGroupClause, lc);
			Node *expr = get_sortgroupclause_expr(clause, root->parse->targetList);

			/* we ignore any columns that can be constified to allow for cases like DISTINCT 'abc',
			 * column */
			if (IsA(estimate_expression_value(root, expr), Const))
				continue;

			num_vars++;
			if (num_vars > 1)
				return NULL;

			/* We ignore binary-compatible relabeling */
			tlexpr = (Expr *) expr;
			while (tlexpr && IsA(tlexpr, RelabelType))
				tlexpr = ((RelabelType *) tlexpr)->arg;
		}
	}
#if PG16_GE
	else if (stage == UPPERREL_GROUP_AGG)
	{
		/* Find all non-nested Aggrefs in the query target list */
		FindAggrefsContext agg_ctx = { NULL };
		find_aggrefs_walker((Node *) root->parse->targetList, &agg_ctx);

		foreach (lc, agg_ctx.aggrefs)
		{
			Aggref *agg = lfirst_node(Aggref, lc);
			/* Only distinct aggs with 1 sorted argument are eligible*/
			if (agg->aggdistinct && agg->aggpresorted && list_length(agg->args) == 1)
			{
				TargetEntry *tle = (TargetEntry *) linitial(agg->args);

				Expr *expr = tle->expr;
				/* We ignore binary-compatible relabeling */
				while (expr && IsA(expr, RelabelType))
					expr = ((RelabelType *) expr)->arg;

				/* Distinct agg over a Const is OK */
				if (IsA(estimate_expression_value(root, (Node *) expr), Const))
					continue;

				/* Don't support no-var arguments */
				if (!IsA(expr, Var))
					return NULL;

				/* Don't support multiple distinct aggs over different columns */
				if (tlexpr && !tlist_member_match_var((Var *) tlexpr, agg->args))
					return NULL;

				/* If Distinct agg path has a groupby column, it needs to match Distinct agg column
				 */
				if (root->processed_groupClause)
				{
					/* Should have bailed out on gby exprs > 1 earlier */
					Assert(list_length(root->processed_groupClause) == 1);
					SortGroupClause *sortcl =
						(SortGroupClause *) linitial(root->processed_groupClause);
					Expr *gbykey = (Expr *) get_sortgroupclause_expr(sortcl, root->processed_tlist);
					if (!equal(gbykey, expr))
						return NULL;
				}
				/* Found a valid distinct agg over a valid Var */
				if (!tlexpr)
				{
					tlexpr = expr;
					num_vars = 1;
				}
			}
			else
			{
				return NULL;
			}
		}
	}
#endif
	if (num_vars != 1)
		return NULL;

	/* SkipScan on expressions not supported */
	if (!tlexpr || !IsA(tlexpr, Var))
		return NULL;

	return tlexpr;
}

static void
obtain_upper_distinct_path(PlannerInfo *root, RelOptInfo *output_rel, DistinctPathInfo *dpinfo)
{
	ListCell *lc;

	/*
	 * look for Unique Path so we dont have to repeat some of
	 * the calculations done by postgres and can also assume
	 * that the DISTINCT clause is eligible for sort based
	 * DISTINCT
	 */
	if (dpinfo->stage == UPPERREL_DISTINCT)
	{
		if (!ts_guc_enable_skip_scan)
			return;

		foreach (lc, output_rel->pathlist)
		{
			if (IsA(lfirst(lc), UpperUniquePath))
			{
				UpperUniquePath *unique = (UpperUniquePath *) lfirst_node(UpperUniquePath, lc);

				/* currently we do not handle DISTINCT on more than one key. To do so,
				 * we would need to break down the SkipScan into subproblems: first
				 * find the minimal tuple then for each prefix find all unique suffix
				 * tuples. For instance, if we are searching over (int, int), we would
				 * first find (0, 0) then find (0, N) for all N in the domain, then
				 * find (1, N), then (2, N), etc
				 */
				if (unique->numkeys > 1)
					return;

				dpinfo->unique_path = (Path *) unique;
				break;
			}
		}
	}
	/* Sorted inputs for Distinct aggs weren't supported until PG16 */
#if PG16_GE
	/* Look for Aggpath with eligible Distinct aggregates */
	else if (dpinfo->stage == UPPERREL_GROUP_AGG)
	{
		if (!ts_guc_enable_skip_scan_for_distinct_aggregates)
			return;

		/* Cannot apply SkipScan to more than one key */
		if (list_length(root->group_pathkeys) > 1)
			return;

		foreach (lc, output_rel->pathlist)
		{
			if (IsA(lfirst(lc), AggPath))
			{
				AggPath *unique = (AggPath *) lfirst_node(AggPath, lc);

				/* If Distinct agg path has a group key, it must match Distinct aggregate input sort
				 * key, otherwise cannot apply SkipScan
				 */
				if (unique->path.pathkeys &&
					!pathkeys_contained_in(unique->path.pathkeys, unique->subpath->pathkeys))
				{
					return;
				}

				dpinfo->unique_path = (Path *) lfirst_node(AggPath, lc);
				break;
			}
		}
	}
#endif
	else
		return;

	if (!dpinfo->unique_path)
		return;

	/* Check if we have valid distinct expression to source from the underlying index */
	dpinfo->distinct_expr = get_upper_distinct_expr(root, dpinfo->stage);
	if (!dpinfo->distinct_expr)
	{
		dpinfo->unique_path = NULL;
		return;
	}

	/* Need to make a copy of the unique path here because add_path() in the
	 * pathlist loop below might prune it if the new unique path
	 * (SkipScanPath) dominates the old one. When the unique path is pruned,
	 * the pointer will no longer be valid in the next iteration of the
	 * pathlist loop. Fortunately, the Path object is not deeply freed, so a
	 * shallow copy is enough. */
	if (dpinfo->stage == UPPERREL_DISTINCT)
	{
		UpperUniquePath *unique = makeNode(UpperUniquePath);
		memcpy(unique, lfirst_node(UpperUniquePath, lc), sizeof(UpperUniquePath));
		dpinfo->unique_path = (Path *) unique;
	}
	else if (dpinfo->stage == UPPERREL_GROUP_AGG)
	{
		AggPath *dist_agg_path = makeNode(AggPath);
		memcpy(dist_agg_path, lfirst_node(AggPath, lc), sizeof(AggPath));
		dpinfo->unique_path = (Path *) dist_agg_path;
	}
}

static SkipScanPath *skip_scan_path_create(PlannerInfo *root, IndexPath *index_path,
										   DistinctPathInfo *dpinfo);

/*
 * Create SkipScan paths based on existing Unique paths.
 * For a Unique path on a simple relation like the following
 *
 *  Unique
 *    ->  Index Scan using skip_scan_dev_name_idx on skip_scan
 *
 * a SkipScan path like this will be created:
 *
 *  Unique
 *    ->  Custom Scan (SkipScan) on skip_scan
 *          ->  Index Scan using skip_scan_dev_name_idx on skip_scan
 *
 * For a Unique path on a hypertable with multiple chunks like the following
 *
 *  Unique
 *    ->  Merge Append
 *          Sort Key: _hyper_2_1_chunk.dev_name
 *          ->  Index Scan using _hyper_2_1_chunk_idx on _hyper_2_1_chunk
 *          ->  Index Scan using _hyper_2_2_chunk_idx on _hyper_2_2_chunk
 *
 * a SkipScan path like this will be created:
 *
 *  Unique
 *    ->  Merge Append
 *          Sort Key: _hyper_2_1_chunk.dev_name
 *          ->  Custom Scan (SkipScan) on _hyper_2_1_chunk
 *                ->  Index Scan using _hyper_2_1_chunk_idx on _hyper_2_1_chunk
 *          ->  Custom Scan (SkipScan) on _hyper_2_2_chunk
 *                ->  Index Scan using _hyper_2_2_chunk_idx on _hyper_2_2_chunk
 */
void
tsl_skip_scan_paths_add(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
						UpperRelationKind stage)
{
	DistinctPathInfo dpinfo = { stage, NULL, NULL };
	obtain_upper_distinct_path(root, output_rel, &dpinfo);
	if (!dpinfo.unique_path)
		return;

	Assert(IsA(dpinfo.unique_path, UpperUniquePath) || IsA(dpinfo.unique_path, AggPath));
	ListCell *lc;
	foreach (lc, input_rel->pathlist)
	{
		bool has_caa = false;

		Path *subpath = lfirst(lc);

		List *top_pathkeys = NULL;

		/* Unique path has to be sorted on at least DISTINCT ON key */
		if (IsA(dpinfo.unique_path, UpperUniquePath))
		{
			if (!pathkeys_contained_in(dpinfo.unique_path->pathkeys, subpath->pathkeys))
				continue;
		}
		/* AggPath with distinct aggs may not be sorted, but the input into distinct aggs needs to
		 * be sorted */
		else if (IsA(dpinfo.unique_path, AggPath))
		{
			if (!subpath->pathkeys ||
				!pathkeys_contained_in(dpinfo.unique_path->pathkeys, subpath->pathkeys))
				continue;
			/* Need to check sortedness for inputs of Distinct aggs, so we'll keep track of the
			 * input pathkeys  */
			top_pathkeys = subpath->pathkeys;
		}

		/* If path is a ProjectionPath we strip it off for processing
		 * but also add a ProjectionPath on top of the SKipScanPaths
		 * later.
		 */
		ProjectionPath *proj = NULL;
		if (IsA(subpath, ProjectionPath))
		{
			proj = castNode(ProjectionPath, subpath);
			subpath = proj->subpath;
		}

		/* Path might be wrapped in a ConstraintAwareAppendPath if this
		 * is a MergeAppend that could benefit from runtime exclusion.
		 * We treat this similar to ProjectionPath and add it back
		 * later
		 */
		if (ts_is_constraint_aware_append_path(subpath))
		{
			subpath = linitial(castNode(CustomPath, subpath)->custom_paths);
			Assert(IsA(subpath, MergeAppendPath));
			has_caa = true;
		}

		if (IsA(subpath, IndexPath))
		{
			IndexPath *index_path = castNode(IndexPath, subpath);

			subpath = (Path *) skip_scan_path_create(root, index_path, &dpinfo);
			if (!subpath)
				continue;
		}
		else if (IsA(subpath, MergeAppendPath))
		{
			MergeAppendPath *merge_path = castNode(MergeAppendPath, subpath);

			List *new_paths = build_subpath(root, merge_path->subpaths, &dpinfo, top_pathkeys);

			/* build_subpath returns NULL when no SkipScanPath was created */
			if (!new_paths)
				continue;

			subpath = (Path *) create_merge_append_path(root,
														merge_path->path.parent,
														new_paths,
														merge_path->path.pathkeys,
														NULL);
			subpath->pathtarget = copy_pathtarget(merge_path->path.pathtarget);
		}
		/* We may have Append over one input which will be removed from the plan later.
		 * Consider it when it is sorted correctly. #7778
		 */
		else if (IsA(subpath, AppendPath))
		{
			AppendPath *append_path = castNode(AppendPath, subpath);

			if (list_length(append_path->subpaths) > 1)
				continue;

			List *new_paths = build_subpath(root, append_path->subpaths, &dpinfo, top_pathkeys);

			/* build_subpath returns NULL when no SkipScanPath was created */
			if (!new_paths)
				continue;

			subpath = (Path *) create_append_path(root,
												  append_path->path.parent,
												  new_paths,
												  NULL,
												  append_path->path.pathkeys,
												  NULL,
												  append_path->path.parallel_workers,
												  append_path->path.parallel_aware,
												  -1);
			subpath->pathtarget = copy_pathtarget(append_path->path.pathtarget);
		}
		else if (ts_is_chunk_append_path(subpath))
		{
			ChunkAppendPath *ca = (ChunkAppendPath *) subpath;
			List *new_paths = build_subpath(root, ca->cpath.custom_paths, &dpinfo, top_pathkeys);
			/* ChunkAppend should never be wrapped in ConstraintAwareAppendPath */
			Assert(!has_caa);

			/* build_subpath returns NULL when no SkipScanPath was created */
			if (!new_paths)
				continue;

			/* We copy the existing ChunkAppendPath here because we don't have all the
			 * information used for creating the original one and we don't want to
			 * duplicate all the checks done when creating the original one.
			 */
			subpath = (Path *) ts_chunk_append_path_copy(ca, new_paths, ca->cpath.path.pathtarget);
		}
		else
		{
			continue;
		}

		/* add ConstraintAwareAppendPath if the original path had one */
		if (has_caa)
			subpath = ts_constraint_aware_append_path_create(root, subpath);

		Path *new_unique = NULL;

		if (IsA(dpinfo.unique_path, UpperUniquePath))
		{
			UpperUniquePath *unique = (UpperUniquePath *) dpinfo.unique_path;
			new_unique = (Path *) create_upper_unique_path(root,
														   output_rel,
														   subpath,
														   unique->numkeys,
														   unique->path.rows);
			new_unique->pathtarget = unique->path.pathtarget;

			if (proj)
				new_unique =
					(Path *) create_projection_path(root,
													output_rel,
													new_unique,
													copy_pathtarget(new_unique->pathtarget));
		}
		else if (IsA(dpinfo.unique_path, AggPath))
		{
			AggPath *dist_agg_path = (AggPath *) dpinfo.unique_path;
			if (proj)
			{
				proj->subpath = subpath;
				subpath = (Path *) proj;
			}
			/* Is there a better way to cost new AggPath w/o recreating AggClauseCosts from the new
			 * input? */
			AggClauseCosts agg_costs;
			MemSet(&agg_costs, 0, sizeof(AggClauseCosts));
			get_agg_clause_costs(root, dist_agg_path->aggsplit, &agg_costs);
			new_unique = (Path *) create_agg_path(root,
												  output_rel,
												  subpath,
												  dist_agg_path->path.pathtarget,
												  dist_agg_path->aggstrategy,
												  dist_agg_path->aggsplit,
												  dist_agg_path->groupClause,
												  dist_agg_path->qual,
												  (const AggClauseCosts *) &agg_costs,
												  dist_agg_path->numGroups);
		}

		add_path(output_rel, new_unique);
	}
}

static SkipScanPath *
skip_scan_path_create(PlannerInfo *root, IndexPath *index_path, DistinctPathInfo *dpinfo)
{
	double startup = index_path->path.startup_cost;
	double total = index_path->path.total_cost;
	double rows = index_path->path.rows;

	/* cannot use SkipScan with non-orderable index or IndexPath without pathkeys */
	if (!index_path->path.pathkeys || !index_path->indexinfo->sortopfamily)
		return NULL;

	/* orderbyops are not compatible with skipscan */
	if (index_path->indexorderbys != NIL)
		return NULL;

	SkipScanPath *skip_scan_path = (SkipScanPath *) newNode(sizeof(SkipScanPath), T_CustomPath);
	int ndistinct = dpinfo->unique_path->rows;
	skip_scan_path->cpath.path.pathtype = T_CustomScan;
	skip_scan_path->cpath.path.pathkeys = index_path->path.pathkeys;
	skip_scan_path->cpath.path.pathtarget = index_path->path.pathtarget;
	skip_scan_path->cpath.path.param_info = index_path->path.param_info;
	skip_scan_path->cpath.path.parent = index_path->path.parent;
	skip_scan_path->cpath.path.rows = ndistinct;
	skip_scan_path->cpath.custom_paths = list_make1(index_path);
	skip_scan_path->cpath.methods = &skip_scan_path_methods;

	/* We calculate SkipScan cost as ndistinct * startup_cost + (ndistinct/rows) * total_cost
	 * ndistinct * startup_cost is to account for the rescans we have to do and since startup
	 * cost for indexes does not include page access cost we add a fraction of the total cost
	 * accounting for the number of rows we expect to fetch.
	 * If the row estimate for the scan is 1 we assume that the estimate got clamped to 1
	 * and no rows would be returned by this scan and this chunk will most likely be excluded
	 * by runtime exclusion. Otherwise the cost for this path would be highly inflated due
	 * to (ndistinct / rows) * total leading to SkipScan not being chosen for queries on
	 * hypertables with a lot of excluded chunks.
	 */
	skip_scan_path->cpath.path.startup_cost = startup;
	if (rows > 1)
		skip_scan_path->cpath.path.total_cost = ndistinct * startup + (ndistinct / rows) * total;
	else
		skip_scan_path->cpath.path.total_cost = startup;

	/* While add_path may pfree paths with higher costs
	 * it will never free IndexPaths and only ever do a shallow
	 * free so reusing the IndexPath here is safe. */
	skip_scan_path->index_path = index_path;

	Var *var = get_distinct_var(root, dpinfo, index_path, skip_scan_path);

	if (!var)
		return NULL;

	skip_scan_path->distinct_var = var;

	/* build skip qual this may fail if we cannot look up the operator */
	if (!build_skip_qual(root, skip_scan_path, index_path, var))
		return NULL;

	return skip_scan_path;
}

/* Extract the Var to use for the SkipScan and do attno mapping if required. */
static Var *
get_distinct_var(PlannerInfo *root, DistinctPathInfo *dpinfo, IndexPath *index_path,
				 SkipScanPath *skip_scan_path)
{
	RelOptInfo *rel = index_path->path.parent;
	Expr *tlexpr = dpinfo->distinct_expr;

	if (!tlexpr || !IsA(tlexpr, Var))
		return NULL;

	Var *var = castNode(Var, tlexpr);

	/* If we are dealing with a hypertable Var extracted from distinctClause will point to
	 * the parent hypertable while the IndexPath will be on a Chunk.
	 * For a normal table they point to the same relation and we are done here. */
	if ((Index) var->varno == rel->relid)
		return var;

	RangeTblEntry *ht_rte = planner_rt_fetch(var->varno, root);
	RangeTblEntry *chunk_rte = planner_rt_fetch(rel->relid, root);

	/* Check for hypertable */
	if (!ts_is_hypertable(ht_rte->relid) || !bms_is_member(var->varno, rel->top_parent_relids))
		return NULL;

	Relation ht_rel = table_open(ht_rte->relid, AccessShareLock);
	Relation chunk_rel = table_open(chunk_rte->relid, AccessShareLock);
	bool found_wholerow;
	TupleConversionMap *map =
		convert_tuples_by_name(RelationGetDescr(chunk_rel), RelationGetDescr(ht_rel));

	/* attno mapping necessary */
	if (map)
	{
		var = (Var *) map_variable_attnos((Node *) var,
										  var->varno,
										  0,
										  map->attrMap,
										  InvalidOid,
										  &found_wholerow);

		free_conversion_map(map);

		/* If we found whole row here skipscan wouldn't be applicable
		 * but this should have been caught already in previous checks */
		Assert(!found_wholerow);
		if (found_wholerow)
		{
			table_close(ht_rel, NoLock);
			table_close(chunk_rel, NoLock);

			return NULL;
		}
	}
	else
	{
		var = copyObject(var);
	}

	table_close(ht_rel, NoLock);
	table_close(chunk_rel, NoLock);

	var->varno = rel->relid;

	return var;
}

/*
 * Creates SkipScanPath for each path of subpaths that is an IndexPath
 * If no subpath can be changed to SkipScanPath returns NULL
 * otherwise returns list of new paths
 */
static List *
build_subpath(PlannerInfo *root, List *subpaths, DistinctPathInfo *dpinfo, List *top_pathkeys)
{
	bool has_skip_path = false;
	List *new_paths = NIL;
	ListCell *lc;

	foreach (lc, subpaths)
	{
		Path *child = lfirst(lc);
		if (IsA(child, IndexPath))
		{
			if (top_pathkeys &&
				!pathkeys_contained_in(top_pathkeys, castNode(IndexPath, child)->path.pathkeys))
				continue;

			SkipScanPath *skip_path =
				skip_scan_path_create(root, castNode(IndexPath, child), dpinfo);

			if (skip_path)
			{
				child = (Path *) skip_path;
				has_skip_path = true;
			}
		}

		new_paths = lappend(new_paths, child);
	}

	if (!has_skip_path && new_paths)
	{
		pfree(new_paths);
		return NIL;
	}

	return new_paths;
}

static bool
build_skip_qual(PlannerInfo *root, SkipScanPath *skip_scan_path, IndexPath *index_path, Var *var)
{
	IndexOptInfo *info = index_path->indexinfo;
	Oid column_type = exprType((Node *) var);
	Oid column_collation = get_typcollation(column_type);
	TypeCacheEntry *tce = lookup_type_cache(column_type, 0);
	bool need_coerce = false;

	/*
	 * Skipscan is not applicable for the following case:
	 * We might have a path with an index that produces the correct pathkeys for the target ordering
	 * without actually including all the columns of the ORDER BY. If the path uses an index that
	 * does not include the distinct column, we cannot use it for skipscan and have to discard this
	 * path from skipscan generation. This happens, for instance, when we have an order by clause
	 * (like ORDER BY a, b) with constraints in the WHERE clause (like WHERE a = <constant>) . "a"
	 * can now be removed from the Pathkeys (since it is a constant) and the query can be satisfied
	 * by using an index on just column "b".
	 *
	 * Example query:
	 * SELECT DISTINCT ON (a) * FROM test WHERE a in (2) ORDER BY a ASC, time DESC;
	 * Since a is always 2 due to the WHERE clause we can create the correct ordering for the
	 * ORDER BY with an index that does not include the a column and only includes the time column.
	 */
	int idx_key = get_idx_key(info, var->varattno);
	if (idx_key < 0)
		return false;

	skip_scan_path->distinct_attno = var->varattno;
	skip_scan_path->distinct_by_val = tce->typbyval;
	skip_scan_path->distinct_typ_len = tce->typlen;
	/* sk_attno of the skip qual */
	skip_scan_path->scankey_attno = idx_key + 1;

	int16 strategy = info->reverse_sort[idx_key] ? BTLessStrategyNumber : BTGreaterStrategyNumber;
	if (index_path->indexscandir == BackwardScanDirection)
	{
		strategy =
			(strategy == BTLessStrategyNumber) ? BTGreaterStrategyNumber : BTLessStrategyNumber;
	}
	Oid opcintype = info->opcintype[idx_key];

	Oid comparator =
		get_opfamily_member(info->sortopfamily[idx_key], column_type, column_type, strategy);

	/* If there is no exact operator match for the column type we have here check
	 * if we can coerce to the type of the operator class. */
	if (!OidIsValid(comparator))
	{
		if (IsBinaryCoercible(column_type, opcintype))
		{
			comparator =
				get_opfamily_member(info->sortopfamily[idx_key], opcintype, opcintype, strategy);
			if (!OidIsValid(comparator))
				return false;
			need_coerce = true;
		}
		else
			return false; /* cannot use this index */
	}

	Const *prev_val = makeNullConst(need_coerce ? opcintype : column_type, -1, column_collation);
	Expr *current_val = (Expr *) makeVar(info->rel->relid /*varno*/,
										 var->varattno /*varattno*/,
										 column_type /*vartype*/,
										 -1 /*vartypmod*/,
										 column_collation /*varcollid*/,
										 0 /*varlevelsup*/);

	if (need_coerce)
	{
		CoerceViaIO *coerce = makeNode(CoerceViaIO);
		coerce->arg = current_val;
		coerce->resulttype = opcintype;
		coerce->resultcollid = column_collation;
		coerce->coerceformat = COERCE_IMPLICIT_CAST;
		coerce->location = -1;

		current_val = &coerce->xpr;
	}

	Expr *comparison_expr = make_opclause(comparator,
										  BOOLOID /*opresulttype*/,
										  false /*opretset*/,
										  current_val /*leftop*/,
										  &prev_val->xpr /*rightop*/,
										  InvalidOid /*opcollid*/,
										  info->indexcollations[idx_key] /*inputcollid*/);
	set_opfuncid(castNode(OpExpr, comparison_expr));

	skip_scan_path->skip_clause = make_simple_restrictinfo(root, comparison_expr);

	return true;
}

static int
get_idx_key(IndexOptInfo *idxinfo, AttrNumber attno)
{
	for (int i = 0; i < idxinfo->nkeycolumns; i++)
	{
		if (attno == idxinfo->indexkeys[i])
			return i;
	}
	return -1;
}

/* Sort quals according to index column order.
 * ScanKeys need to be sorted by the position of the index column
 * they are referencing but since we don't want to adjust actual
 * ScanKey array we presort qual list when creating plan.
 */
static List *
sort_indexquals(IndexOptInfo *indexinfo, List *quals)
{
	List *indexclauses[INDEX_MAX_KEYS] = { 0 };
	List *ordered_list = NIL;
	int quals_len = list_length(quals);
	ListCell *lc;
	int i;

	foreach (lc, quals)
	{
		Bitmapset *bms = NULL;
		pull_varattnos(lfirst(lc), INDEX_VAR, &bms);
		Assert(bms_num_members(bms) >= 1);

		i = bms_next_member(bms, -1) + FirstLowInvalidHeapAttributeNumber - 1;
		indexclauses[i] = lappend(indexclauses[i], lfirst(lc));
	}

	for (i = 0; i < quals_len; i++)
	{
		if (indexclauses[i] != NIL)
			ordered_list = list_concat(ordered_list, indexclauses[i]);
	}

	return ordered_list;
}

static OpExpr *
fix_indexqual(IndexOptInfo *index, RestrictInfo *rinfo, AttrNumber scankey_attno)
{
	/* technically our placeholder col > NULL is unsatisfiable, and in some instances
	 * the planner will realize this and use is as an excuse to remove other quals.
	 * in order to prevent this, we prepare this qual ourselves.
	 */

	/* fix_indexqual_references */
	OpExpr *op = copyObject(castNode(OpExpr, rinfo->clause));
	Assert(list_length(op->args) == 2);
	Assert(bms_equal(rinfo->left_relids, index->rel->relids));

	/* fix_indexqual_operand */
	Assert(index->indexkeys[scankey_attno - 1] != 0);
	Var *node = linitial_node(Var, pull_var_clause(linitial(op->args), 0));

	Assert((Index) ((Var *) node)->varno == index->rel->relid &&
		   ((Var *) node)->varattno == index->indexkeys[scankey_attno - 1]);

	Var *result = (Var *) copyObject(node);
	result->varno = INDEX_VAR;
	result->varattno = scankey_attno;

	linitial(op->args) = result;

	return op;
}

/*
 * tlist_member_match_var
 *    Same as tlist_member, except that we match the provided Var on the basis
 *    of varno/varattno/varlevelsup/vartype only, rather than full equal().
 *
 * This is needed in some cases where we can't be sure of an exact typmod
 * match.  For safety, though, we insist on vartype match.
 *
 * static function copied from src/backend/optimizer/util/tlist.c
 */
static TargetEntry *
tlist_member_match_var(Var *var, List *targetlist)
{
	ListCell *temp;

	foreach (temp, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(temp);
		Var *tlvar = (Var *) tlentry->expr;

		if (!tlvar || !IsA(tlvar, Var))
			continue;
		if (var->varno == tlvar->varno && var->varattno == tlvar->varattno &&
			var->varlevelsup == tlvar->varlevelsup && var->vartype == tlvar->vartype)
			return tlentry;
	}
	return NULL;
}
