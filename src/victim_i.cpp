#include <stdint.h>     /* [u]int_*t */
#include <string.h>     /* strcmp    */
#include <stdlib.h>     /* getenv    */
#include <ctype.h>      /* toupper   */

#include "util.h"


int32_t main(int32_t argc, char *argv[])
{
    int8_t   use_toupper;    /* using target function */
    uint32_t x;

    INFO("toupper() is at %p", (void *) toupper);

    /* decide mode of operation */
    DIE(argc != 2, "Usage: ./victim <toupper|...>");
    use_toupper = !strcmp(argv[1], "toupper");
    INFO("Using toupper(): %hhu", use_toupper);

    /* main loop */
    while (1) {
        if (use_toupper) {
            toupper('a');
        } else {
            getenv("I_DONT_EVEN_KNOW");
        }
    }

    return 0;
}

