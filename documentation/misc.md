# Linux headers used

## General discussion of SGX driver

- https://lwn.net/Articles/686393/

## Data Structures

* `linux/list.h`
* `linux/hashtable.h`
* `linux/rbtree.h`
* `linux/radix-tree.h`

Documentation:
* `list.h`: intrusive doubly-linked lists in kernel
  - https://www.kernel.org/doc/htmldocs/kernel-api/adt.html#id-1.3.2
  - https://isis.poly.edu/kulesh/stuff/src/klist/

## Memory management

* `linux/kref.h`
  - reference counters added to structs: `struct kref refcount`
  - https://www.kernel.org/doc/Documentation/kref.txt
* `linux/mmu_notifier.h`
  - notifications from host's memory management subsystem to the guest that pages are reclaimed / everything is released
  - https://lwn.net/Articles/266320/
* `asm/mman.h`
  - macros for page permissions, sizes, mmap() flags, etc
* `linux/highmem.h`
  - functions `kmap` and `kunmap` which return *kernel virtual addresses*
  - http://www.makelinux.net/ldd3/chp-15-sect-1
  - for low-memory pages: kernel virtual address = kernel logical address (physical address + predefined offset)
  - for high-memory pages: kernel virtual address is mapped to physical address via PTs
* `linux/slab.h`
  - slab allocator
  - https://www.kernel.org/doc/gorman/html/understand/understand011.html
* `linux/mm.h`
  - memory management routines
  - http://duartes.org/gustavo/blog/post/how-the-kernel-manages-your-memory/
* `linux/shmem_fs.h`
  - https://www.kernel.org/doc/gorman/html/understand/understand015.html
  - http://elixir.free-electrons.com/linux/v3.7/source/mm/shmem.c

## Scheduling and synchronization

* `linux/kthread.h` 
  - simple interface for creating kernel threads
  - http://www.cs.fsu.edu/~cop4610t/lectures/project2/kthreads/kthreads.pdf
* `linux/sched.h`
  - main scheduler APIs
* `linux/workqueue.h`
  - work queue to execute work-tasks at a later point in time
  - https://www.ibm.com/developerworks/library/l-tasklets/
* `linux/rwsem.h` 
  - R/W semaphore
  - http://www.makelinux.net/ldd3/chp-5-sect-3

## Time and hybernation/suspend

* `linux/delay.h`
  - functions `mdeday`, `ndelay`, `udelay` for very short (jiffy) time periods
  - http://www.makelinux.net/ldd3/chp-7-sect-3
* `linux/ratelimit.h`
  - function `ratelimit` to supressed some of the callbacks if more than `rs->burst`
* `linux/suspend.h`
  - functions to do cleanup/resume on hybernation/suspend
  - https://01.org/linuxgraphics/gfx-docs/drm/driver-api/pm/notifiers.html
* `linux/freezer.h`
  - callbacks to perform freeze/thaw cleanup on hybernation/suspend
  - https://www.kernel.org/doc/Documentation/power/freezing-of-tasks.txt

## Module and devices

* `linux/module.h`
  - dynamic loading of modules into the kernel
* `linux/platform_device.h`
  - platform device API
  - https://lwn.net/Articles/448499/
* `linux/miscdevice.h`
  - register/deregister of a "miscellenous" driver which doesn't need its own major version
  - https://stackoverflow.com/questions/18456155/what-is-the-difference-between-misc-drivers-and-char-drivers
* `linux/signal.h`
  - functions to work with signals and signal masks
* `linux/acpi.h`
  - ACPI (Advanced Configuration & Power Interface) functions to add information to `/proc` or `/sys`
  - http://www.acpi.info/
* `linux/ioctl.h`
  - macros to encode/decode ioctl numbers in one 32-bit int

## Misc

* `linux/version.h`
  - two macros `DVB_API_VERSION` and `DVB_API_VERSION_MINOR`
* `asm/asm.h`
  - macro `ASM_EXTABLE` for x86 Exception Handling
  - https://www.kernel.org/doc/Documentation/x86/exception-tables.txt
* `linux/bitops.h`
  - macros for bitwise operations
* `linux/err.h`
  - macros for ERR_PTRs (invalid ptrs with upper bits containing errno)
* `linux/types.h`
  - typical type macros (`size_t`, `uid_t`, etc)
* `linux/file.h`
  - functions for accessing files via fd


# Linux variables used

* `current`: pointer to the current process (i.e. the process that issued the system call)
  - https://stackoverflow.com/questions/12434651/what-is-the-current-in-linux-kernel-source


# Useful Information on SGX

## Why is already encrypted EPC page re-encrypted during eviction?

Because transparent MEE encryption is an implementation detail (due to PRM residing in RAM and not on CPU chip), and encryption during page eviction (`EWB`) is an architectural choice: https://software.intel.com/en-us/forums/intel-software-guard-extensions-intel-sgx/topic/722444

## Why SGX needs ELDB (reload with BLOCKED state) when there is ELBU (reload with UNBLOCKED)?

> ELDB is intended for use by a Virtual Machine Monitor (VMM). When a VMM reloads an evicted page, it needs to restore it to the correct state of the page (BLOCKED vs. UNBLOCKED) as it existed at the time the page was evicted. Based on the state of the page at eviction, the VMM chooses either ELDB or ELDU.
> 
>> -- <cite> Intel Software Developer Manual (Chapter 37) </cite>

Note that the current SGX driver *does not* use `ELDB`.
