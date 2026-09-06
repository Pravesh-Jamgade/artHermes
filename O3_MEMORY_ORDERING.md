# ROB-gated load/store ordering

## Motivation

The previous core allocated a dependent load in the load queue, marked the LQ
entry with its store producer, and later copied the producer's translation and
data into that entry.  Consequently, a dependent operation could occupy an LSQ
slot before its dependency was resolved and could bypass the normal
translation/data pipeline.

## New dependency model

Dependency discovery now occurs while the instruction is scheduled in the
reorder buffer (ROB), before any LQ or SQ allocation.  For each older,
incomplete instruction from the same hardware thread, the core checks:

* **register RAW:** an older destination register matches a younger source;
* **register WAR:** an older source register matches a younger destination;
* **memory RAW:** an older store address matches a younger load address; and
* **memory WAR:** an older load address matches a younger store address.

Only a younger memory instruction needs the new LSQ-admission dependency.  Each
older producer/consumer pair is recorded once in the producer's ROB entry, and
the consumer maintains a count so that multiple older dependencies are handled
correctly.  Existing register RAW execution dependencies remain in place; they
are now also scoped by thread ID so SMT contexts cannot create false hazards.

## Admission and release

A memory instruction may call the LQ/SQ allocation path only when both its
normal register operands are ready and its ROB LSQ-dependency count is zero.
The count is decremented only when each producer becomes `COMPLETED`, not when a
translation or data request happens to return.  Therefore no dependent entry is
present in LQ/SQ while its producer is unresolved.

## Queue flow and forwarding removal

After release, a load allocates a fresh LQ entry and enters `RTL0`; a store
allocates a fresh SQ entry and enters `RTS0`.  Both then perform their own DTLB
translation and proceed to `RTL1`/`RTS1` in the existing queue order.

The old store-to-load shortcut was removed from both LQ insertion and store
execution.  A completing store no longer supplies a physical address, marks a
load fetched, decrements the consumer's memory-operation count, or releases the
consumer LQ entry.  Completion now only clears the ROB admission dependency;
the newly admitted consumer follows the ordinary translation and data paths.

Cache-request merging for independently admitted requests is unchanged.  It is
not dependency forwarding: dependent requests cannot be present in the cache
queues at the same time under this model.
