#include <postgres.h>
#include <utils/rel.h>
#include <commands/trigger.h>

#include "dimension.h"
#include "errors.h"
#include "utils.h"
#include "chunk.h"
#include "hypertable.h"
#include "insert_chunk_state.h"
#include "insert_statement_state.h"
#include "executor.h"

static InsertStatementState *insert_statement_state = NULL;

static inline void
insert_statement_state_init(InsertStatementState **state_p, Oid relid)
{
	if (*state_p == NULL)
		*state_p = insert_statement_state_new(relid);
}

static inline void
insert_statement_state_cleanup(InsertStatementState **state_p)
{
	if (*state_p != NULL)
	{
		insert_statement_state_destroy(*state_p);
		*state_p = NULL;
	}
}


Datum		insert_main_table_trigger(PG_FUNCTION_ARGS);
Datum		insert_main_table_trigger_after(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(insert_main_table_trigger);
PG_FUNCTION_INFO_V1(insert_main_table_trigger_after);

/*
 * This row-level trigger is called for every row INSERTed into a hypertable. We
 * use it to redirect inserted tuples to the correct hypertable chunk in an
 * N-dimensional hyperspace.
 */
Datum
insert_main_table_trigger(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	HeapTuple	tuple;
	Hypertable *ht;
	Point	   *point;
	InsertChunkState *cstate;
	Oid			relid = trigdata->tg_relation->rd_id;
	TupleDesc	tupdesc = trigdata->tg_relation->rd_att;

	PG_TRY();
	{
		/* Check that this is called the way it should be */
		if (!CALLED_AS_TRIGGER(fcinfo))
			elog(ERROR, "Trigger not called by trigger manager");

		if (!TRIGGER_FIRED_BEFORE(trigdata->tg_event))
			elog(ERROR, "Trigger should only fire before insert");

		if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			tuple = trigdata->tg_newtuple;
		else if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			tuple = trigdata->tg_trigtuple;
		else
			elog(ERROR, "Unsupported event for trigger");

		insert_statement_state_init(&insert_statement_state, relid);

		ht = insert_statement_state->hypertable;

		/* Calculate the tuple's point in the N-dimensional hyperspace */
		point = hyperspace_calculate_point(ht->space, tuple, tupdesc);

		/* Find or create the insert state matching the point */
		cstate = insert_statement_state_get_insert_chunk_state(insert_statement_state,
														   ht->space, point);
		/* Insert the tuple into the chunk */
		insert_chunk_state_insert_tuple(cstate, tuple);
	}
	PG_CATCH();
	{
		insert_statement_state_cleanup(&insert_statement_state);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * add 1 to the number of processed tuples in the commandTag. Without this
	 * tuples that return NULL in before triggers are not counted.
	 */
	executor_add_number_tuples_processed(1);

	/* Return NULL since we do not want the tuple in the trigger's table */
	return PointerGetDatum(NULL);
}

/* Trigger called after all tuples of the insert statement are done */

Datum
insert_main_table_trigger_after(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;

	PG_TRY();
	{
		if (!CALLED_AS_TRIGGER(fcinfo))
			elog(ERROR, "not called by trigger manager");

		if (!TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event) &&
			!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
			elog(ERROR, "Unsupported event for trigger");
	}
	PG_CATCH();
	{
		insert_statement_state_cleanup(&insert_statement_state);
		PG_RE_THROW();
	}
	PG_END_TRY();

	insert_statement_state_cleanup(&insert_statement_state);

	return PointerGetDatum(NULL);
}
