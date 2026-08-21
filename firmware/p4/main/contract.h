#ifndef CONTRACT_H
#define CONTRACT_H

/* The generated contract, included exactly once per translation unit.
 *
 * cfg_table.inc is generated from contract/car-api.json and carries no include guard
 * of its own — it is a generated file, and a generated file is not edited. Every
 * consumer therefore goes through this header, so that a file needing both link.h
 * (which spells the `ctl` vocabulary from the schema) and the config table does not
 * define the table twice.
 *
 * The table names NULL, which the generated file does not include a header for — it is
 * written to be dropped into a translation unit that already has one. Here that is not
 * a safe assumption, so the wrapper makes it true.
 */
#include <stddef.h>
#include "cfg_table.inc"

#endif /* CONTRACT_H */
