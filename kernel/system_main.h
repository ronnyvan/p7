/* Copyright (C) 2025 Ahmed Gheith and contributors.
 *
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "atomic.h"
#include "x86_64.h"
#include <cstdint>

namespace impl {}

namespace Sys {
extern bool normal;
extern uint64_t core_count;
extern volatile bool bootstraping;
extern uint64_t start_tsc;
extern uint64_t hhdm_offset;
extern GDTR gdtr;
extern uint64_t base_tss_index;
} // namespace Sys
