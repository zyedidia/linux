#define RUNTIME_DISCARD_EXIT
#include <asm/page.h>
KERNEL_STACK_SIZE = PAGE_SIZE * (1 << CONFIG_KERNEL_STACK_ORDER);

#ifdef CONFIG_LD_SCRIPT_STATIC
#include "uml.lds.S"
#else
#include "dyn.lds.S"
#endif
