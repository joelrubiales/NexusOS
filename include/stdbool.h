/* Freestanding stdbool para NexusOS (-nostdinc). Compatible con C11/C17. */
#ifndef NEXUS_STDBOOL_H
#define NEXUS_STDBOOL_H

#define bool  _Bool
#define true  1
#define false 0

#define __bool_true_false_are_defined 1

#endif
