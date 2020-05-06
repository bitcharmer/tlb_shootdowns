## The problem

Every once in a while I involuntarily get involved in heated debates about if reusing memory is better than freeing it.  
**TLDR**: it is. If you want to find out why read on.

Now, I'm dumb; I'm not even close to Martin Thompson, Brendan Gregg or Herb Sutter types so I just uncritically accept whatever they have to say about pretty much anything, especially when they talk about [CPU caches](https://en.wikipedia.org/wiki/CPU_cache), TLB misses or invoke other terms that I always acknowledge with nervous laugh while noting it down to check what it actually is later.  
But this time "because Martin says so" wasn't good enough for my peers as if they actually knew I had no idea what I was talking about.
And so I was kind of forced onto this path of misery, doubt and self-loathing that some people call "doing research".

Because I'm not the sharpest tool in the shed I take longer to learn new things and require working examples for everything. 
I decided to steal someone else's code from stack overflow and find out for myself if reusing memory is more efficient than just freeing it and letting the allocator do its magic.
Unfortunately there was nothing to steal except some academic discussions on the undesired side effects of freeing (unmapping) memory.

The reason for associating free() with munmap() is that some allocations with malloc()/calloc() will not use sbrk() and fall back to mmap() under the hood (with corresponding munmap() to free memory).  
It's really well captured in the original documentation:

> Normally, malloc() allocates memory from the heap, and adjusts the size of the heap as required, using sbrk(2). When allocating blocks of memory larger than MMAP_THRESHOLD bytes, the glibc malloc() implementation allocates the memory as a private anonymous mapping using mmap(2). MMAP_THRESHOLD is 128 kB by default, but is adjustable using mallopt(3). Allocations performed using mmap(2) are unaffected by the RLIMIT_DATA resource limit (see getrlimit(2)).

Regardless of the method in which your program acquired memory there are side effects of freeing/reclaiming it.
This post focuses on the impact of so called TLB-shootdowns.


## The theory

Due to the mechanics of handling memory loads and stores in modern hardware and the supporting physical design of most contemporary CPUs, threads running in the same [VAS](https://en.wikipedia.org/wiki/Virtual_address_space) will negatively impact one another just by deallocating memory.  
In formal terms: for a single program P its thread T<sub>0</sub> running on CPU<sub>0</sub> is expected to disrupt P's thread T<sub>1</sub> running on CPU<sub>1</sub> solely by sharing the same address space.  
In my terms - within the same program, threads can screw with other threads by freeing memory those other threads aren't even using.  
You can probably guess how some react to a seemingly ludicrous statement like this.

![alt text](img/tenor.gif "")  
I don't blame them; the first time around  that was my reaction too.

In order to understand the phenomenon we have to explore the anatomy of a few crucial components and their mutual interactions.
I'll assume we all know what [virtual memory](https://en.wikipedia.org/wiki/Virtual_memory) is as a concept and start from here.
  
When the CPU executes an instruction that accesses some part of memory, the address points to a virtual and not physical address.
This virtual address has to be translated to the physical address; this means that there has to be some mapping maintained that when given a virtual address returns a corresponding physical address.
Such mapping is maintained in the [page table](https://en.wikipedia.org/wiki/Page_table).  
Nowadays these structures are quite complex with up to [5 levels](https://en.wikipedia.org/wiki/Intel_5-level_paging) from [Intel's Icelake](https://en.wikipedia.org/wiki/Ice_Lake_(microprocessor\)) onwards.
Here's some [nice read](https://lwn.net/Articles/717293/) on how this support came to be in Linux and how stuff works at this level of complexity.
Now, because this mapping has to be performed for each and every memory access the process of going to the page table, finding corresponding level 1 entry and following deeper into levels 4 or 5 seems like a lot of work for every (not only) mov instruction.
There's a lot of pointer chasing involved so such overhead would degrade our computers' performance by orders of magnitude.  

So why don't we see this happening? Enter the [TLB](https://en.wikipedia.org/wiki/Translation_lookaside_buffer).

Just like CPU cache, TLB caches the virtual-to-physical address mappings so we don't have to go through the pain of inspecting page tables every single time CPU needs to do anything.
Nowadays, on x86 there are separate TLBs for data (dTLB) and instructions (iTLB). What's more - just like CPU caches - they are divided into access levels.
For example Intel's Xeon E5-2689 v4 [has 5 TLB caches](http://www.cpu-world.com/CPUs/Xeon/Intel-Xeon%20E5-2689.html):
* Data TLB0: 2-MB or 4-MB pages, 4-way set associative, 32 entries
* Data TLB: 4-KB Pages, 4-way set associative, 64 entries
* Instruction TLB: 4-KB pages, 4-way set associative, 64 entries
* L2 TLB: 1-MB, 4-way set associative, 64-byte line size
* Shared 2nd-level TLB: 4 KB pages, 4-way set associative, 512 entries 

Fun fact: the first hardware cache used in a computer system was not actually a data or instruction cache, [but rather a TLB](http://www.chilton-computing.org.uk/acl/technology/atlas/p019.htm).  
To make things more interesting there are 4 types of CPU caches that interact with TLB differently:
* Physically indexed, physically tagged (PIPT) caches use the physical address for both the index and the tag. While this is simple and avoids problems with aliasing, it is also slow, as the physical address must be looked up (which could involve a TLB miss and access to main memory) before that address can be looked up in the cache.
* Virtually indexed, virtually tagged (VIVT) caches use the virtual address for both the index and the tag. This is a pretty dodgy scheme, not used broadly due to its problems with aliasing (multiple virtual addresses pointing to the same physical address) causing coherency challenges or homonyms where the same virtual address maps to several different physical addresses. 
* Virtually indexed, physically tagged (VIPT) caches use the virtual address for the index and the physical address in the tag. They are faster than PIPT because a cache line can be looked up in parallel with the TLB translation (with tag comparison delayed until the physical address is available). This type of cache can detect homonyms.
* Physically indexed, virtually tagged (PIVT) caches. Not very useful these days (only MIPS R6000 had one).

Most level-1 caches are virtually indexed nowadays, which allows a neat performance trick where the MMU's TLB lookup happens in parallel with fetching the data from the cache RAM.
Due to aliasing problem virtual indexing is not the best option for all cache levels. Aliasing overhead gets even bigger with the cache size. Because of that most level-2 and larger caches are physically indexed.

So that clears things up, doesn't it?

![alt text](img/dafuq.jpg "")  

I struggled with these concepts for a while and highly recommend watching an [explanatory video](https://www.youtube.com/watch?v=95QpHJX55bM) that shows how TLB works for different cases (misses vs hits). [Another one](https://www.youtube.com/watch?v=uyrSn3qbZ8U&t=191s) focuses more on how employing TLB improves performance of memory accesses. 
Although simplified I found these videos a great starting point if you would like to get a better understanding of this particular part of memory management on modern platforms.    

Mind you, these show the world of hardware TLBs, however there are architectures that either entirely rely on TLB done in software (MIPS) or support software and hardware (SPARC v9).
  
To put the impact of TLB-assisted address translation in numbers: 
- a hit takes 0.5 - 1 CPU cycle
- a miss can take anywhere between 10 and even hundreds of CPU cycles. 

Now that we know everything about TLBs it's time to describe what a TLB shootdown is and how we can measure its impact.
We know that TLB is essentially a cache of page table entries and a very small one at that (at least compared to CPU caches). This means we have to be very careful not to mess with it too much or else we'll have to pay the price of TLB misses.
One such case is a full context switch when a CPU is about to execute code in an entirely different virtual address space. Depending on CPU model this will result in a "legacy" TLB flush with [_invlpg_](https://www.felixcloutier.com/x86/invlpg) instruction (ouch!) or partial entry invalidations, depending on the availability of CPU's _PCID_ feature ([_INVPCID_](https://www.felixcloutier.com/x86/invpcid)). If I'm not mistaken, it's been available from around Sandy Bridge onward.     
But that case is easy to understand, follow and even trace. A TLB-shootdown is much more subtle and often comes from the hand of a backstabbing thread from our own process.

Imagine a process with two threads running on two separate CPUs. They both allocate some memory to work with (let's call it chunk A). They later decide to allocate some more memory (chunk B). Eventually they only work on chunk B and don't need chunk A any more so one of the threads calls _free()_ to release unused memory back to the OS.
What needs to happen now is that the CPU which executed the _free()_ call has perfect information about valid mappings because it flushed outdated entries in its own TLB. But what about the other CPU running the other thread of the same process?
How does it know that some virtual-to-physical mappings are not legal any more? We mustn't let it access addresses that map to physical memory that has been freed and can now belong to some completely different process, can we? I mean, that would be really bad memory coherency :)  

This is where we finally get to meet Mr TLB-shootdown. 
It goes more or less like this. Thread A calls _free()_ which eventually propagates to the OS which knows which other CPUs are currently running threads that might access the memory area that's about to get freed. It raises an [IPI (inter-processor interrupt)](https://en.wikipedia.org/wiki/Inter-processor_interrupt) that targets that specific CPU to tell it to pause whatever it's doing now and first invalidate some virtual-to-physical mappings before resuming work.
Note: IPIs are also used to implement other functionality like signaling timer events or rescheduling tasks.

In Linux kernel there's a really cool function called [smp_call_function_many](https://elixir.bootlin.com/linux/v4.15/source/kernel/smp.c#L403) which generally lets you call functions on other CPUs. So when the OS wants to tell a bunch of CPUs to immediately invalidate their TLBs it uses the _smp_call_function_many_ facility with appropriate cpu mask to invoke a dedicated function on each of the qualifying CPUs: [flush_tlb_func_remote](https://elixir.bootlin.com/linux/v4.15/source/arch/x86/mm/tlb.c#L510). 
It's all nicely encapsulated in [native_flush_tlb_others](https://elixir.bootlin.com/linux/v4.15/source/arch/x86/mm/tlb.c#L520) function and I strongly recommend you have a look to get a better understanding of what is really going on when this happens. 

If our understanding is correct, we should see an execution stall on an unsuspecting thread that's doing its own thing when suddenly it gets hit with a giant IPI hammer. How do we even measure this?

## The reality

<cries in assembly>
<totally makes sense if you don't think about it>
