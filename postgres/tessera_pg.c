#include "postgres.h"

#include <limits.h>

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_inherits.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "common/file_utils.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "funcapi.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rls.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "tessera_c.h"

PG_MODULE_MAGIC;

void _PG_init(void);

static bool tessera_enabled = true;
static int tessera_threads = 0;

static int
query_threads(int requested)
{
	return requested > 0 ? requested : max_parallel_workers_per_gather + 1;
}
static int tessera_frontier = -1;
static bool tessera_verbose = false;
static create_upper_paths_hook_type prev_upper_hook = NULL;

enum {
	P_RELID, P_KATT, P_XATT, P_VATT, P_KTYPE, P_XTYPE, P_VTYPE, P_MODE, P_COL0_SMALLER, P_AGGTYPE, P_MINVAL,
	P_CROSS, P_RELID2, P_KATT2, P_XATT2, P_VATT2, P_KTYPE2, P_XTYPE2, P_VTYPE2, P_NPARAM
};

typedef struct TesseraMatch {
	int64 p[P_NPARAM];
	Aggref *agg;
	int va;
} TesseraMatch;

static Var *as_var(Node *n) {
	while (n && IsA(n, RelabelType)) {
		n = (Node *)((RelabelType *)n)->arg;
	}
	if (n && IsA(n, Var) && ((Var *)n)->varlevelsup == 0 && ((Var *)n)->varattno > 0) {
		return (Var *)n;
	}
	return NULL;
}

static bool att_not_null(Oid relid, AttrNumber att) {
	Relation r = table_open(relid, NoLock);
	TupleDesc d = RelationGetDescr(r);
	bool nn = att > 0 && att <= d->natts && !TupleDescAttr(d, att - 1)->attisdropped &&
	          TupleDescAttr(d, att - 1)->attgenerated != ATTRIBUTE_GENERATED_VIRTUAL &&
	          TupleDescCompactAttr(d, att - 1)->attnullability == ATTNULLABLE_VALID;
	table_close(r, NoLock);
	return nn;
}

static const char *catalog_opname(Oid opno) {
	HeapTuple tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (!HeapTupleIsValid(tup)) {
		return NULL;
	}
	Form_pg_operator f = (Form_pg_operator)GETSTRUCT(tup);
	const char *name = f->oprnamespace == PG_CATALOG_NAMESPACE ? pstrdup(NameStr(f->oprname)) : NULL;
	ReleaseSysCache(tup);
	return name;
}

static bool int_type(Oid t) {
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

static bool str_type(Oid t) {
	return t == TEXTOID || t == VARCHAROID;
}

static bool key_ok(Oid t, Oid coll) {
	return int_type(t) || (str_type(t) && OidIsValid(coll) && get_collation_isdeterministic(coll));
}

static bool binop_cross(Node *n, int vx, int vy, const char **op, Var **x, Var **y, Oid *coll) {
	if (!IsA(n, OpExpr) || list_length(((OpExpr *)n)->args) != 2) {
		return false;
	}
	OpExpr *o = (OpExpr *)n;
	Var *l = as_var(linitial(o->args)), *r = as_var(lsecond(o->args));
	if (!l || !r || !((l->varno == vx && r->varno == vy) || (l->varno == vy && r->varno == vx))) {
		return false;
	}
	*op = catalog_opname(o->opno);
	*x = l->varno == vx ? l : r;
	*y = l->varno == vx ? r : l;
	*coll = o->inputcollid;
	return *op != NULL;
}

static bool binop_vars(Node *n, int va, int vb, const char **op, Var **left, Var **right, Oid *coll) {
	if (!IsA(n, OpExpr) || list_length(((OpExpr *)n)->args) != 2) {
		return false;
	}
	OpExpr *o = (OpExpr *)n;
	Var *l = as_var(linitial(o->args)), *r = as_var(lsecond(o->args));
	if (!l || !r || l->varattno != r->varattno || !((l->varno == va && r->varno == vb) || (l->varno == vb && r->varno == va))) {
		return false;
	}
	*op = catalog_opname(o->opno);
	*left = l;
	*right = r;
	*coll = o->inputcollid;
	return *op != NULL;
}

static bool match_cross(PlannerInfo *root, TesseraMatch *m, Var *g0, Var *g1);
static bool match_agg(PlannerInfo *root, TesseraMatch *m, int va, int vb, Var *g0, Var *g1);

static bool join_inputs(Query *parse, int *va, int *vb, List **quals) {
	List *from = parse->jointree->fromlist;
	*quals = (List *)parse->jointree->quals;
	if (list_length(from) == 2 && IsA(linitial(from), RangeTblRef) && IsA(lsecond(from), RangeTblRef)) {
		*va = linitial_node(RangeTblRef, from)->rtindex;
		*vb = lsecond_node(RangeTblRef, from)->rtindex;
		return true;
	}
	if (list_length(from) == 1 && IsA(linitial(from), JoinExpr)) {
		JoinExpr *j = linitial_node(JoinExpr, from);
		if (j->jointype != JOIN_INNER || !IsA(j->larg, RangeTblRef) || !IsA(j->rarg, RangeTblRef)) {
			return false;
		}
		*va = ((RangeTblRef *)j->larg)->rtindex;
		*vb = ((RangeTblRef *)j->rarg)->rtindex;
		*quals = list_concat_copy((List *)j->quals, *quals);
		return true;
	}
	return false;
}

static bool match_query(PlannerInfo *root, TesseraMatch *m) {
	Query *parse = root->parse;
	int va, vb;
	List *quals;
	if (parse->commandType != CMD_SELECT || parse->setOperations || parse->groupingSets || parse->hasWindowFuncs ||
	    parse->hasTargetSRFs || parse->distinctClause || parse->hasSubLinks || list_length(parse->groupClause) != 2 ||
	    !join_inputs(parse, &va, &vb, &quals)) {
		return false;
	}
	RangeTblEntry *ra = rt_fetch(va, parse->rtable), *rb = rt_fetch(vb, parse->rtable);
	if (ra->rtekind != RTE_RELATION || rb->rtekind != RTE_RELATION || ra->relkind != RELKIND_RELATION ||
	    rb->relkind != RELKIND_RELATION || ra->tablesample || rb->tablesample || has_subclass(ra->relid) ||
	    has_subclass(rb->relid)) {
		return false;
	}
	if (parse->hasRowSecurity || ra->securityQuals || rb->securityQuals ||
	    check_enable_rls(ra->relid, InvalidOid, true) != RLS_NONE ||
	    check_enable_rls(rb->relid, InvalidOid, true) != RLS_NONE) {
		return false;
	}
	memset(m, 0, sizeof *m);
	m->p[P_RELID] = ra->relid;
	m->va = va;
	Var *g0 = as_var((Node *)get_sortgroupclause_expr(linitial_node(SortGroupClause, parse->groupClause), parse->targetList));
	Var *g1 = as_var((Node *)get_sortgroupclause_expr(lsecond_node(SortGroupClause, parse->groupClause), parse->targetList));
	if (!g0 || !g1 || g0->varno == g1->varno ||
	    !((g0->varno == va || g0->varno == vb) && (g1->varno == va || g1->varno == vb))) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		Node *e = (Node *)get_sortgroupclause_expr(list_nth_node(SortGroupClause, parse->groupClause, i), parse->targetList);
		Oid c = exprCollation(e);
		if (str_type(exprType(e)) && !(OidIsValid(c) && get_collation_isdeterministic(c))) {
			return false;
		}
	}
	if (ra->relid != rb->relid || g0->varattno != g1->varattno) {
		return match_cross(root, m, g0, g1);
	}
	if (!key_ok(g0->vartype, g0->varcollid)) {
		return false;
	}
	m->p[P_XATT] = g0->varattno;
	m->p[P_XTYPE] = g0->vartype;
	bool have_eq = false, have_cmp = false;
	ListCell *lc;
	foreach (lc, quals) {
		const char *op;
		Var *l, *r;
		Oid coll;
		if (!binop_vars((Node *)lfirst(lc), va, vb, &op, &l, &r, &coll)) {
			return false;
		}
		if (strcmp(op, "=") == 0 && !have_eq && l->varattno != m->p[P_XATT] && key_ok(l->vartype, coll)) {
			m->p[P_KATT] = l->varattno;
			m->p[P_KTYPE] = l->vartype;
			have_eq = true;
		} else if (!have_cmp && l->varattno == m->p[P_XATT] &&
		           (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) &&
		           (!str_type(l->vartype) || pg_newlocale_from_collation(coll)->collate_is_c)) {
			bool lt = op[0] == '<';
			m->p[P_MODE] = op[1] == '=' ? 2 : 1;
			int smaller = lt ? l->varno : r->varno;
			m->p[P_COL0_SMALLER] = smaller == va;
			have_cmp = true;
		} else {
			return false;
		}
	}
	if (!have_eq) {
		return false;
	}
	if (!att_not_null(ra->relid, (AttrNumber)m->p[P_XATT])) {
		return false;
	}
	return match_agg(root, m, va, vb, g0, g1);
}

static bool count_all(TesseraMatch *m, bool cross, Aggref *a) {
	if (list_length(a->args) != 1) {
		return false;
	}
	Node *e = (Node *)linitial_node(TargetEntry, a->args)->expr;
	if (IsA(e, Const)) {
		return !((Const *)e)->constisnull;
	}
	Var *v = as_var(e);
	if (!v || v->varlevelsup != 0) {
		return false;
	}
	Oid rel = cross && v->varno != m->va ? (Oid)m->p[P_RELID2] : (Oid)m->p[P_RELID];
	return att_not_null(rel, v->varattno);
}

static bool match_agg(PlannerInfo *root, TesseraMatch *m, int va, int vb, Var *g0, Var *g1) {
	Query *parse = root->parse;
	const bool cross = m->p[P_CROSS] != 0;
	ListCell *lc;
	foreach (lc, parse->targetList) {
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		if (te->resjunk) {
			continue;
		}
		if (as_var((Node *)te->expr) && (equal(as_var((Node *)te->expr), g0) || equal(as_var((Node *)te->expr), g1))) {
			continue;
		}
		if (!IsA(te->expr, Aggref) || m->agg) {
			return false;
		}
		Aggref *a = (Aggref *)te->expr;
		if (a->aggdistinct || a->aggorder || a->aggfilter || a->aggkind != AGGKIND_NORMAL || a->agglevelsup != 0) {
			return false;
		}
		char *fn = get_func_namespace(a->aggfnoid) == PG_CATALOG_NAMESPACE ? get_func_name(a->aggfnoid) : NULL;
		if (fn && strcmp(fn, "count") == 0 && (a->aggstar || count_all(m, cross, a))) {
			m->p[P_VATT] = 0;
		} else if (fn && strcmp(fn, "sum") == 0 && list_length(a->args) == 1) {
			const char *op;
			Var *l, *r;
			Oid coll;
			Node *e = (Node *)linitial_node(TargetEntry, a->args)->expr;
			if (cross) {
				if (!binop_cross(e, va, vb, &op, &l, &r, &coll) || strcmp(op, "*") != 0 ||
				    !(int_type(l->vartype) || l->vartype == FLOAT8OID || l->vartype == FLOAT4OID) ||
				    !(int_type(r->vartype) || r->vartype == FLOAT8OID || r->vartype == FLOAT4OID) ||
				    !att_not_null((Oid)m->p[P_RELID], l->varattno) || !att_not_null((Oid)m->p[P_RELID2], r->varattno)) {
					return false;
				}
				m->p[P_VATT2] = r->varattno;
				m->p[P_VTYPE2] = r->vartype;
			} else if (!binop_vars(e, va, vb, &op, &l, &r, &coll) || strcmp(op, "*") != 0 ||
			           l->varattno == m->p[P_KATT] ||
			           !(int_type(l->vartype) || l->vartype == FLOAT8OID || l->vartype == FLOAT4OID) ||
			           !att_not_null((Oid)m->p[P_RELID], l->varattno)) {
				return false;
			}
			m->p[P_VATT] = l->varattno;
			m->p[P_VTYPE] = l->vartype;
		} else {
			return false;
		}
		if (a->aggtype != INT8OID && a->aggtype != FLOAT8OID && a->aggtype != NUMERICOID) {
			return false;
		}
		m->p[P_AGGTYPE] = a->aggtype;
		m->agg = a;
	}
	foreach (lc, parse->targetList) {
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		if (!te->resjunk) {
			continue;
		}
		Var *v = as_var((Node *)te->expr);
		if (v && (equal(v, g0) || equal(v, g1))) {
			continue;
		}
		if (!m->agg || !IsA(te->expr, Aggref) || !equal(te->expr, m->agg)) {
			return false;
		}
	}
	List *having = (List *)parse->havingQual;
	if (having) {
		if (!m->agg || list_length(having) != 1 || !IsA(linitial(having), OpExpr)) {
			return false;
		}
		OpExpr *o = linitial_node(OpExpr, having);
		const char *op = catalog_opname(o->opno);
		Node *l = linitial(o->args), *r = lsecond(o->args);
		if (!op || !equal(l, m->agg) || !IsA(r, Const) || ((Const *)r)->constisnull || !int_type(((Const *)r)->consttype)) {
			return false;
		}
		Const *c = (Const *)r;
		int64 v = c->consttype == INT8OID ? DatumGetInt64(c->constvalue)
		          : c->consttype == INT4OID ? DatumGetInt32(c->constvalue)
		                                    : DatumGetInt16(c->constvalue);
		if (strcmp(op, ">=") == 0) {
			m->p[P_MINVAL] = v;
		} else if (strcmp(op, ">") == 0) {
			m->p[P_MINVAL] = v + 1;
		} else {
			return false;
		}
		if (m->p[P_MINVAL] < 0) {
			m->p[P_MINVAL] = 0;
		}
	}
	return true;
}

static bool match_cross(PlannerInfo *root, TesseraMatch *m, Var *g0, Var *g1) {
	Query *parse = root->parse;
	const int vx = g0->varno, vy = g1->varno;
	RangeTblEntry *rx = rt_fetch(vx, parse->rtable), *ry = rt_fetch(vy, parse->rtable);
	if (!key_ok(g0->vartype, g0->varcollid) || !key_ok(g1->vartype, g1->varcollid)) {
		return false;
	}
	m->va = vx;
	m->p[P_CROSS] = 1;
	m->p[P_RELID] = rx->relid;
	m->p[P_XATT] = g0->varattno;
	m->p[P_XTYPE] = g0->vartype;
	m->p[P_RELID2] = ry->relid;
	m->p[P_XATT2] = g1->varattno;
	m->p[P_XTYPE2] = g1->vartype;
	int ja, jb;
	List *quals;
	if (!join_inputs(parse, &ja, &jb, &quals)) {
		return false;
	}
	const char *op;
	Var *kx, *ky;
	Oid coll;
	if (list_length(quals) != 1 || !binop_cross((Node *)linitial(quals), vx, vy, &op, &kx, &ky, &coll) ||
	    strcmp(op, "=") != 0 || !key_ok(kx->vartype, coll) || !key_ok(ky->vartype, coll) ||
	    int_type(kx->vartype) != int_type(ky->vartype)) {
		return false;
	}
	m->p[P_KATT] = kx->varattno;
	m->p[P_KTYPE] = kx->vartype;
	m->p[P_KATT2] = ky->varattno;
	m->p[P_KTYPE2] = ky->vartype;
	if (!att_not_null(rx->relid, g0->varattno) || !att_not_null(ry->relid, g1->varattno)) {
		return false;
	}
	return match_agg(root, m, vx, vy, g0, g1);
}

static List *params_list(const int64 *p) {
	List *l = NIL;
	for (int i = 0; i < P_NPARAM; i++) {
		l = lappend(l, makeConst(INT8OID, -1, InvalidOid, 8, Int64GetDatum(p[i]), false, true));
	}
	return l;
}

static Plan *tessera_plan_path(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist,
                               List *clauses, List *custom_plans);
static Node *tessera_create_state(CustomScan *cscan);

static const CustomPathMethods tessera_path_methods = {
    .CustomName = "Tessera",
    .PlanCustomPath = tessera_plan_path,
};
static const CustomScanMethods tessera_scan_methods = {
    .CustomName = "Tessera",
    .CreateCustomScanState = tessera_create_state,
};

static void tessera_upper_hook(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel,
                               RelOptInfo *output_rel, void *extra) {
	if (prev_upper_hook) {
		prev_upper_hook(root, stage, input_rel, output_rel, extra);
	}
	TesseraMatch m;
	if (!tessera_enabled || stage != UPPERREL_GROUP_AGG || !tessera_c_supported() || !match_query(root, &m)) {
		return;
	}
	CustomPath *cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = output_rel->rows > 0 ? output_rel->rows : 1;
	cp->path.startup_cost = 0;
	cp->path.total_cost = 1;
	cp->path.pathkeys = NIL;
	cp->flags = 0;
	cp->custom_paths = NIL;
	cp->custom_private = list_make2(params_list(m.p), makeInteger(m.va));
	cp->methods = &tessera_path_methods;
	add_path(output_rel, &cp->path);
}

static Plan *tessera_plan_path(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist,
                               List *clauses, List *custom_plans) {
	CustomScan *cs = makeNode(CustomScan);
	List *params = linitial(best_path->custom_private);
	int va = intVal(lsecond(best_path->custom_private));
	List *kinds = NIL;
	ListCell *lc;
	foreach (lc, tlist) {
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		Var *v = as_var((Node *)te->expr);
		int kind = IsA(te->expr, Aggref) ? 2 : (v && v->varno == va) ? 0 : (v ? 1 : -1);
		if (kind < 0) {
			elog(ERROR, "tessera: unexpected expression in the grouping target");
		}
		kinds = lappend_int(kinds, kind);
	}
	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = (List *)copyObject(root->parse->havingQual);
	cs->scan.scanrelid = 0;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_exprs = NIL;
	cs->custom_private = list_make2(params, kinds);
	cs->custom_scan_tlist = (List *)copyObject(tlist);
	cs->custom_relids = NULL;
	cs->methods = &tessera_scan_methods;
	return &cs->scan.plan;
}

typedef struct TesseraState {
	CustomScanState css;
	int64 p[P_NPARAM];
	int *kind;
	int ncol;
	tessera_query_c *q;
	tessera_dict_c *dk, *dx, *dy;
	bool ran;
	bool wide;
	MemoryContextCallback release;
} TesseraState;

static void tessera_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *tessera_exec(CustomScanState *node);
static void tessera_end(CustomScanState *node);
static void tessera_rescan(CustomScanState *node);
static void tessera_explain(CustomScanState *node, List *ancestors, ExplainState *es);

static const CustomExecMethods tessera_exec_methods = {
    .CustomName = "Tessera",
    .BeginCustomScan = tessera_begin,
    .ExecCustomScan = tessera_exec,
    .EndCustomScan = tessera_end,
    .ReScanCustomScan = tessera_rescan,
    .ExplainCustomScan = tessera_explain,
};

static Node *tessera_create_state(CustomScan *cscan) {
	TesseraState *s = (TesseraState *)newNode(sizeof(TesseraState), T_CustomScanState);
	List *params = linitial(cscan->custom_private), *kinds = lsecond(cscan->custom_private);
	ListCell *lc;
	int i = 0;
	foreach (lc, params) {
		s->p[i++] = DatumGetInt64(lfirst_node(Const, lc)->constvalue);
	}
	s->ncol = 0;
	s->kind = palloc(sizeof(int) * Max(list_length(kinds), 1));
	foreach (lc, kinds) {
		s->kind[s->ncol++] = lfirst_int(lc);
	}
	s->css.methods = &tessera_exec_methods;
	return (Node *)s;
}

static void tessera_release(void *arg) {
	TesseraState *s = (TesseraState *)arg;
	if (s->q) {
		tessera_c_free(s->q);
		s->q = NULL;
	}
	if (s->dk) {
		tessera_dict_free(s->dk);
		s->dk = NULL;
	}
	if (s->dx) {
		tessera_dict_free(s->dx);
		s->dx = NULL;
	}
	if (s->dy) {
		tessera_dict_free(s->dy);
		s->dy = NULL;
	}
}

static void tessera_begin(CustomScanState *node, EState *estate, int eflags) {
	TesseraState *s = (TesseraState *)node;
	s->q = NULL;
	s->dk = s->dx = s->dy = NULL;
	s->ran = false;
	s->wide = false;
	s->release.func = tessera_release;
	s->release.arg = s;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, &s->release);
}

static int64 datum_key(Datum d, Oid t, tessera_dict_c *dict) {
	if (str_type(t)) {
		text *v = DatumGetTextPP(d);
		int64 c = tessera_dict_code(dict, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
		if (c < 0) {
			elog(ERROR, "tessera: out of memory");
		}
		return c;
	}
	return t == INT8OID ? DatumGetInt64(d) : t == INT4OID ? (int64)DatumGetInt32(d) : (int64)DatumGetInt16(d);
}

static int pg_should_stop(void *ctx) {
	return QueryCancelPending || ProcDiePending;
}

static int64 datum_int(Datum d, Oid t) {
	return t == INT8OID ? DatumGetInt64(d) : t == INT4OID ? (int64)DatumGetInt32(d) : (int64)DatumGetInt16(d);
}

static double datum_double(Datum d, Oid t) {
	return t == FLOAT8OID ? DatumGetFloat8(d) : t == FLOAT4OID ? (double)DatumGetFloat4(d) : (double)datum_int(d, t);
}

static void scan_rel(tessera_query_c *q, tessera_dict_c *dk, tessera_dict_c *dxs, Oid relid, AttrNumber ka,
                     AttrNumber xa, AttrNumber vatt, Oid kt, Oid xt, Oid vt, int side, bool int_v) {
	const bool has_v = vatt != 0;
	MemoryContext rowcxt = AllocSetContextCreate(CurrentMemoryContext, "tessera scan", ALLOCSET_DEFAULT_SIZES);
	Relation rel = table_open(relid, AccessShareLock);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	TableScanDesc sd = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
	enum { BATCH = 65536 };
	int64 *kb = palloc(sizeof(int64) * BATCH), *xb = palloc(sizeof(int64) * BATCH);
	double *vb = has_v && !int_v ? palloc(sizeof(double) * BATCH) : NULL;
	int64 *vib = has_v && int_v ? palloc(sizeof(int64) * BATCH) : NULL;
	int n = 0;
#define TESSERA_ADD()                                                                                                 \
	(int_v ? (side ? tessera_c_add_y_i64(q, kb, xb, (const int64_t *)vib, n) : tessera_c_add_i64(q, kb, xb, (const int64_t *)vib, n)) \
	       : (side ? tessera_c_add_y(q, kb, xb, vb, n) : tessera_c_add(q, kb, xb, vb, n)))
	while (table_scan_getnextslot(sd, ForwardScanDirection, slot)) {
		bool kn, xn, vn = false;
		Datum kd = slot_getattr(slot, ka, &kn), xd = slot_getattr(slot, xa, &xn), vd = 0;
		if (has_v) {
			vd = slot_getattr(slot, vatt, &vn);
		}
		if (kn) {
			continue;
		}
		if (xn || vn) {
			elog(ERROR, "tessera: NULL group keys or values");
		}
		MemoryContext old = MemoryContextSwitchTo(rowcxt);
		kb[n] = datum_key(kd, kt, dk);
		xb[n] = datum_key(xd, xt, dxs);
		MemoryContextSwitchTo(old);
		if (has_v && int_v) {
			vib[n] = datum_int(vd, vt);
		} else if (has_v) {
			vb[n] = datum_double(vd, vt);
		}
		if (++n == BATCH) {
			if (TESSERA_ADD() != 0) {
				elog(ERROR, "tessera: out of memory");
			}
			n = 0;
			MemoryContextReset(rowcxt);
		}
		CHECK_FOR_INTERRUPTS();
	}
	if (n && TESSERA_ADD() != 0) {
		elog(ERROR, "tessera: out of memory");
	}
#undef TESSERA_ADD
	table_endscan(sd);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, NoLock);
	pfree(kb);
	pfree(xb);
	if (vb) {
		pfree(vb);
	}
	if (vib) {
		pfree(vib);
	}
	MemoryContextDelete(rowcxt);
}

static void set_spill(tessera_query_c *q) {
	char dir[MAXPGPATH];
	PrepareTempTablespaces();
	TempTablespacePath(dir, GetNextTempTableSpace());
	(void)MakePGDirectory(dir);
	tessera_c_set_memory(q, get_hash_memory_limit(), dir);
	if (temp_file_limit >= 0) {
		tessera_c_set_spill_limit(q, (uint64_t)temp_file_limit * 1024);
	}
}

static void run_error(int rc, const char *err) {
	if (rc == TESSERA_C_CANCELLED) {
		CHECK_FOR_INTERRUPTS();
	}
	if (rc == TESSERA_C_OUT_OF_MEMORY) {
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory"), errdetail("%s", err)));
	}
	if (rc == TESSERA_C_SPILL_LIMIT) {
		ereport(ERROR, (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
		                errmsg("temporary file size exceeds \"temp_file_limit\" (%dkB)", temp_file_limit),
		                errdetail("%s", err)));
	}
	if (rc == TESSERA_C_SPILL_ERROR) {
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("%s", err)));
	}
	elog(ERROR, "%s", err);
}

static int int_bits(Oid t) {
	return t == INT2OID ? 16 : t == INT4OID ? 32 : 64;
}

static Datum numeric_of_i128(int64_t hi, uint64_t lo) {
	if (hi == ((lo >> 63) ? -1 : 0)) {
		return NumericGetDatum(int64_to_numeric((int64)lo));
	}
	__int128 v = (__int128)(((unsigned __int128)(uint64_t)hi << 64) | lo);
	char buf[48], *p = buf + sizeof buf - 1;
	const bool neg = v < 0;
	unsigned __int128 u = neg ? (unsigned __int128)0 - (unsigned __int128)v : (unsigned __int128)v;
	*p = 0;
	do {
		*--p = (char)('0' + (int)(u % 10));
		u /= 10;
	} while (u);
	if (neg) {
		*--p = '-';
	}
	return DirectFunctionCall3(numeric_in, CStringGetDatum(p), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

static void tessera_run(TesseraState *s) {
	const bool has_v = s->p[P_VATT] != 0, cross = s->p[P_CROSS] != 0;
	const Oid kt = (Oid)s->p[P_KTYPE], xt = (Oid)s->p[P_XTYPE], yt = (Oid)s->p[P_XTYPE2];
	s->q = cross ? tessera_c_new_cross(has_v) : tessera_c_new(has_v);
	if (!s->q || (str_type(kt) && !(s->dk = tessera_dict_new())) || (str_type(xt) && !(s->dx = tessera_dict_new())) ||
	    (cross && str_type(yt) && !(s->dy = tessera_dict_new()))) {
		elog(ERROR, "tessera: out of memory");
	}
	const bool int_v = has_v && int_type((Oid)s->p[P_VTYPE]) && (!cross || int_type((Oid)s->p[P_VTYPE2]));
	scan_rel(s->q, s->dk, s->dx, (Oid)s->p[P_RELID], (AttrNumber)s->p[P_KATT], (AttrNumber)s->p[P_XATT],
	         (AttrNumber)s->p[P_VATT], kt, xt, (Oid)s->p[P_VTYPE], 0, int_v);
	if (cross) {
		scan_rel(s->q, s->dk, s->dy, (Oid)s->p[P_RELID2], (AttrNumber)s->p[P_KATT2], (AttrNumber)s->p[P_XATT2],
		         (AttrNumber)s->p[P_VATT2], (Oid)s->p[P_KTYPE2], yt, (Oid)s->p[P_VTYPE2], 1, int_v);
	}
	if (s->dx) {
		tessera_c_set_xdict(s->q, s->dx);
	}
	if (s->dy) {
		tessera_c_set_ydict(s->q, s->dy);
	}
	set_spill(s->q);
	tessera_c_set_stop(s->q, pg_should_stop, NULL);
	char err[256];
	int rc = tessera_c_run(s->q, (int)s->p[P_MODE], (int)s->p[P_COL0_SMALLER], (uint64_t)s->p[P_MINVAL], -1,
	                       query_threads(tessera_threads), tessera_frontier, tessera_verbose, err, sizeof err);
	if (rc != TESSERA_C_OK) {
		run_error(rc, err);
	}
	if (int_v) {
		const int pb = Max(int_bits((Oid)s->p[P_VTYPE]), int_bits(cross ? (Oid)s->p[P_VTYPE2] : (Oid)s->p[P_VTYPE]));
		const int ov = tessera_c_overflow(s->q, pb, (Oid)s->p[P_AGGTYPE] == INT8OID ? 64 : 0);
		if (ov & 1) {
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			                errmsg(pb == 16 ? "smallint out of range" : pb == 32 ? "integer out of range" : "bigint out of range")));
		}
		if (ov & 2) {
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
		}
		s->wide = tessera_c_wide(s->q) != 0;
	}
	s->ran = true;
}

static Datum key_datum(TesseraState *s, int64 x, Oid t, int side) {
	if (str_type(t)) {
		size_t len;
		const char *p = side ? tessera_c_ystr(s->q, x, &len) : tessera_c_xstr(s->q, x, &len);
		MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
		Datum d = PointerGetDatum(cstring_to_text_with_len(p, (int)len));
		MemoryContextSwitchTo(old);
		return d;
	}
	return t == INT8OID ? Int64GetDatum(x) : t == INT4OID ? Int32GetDatum((int32)x) : Int16GetDatum((int16)x);
}

static TupleTableSlot *tessera_next(ScanState *ss) {
	TesseraState *s = (TesseraState *)ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	ExecClearTuple(slot);
	if (!s->ran) {
		tessera_run(s);
	}
	int64_t x0, x1, vi, hi = 0;
	uint64_t lo = 0;
	double vd = 0;
	int general = 0;
	if (s->wide) {
		if (tessera_c_next_batch128(s->q, &x0, &x1, &hi, &lo, 1) == 0) {
			return slot;
		}
		vi = (int64_t)lo;
		vd = (double)(__int128)(((unsigned __int128)(uint64_t)hi << 64) | lo);
	} else if (!tessera_c_next(s->q, &x0, &x1, &vi, &vd, &general)) {
		return slot;
	}
	const Oid xt = (Oid)s->p[P_XTYPE], at = (Oid)s->p[P_AGGTYPE];
	for (int c = 0; c < s->ncol; c++) {
		Datum d;
		if (s->kind[c] == 2 && at == NUMERICOID) {
			MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
			d = s->wide ? numeric_of_i128(hi, lo) : NumericGetDatum(int64_to_numeric(vi));
			MemoryContextSwitchTo(old);
		} else if (s->kind[c] == 2) {
			d = at == FLOAT8OID ? Float8GetDatum(general || s->wide ? vd : (double)vi) : Int64GetDatum(vi);
		} else {
			const int side = s->p[P_CROSS] && s->kind[c] == 1;
			d = key_datum(s, s->kind[c] == 0 ? x0 : x1, side ? (Oid)s->p[P_XTYPE2] : xt, side);
		}
		slot->tts_values[c] = d;
		slot->tts_isnull[c] = false;
	}
	return ExecStoreVirtualTuple(slot);
}

static bool tessera_recheck(ScanState *ss, TupleTableSlot *slot) {
	return true;
}

static TupleTableSlot *tessera_exec(CustomScanState *node) {
	return ExecScan(&node->ss, (ExecScanAccessMtd)tessera_next, (ExecScanRecheckMtd)tessera_recheck);
}

static void tessera_end(CustomScanState *node) {
	tessera_release(node);
}

static void tessera_rescan(CustomScanState *node) {
	TesseraState *s = (TesseraState *)node;
	if (s->q) {
		tessera_c_rewind(s->q);
	}
}

static void tessera_explain(CustomScanState *node, List *ancestors, ExplainState *es) {
	TesseraState *s = (TesseraState *)node;
	static const char *modes[] = {"all ordered pairs", "x_a < x_b", "x_a <= x_b"};
	ExplainPropertyText("Tessera pairs", s->p[P_CROSS] ? "cross product of two inputs" : modes[s->p[P_MODE] % 3], es);
	ExplainPropertyText("Tessera aggregate", s->p[P_VATT] ? "SUM(a.v * b.v)" : "COUNT(*)", es);
	if (s->p[P_MINVAL] > 0) {
		ExplainPropertyInteger("Tessera HAVING prune below", NULL, s->p[P_MINVAL], es);
	}
	ExplainPropertyText("Tessera core", psprintf("%s (%s)", tessera_c_version(), tessera_c_isa()), es);
}

PG_FUNCTION_INFO_V1(tessera_sim);
PG_FUNCTION_INFO_V1(tessera_sim_text);

static AttrNumber sim_column(Oid relid, int arg, FunctionCallInfo fcinfo, Oid *type, Oid *coll) {
	if (PG_ARGISNULL(arg)) {
		elog(ERROR, "tessera_sim: argument %d is NULL", arg + 1);
	}
	char *name = text_to_cstring(PG_GETARG_TEXT_PP(arg));
	AttrNumber att = get_attnum(relid, name);
	if (att <= 0) {
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
		                errmsg("tessera_sim: column \"%s\" of relation \"%s\" does not exist", name, get_rel_name(relid))));
	}
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK &&
	    pg_attribute_aclcheck(relid, att, GetUserId(), ACL_SELECT) != ACLCHECK_OK) {
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(get_rel_relkind(relid)), get_rel_name(relid));
	}
	if (check_enable_rls(relid, InvalidOid, true) != RLS_NONE) {
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		                errmsg("tessera_sim: relation \"%s\" has row-level security, which tessera_sim does not apply",
		                       get_rel_name(relid))));
	}
	if (get_attgenerated(relid, att) == ATTRIBUTE_GENERATED_VIRTUAL) {
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		                errmsg("tessera_sim: column \"%s\" is a virtual generated column, which has no stored values", name)));
	}
	Oid typmod_coll;
	int32 typmod;
	get_atttypetypmodcoll(relid, att, type, &typmod, &typmod_coll);
	*coll = typmod_coll;
	return att;
}

static Datum sim_run(FunctionCallInfo fcinfo, bool text_items) {
	const char *fn = text_items ? "tessera_sim_text" : "tessera_sim";
	for (int a = 0; a < 5; a++) {
		if (PG_ARGISNULL(a)) {
			elog(ERROR, "%s: argument %d is NULL", fn, a + 1);
		}
	}
	if (!tessera_c_supported()) {
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		                errmsg("%s: this CPU lacks x86-64-v3 (AVX2, BMI2, FMA), which Tessera needs", fn)));
	}
	const Oid relid = PG_GETARG_OID(0);
	LockRelationOid(relid, AccessShareLock);
	const char relkind = get_rel_relkind(relid);
	if ((relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW) || has_subclass(relid)) {
		elog(ERROR, "%s: \"%s\" is not a table without inheritance children", fn, get_rel_name(relid));
	}
	if (relkind == RELKIND_MATVIEW) {
		Relation r = table_open(relid, NoLock);
		const bool populated = RelationIsPopulated(r);
		table_close(r, NoLock);
		if (!populated) {
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			                errmsg("materialized view \"%s\" has not been populated", get_rel_name(relid)),
			                errhint("Use the REFRESH MATERIALIZED VIEW command.")));
		}
	}
	Oid kt, xt, vt = InvalidOid, kc, xc, vc;
	const AttrNumber ka = sim_column(relid, 1, fcinfo, &kt, &kc), xa = sim_column(relid, 2, fcinfo, &xt, &xc);
	const AttrNumber va = PG_ARGISNULL(5) ? 0 : sim_column(relid, 5, fcinfo, &vt, &vc);
	if (!key_ok(kt, kc)) {
		elog(ERROR, "%s: k must be an integer, or text or varchar with a deterministic collation", fn);
	}
	if (text_items ? !(str_type(xt) && key_ok(xt, xc)) : !int_type(xt)) {
		if (text_items) {
			elog(ERROR, "%s: x must be text or varchar with a deterministic collation (tessera_sim takes integers)", fn);
		}
		elog(ERROR, "%s: x must be an integer (tessera_sim_text takes strings)", fn);
	}
	if (va && !(int_type(vt) || vt == FLOAT4OID || vt == FLOAT8OID)) {
		elog(ERROR, "%s: v must be an integer or a float", fn);
	}
	const char *ms = text_to_cstring(PG_GETARG_TEXT_PP(3));
	const int measure = !strcmp(ms, "cosine") ? 1 : !strcmp(ms, "jaccard") ? 2 : !strcmp(ms, "dice") ? 3
	                    : !strcmp(ms, "overlap") ? 4 : 0;
	if (!measure) {
		elog(ERROR, "%s: measure is 'cosine', 'jaccard', 'dice' or 'overlap'", fn);
	}
	const char *md = PG_ARGISNULL(6) ? "lt" : text_to_cstring(PG_GETARG_TEXT_PP(6));
	const int mode = !strcmp(md, "full") ? 0 : !strcmp(md, "lt") ? 1 : !strcmp(md, "le") ? 2 : -1;
	if (mode < 0) {
		elog(ERROR, "%s: mode is 'lt', 'le' or 'full'", fn);
	}
	const int threads = query_threads(!PG_ARGISNULL(7) && PG_GETARG_INT32(7) > 0 ? PG_GETARG_INT32(7) : tessera_threads);
	InitMaterializedSRF(fcinfo, 0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
	tessera_query_c *q = tessera_c_new(va != 0);
	tessera_dict_c *volatile dk = NULL, *volatile dx = NULL;
	PG_TRY();
	{
		if (!q || (str_type(kt) && !(dk = tessera_dict_new())) || (text_items && !(dx = tessera_dict_new()))) {
			elog(ERROR, "%s: out of memory", fn);
		}
		scan_rel(q, dk, dx, relid, ka, xa, va, kt, xt, vt, 0, false);
		if (dx) {
			tessera_c_set_xdict(q, dx);
		}
		tessera_c_set_similarity(q, measure, PG_GETARG_FLOAT8(4));
		set_spill(q);
		tessera_c_set_stop(q, pg_should_stop, NULL);
		char err[256];
		const int rc = tessera_c_run(q, mode, 1, 0, -1, threads, tessera_frontier, tessera_verbose, err, sizeof err);
		if (rc != TESSERA_C_OK) {
			run_error(rc, err);
		}
		int64_t x0, x1, vi;
		double vd;
		int general;
		Datum values[3];
		bool nulls[3] = {false, false, false};
		MemoryContext rowcxt = AllocSetContextCreate(CurrentMemoryContext, "tessera_sim rows", ALLOCSET_DEFAULT_SIZES);
		while (tessera_c_next(q, &x0, &x1, &vi, &vd, &general)) {
			MemoryContext old = MemoryContextSwitchTo(rowcxt);
			if (text_items) {
				size_t l0, l1;
				const char *p0 = tessera_c_xstr(q, x0, &l0), *p1 = tessera_c_xstr(q, x1, &l1);
				values[0] = PointerGetDatum(cstring_to_text_with_len(p0, (int)l0));
				values[1] = PointerGetDatum(cstring_to_text_with_len(p1, (int)l1));
			} else {
				values[0] = Int64GetDatum(x0);
				values[1] = Int64GetDatum(x1);
			}
			values[2] = Float8GetDatum(vd);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
			MemoryContextSwitchTo(old);
			MemoryContextReset(rowcxt);
			CHECK_FOR_INTERRUPTS();
		}
		MemoryContextDelete(rowcxt);
	}
	PG_FINALLY();
	{
		if (q) {
			tessera_c_free(q);
		}
		if (dk) {
			tessera_dict_free(dk);
		}
		if (dx) {
			tessera_dict_free(dx);
		}
	}
	PG_END_TRY();
	return (Datum)0;
}

Datum tessera_sim(PG_FUNCTION_ARGS) {
	return sim_run(fcinfo, false);
}

Datum tessera_sim_text(PG_FUNCTION_ARGS) {
	return sim_run(fcinfo, true);
}

void _PG_init(void) {
	if (tessera_c_api_version() != TESSERA_API_VERSION) {
		elog(ERROR, "tessera: the core library has interface version %d, this module needs %d", tessera_c_api_version(),
		     TESSERA_API_VERSION);
	}
	DefineCustomBoolVariable("tessera.enabled", "Plan self-join aggregates with the Tessera operator", NULL,
	                         &tessera_enabled, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("tessera.threads", "Threads of the Tessera operator (0: max_parallel_workers_per_gather + 1)",
	                        NULL, &tessera_threads, 0, 0, 1024,
	                        PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("tessera.frontier", "Items on the matrix engine (-1: cost model)", NULL, &tessera_frontier,
	                        -1, -1, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("tessera.trace", "Tessera phase times to the server log (stderr)", NULL, &tessera_verbose,
	                         false, PGC_USERSET, 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("tessera");
	RegisterCustomScanMethods(&tessera_scan_methods);
	prev_upper_hook = create_upper_paths_hook;
	create_upper_paths_hook = tessera_upper_hook;
}
