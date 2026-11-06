/* Link check: the runtime builds and links with only the stub backends. */
#include <stdio.h>
int main(void) { puts("external backends link"); return 0; }
