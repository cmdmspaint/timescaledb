/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>
#include <catalog/namespace.h>
#include <catalog/pg_trigger.h>
#include <catalog/indexing.h>
#include <catalog/pg_inherits.h>
#include <catalog/toasting.h>
#include <commands/trigger.h>
#include <commands/tablecmds.h>
#include <commands/defrem.h>
#include <tcop/tcopprot.h>
#include <access/htup.h>
#include <access/htup_details.h>
#include <access/xact.h>
#include <access/reloptions.h>
#include <nodes/makefuncs.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/hsearch.h>
#include <storage/lmgr.h>
#include <miscadmin.h>
#include <funcapi.h>
#include <fmgr.h>
#include <utils/datum.h>
#include <catalog/pg_type.h>
#include <utils/timestamp.h>
#include <nodes/execnodes.h>
#include <executor/executor.h>
#include <access/tupdesc.h>

#include "export.h"
#include "chunk.h"
#include "chunk_index.h"
#include "chunk_data_node.h"
#include "cross_module_fn.h"
#include "catalog.h"
#include "continuous_agg.h"
#include "cross_module_fn.h"
#include "dimension.h"
#include "dimension_slice.h"
#include "dimension_vector.h"
#include "errors.h"
#include "partitioning.h"
#include "hypertable.h"
#include "hypertable_data_node.h"
#include "hypercube.h"
#include "scanner.h"
#include "process_utility.h"
#include "trigger.h"
#include "compat.h"
#include "utils.h"
#include "interval.h"
#include "hypertable_cache.h"
#include "cache.h"
#include "bgw_policy/chunk_stats.h"
#include "errors.h"

TS_FUNCTION_INFO_V1(ts_chunk_show_chunks);
TS_FUNCTION_INFO_V1(ts_chunk_drop_chunks);
TS_FUNCTION_INFO_V1(ts_chunks_in);
TS_FUNCTION_INFO_V1(ts_chunk_id_from_relid);
TS_FUNCTION_INFO_V1(ts_chunk_show);
TS_FUNCTION_INFO_V1(ts_chunk_create);

/* Used when processing scanned chunks */
typedef enum ChunkResult
{
	CHUNK_DONE,
	CHUNK_IGNORED,
	CHUNK_PROCESSED
} ChunkResult;

typedef ChunkResult (*on_chunk_func)(ChunkScanCtx *ctx, Chunk *chunk);
static void chunk_scan_ctx_init(ChunkScanCtx *ctx, Hyperspace *hs, Point *p);
static void chunk_scan_ctx_destroy(ChunkScanCtx *ctx);
static void chunk_collision_scan(ChunkScanCtx *scanctx, Hypercube *cube);
static int chunk_scan_ctx_foreach_chunk(ChunkScanCtx *ctx, on_chunk_func on_chunk, uint16 limit);
static Chunk **chunk_get_chunks_in_time_range(Oid table_relid, Datum older_than_datum,
											  Datum newer_than_datum, Oid older_than_type,
											  Oid newer_than_type, char *caller_name,
											  MemoryContext mctx, uint64 *num_chunks_returned);
static Datum chunks_return_srf(FunctionCallInfo fcinfo);
static int chunk_cmp(const void *ch1, const void *ch2);

static void
chunk_insert_relation(Relation rel, Chunk *chunk)
{
	TupleDesc desc = RelationGetDescr(rel);
	Datum values[Natts_chunk];
	bool nulls[Natts_chunk] = { false };
	CatalogSecurityContext sec_ctx;

	memset(values, 0, sizeof(values));
	values[AttrNumberGetAttrOffset(Anum_chunk_id)] = Int32GetDatum(chunk->fd.id);
	values[AttrNumberGetAttrOffset(Anum_chunk_hypertable_id)] =
		Int32GetDatum(chunk->fd.hypertable_id);
	values[AttrNumberGetAttrOffset(Anum_chunk_schema_name)] = NameGetDatum(&chunk->fd.schema_name);
	values[AttrNumberGetAttrOffset(Anum_chunk_table_name)] = NameGetDatum(&chunk->fd.table_name);

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_insert_values(rel, desc, values, nulls);
	ts_catalog_restore_user(&sec_ctx);
}

static void
chunk_insert_lock(Chunk *chunk, LOCKMODE lock)
{
	Catalog *catalog = ts_catalog_get();
	Relation rel;

	rel = heap_open(catalog_get_table_id(catalog, CHUNK), lock);
	chunk_insert_relation(rel, chunk);
	heap_close(rel, lock);
}

static void
chunk_fill(Chunk *chunk, HeapTuple tuple, TupleDesc desc)
{
	Oid schema_id;

	memcpy(&chunk->fd, GETSTRUCT(tuple), sizeof(FormData_chunk));

	schema_id = get_namespace_oid(NameStr(chunk->fd.schema_name), true);

	Assert(OidIsValid(schema_id));

	chunk->table_id = get_relname_relid(NameStr(chunk->fd.table_name), schema_id);
	chunk->hypertable_relid = ts_inheritance_parent_relid(chunk->table_id);
	chunk->relkind = get_rel_relkind(chunk->table_id);

	Assert(OidIsValid(chunk->table_id));
	Assert(OidIsValid(chunk->hypertable_relid));
	Assert(chunk->relkind == 'r' || chunk->relkind == 'f');
}

static ScanTupleResult
chunk_tuple_found(TupleInfo *ti, void *arg)
{
	Chunk *chunk = arg;

	chunk_fill(chunk, ti->tuple, ti->desc);
	return SCAN_DONE;
}

/* Fill in a chunk stub. The stub data structure needs the chunk ID and
 * constraints set.  The rest of the fields will be filled in from the table
 * data. */
static Chunk *
chunk_fill_stub(Chunk *chunk_stub, bool tuplock)
{
	ScanKeyData scankey[1];
	Catalog *catalog = ts_catalog_get();
	int num_found;
	ScannerCtx	ctx = {
		.table = catalog->tables[CHUNK].id,
		.index = catalog_get_index(catalog, CHUNK, CHUNK_ID_INDEX),
		.nkeys = 1,
		.scankey = scankey,
		.data = chunk_stub,
		.tuple_found = chunk_tuple_found,
		.lockmode = AccessShareLock,
		.tuplock = {
			.lockmode = LockTupleShare,
			.enabled = tuplock,
		},
		.scandirection = ForwardScanDirection,
	};

	/*
	 * Perform an index scan on chunk ID.
	 */
	ScanKeyInit(&scankey[0],
				Anum_chunk_id,
				BTEqualStrategyNumber,
				F_INT4EQ,
				Int32GetDatum(chunk_stub->fd.id));

	num_found = ts_scanner_scan(&ctx);

	if (num_found != 1)
		elog(ERROR, "no chunk found with ID %d", chunk_stub->fd.id);

	if (NULL == chunk_stub->cube)
		chunk_stub->cube =
			ts_hypercube_from_constraints(chunk_stub->constraints, CurrentMemoryContext);
	else

		/*
		 * The hypercube slices were filled in during the scan. Now we need to
		 * sort them in dimension order.
		 */
		ts_hypercube_slice_sort(chunk_stub->cube);

	if (chunk_stub->relkind == RELKIND_FOREIGN_TABLE)
		chunk_stub->data_nodes =
			ts_chunk_data_node_scan_by_chunk_id(chunk_stub->fd.id, CurrentMemoryContext);

	return chunk_stub;
}

typedef struct CollisionInfo
{
	Hypercube *cube;
	Chunk *colliding_chunk;
} CollisionInfo;

/*-
 * Align a chunk's hypercube in 'aligned' dimensions.
 *
 * Alignment ensures that chunks line up in a particular dimension, i.e., their
 * ranges should either be identical or not overlap at all.
 *
 * Non-aligned:
 *
 * ' [---------]      <- existing slice
 * '      [---------] <- calculated (new) slice
 *
 * To align the slices above there are two cases depending on where the
 * insertion point happens:
 *
 * Case 1 (reuse slice):
 *
 * ' [---------]
 * '      [--x------]
 *
 * The insertion point x falls within the range of the existing slice. We should
 * reuse the existing slice rather than creating a new one.
 *
 * Case 2 (cut to align):
 *
 * ' [---------]
 * '      [-------x-]
 *
 * The insertion point falls outside the range of the existing slice and we need
 * to cut the new slice to line up.
 *
 * ' [---------]
 * '        cut [---]
 * '
 *
 * Note that slice reuse (case 1) happens already when calculating the tentative
 * hypercube for the chunk, and is thus already performed once reaching this
 * function. Thus, we deal only with case 2 here. Also note that a new slice
 * might overlap in complicated ways, requiring multiple cuts. For instance,
 * consider the following situation:
 *
 * ' [------]   [-] [---]
 * '      [---x-------]  <- calculated slice
 *
 * This should but cut-to-align as follows:
 *
 * ' [------]   [-] [---]
 * '         [x]
 *
 * After a chunk collision scan, this function is called for each chunk in the
 * chunk scan context. Chunks in the scan context may have only a partial set of
 * slices if they only overlap in some, but not all, dimensions (see
 * illustrations below). Still, partial chunks may still be of interest for
 * alignment in a particular dimension. Thus, if a chunk has an overlapping
 * slice in an aligned dimension, we cut to not overlap with that slice.
 */
static ChunkResult
do_dimension_alignment(ChunkScanCtx *scanctx, Chunk *chunk)
{
	CollisionInfo *info = scanctx->data;
	Hypercube *cube = info->cube;
	Hyperspace *space = scanctx->space;
	ChunkResult res = CHUNK_IGNORED;
	int i;

	for (i = 0; i < space->num_dimensions; i++)
	{
		Dimension *dim = &space->dimensions[i];
		DimensionSlice *chunk_slice, *cube_slice;
		int64 coord = scanctx->point->coordinates[i];

		if (!dim->fd.aligned)
			continue;

		/*
		 * The chunk might not have a slice for each dimension, so we cannot
		 * use array indexing. Fetch slice by dimension ID instead.
		 */
		chunk_slice = ts_hypercube_get_slice_by_dimension_id(chunk->cube, dim->fd.id);

		if (NULL == chunk_slice)
			continue;

		cube_slice = cube->slices[i];

		/*
		 * Only cut-to-align if the slices collide and are not identical
		 * (i.e., if we are reusing an existing slice we should not cut it)
		 */
		if (!ts_dimension_slices_equal(cube_slice, chunk_slice) &&
			ts_dimension_slices_collide(cube_slice, chunk_slice))
		{
			ts_dimension_slice_cut(cube_slice, chunk_slice, coord);
			res = CHUNK_PROCESSED;
		}
	}

	return res;
}

/*
 * Calculate, and potentially set, a new chunk interval for an open dimension.
 */
static bool
calculate_and_set_new_chunk_interval(Hypertable *ht, Point *p)
{
	Hyperspace *hs = ht->space;
	Dimension *dim = NULL;
	Datum datum;
	int64 chunk_interval, coord;
	int i;

	if (!OidIsValid(ht->chunk_sizing_func) || ht->fd.chunk_target_size <= 0)
		return false;

	/* Find first open dimension */
	for (i = 0; i < hs->num_dimensions; i++)
	{
		dim = &hs->dimensions[i];

		if (IS_OPEN_DIMENSION(dim))
			break;

		dim = NULL;
	}

	/* Nothing to do if no open dimension */
	if (NULL == dim)
	{
		elog(WARNING,
			 "adaptive chunking enabled on hypertable \"%s\" without an open (time) dimension",
			 get_rel_name(ht->main_table_relid));

		return false;
	}

	coord = p->coordinates[i];
	datum = OidFunctionCall3(ht->chunk_sizing_func,
							 Int32GetDatum(dim->fd.id),
							 Int64GetDatum(coord),
							 Int64GetDatum(ht->fd.chunk_target_size));
	chunk_interval = DatumGetInt64(datum);

	/* Check if the function didn't set and interval or nothing changed */
	if (chunk_interval <= 0 || chunk_interval == dim->fd.interval_length)
		return false;

	/* Update the dimension */
	ts_dimension_set_chunk_interval(dim, chunk_interval);

	return true;
}

/*
 * Resolve chunk collisions.
 *
 * After a chunk collision scan, this function is called for each chunk in the
 * chunk scan context. We only care about chunks that have a full set of
 * slices/constraints that overlap with our tentative hypercube, i.e., they
 * fully collide. We resolve those collisions by cutting the hypercube.
 */
static ChunkResult
do_collision_resolution(ChunkScanCtx *scanctx, Chunk *chunk)
{
	CollisionInfo *info = scanctx->data;
	Hypercube *cube = info->cube;
	Hyperspace *space = scanctx->space;
	ChunkResult res = CHUNK_IGNORED;
	int i;

	if (chunk->cube->num_slices != space->num_dimensions ||
		!ts_hypercubes_collide(cube, chunk->cube))
		return CHUNK_IGNORED;

	for (i = 0; i < space->num_dimensions; i++)
	{
		DimensionSlice *cube_slice = cube->slices[i];
		DimensionSlice *chunk_slice = chunk->cube->slices[i];
		int64 coord = scanctx->point->coordinates[i];

		/*
		 * Only cut if we aren't reusing an existing slice and there is a
		 * collision
		 */
		if (!ts_dimension_slices_equal(cube_slice, chunk_slice) &&
			ts_dimension_slices_collide(cube_slice, chunk_slice))
		{
			ts_dimension_slice_cut(cube_slice, chunk_slice, coord);
			info->colliding_chunk = chunk;
			res = CHUNK_PROCESSED;

			/*
			 * Redo the collision check after each cut since cutting in one
			 * dimension might have resolved the collision in another
			 */
			if (!ts_hypercubes_collide(cube, chunk->cube))
				return res;
		}
	}

	Assert(!ts_hypercubes_collide(cube, chunk->cube));

	return res;
}

static ChunkResult
check_for_collisions(ChunkScanCtx *scanctx, Chunk *chunk)
{
	CollisionInfo *info = scanctx->data;
	Hypercube *cube = info->cube;
	Hyperspace *space = scanctx->space;

	/* Check if this chunk collides with our hypercube */
	if (chunk->cube->num_slices == space->num_dimensions &&
		ts_hypercubes_collide(cube, chunk->cube))
	{
		info->colliding_chunk = chunk;
		return CHUNK_DONE;
	}

	return CHUNK_IGNORED;
}

/*
 * Check if a (tentative) chunk collides with existing chunks.
 *
 * Return the colliding chunk. Note that the chunk is a stub that needs to be
 * filled.
 */
static Chunk *
chunk_collides(Hyperspace *hs, Hypercube *hc)
{
	ChunkScanCtx scanctx;
	CollisionInfo info = {
		.cube = hc,
		.colliding_chunk = NULL,
	};

	chunk_scan_ctx_init(&scanctx, hs, NULL);

	/* Scan for all chunks that collide with the hypercube of the new chunk */
	chunk_collision_scan(&scanctx, hc);
	scanctx.data = &info;

	/* Find chunks that collide */
	chunk_scan_ctx_foreach_chunk(&scanctx, check_for_collisions, 0);

	chunk_scan_ctx_destroy(&scanctx);

	return info.colliding_chunk;
}

/*-
 * Resolve collisions and perform alignmment.
 *
 * Chunks collide only if their hypercubes overlap in all dimensions. For
 * instance, the 2D chunks below collide because they overlap in both the X and
 * Y dimensions:
 *
 * ' _____
 * ' |    |
 * ' | ___|__
 * ' |_|__|  |
 * '   |     |
 * '   |_____|
 *
 * While the following chunks do not collide, although they still overlap in the
 * X dimension:
 *
 * ' _____
 * ' |    |
 * ' |    |
 * ' |____|
 * '   ______
 * '   |     |
 * '   |    *|
 * '   |_____|
 *
 * For the collision case above we obviously want to cut our hypercube to no
 * longer collide with existing chunks. However, the second case might still be
 * of interest for alignment in case X is an 'aligned' dimension. If '*' is the
 * insertion point, then we still want to cut the hypercube to ensure that the
 * dimension remains aligned, like so:
 *
 * ' _____
 * ' |    |
 * ' |    |
 * ' |____|
 * '       ___
 * '       | |
 * '       |*|
 * '       |_|
 *
 *
 * We perform alignment first as that might actually resolve chunk
 * collisions. After alignment we check for any remaining collisions.
 */
static void
chunk_collision_resolve(Hyperspace *hs, Hypercube *cube, Point *p)
{
	ChunkScanCtx scanctx;
	CollisionInfo info = {
		.cube = cube,
		.colliding_chunk = NULL,
	};

	chunk_scan_ctx_init(&scanctx, hs, p);

	/* Scan for all chunks that collide with the hypercube of the new chunk */
	chunk_collision_scan(&scanctx, cube);
	scanctx.data = &info;

	/* Cut the hypercube in any aligned dimensions */
	chunk_scan_ctx_foreach_chunk(&scanctx, do_dimension_alignment, 0);

	/*
	 * If there are any remaining collisions with chunks, then cut-to-fit to
	 * resolve those collisions
	 */
	chunk_scan_ctx_foreach_chunk(&scanctx, do_collision_resolution, 0);

	chunk_scan_ctx_destroy(&scanctx);
}

static int
chunk_add_constraints(Chunk *chunk)
{
	int num_added;

	num_added = ts_chunk_constraints_add_dimension_constraints(chunk->constraints,
															   chunk->fd.id,
															   chunk->cube);
	num_added += ts_chunk_constraints_add_inheritable_constraints(chunk->constraints,
																  chunk->fd.id,
																  chunk->relkind,
																  chunk->hypertable_relid);

	return num_added;
}

static List *
get_reloptions(Oid relid)
{
	HeapTuple tuple;
	Datum datum;
	bool isnull;
	List *options;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);

	options = untransformRelOptions(datum);

	ReleaseSysCache(tuple);

	return options;
}

/* applies the attributes and statistics target for columns on the hypertable
   to columns on the chunk */
static void
set_attoptions(Relation ht_rel, Oid chunk_oid)
{
	TupleDesc tupleDesc = RelationGetDescr(ht_rel);
	int natts = tupleDesc->natts;
	int attno;

	for (attno = 1; attno <= natts; attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc, attno - 1);
		char *attributeName = NameStr(attribute->attname);
		HeapTuple tuple;
		Datum options;
		bool isnull;

		/* Ignore dropped */
		if (attribute->attisdropped)
			continue;

		tuple = SearchSysCacheAttName(RelationGetRelid(ht_rel), attributeName);

		Assert(tuple != NULL);

		/*
		 * Pass down the attribute options (ALTER TABLE ALTER COLUMN SET
		 * attribute_option)
		 */
		options = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions, &isnull);

		if (!isnull)
		{
			AlterTableCmd *cmd = makeNode(AlterTableCmd);

			cmd->subtype = AT_SetOptions;
			cmd->name = attributeName;
			cmd->def = (Node *) untransformRelOptions(options);
			AlterTableInternal(chunk_oid, list_make1(cmd), false);
		}

		/*
		 * Pass down the attribute options (ALTER TABLE ALTER COLUMN SET
		 * STATISTICS)
		 */
		options = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attstattarget, &isnull);
		if (!isnull)
		{
			int32 target = DatumGetInt32(options);

			/* Don't do anything if it's set to the default */
			if (target != -1)
			{
				AlterTableCmd *cmd = makeNode(AlterTableCmd);

				cmd->subtype = AT_SetStatistics;
				cmd->name = attributeName;
				cmd->def = (Node *) makeInteger(target);
				AlterTableInternal(chunk_oid, list_make1(cmd), false);
			}
		}

		ReleaseSysCache(tuple);
	}
}

static void
create_toast_table(CreateStmt *stmt, Oid chunk_oid)
{
	/* similar to tcop/utility.c */
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	Datum toast_options =
		transformRelOptions((Datum) 0, stmt->options, "toast", validnsps, true, false);

	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options, true);

	NewRelationCreateToastTable(chunk_oid, toast_options);
}

/*
 * Create a chunk's table.
 *
 * A chunk inherits from the main hypertable and will have the same owner. Since
 * chunks can be created either in the TimescaleDB internal schema or in a
 * user-specified schema, some care has to be taken to use the right
 * permissions, depending on the case:
 *
 * 1. if the chunk is created in the internal schema, we create it as the
 * catalog/schema owner (i.e., anyone can create chunks there via inserting into
 * a hypertable, but can not do it via CREATE TABLE).
 *
 * 2. if the chunk is created in a user-specified "associated schema", then we
 * shouldn't use the catalog owner to create the table since that typically
 * implies super-user permissions. If we would allow that, anyone can specify
 * someone else's schema in create_hypertable() and create chunks in it without
 * having the proper permissions to do so. With this logic, the hypertable owner
 * must have permissions to create tables in the associated schema, or else
 * table creation will fail. If the schema doesn't yet exist, the table owner
 * instead needs the proper permissions on the database to create the schema.
 */
static Oid
chunk_create_table(Chunk *chunk, Hypertable *ht)
{
	Relation rel;
	ObjectAddress objaddr;
	int sec_ctx;

	/*
	 * The CreateForeignTableStmt embeds a regular CreateStmt, so we can use
	 * it to create both regular and foreign tables
	 */
	CreateForeignTableStmt stmt = {
		.base.type = T_CreateStmt,
		.base.relation =
			makeRangeVar(NameStr(chunk->fd.schema_name), NameStr(chunk->fd.table_name), 0),
		.base.inhRelations =
			list_make1(makeRangeVar(NameStr(ht->fd.schema_name), NameStr(ht->fd.table_name), 0)),
		.base.tablespacename = ts_hypertable_select_tablespace_name(ht, chunk),
		.base.options = get_reloptions(ht->main_table_relid),
	};
	Oid uid, saved_uid;

	rel = heap_open(ht->main_table_relid, AccessShareLock);

	/*
	 * If the chunk is created in the internal schema, become the catalog
	 * owner, otherwise become the hypertable owner
	 */
	if (namestrcmp(&chunk->fd.schema_name, INTERNAL_SCHEMA_NAME) == 0)
		uid = ts_catalog_database_info_get()->owner_uid;
	else
		uid = rel->rd_rel->relowner;

	GetUserIdAndSecContext(&saved_uid, &sec_ctx);

	if (uid != saved_uid)
		SetUserIdAndSecContext(uid, sec_ctx | SECURITY_LOCAL_USERID_CHANGE);

	objaddr = DefineRelation(&stmt.base,
							 chunk->relkind,
							 rel->rd_rel->relowner,
							 NULL
#if !PG96
							 ,
							 NULL
#endif
	);

	if (chunk->relkind == RELKIND_RELATION)
	{
		/*
		 * need to create a toast table explicitly for some of the option
		 * setting to work
		 */
		create_toast_table(&stmt.base, objaddr.objectId);

		if (uid != saved_uid)
			SetUserIdAndSecContext(saved_uid, sec_ctx);
	}
	else if (chunk->relkind == RELKIND_FOREIGN_TABLE)
	{
		ChunkDataNode *cdn;

		if (list_length(chunk->data_nodes) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_TS_NO_DATA_NODES),
					 (errmsg("no data nodes associated with chunk \"%s\"",
							 get_rel_name(chunk->table_id)))));

		/*
		 * Use the first chunk data node as the "primary" to put in the foreign
		 * table
		 */
		cdn = linitial(chunk->data_nodes);
		stmt.base.type = T_CreateForeignServerStmt;
		stmt.servername = NameStr(cdn->fd.node_name);

		/* Create the foreign table catalog information */
		CreateForeignTable(&stmt, objaddr.objectId);

		/*
		 * Need to restore security context to execute remote commands as the
		 * original user
		 */
		if (uid != saved_uid)
			SetUserIdAndSecContext(saved_uid, sec_ctx);

		/* Create the corresponding chunk replicas on the remote data nodes */
		ts_cm_functions->create_chunk_on_data_nodes(chunk, ht);

		/* Record the remote data node chunk ID mappings */
		ts_chunk_data_node_insert_multi(chunk->data_nodes);
	}
	else
		elog(ERROR, "invalid relkind \"%c\" when creating chunk", chunk->relkind);

	set_attoptions(rel, objaddr.objectId);

	heap_close(rel, AccessShareLock);

	return objaddr.objectId;
}

static List *
chunk_assign_data_nodes(Chunk *chunk, Hypertable *ht)
{
	List *htnodes;
	List *chunk_data_nodes = NIL;
	ListCell *lc;

	if (chunk->relkind != RELKIND_FOREIGN_TABLE)
		return NIL;

	if (ht->data_nodes == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_TS_NO_DATA_NODES),
				 (errmsg("no data nodes associated with hypertable \"%s\"",
						 get_rel_name(ht->main_table_relid)))));

	Assert(chunk->cube != NULL);

	htnodes = ts_hypertable_assign_chunk_data_nodes(ht, chunk->cube);
	Assert(htnodes != NIL);

	foreach (lc, htnodes)
	{
		HypertableDataNode *htnode = lfirst(lc);
		ForeignServer *foreign_server =
			GetForeignServerByName(NameStr(htnode->fd.node_name), false);
		ChunkDataNode *chunk_data_node = palloc0(sizeof(ChunkDataNode));

		/*
		 * Create a stub data node (partially filled in entry). This will be
		 * fully filled in and persisted to metadata tables once we create the
		 * remote tables during insert
		 */
		chunk_data_node->fd.chunk_id = chunk->fd.id;
		chunk_data_node->fd.node_chunk_id = -1;
		namestrcpy(&chunk_data_node->fd.node_name, foreign_server->servername);
		chunk_data_node->foreign_server_oid = foreign_server->serverid;
		chunk_data_nodes = lappend(chunk_data_nodes, chunk_data_node);
	}

	return chunk_data_nodes;
}

static inline const char *
get_chunk_name_suffix(const char relkind)
{
	if (relkind == RELKIND_FOREIGN_TABLE)
		return "dist_chunk";
	return "chunk";
}

/*
 * Create a chunk from the dimensional constraints in the given hypercube.
 *
 * The table name for the chunk can be given explicitly, or generated if
 * table_name is NULL. If the table name is generated, it will use the given
 * prefix or, if NULL, use the hypertable's associated table prefix. Similarly,
 * if schema_name is NULL it will use the hypertable's associated schema for
 * the chunk.
 */
static Chunk *
ts_chunk_create_from_hypercube(Hypertable *ht, Hypercube *hc, const char *schema_name,
							   const char *table_name, const char *prefix)
{
	Hyperspace *hs = ht->space;
	Catalog *catalog = ts_catalog_get();
	CatalogSecurityContext sec_ctx;
	Chunk *chunk;
	const char relkind = ht->fd.replication_factor > 0 ? RELKIND_FOREIGN_TABLE : RELKIND_RELATION;

	if (NULL == schema_name || schema_name[0] == '\0')
		schema_name = NameStr(ht->fd.associated_schema_name);

	/* Create a new chunk based on the hypercube */
	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	chunk = ts_chunk_create_stub(ts_catalog_table_next_seq_id(catalog, CHUNK),
								 hs->num_dimensions,
								 relkind);
	ts_catalog_restore_user(&sec_ctx);

	chunk->fd.hypertable_id = hs->hypertable_id;
	chunk->cube = hc;
	chunk->hypertable_relid = ht->main_table_relid;
	namestrcpy(&chunk->fd.schema_name, schema_name);

	if (NULL == table_name || table_name[0] == '\0')
	{
		int len;

		if (NULL == prefix)
			prefix = NameStr(ht->fd.associated_table_prefix);

		len = snprintf(chunk->fd.table_name.data,
					   NAMEDATALEN,
					   "%s_%d_%s",
					   prefix,
					   chunk->fd.id,
					   get_chunk_name_suffix(relkind));

		if (len >= NAMEDATALEN)
			elog(ERROR, "chunk table name too long");
	}
	else
		namestrcpy(&chunk->fd.table_name, table_name);

	/* Insert chunk */
	chunk_insert_lock(chunk, RowExclusiveLock);

	/* Insert any new dimension slices */
	ts_dimension_slice_insert_multi(hc->slices, hc->num_slices, true);

	/* Add metadata for dimensional and inheritable constraints */
	chunk_add_constraints(chunk);

	/* If this is a remote chunk we assign data nodes */
	if (chunk->relkind == RELKIND_FOREIGN_TABLE)
		chunk->data_nodes = chunk_assign_data_nodes(chunk, ht);

	/* Create the actual table relation for the chunk */
	chunk->table_id = chunk_create_table(chunk, ht);

	if (!OidIsValid(chunk->table_id))
		elog(ERROR, "could not create chunk table");

	/* Create the chunk's constraints, triggers, and indexes */
	ts_chunk_constraints_create(chunk->constraints,
								chunk->table_id,
								chunk->fd.id,
								chunk->hypertable_relid,
								chunk->fd.hypertable_id);

	if (chunk->relkind == RELKIND_RELATION)
	{
		ts_trigger_create_all_on_chunk(ht, chunk);

		ts_chunk_index_create_all(chunk->fd.hypertable_id,
								  chunk->hypertable_relid,
								  chunk->fd.id,
								  chunk->table_id);
	}

	return chunk;
}

static Chunk *
chunk_create_after_lock(Hypertable *ht, Point *p, const char *schema, const char *prefix)
{
	Hyperspace *hs = ht->space;
	Hypercube *cube;

	/*
	 * If the user has enabled adaptive chunking, call the function to
	 * calculate and set the new chunk time interval.
	 */
	calculate_and_set_new_chunk_interval(ht, p);

	/* Calculate the hypercube for a new chunk that covers the tuple's point */
	cube = ts_hypercube_calculate_from_point(hs, p);

	/* Resolve collisions with other chunks by cutting the new hypercube */
	chunk_collision_resolve(hs, cube, p);

	return ts_chunk_create_from_hypercube(ht, cube, schema, NULL, prefix);
}

TSDLLEXPORT Chunk *
ts_chunk_find_or_create_without_cuts(Hypertable *ht, Hypercube *hc, const char *schema_name,
									 const char *table_name, bool *created)
{
	Chunk *chunk;

	LockRelationOid(ht->main_table_relid, ShareUpdateExclusiveLock);

	chunk = chunk_collides(ht->space, hc);

	if (NULL == chunk)
	{
		chunk = ts_chunk_create_from_hypercube(ht, hc, schema_name, table_name, NULL);

		if (NULL != created)
			*created = true;
	}
	else
	{
		if (!ts_hypercube_equal(chunk->cube, hc))
			ereport(ERROR,
					(errcode(ERRCODE_TS_CHUNK_COLLISION),
					 errmsg("chunk creation failed due to collision")));

		/* chunk_collides only returned a stub, so need to fill it */
		chunk_fill_stub(chunk, false);

		/*
		 * We only scanned for dimensional constraints, so we now need to
		 * rescan the constraints to also get the inherited constraints.
		 */
		chunk->constraints = ts_chunk_constraint_scan_by_chunk_id(chunk->fd.id,
																  ht->space->num_dimensions,
																  CurrentMemoryContext);

		if (NULL != created)
			*created = false;
	}

	Assert(chunk != NULL);

	return chunk;
}

Chunk *
ts_chunk_create_from_point(Hypertable *ht, Point *p, const char *schema, const char *prefix)
{
	Chunk *chunk;

	/*
	 * Serialize chunk creation around a lock on the "main table" to avoid
	 * multiple processes trying to create the same chunk. We use a
	 * ShareUpdateExclusiveLock, which is the weakest lock possible that
	 * conflicts with itself. The lock needs to be held until transaction end.
	 */
	LockRelationOid(ht->main_table_relid, ShareUpdateExclusiveLock);

	/* Recheck if someone else created the chunk before we got the table lock */
	chunk = ts_chunk_find(ht->space, p);

	if (NULL == chunk)
		chunk = chunk_create_after_lock(ht, p, schema, prefix);

	Assert(chunk != NULL);

	return chunk;
}

Chunk *
ts_chunk_create_stub(int32 id, int16 num_constraints, const char relkind)
{
	Chunk *chunk;

	chunk = palloc0(sizeof(Chunk));
	chunk->fd.id = id;
	chunk->relkind = relkind;

	if (num_constraints > 0)
		chunk->constraints = ts_chunk_constraints_alloc(num_constraints, CurrentMemoryContext);

	return chunk;
}

/*
 * Initialize a chunk scan context.
 *
 * A chunk scan context is used to join chunk-related information from metadata
 * tables during scans.
 */
static void
chunk_scan_ctx_init(ChunkScanCtx *ctx, Hyperspace *hs, Point *p)
{
	struct HASHCTL hctl = {
		.keysize = sizeof(int32),
		.entrysize = sizeof(ChunkScanEntry),
		.hcxt = CurrentMemoryContext,
	};

	ctx->htab = hash_create("chunk-scan-context", 20, &hctl, HASH_ELEM | HASH_CONTEXT | HASH_BLOBS);
	ctx->space = hs;
	ctx->point = p;
	ctx->num_complete_chunks = 0;
	ctx->early_abort = false;
	ctx->lockmode = NoLock;
}

/*
 * Destroy the chunk scan context.
 *
 * This will free the hash table in the context, but not the chunks within since
 * they are not allocated on the hash tables memory context.
 */
static void
chunk_scan_ctx_destroy(ChunkScanCtx *ctx)
{
	hash_destroy(ctx->htab);
}

static inline void
dimension_slice_and_chunk_constraint_join(ChunkScanCtx *scanctx, DimensionVec *vec)
{
	int i;

	for (i = 0; i < vec->num_slices; i++)
	{
		/*
		 * For each dimension slice, find matching constraints. These will be
		 * saved in the scan context
		 */
		ts_chunk_constraint_scan_by_dimension_slice(vec->slices[i], scanctx, CurrentMemoryContext);
	}
}

/*
 * Scan for the chunk that encloses the given point.
 *
 * In each dimension there can be one or more slices that match the point's
 * coordinate in that dimension. Slices are collected in the scan context's hash
 * table according to the chunk IDs they are associated with. A slice might
 * represent the dimensional bound of multiple chunks, and thus is added to all
 * the hash table slots of those chunks. At the end of the scan there will be at
 * most one chunk that has a complete set of slices, since a point cannot belong
 * to two chunks.
 */
static void
chunk_point_scan(ChunkScanCtx *scanctx, Point *p)
{
	int i;

	/* Scan all dimensions for slices enclosing the point */
	for (i = 0; i < scanctx->space->num_dimensions; i++)
	{
		DimensionVec *vec;

		vec = dimension_slice_scan(scanctx->space->dimensions[i].fd.id, p->coordinates[i]);

		dimension_slice_and_chunk_constraint_join(scanctx, vec);
	}
}

/*
 * Scan for chunks that collide with the given hypercube.
 *
 * Collisions are determined using axis-aligned bounding box collision detection
 * generalized to N dimensions. Slices are collected in the scan context's hash
 * table according to the chunk IDs they are associated with. A slice might
 * represent the dimensional bound of multiple chunks, and thus is added to all
 * the hash table slots of those chunks. At the end of the scan, those chunks
 * that have a full set of slices are the ones that actually collide with the
 * given hypercube.
 *
 * Chunks in the scan context that do not collide (do not have a full set of
 * slices), might still be important for ensuring alignment in those dimensions
 * that require alignment.
 */
static void
chunk_collision_scan(ChunkScanCtx *scanctx, Hypercube *cube)
{
	int i;

	/* Scan all dimensions for colliding slices */
	for (i = 0; i < scanctx->space->num_dimensions; i++)
	{
		DimensionVec *vec;
		DimensionSlice *slice = cube->slices[i];

		vec = dimension_slice_collision_scan(slice->fd.dimension_id,
											 slice->fd.range_start,
											 slice->fd.range_end);

		/* Add the slices to all the chunks they are associated with */
		dimension_slice_and_chunk_constraint_join(scanctx, vec);
	}
}

/*
 * Apply a function to each chunk in the scan context's hash table. If the limit
 * is greater than zero only a limited number of chunks will be processed.
 *
 * The chunk handler function (on_chunk_func) should return CHUNK_PROCESSED if
 * the chunk should be considered processed and count towards the given
 * limit. CHUNK_IGNORE can be returned to have a chunk NOT count towards the
 * limit. CHUNK_DONE counts the chunk but aborts processing irrespective of
 * whether the limit is reached or not.
 *
 * Returns the number of processed chunks.
 */
static int
chunk_scan_ctx_foreach_chunk(ChunkScanCtx *ctx, on_chunk_func on_chunk, uint16 limit)
{
	HASH_SEQ_STATUS status;
	ChunkScanEntry *entry;
	uint16 num_processed = 0;

	hash_seq_init(&status, ctx->htab);

	for (entry = hash_seq_search(&status); entry != NULL; entry = hash_seq_search(&status))
	{
		switch (on_chunk(ctx, entry->chunk))
		{
			case CHUNK_DONE:
				num_processed++;
				hash_seq_term(&status);
				return num_processed;
			case CHUNK_PROCESSED:
				num_processed++;

				if (limit > 0 && num_processed == limit)
				{
					hash_seq_term(&status);
					return num_processed;
				}
				break;
			case CHUNK_IGNORED:
				break;
		}
	}

	return num_processed;
}

static ChunkResult
set_complete_chunk(ChunkScanCtx *scanctx, Chunk *chunk)
{
	if (chunk_is_complete(chunk, scanctx->space))
	{
		scanctx->data = chunk;
#ifdef USE_ASSERT_CHECKING
		return CHUNK_PROCESSED;
#else
		return CHUNK_DONE;
#endif
	}
	return CHUNK_IGNORED;
}

static ChunkResult
chunk_scan_context_add_chunk(ChunkScanCtx *scanctx, Chunk *chunk)
{
	Chunk **chunks = (Chunk **) scanctx->data;

	chunk_fill_stub(chunk, false);
	*chunks = chunk;
	scanctx->data = chunks + 1;
	return CHUNK_PROCESSED;
}

/* Finds the first chunk that has a complete set of constraints. There should be
 * only one such chunk in the scan context when scanning for the chunk that
 * holds a particular tuple/point. */
static Chunk *
chunk_scan_ctx_get_chunk(ChunkScanCtx *ctx)
{
	ctx->data = NULL;

#ifdef USE_ASSERT_CHECKING
	{
		int n = chunk_scan_ctx_foreach_chunk(ctx, set_complete_chunk, 0);

		Assert(n == 0 || n == 1);
	}
#else
	chunk_scan_ctx_foreach_chunk(ctx, set_complete_chunk, 1);
#endif

	return ctx->data;
}

/*
 * Find a chunk matching a point in a hypertable's N-dimensional hyperspace.
 *
 * This involves:
 *
 * 1) For each dimension:
 *	  - Find all dimension slices that match the dimension
 * 2) For each dimension slice:
 *	  - Find all chunk constraints matching the dimension slice
 * 3) For each matching chunk constraint
 *	  - Insert a (stub) chunk in a hash table and add the constraint to the chunk
 *	  - If chunk already exists in hash table, add the constraint to the chunk
 * 4) At the end of the scan, only one chunk in the hash table should have
 *	  N number of constraints. This is the matching chunk.
 *
 * NOTE: this function allocates transient data, e.g., dimension slice,
 * constraints and chunks, that in the end are not part of the returned
 * chunk. Therefore, this scan should be executed on a transient memory
 * context. The returned chunk needs to be copied into another memory context in
 * case it needs to live beyond the lifetime of the other data.
 */
Chunk *
ts_chunk_find(Hyperspace *hs, Point *p)
{
	Chunk *chunk;
	ChunkScanCtx ctx;

	/* The scan context will keep the state accumulated during the scan */
	chunk_scan_ctx_init(&ctx, hs, p);

	/* Abort the scan when the chunk is found */
	ctx.early_abort = true;

	/* Scan for the chunk matching the point */
	chunk_point_scan(&ctx, p);

	/* Find the chunk that has N matching dimension constraints */
	chunk = chunk_scan_ctx_get_chunk(&ctx);

	chunk_scan_ctx_destroy(&ctx);

	if (NULL != chunk)
	{
		/* Fill in the rest of the chunk's data from the chunk table */
		chunk_fill_stub(chunk, false);

		/*
		 * When searching for the chunk that matches the point, we only
		 * scanned for dimensional constraints. We now need to rescan the
		 * constraints to also get the inherited constraints.
		 */
		chunk->constraints = ts_chunk_constraint_scan_by_chunk_id(chunk->fd.id,
																  hs->num_dimensions,
																  CurrentMemoryContext);
	}

	return chunk;
}

/*
 * Find all the chunks in hyperspace that include
 * elements (dimension slices) calculated by given range constraints and return the corresponding
 * ChunkScanCxt. It is the caller's responsibility to destroy this context after usage.
 */
static ChunkScanCtx *
chunks_find_all_in_range_limit(Hyperspace *hs, Dimension *time_dim, StrategyNumber start_strategy,
							   int64 start_value, StrategyNumber end_strategy, int64 end_value,
							   int limit, uint64 *num_found)
{
	ChunkScanCtx *ctx = palloc(sizeof(ChunkScanCtx));
	DimensionVec *slices;

	Assert(hs != NULL);

	/* must have been checked earlier that this is the case */
	Assert(time_dim != NULL);

	slices = ts_dimension_slice_scan_range_limit(time_dim->fd.id,
												 start_strategy,
												 start_value,
												 end_strategy,
												 end_value,
												 limit);

	/* The scan context will keep the state accumulated during the scan */
	chunk_scan_ctx_init(ctx, hs, NULL);

	/* No abort when the first chunk is found */
	ctx->early_abort = false;

	/* Scan for chunks that are in range */
	dimension_slice_and_chunk_constraint_join(ctx, slices);

	*num_found += hash_get_num_entries(ctx->htab);
	return ctx;
}

static ChunkScanCtx *
chunks_typecheck_and_find_all_in_range_limit(Hyperspace *hs, Dimension *time_dim,
											 Datum older_than_datum, Oid older_than_type,
											 Datum newer_than_datum, Oid newer_than_type, int limit,
											 MemoryContext multi_call_memory_ctx, char *caller_name,
											 uint64 *num_found)
{
	ChunkScanCtx *chunk_ctx = NULL;
	FormData_ts_interval *invl;

	int64 older_than = -1;
	int64 newer_than = -1;

	StrategyNumber start_strategy = InvalidStrategy;
	StrategyNumber end_strategy = InvalidStrategy;

	MemoryContext oldcontext;

	if (time_dim == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("no time dimension found")));

	if (older_than_type != InvalidOid)
	{
		Oid partitioning_type = ts_dimension_get_partition_type(time_dim);
		ts_dimension_open_typecheck(older_than_type, partitioning_type, caller_name);

		if (older_than_type == INTERVALOID)
		{
			invl = ts_interval_from_sql_input(hs->main_table_relid,
											  older_than_datum,
											  older_than_type,
											  "older_than",
											  caller_name);
			older_than = ts_time_value_to_internal(ts_interval_subtract_from_now(invl, time_dim),
												   partitioning_type);
		}
		else
		{
			older_than = ts_time_value_to_internal(older_than_datum, older_than_type);
		}

		end_strategy = BTLessStrategyNumber;
	}

	if (newer_than_type != InvalidOid)
	{
		Oid partitioning_type = ts_dimension_get_partition_type(time_dim);
		ts_dimension_open_typecheck(newer_than_type, partitioning_type, caller_name);

		if (newer_than_type == INTERVALOID)
		{
			invl = ts_interval_from_sql_input(hs->main_table_relid,
											  newer_than_datum,
											  newer_than_type,
											  "newer_than",
											  caller_name);
			newer_than = ts_time_value_to_internal(ts_interval_subtract_from_now(invl, time_dim),
												   partitioning_type);
		}
		else
		{
			newer_than = ts_time_value_to_internal(newer_than_datum, newer_than_type);
		}

		start_strategy = BTGreaterEqualStrategyNumber;
	}

	if (older_than_type != InvalidOid && newer_than_type != InvalidOid && older_than < newer_than)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("When both older_than and newer_than are specified, "
						"older_than must refer to a time that is more recent than newer_than so "
						"that a valid overlapping range is specified")));

	oldcontext = MemoryContextSwitchTo(multi_call_memory_ctx);
	chunk_ctx = chunks_find_all_in_range_limit(hs,
											   time_dim,
											   start_strategy,
											   newer_than,
											   end_strategy,
											   older_than,
											   limit,
											   num_found);
	MemoryContextSwitchTo(oldcontext);

	return chunk_ctx;
}

static ChunkResult
append_chunk_oid(ChunkScanCtx *scanctx, Chunk *chunk)
{
	if (!chunk_is_complete(chunk, scanctx->space))
		return CHUNK_IGNORED;

	/* Fill in the rest of the chunk's data from the chunk table */
	chunk_fill_stub(chunk, false);

	if (scanctx->lockmode != NoLock)
		LockRelationOid(chunk->table_id, scanctx->lockmode);

	scanctx->data = lappend_oid(scanctx->data, chunk->table_id);

	return CHUNK_PROCESSED;
}

static ChunkResult
append_chunk(ChunkScanCtx *scanctx, Chunk *chunk)
{
	Chunk **chunks = scanctx->data;

	if (!chunk_is_complete(chunk, scanctx->space))
		return CHUNK_IGNORED;

	/* Fill in the rest of the chunk's data from the chunk table */
	chunk_fill_stub(chunk, false);

	if (scanctx->lockmode != NoLock)
		LockRelationOid(chunk->table_id, scanctx->lockmode);

	if (NULL == chunks && scanctx->num_complete_chunks > 0)
		scanctx->data = chunks = palloc(sizeof(Chunk *) * scanctx->num_complete_chunks);

	if (scanctx->num_complete_chunks > 0)
		chunks[--scanctx->num_complete_chunks] = chunk;

	return CHUNK_PROCESSED;
}

static void *
chunk_find_all(Hyperspace *hs, List *dimension_vecs, on_chunk_func on_chunk, LOCKMODE lockmode,
			   unsigned int *num_chunks)
{
	ChunkScanCtx ctx;
	ListCell *lc;

	/* The scan context will keep the state accumulated during the scan */
	chunk_scan_ctx_init(&ctx, hs, NULL);

	/* Do not abort the scan when one chunk is found */
	ctx.early_abort = false;
	ctx.lockmode = lockmode;

	/* Scan all dimensions for slices enclosing the point */
	foreach (lc, dimension_vecs)
	{
		DimensionVec *vec = lfirst(lc);

		dimension_slice_and_chunk_constraint_join(&ctx, vec);
	}

	if (NULL != num_chunks)
		*num_chunks = ctx.num_complete_chunks;

	ctx.data = NULL;
	chunk_scan_ctx_foreach_chunk(&ctx, on_chunk, 0);

	chunk_scan_ctx_destroy(&ctx);

	return ctx.data;
}

Chunk **
ts_chunk_find_all(Hyperspace *hs, List *dimension_vecs, LOCKMODE lockmode, unsigned int *num_chunks)
{
	return chunk_find_all(hs, dimension_vecs, append_chunk, lockmode, num_chunks);
}

List *
ts_chunk_find_all_oids(Hyperspace *hs, List *dimension_vecs, LOCKMODE lockmode)
{
	return chunk_find_all(hs, dimension_vecs, append_chunk_oid, lockmode, NULL);
}

/* show_chunks SQL function handler */
Datum
ts_chunk_show_chunks(PG_FUNCTION_ARGS)
{
	/*
	 * chunks_return_srf is called even when it is not the first call but only
	 * after doing some computation first
	 */
	if (SRF_IS_FIRSTCALL())
	{
		FuncCallContext *funcctx;

		Oid table_relid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
		Datum older_than_datum = PG_GETARG_DATUM(1);
		Datum newer_than_datum = PG_GETARG_DATUM(2);

		/*
		 * get_fn_expr_argtype defaults to UNKNOWNOID if argument is NULL but
		 * making it InvalidOid makes the logic simpler later
		 */
		Oid older_than_type = PG_ARGISNULL(1) ? InvalidOid : get_fn_expr_argtype(fcinfo->flinfo, 1);
		Oid newer_than_type = PG_ARGISNULL(2) ? InvalidOid : get_fn_expr_argtype(fcinfo->flinfo, 2);

		funcctx = SRF_FIRSTCALL_INIT();

		funcctx->user_fctx = chunk_get_chunks_in_time_range(table_relid,
															older_than_datum,
															newer_than_datum,
															older_than_type,
															newer_than_type,
															"show_chunks",
															funcctx->multi_call_memory_ctx,
															&funcctx->max_calls);
	}

	return chunks_return_srf(fcinfo);
}

static Chunk **
chunk_get_chunks_in_time_range(Oid table_relid, Datum older_than_datum, Datum newer_than_datum,
							   Oid older_than_type, Oid newer_than_type, char *caller_name,
							   MemoryContext mctx, uint64 *num_chunks_returned)
{
	ListCell *lc;
	MemoryContext oldcontext;
	ChunkScanCtx **chunk_scan_ctxs;
	Chunk **chunks;
	Chunk **current;
	Cache *hypertable_cache;
	Hypertable *ht;
	Dimension *time_dim;
	Oid time_dim_type = InvalidOid;

	/*
	 * contains the list of hypertables which need to be considered. this is a
	 * list containing a single hypertable if we are passed an invalid table
	 * OID. Otherwise, it will have the list of all hypertables in the system
	 */
	List *hypertables = NIL;
	int ht_index = 0;
	uint64 num_chunks = 0;
	int i;

	if (older_than_type != InvalidOid && newer_than_type != InvalidOid &&
		older_than_type != newer_than_type)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("older_than_type and newer_than_type should have the same type")));

	/*
	 * Cache outside the if block to make sure cached hypertable entry
	 * returned will still be valid in foreach block below
	 */
	hypertable_cache = ts_hypertable_cache_pin();
	if (!OidIsValid(table_relid))
	{
		hypertables = ts_hypertable_get_all();
	}
	else
	{
		ht = ts_hypertable_cache_get_entry(hypertable_cache, table_relid);
		if (!ht)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("table \"%s\" does not exist or is not a hypertable",
							get_rel_name(table_relid))));
		hypertables = list_make1(ht);
	}

	oldcontext = MemoryContextSwitchTo(mctx);
	chunk_scan_ctxs = palloc(sizeof(ChunkScanCtx *) * list_length(hypertables));
	MemoryContextSwitchTo(oldcontext);
	foreach (lc, hypertables)
	{
		ht = lfirst(lc);

		time_dim = hyperspace_get_open_dimension(ht->space, 0);

		if (time_dim_type == InvalidOid)
			time_dim_type = ts_dimension_get_partition_type(time_dim);

		/*
		 * Even though internally all time columns are represented as bigints,
		 * it is locally unclear what set of chunks should be returned if
		 * there are multiple tables on the system some of which care about
		 * timestamp when others do not. That is why, whenever there is any
		 * time dimension constraint given as an argument (older_than or
		 * newer_than) we make sure all hypertables have the time dimension
		 * type of the given type or through an error. This check is done
		 * across hypertables that is why it is not in the helper function
		 * below.
		 */
		if (time_dim_type != ts_dimension_get_partition_type(time_dim) &&
			(older_than_type != InvalidOid || newer_than_type != InvalidOid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("cannot call \"%s\" on all hypertables "
							"when all hypertables do not have the same time dimension type",
							caller_name)));

		chunk_scan_ctxs[ht_index++] = chunks_typecheck_and_find_all_in_range_limit(ht->space,
																				   time_dim,
																				   older_than_datum,
																				   older_than_type,
																				   newer_than_datum,
																				   newer_than_type,
																				   -1,
																				   mctx,
																				   caller_name,
																				   &num_chunks);
	}
	oldcontext = MemoryContextSwitchTo(mctx);

	/*
	 * num_chunks can safely be 0 as palloc protects against unportable
	 * behavior.
	 */
	chunks = palloc(sizeof(Chunk *) * num_chunks);
	current = chunks;

	MemoryContextSwitchTo(oldcontext);
	for (i = 0; i < list_length(hypertables); i++)
	{
		/* Get all the chunks from the context */
		chunk_scan_ctxs[i]->data = current;
		chunk_scan_ctx_foreach_chunk(chunk_scan_ctxs[i], chunk_scan_context_add_chunk, -1);

		current = chunk_scan_ctxs[i]->data;

		/*
		 * only affects ctx.htab Got all the chunk already so can now safely
		 * destroy the context
		 */
		chunk_scan_ctx_destroy(chunk_scan_ctxs[i]);
	}

	qsort(chunks, num_chunks, sizeof(Chunk *), chunk_cmp);

	*num_chunks_returned = num_chunks;
	ts_cache_release(hypertable_cache);
	return chunks;
}

List *
ts_chunk_data_nodes_copy(Chunk *chunk)
{
	List *lcopy = NIL;
	ListCell *lc;

	foreach (lc, chunk->data_nodes)
	{
		ChunkDataNode *node = lfirst(lc);
		ChunkDataNode *copy = palloc(sizeof(ChunkDataNode));

		memcpy(copy, node, sizeof(ChunkDataNode));

		lcopy = lappend(lcopy, copy);
	}

	return lcopy;
}

Chunk *
ts_chunk_copy(Chunk *chunk)
{
	Chunk *copy;

	copy = palloc(sizeof(Chunk));
	memcpy(copy, chunk, sizeof(Chunk));

	if (NULL != chunk->constraints)
		copy->constraints = ts_chunk_constraints_copy(chunk->constraints);

	if (NULL != chunk->cube)
		copy->cube = ts_hypercube_copy(chunk->cube);

	copy->data_nodes = ts_chunk_data_nodes_copy(chunk);

	return copy;
}

static int
chunk_scan_internal(int indexid, ScanKeyData scankey[], int nkeys, tuple_found_func tuple_found,
					void *data, int limit, ScanDirection scandir, LOCKMODE lockmode,
					MemoryContext mctx)
{
	Catalog *catalog = ts_catalog_get();
	ScannerCtx ctx = {
		.table = catalog_get_table_id(catalog, CHUNK),
		.index = catalog_get_index(catalog, CHUNK, indexid),
		.nkeys = nkeys,
		.data = data,
		.scankey = scankey,
		.tuple_found = tuple_found,
		.limit = limit,
		.lockmode = lockmode,
		.scandirection = scandir,
		.result_mctx = mctx,
	};

	return ts_scanner_scan(&ctx);
}

/*
 * Get a window of chunks that "precede" the given dimensional point.
 *
 * For instance, if the dimension is "time", then given a point in time the
 * function returns the recent chunks that come before the chunk that includes
 * that point. The count parameter determines the number or slices the window
 * should include in the given dimension. Note, that with multi-dimensional
 * partitioning, there might be multiple chunks in each dimensional slice that
 * all precede the given point. For instance, the example below shows two
 * different situations that each go "back" two slices (count = 2) in the
 * x-dimension, but returns two vs. eight chunks due to different
 * partitioning.
 *
 * '_____________
 * '|   |   | * |
 * '|___|___|___|
 * '
 * '
 * '____ ________
 * '|   |   | * |
 * '|___|___|___|
 * '|   |   |   |
 * '|___|___|___|
 * '|   |   |   |
 * '|___|___|___|
 * '|   |   |   |
 * '|___|___|___|
 *
 * Note that the returned chunks will be allocated on the given memory
 * context, including the list itself. So, beware of not leaking the list if
 * the chunks are later cached somewhere else.
 */
List *
ts_chunk_get_window(int32 dimension_id, int64 point, int count, MemoryContext mctx)
{
	List *chunks = NIL;
	DimensionVec *dimvec;
	int i;

	/* Scan for "count" slices that precede the point in the given dimension */
	dimvec = ts_dimension_slice_scan_by_dimension_before_point(dimension_id,
															   point,
															   count,
															   BackwardScanDirection,
															   mctx);

	/*
	 * For each slice, join with any constraints that reference the slice.
	 * There might be multiple constraints for each slice in case of
	 * multi-dimensional partitioning.
	 */
	for (i = 0; i < dimvec->num_slices; i++)
	{
		DimensionSlice *slice = dimvec->slices[i];
		ChunkConstraints *ccs = ts_chunk_constraints_alloc(1, mctx);
		int j;

		ts_chunk_constraint_scan_by_dimension_slice_id(slice->fd.id, ccs, mctx);

		/* For each constraint, find the corresponding chunk */
		for (j = 0; j < ccs->num_constraints; j++)
		{
			ChunkConstraint *cc = &ccs->constraints[j];
			Chunk *chunk = ts_chunk_get_by_id(cc->fd.chunk_id, 0, true);
			MemoryContext old;

			chunk->constraints = ts_chunk_constraint_scan_by_chunk_id(chunk->fd.id, 1, mctx);
			chunk->cube = ts_hypercube_from_constraints(chunk->constraints, mctx);

			/* Allocate the list on the same memory context as the chunks */
			old = MemoryContextSwitchTo(mctx);
			chunks = lappend(chunks, chunk);
			MemoryContextSwitchTo(old);
		}
	}

	return chunks;
}

static Chunk *
chunk_scan_find(int indexid, ScanKeyData scankey[], int nkeys, int16 num_constraints,
				MemoryContext mctx, bool fail_if_not_found)
{
	Chunk *chunk = MemoryContextAllocZero(mctx, sizeof(Chunk));
	int num_found;

	num_found = chunk_scan_internal(indexid,
									scankey,
									nkeys,
									chunk_tuple_found,
									chunk,
									1,
									ForwardScanDirection,
									AccessShareLock,
									mctx);

	switch (num_found)
	{
		case 0:
			if (fail_if_not_found)
				elog(ERROR, "chunk not found");
			pfree(chunk);
			chunk = NULL;
			break;
		case 1:
			if (num_constraints > 0)
			{
				chunk->constraints =
					ts_chunk_constraint_scan_by_chunk_id(chunk->fd.id, num_constraints, mctx);
				chunk->cube = ts_hypercube_from_constraints(chunk->constraints, mctx);
			}

			if (chunk->relkind == RELKIND_FOREIGN_TABLE)
				chunk->data_nodes = ts_chunk_data_node_scan_by_chunk_id(chunk->fd.id, mctx);

			break;
		default:
			elog(ERROR, "unexpected number of chunks found: %d", num_found);
	}

	return chunk;
}

TSDLLEXPORT Chunk *
ts_chunk_get_by_name_with_memory_context(const char *schema_name, const char *table_name,
										 int16 num_constraints, MemoryContext mctx,
										 bool fail_if_not_found)
{
	NameData schema, table;
	ScanKeyData scankey[2];

	namestrcpy(&schema, schema_name);
	namestrcpy(&table, table_name);

	/*
	 * Perform an index scan on chunk name.
	 */
	ScanKeyInit(&scankey[0],
				Anum_chunk_schema_name_idx_schema_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				NameGetDatum(&schema));
	ScanKeyInit(&scankey[1],
				Anum_chunk_schema_name_idx_table_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				NameGetDatum(&table));

	return chunk_scan_find(CHUNK_SCHEMA_NAME_INDEX,
						   scankey,
						   2,
						   num_constraints,
						   mctx,
						   fail_if_not_found);
}

TSDLLEXPORT Chunk *
ts_chunk_get_by_relid(Oid relid, int16 num_constraints, bool fail_if_not_found)
{
	char *schema;
	char *table;

	if (!OidIsValid(relid))
		return NULL;

	schema = get_namespace_name(get_rel_namespace(relid));
	table = get_rel_name(relid);
	return chunk_get_by_name(schema, table, num_constraints, fail_if_not_found);
}

/* Lookup a chunk_id from a chunk's relid.
 * Optimize with memoization
 */
TSDLLEXPORT Datum
ts_chunk_id_from_relid(PG_FUNCTION_ARGS)
{
	static Oid last_relid = InvalidOid;
	static int32 last_id = 0;
	Oid relid = PG_GETARG_OID(0);
	Chunk *chunk;

	if (last_relid == relid)
		return last_id;

	chunk = ts_chunk_get_by_relid(relid, 0, true);
	last_relid = relid;
	last_id = chunk->fd.id;
	return last_id;
}

Chunk *
ts_chunk_get_by_id(int32 id, int16 num_constraints, bool fail_if_not_found)
{
	ScanKeyData scankey[1];

	/*
	 * Perform an index scan on chunk id.
	 */
	ScanKeyInit(&scankey[0], Anum_chunk_idx_id, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(id));

	return chunk_scan_find(CHUNK_ID_INDEX,
						   scankey,
						   1,
						   num_constraints,
						   CurrentMemoryContext,
						   fail_if_not_found);
}

static ScanTupleResult
chunk_form_tuple_found(TupleInfo *ti, void *data)
{
	FormData_chunk *form = data;

	memcpy(form, GETSTRUCT(ti->tuple), sizeof(FormData_chunk));

	return SCAN_DONE;
}

static ScanTupleResult
chunk_form_tuples_found(TupleInfo *ti, void *arg)
{
	List **chunkids = arg;
	FormData_chunk *form = palloc(sizeof(FormData_chunk));

	chunk_form_tuple_found(ti, form);

	*chunkids = lappend_int(*chunkids, form->id);
	return SCAN_CONTINUE;
}

static bool
chunk_get_form(int32 chunk_id, FormData_chunk *form, bool missing_ok)
{
	ScanKeyData scankey[1];
	int num_found;

	/*
	 * Perform an index scan on chunk id.
	 */
	ScanKeyInit(&scankey[0], Anum_chunk_idx_id, BTEqualStrategyNumber, F_INT4EQ, chunk_id);

	num_found = chunk_scan_internal(CHUNK_ID_INDEX,
									scankey,
									1,
									chunk_form_tuple_found,
									form,
									0,
									ForwardScanDirection,
									AccessShareLock,
									CurrentMemoryContext);

	if (num_found == 0 && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("chunk with ID %u does not exist", chunk_id)));

	return num_found == 1;
}

/*
 * Get the relid of a chunk given its ID.
 *
 * This is a lightweight way to get the relid of a chunk that does not require
 * getting a full Chunk object.
 */
Oid
ts_chunk_get_relid(int32 chunk_id, bool missing_ok)
{
	FormData_chunk form = { 0 };
	Oid schemaid, relid;

	if (chunk_get_form(chunk_id, &form, missing_ok) == 0)
		return InvalidOid;

	schemaid = get_namespace_oid(NameStr(form.schema_name), missing_ok);

	if (!OidIsValid(schemaid))
		return InvalidOid;

	relid = get_relname_relid(NameStr(form.table_name), schemaid);

	if (!OidIsValid(relid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("table \"%s.%s\" does not exist",
						NameStr(form.schema_name),
						NameStr(form.table_name))));

	return relid;
}

/*
 * Get the schema (namespace) of a chunk given its ID.
 *
 * This is a lightweight way to get the schema of a chunk that does not
 * require getting a full Chunk object.
 */
Oid
ts_chunk_get_schema_id(int32 chunk_id, bool missing_ok)
{
	FormData_chunk form = { 0 };

	if (chunk_get_form(chunk_id, &form, missing_ok) == 0)
		return InvalidOid;

	return get_namespace_oid(NameStr(form.schema_name), missing_ok);
}

bool
ts_chunk_exists_relid(Oid relid)
{
	return ts_chunk_get_by_relid(relid, 0, false) != NULL;
}

static ScanTupleResult
chunk_tuple_delete(TupleInfo *ti, void *data)
{
	FormData_chunk *form = (FormData_chunk *) GETSTRUCT(ti->tuple);
	CatalogSecurityContext sec_ctx;
	ChunkConstraints *ccs = ts_chunk_constraints_alloc(2, ti->mctx);
	int i;

	ts_chunk_constraint_delete_by_chunk_id(form->id, ccs);
	ts_chunk_index_delete_by_chunk_id(form->id, true);
	ts_chunk_data_node_delete_by_chunk_id(form->id);

	/* Check for dimension slices that are orphaned by the chunk deletion */
	for (i = 0; i < ccs->num_constraints; i++)
	{
		ChunkConstraint *cc = &ccs->constraints[i];

		/*
		 * Delete the dimension slice if there are no remaining constraints
		 * referencing it
		 */
		if (is_dimension_constraint(cc) &&
			ts_chunk_constraint_scan_by_dimension_slice_id(cc->fd.dimension_slice_id,
														   NULL,
														   CurrentMemoryContext) == 0)
			ts_dimension_slice_delete_by_id(cc->fd.dimension_slice_id, false);
	}

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_delete(ti->scanrel, ti->tuple);
	ts_catalog_restore_user(&sec_ctx);

	return SCAN_CONTINUE;
}

int
ts_chunk_delete_by_name(const char *schema, const char *table)
{
	ScanKeyData scankey[2];

	ScanKeyInit(&scankey[0],
				Anum_chunk_schema_name_idx_schema_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				DirectFunctionCall1(namein, CStringGetDatum(schema)));
	ScanKeyInit(&scankey[1],
				Anum_chunk_schema_name_idx_table_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				DirectFunctionCall1(namein, CStringGetDatum(table)));

	return chunk_scan_internal(CHUNK_SCHEMA_NAME_INDEX,
							   scankey,
							   2,
							   chunk_tuple_delete,
							   NULL,
							   0,
							   ForwardScanDirection,
							   RowExclusiveLock,
							   CurrentMemoryContext);
}

bool
ts_chunk_get_id(const char *schema, const char *table, int32 *chunk_id)
{
	ScanKeyData scankey[2];
	FormData_chunk form;
	int num_found;

	ScanKeyInit(&scankey[0],
				Anum_chunk_schema_name_idx_schema_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				DirectFunctionCall1(namein, CStringGetDatum(schema)));
	ScanKeyInit(&scankey[1],
				Anum_chunk_schema_name_idx_table_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				DirectFunctionCall1(namein, CStringGetDatum(table)));

	num_found = chunk_scan_internal(CHUNK_SCHEMA_NAME_INDEX,
									scankey,
									2,
									chunk_form_tuple_found,
									&form,
									0,
									ForwardScanDirection,
									RowExclusiveLock,
									CurrentMemoryContext);

	Assert(num_found == 1 || num_found == 0);

	if (num_found != 1)
		return false;

	if (NULL != chunk_id)
		*chunk_id = form.id;

	return true;
}

int
ts_chunk_delete_by_relid(Oid relid)
{
	if (!OidIsValid(relid))
		return 0;

	return ts_chunk_delete_by_name(get_namespace_name(get_rel_namespace(relid)),
								   get_rel_name(relid));
}

int
ts_chunk_delete_by_hypertable_id(int32 hypertable_id)
{
	ScanKeyData scankey[1];

	ScanKeyInit(&scankey[0],
				Anum_chunk_hypertable_id_idx_hypertable_id,
				BTEqualStrategyNumber,
				F_INT4EQ,
				Int32GetDatum(hypertable_id));

	return chunk_scan_internal(CHUNK_HYPERTABLE_ID_INDEX,
							   scankey,
							   1,
							   chunk_tuple_delete,
							   NULL,
							   0,
							   ForwardScanDirection,
							   RowExclusiveLock,
							   CurrentMemoryContext);
}

TSDLLEXPORT List *
ts_chunk_find_chunk_ids_by_hypertable_id(int32 hypertable_id)
{
	List *chunkids = NIL;
	ScanKeyData scankey[1];

	ScanKeyInit(&scankey[0],
				Anum_chunk_hypertable_id_idx_hypertable_id,
				BTEqualStrategyNumber,
				F_INT4EQ,
				Int32GetDatum(hypertable_id));

	chunk_scan_internal(CHUNK_HYPERTABLE_ID_INDEX,
						scankey,
						1,
						chunk_form_tuples_found,
						&chunkids,
						0,
						ForwardScanDirection,
						AccessShareLock,
						CurrentMemoryContext);
	return chunkids;
}

static ChunkResult
chunk_recreate_constraint(ChunkScanCtx *ctx, Chunk *chunk)
{
	ChunkConstraints *ccs = chunk->constraints;
	int i;

	chunk_fill_stub(chunk, false);

	for (i = 0; i < ccs->num_constraints; i++)
		ts_chunk_constraint_recreate(&ccs->constraints[i], chunk->table_id);

	return CHUNK_PROCESSED;
}

void
ts_chunk_recreate_all_constraints_for_dimension(Hyperspace *hs, int32 dimension_id)
{
	DimensionVec *slices;
	ChunkScanCtx chunkctx;
	int i;

	slices = ts_dimension_slice_scan_by_dimension(dimension_id, 0);

	if (NULL == slices)
		return;

	chunk_scan_ctx_init(&chunkctx, hs, NULL);

	for (i = 0; i < slices->num_slices; i++)
		ts_chunk_constraint_scan_by_dimension_slice(slices->slices[i],
													&chunkctx,
													CurrentMemoryContext);

	chunk_scan_ctx_foreach_chunk(&chunkctx, chunk_recreate_constraint, 0);
	chunk_scan_ctx_destroy(&chunkctx);
}

static ScanTupleResult
chunk_tuple_update(TupleInfo *ti, void *data)
{
	HeapTuple tuple = heap_copytuple(ti->tuple);
	FormData_chunk *form = (FormData_chunk *) GETSTRUCT(tuple);
	FormData_chunk *update = data;
	CatalogSecurityContext sec_ctx;

	namecpy(&form->schema_name, &update->schema_name);
	namecpy(&form->table_name, &update->table_name);

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_update(ti->scanrel, tuple);
	ts_catalog_restore_user(&sec_ctx);

	heap_freetuple(tuple);

	return SCAN_DONE;
}

static bool
chunk_update_form(FormData_chunk *form)
{
	ScanKeyData scankey[1];

	ScanKeyInit(&scankey[0], Anum_chunk_idx_id, BTEqualStrategyNumber, F_INT4EQ, form->id);

	return chunk_scan_internal(CHUNK_ID_INDEX,
							   scankey,
							   1,
							   chunk_tuple_update,
							   form,
							   0,
							   ForwardScanDirection,
							   AccessShareLock,
							   CurrentMemoryContext) > 0;
}

bool
ts_chunk_set_name(Chunk *chunk, const char *newname)
{
	namestrcpy(&chunk->fd.table_name, newname);

	return chunk_update_form(&chunk->fd);
}

bool
ts_chunk_set_schema(Chunk *chunk, const char *newschema)
{
	namestrcpy(&chunk->fd.schema_name, newschema);

	return chunk_update_form(&chunk->fd);
}

/* Used as a tuple found function */
static ScanTupleResult
chunk_rename_schema_name(TupleInfo *ti, void *data)
{
	HeapTuple tuple = heap_copytuple(ti->tuple);
	FormData_chunk *chunk = (FormData_chunk *) GETSTRUCT(tuple);

	/* Rename schema name */
	namestrcpy(&chunk->schema_name, (char *) data);
	ts_catalog_update(ti->scanrel, tuple);
	heap_freetuple(tuple);

	return SCAN_CONTINUE;
}

/* Go through the internal chunk table and rename all matching schemas */
void
ts_chunks_rename_schema_name(char *old_schema, char *new_schema)
{
	NameData old_schema_name;
	ScanKeyData scankey[1];
	Catalog *catalog = ts_catalog_get();
	ScannerCtx scanctx = {
		.table = catalog_get_table_id(catalog, CHUNK),
		.index = catalog_get_index(catalog, CHUNK, CHUNK_SCHEMA_NAME_INDEX),
		.nkeys = 1,
		.scankey = scankey,
		.tuple_found = chunk_rename_schema_name,
		.data = new_schema,
		.lockmode = RowExclusiveLock,
		.scandirection = ForwardScanDirection,
	};

	namestrcpy(&old_schema_name, old_schema);

	ScanKeyInit(&scankey[0],
				Anum_chunk_schema_name_idx_schema_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				NameGetDatum(&old_schema_name));

	ts_scanner_scan(&scanctx);
}

static int
chunk_cmp(const void *ch1, const void *ch2)
{
	const Chunk *v1 = *((const Chunk **) ch1);
	const Chunk *v2 = *((const Chunk **) ch2);

	if (v1->fd.hypertable_id < v2->fd.hypertable_id)
		return -1;
	if (v1->fd.hypertable_id > v2->fd.hypertable_id)
		return 1;
	if (v1->table_id < v2->table_id)
		return -1;
	if (v1->table_id > v2->table_id)
		return 1;
	return 0;
}

/*
 * This is a helper set returning function (SRF) that takes a set returning function context and as
 * argument and returns oids extracted from funcctx->user_fctx (which is Chunk* array).
 * Note that the caller needs to be registered as a
 * set returning function for this to work.
 */
static Datum
chunks_return_srf(FunctionCallInfo fcinfo)
{
	FuncCallContext *funcctx;
	uint64 call_cntr;
	TupleDesc tupdesc;
	Chunk **result_set;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* Build a tuple descriptor for our result type */
		/* not quite necessary */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_SCALAR)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	result_set = (Chunk **) funcctx->user_fctx;

	/* do when there is more left to send */
	if (call_cntr < funcctx->max_calls)
		SRF_RETURN_NEXT(funcctx, result_set[call_cntr]->table_id);
	else /* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}

List *
ts_chunk_do_drop_chunks(Oid table_relid, Datum older_than_datum, Datum newer_than_datum,
						Oid older_than_type, Oid newer_than_type, bool cascade,
						bool cascades_to_materializations, int32 log_level)
{
	int i = 0;
	uint64 num_chunks = 0;
	Chunk **chunks;
	List *dropped_chunk_names = NIL;
	const char *schema_name, *table_name;
	int32 hypertable_id = ts_hypertable_relid_to_id(table_relid);

	ts_hypertable_permissions_check(table_relid, GetUserId());

	switch (ts_continuous_agg_hypertable_status(hypertable_id))
	{
		case HypertableIsMaterialization:
		case HypertableIsMaterializationAndRaw:
			elog(ERROR, "cannot drop_chunks on a continuous aggregate materialization table");
		case HypertableIsRawTable:
			if (!cascades_to_materializations)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("cannot drop_chunks on hypertable that has a continuous aggregate "
								"without cascade_to_materializations set to true")));
			break;
		default:
			cascades_to_materializations = false;
			break;
	}

	chunks = chunk_get_chunks_in_time_range(table_relid,
											older_than_datum,
											newer_than_datum,
											older_than_type,
											newer_than_type,
											"drop_chunks",
											CurrentMemoryContext,
											&num_chunks);

	for (; i < num_chunks; i++)
	{
		size_t len;
		char *chunk_name;

		ObjectAddress objaddr = {
			.classId = RelationRelationId,
			.objectId = chunks[i]->table_id,
		};

		elog(log_level,
			 "dropping chunk %s.%s",
			 chunks[i]->fd.schema_name.data,
			 chunks[i]->fd.table_name.data);

		/* Store chunk name for output */
		schema_name = quote_identifier(chunks[i]->fd.schema_name.data);
		table_name = quote_identifier(chunks[i]->fd.table_name.data);

		len = strlen(schema_name) + strlen(table_name) + 2;
		chunk_name = palloc(len);

		snprintf(chunk_name, len, "%s.%s", schema_name, table_name);
		dropped_chunk_names = lappend(dropped_chunk_names, chunk_name);

		/* Remove the chunk from the hypertable table */
		ts_chunk_delete_by_relid(chunks[i]->table_id);

		/* Drop the table */
		performDeletion(&objaddr, cascade, 0);
	}

	if (cascades_to_materializations)
		ts_cm_functions->continuous_agg_drop_chunks_by_chunk_id(hypertable_id, chunks, num_chunks);

	return dropped_chunk_names;
}

/*
 * This is a helper set returning function (SRF) that takes a set returning function context and as
 * argument and returns cstrings extracted from funcctx->user_fctx (which is a List).
 * Note that the caller needs to be registered as a
 * set returning function for this to work.
 */
static Datum
list_return_srf(FunctionCallInfo fcinfo)
{
	FuncCallContext *funcctx;
	uint64 call_cntr;
	TupleDesc tupdesc;
	List *result_set;
	Datum retval;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* Build a tuple descriptor for our result type */
		/* not quite necessary */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_SCALAR)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	result_set = (List *) funcctx->user_fctx;

	/* do when there is more left to send */
	if (call_cntr < funcctx->max_calls)
	{
		/* store return value and increment linked list */
		retval = CStringGetTextDatum(linitial(result_set));
		funcctx->user_fctx = list_delete_first(result_set);
		SRF_RETURN_NEXT(funcctx, retval);
	}
	else /* do when there is no more left */
		SRF_RETURN_DONE(funcctx);
}

Datum
ts_chunk_drop_chunks(PG_FUNCTION_ARGS)
{
	MemoryContext oldcontext;
	FuncCallContext *funcctx;
	ListCell *lc;
	List *ht_oids, *dc_names = NIL;

	Name table_name, schema_name;
	Datum older_than_datum, newer_than_datum;

	Oid older_than_type, newer_than_type;
	bool cascade, verbose, cascades_to_materializations;
	int elevel;

	/*
	 * When past the first call of the SRF, dropping has already been completed,
	 * so we just return the next chunk in the list of dropped chunks.
	 */
	if (!SRF_IS_FIRSTCALL())
		return list_return_srf(fcinfo);

	table_name = PG_ARGISNULL(1) ? NULL : PG_GETARG_NAME(1);
	schema_name = PG_ARGISNULL(2) ? NULL : PG_GETARG_NAME(2);
	older_than_datum = PG_GETARG_DATUM(0);
	newer_than_datum = PG_GETARG_DATUM(4);

	/* Making types InvalidOid makes the logic simpler later */
	older_than_type = PG_ARGISNULL(0) ? InvalidOid : get_fn_expr_argtype(fcinfo->flinfo, 0);
	newer_than_type = PG_ARGISNULL(4) ? InvalidOid : get_fn_expr_argtype(fcinfo->flinfo, 4);
	cascade = PG_GETARG_BOOL(3);
	verbose = PG_ARGISNULL(5) ? false : PG_GETARG_BOOL(5);
	cascades_to_materializations = PG_ARGISNULL(6) ? false : PG_GETARG_BOOL(6);
	elevel = verbose ? INFO : DEBUG2;

	if (PG_ARGISNULL(0) && PG_ARGISNULL(4))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("older_than and newer_than timestamps provided to drop_chunks cannot both "
						"be NULL")));

	ht_oids = ts_hypertable_get_all_by_name(schema_name, table_name, CurrentMemoryContext);

	if (table_name != NULL)
	{
		if (ht_oids == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_TS_HYPERTABLE_NOT_EXIST),
					 errmsg("hypertable \"%s\" does not exist", NameStr(*table_name))));
	}

	/* Initial multi function call setup */
	funcctx = SRF_FIRSTCALL_INIT();

	/* Drop chunks and build list of dropped chunks */
	foreach (lc, ht_oids)
	{
		Oid table_relid = lfirst_oid(lc);
		List *fk_relids = NIL;
		List *dc_temp = NIL;
		ListCell *lf;

		ts_hypertable_permissions_check(table_relid, GetUserId());

		/* get foreign key tables associated with the hypertable */
		{
			List *cachedfkeys = NIL;
			ListCell *lf;
			Relation table_rel;

			table_rel = heap_open(table_relid, AccessShareLock);

			/*
			 * this list is from the relcache and can disappear with a cache
			 * flush, so no further catalog access till we save the fk relids
			 */
			cachedfkeys = RelationGetFKeyList(table_rel);
			foreach (lf, cachedfkeys)
			{
				ForeignKeyCacheInfo *cachedfk = (ForeignKeyCacheInfo *) lfirst(lf);

				/*
				 * conrelid should always be that of the table we're
				 * considering
				 */
				Assert(cachedfk->conrelid == RelationGetRelid(table_rel));
				fk_relids = lappend_oid(fk_relids, cachedfk->confrelid);
			}
			heap_close(table_rel, AccessShareLock);
		}

		/*
		 * We have a FK between hypertable H and PAR. Hypertable H has number
		 * of chunks C1, C2, etc. When we execute "drop table C", PG acquires
		 * locks on C and PAR. If we have a query as "select * from
		 * hypertable", this acquires a lock on C and PAR as well. But the
		 * order of the locks is not the same and results in deadlocks. -
		 * github issue 865 We hope to alleviate the problem by acquiring a
		 * lock on PAR before executing the drop table stmt. This is not
		 * fool-proof as we could have multiple fkrelids and the order of lock
		 * acquisition for these could differ as well. Do not unlock - let the
		 * transaction semantics take care of it.
		 */
		foreach (lf, fk_relids)
		{
			LockRelationOid(lfirst_oid(lf), AccessExclusiveLock);
		}

		/* Drop chunks and store their names for return */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		dc_temp = ts_chunk_do_drop_chunks(table_relid,
										  older_than_datum,
										  newer_than_datum,
										  older_than_type,
										  newer_than_type,
										  cascade,
										  cascades_to_materializations,
										  elevel);
		dc_names = list_concat(dc_names, dc_temp);

		MemoryContextSwitchTo(oldcontext);
	}

	/* store data for multi function call */
	funcctx->max_calls = list_length(dc_names);
	funcctx->user_fctx = dc_names;

	return list_return_srf(fcinfo);
}

/**
 * This function is used to explicitly specify chunks that are being scanned. It's being processed
 * in the planning phase and removed from the query tree. This means that the actual function
 * implementation will only be executed if something went wrong during explicit chunk exclusion.
 */
Datum
ts_chunks_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("illegal invocation of chunks_in function"),
			 errhint("chunks_in function must appear in the WHERE clause and can only be combined "
					 "with AND operator")));
	pg_unreachable();
}

Datum
ts_chunk_show(PG_FUNCTION_ARGS)
{
	return ts_cm_functions->show_chunk(fcinfo);
}

Datum
ts_chunk_create(PG_FUNCTION_ARGS)
{
	return ts_cm_functions->create_chunk(fcinfo);
}
