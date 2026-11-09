#ifndef _BS_FONT_RUNTIME_H_
#define _BS_FONT_RUNTIME_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct Runner Runner;

// Implements font_add(name, size, bold, italic, first, last).
// Returns the index of the new Font in DataWin's font chunk, or -1 on failure.
int32_t RuntimeFont_add(Runner* runner, const char* name, double size, bool bold, bool italic, int32_t first, int32_t last);

#endif /* _BS_FONT_RUNTIME_H_ */
