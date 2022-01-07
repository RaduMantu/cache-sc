### cache-sc
## Description
A (mostly) L3 cache side channel attack. The attacker `src/attacker.c` spawns two threads: one focused on cache eviction and the other, on memory probing.

The former is supposed to run on the same physical core as the victim, continuously replacing cache lines by doing either data accesses or instruction fetches. These two strategies are meant to target either the L1 data cache, or instruction cache (L2-3 are unified).

The latter of the two threads will perform memory probing at a certain address and report the elapsed cycle count via `RDTSC`. This cycle count is not entirely accurate for multiple reasons:

 1. **extra instructions**: there are other instructions (e.g.: `LFENCE`, `MOV`, etc.) between the two `RDTSC`s that inflate the real value, mostly by a constant amount. So if L1 cache access time is supposed to be 4 cycles, we might obtain ~30 cycles. This should not be that much of an issue, and can be ignored.
 2. **hyperthreading**: taking advantage of the superscalar architecture, the CPU may interlace instructions from the other logical core (located on the same physical core) with that of our thread, leading to an unpredictable inflation of reported clock cycles. This is the reason why we separated the probing thread from the victim and evictor thread: to reduce channel noise. Note, however, that this approach works only on L3 cache and may in fact be more susceptible to channel noise if we have too many processes running on other cores.
 3. **context switches**: these are inevitable. For example, if we have network traffic, the kernel may assign a batch of packets to our logical core's worker thread for processing. This usually results in impossibly high latency values and can be filtered out by establishing an outlier threshold.

## Preliminary system configuration
Information regarding each individual logical core is available at `/sys/bus/cpu/devices`. Before getting started, we will want to place three CPUs (victim + 2 attacker threads) in *performance* mode. This will hopefully maintain a constant CPU frequency:
```
$ echo performance | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
```
Unfortunately, Intel exposes only *powersave* and *performance* as scaling governors. If your CPU supports the *userspace* mode as well, you can set the frequency at a lower value (from a pre-established set of frequencies). An important thing to note is how the logical cores are indexed by the kernel. `cpu0` and `cpu1` most likely belong to different physical cores. You can determine which CPUs are paired by investigating the L1 and L2 memory sharing list:
```
$ cat /sys/bus/cpu/devices/cpu0/cache/index2/shared_cpu_list
0,6
```
Since I have 12 logical cores numbered 0-11, `cpu0` and `cpu6` seem to reside on the first physical core.

When running the attacker program, you will need to specify a few values that determine the  cache size. These can all be obtained from `/sys/bus/cpu/devices/cpu*/cache/`:

 - Number of L3 cache sets: `index3/number_of_sets` (e.g.: 12288)
 - Number of cache lines per set: `index3/ways_of_associativity` (e.g.: 16)
 - Number of bytes per cache line: `index3/coherency_line_size` (e.g.: 64)

Multiplied, these values should equal `index3/size`.

## Running the experiment
For now, we will only be using `victim_d`. This victim performs *data fetches* from the beginning of the `toupper()` *libc* function when the *"toupper"* CLI argument is passed, or from `getenv()` when any other argument is provided. Note that the attacker uses `argp`, so just run it with the `--help` flag to get more info on the available flags. This is an example run:
```
$ taskset -c 0 ./bin/victim_d toupper
$ ./bin/attacker -s 12288 -a 16 -c 64 -e 6 -p 1 -l 10 -t 750 -w 1000 -d -n toupper
```
Here is a quick rundown of the attacker's flags that were used:

 - `-s 12288` : number of L3 cache sets
 - `-a 16` : ways of association / number of cache lines per set
 - `-c 64` : size of cache line (pretty standard on modern CPUs)
 - `-e 6` : core that the evictor thread is bound to
 - `-p 1` : core that the prober thread is bound to
 - `-l 10` : amount of μs that the prober waits for the victim to do it's thing, after the eviction happened
 - `-t 750` : outlier threshold, in clock cycles
 - `-w 1000` : window size for the load latency moving average
 - `-d` : evict cache by data load, not instruction fetch (since using `victim_d`)
 - `-n toupper` : target probing address, resolved from symbol name (we also have `-x` but ASLR...)

Right now, we can see a ~10 clock cycle difference between victims that run with *"toupper"* and *"not_toupper"* arguments. But there is a problem that if fixed, should make the distinction a lot clearer (see TODO#1).
## TODO

 1. At the moment, the cache replacing buffer is an anonymous map. There is no guarantee that the allocated pages are continuous in physical memory. Since L1 cache works based on virtual addresses and L2-3 on physical addresses, this is a big deal. We need a way to guarantee that the [set indices](https://diveintosystems.org/book/C11-MemHierarchy/caching.html) are distinct (i.e.: we have full L3 coverage). There are a few solutions that we might try:
	- **kernel module** : a module that allocates physically continuous memory pages and exposes a device that we can `mmap()` in userspace. I know that we're not supposed to be able to do this, but it's the cleanest way to check if it works.
    - **/proc/\<pid>/pagemap** : this interface should allow us to correlate virtual page addresses with physical page frames. So we could theoretically keep allocating pages am `mlock()`-ing them into memory until we get a physically continuous subset, then free the rest. Unfortunately, access is now [restricted](https://www.kernel.org/doc/Documentation/vm/pagemap.txt) because Rowhammer implementations can exploit it. 
    - **hugepages** : just a thought; need to look more into this.
2. Refine eviction method, to focus only on the cache set that should host the victim's line. Right now, full L3 eviction may take a bit too much time, especially for the instruction fetch method.
