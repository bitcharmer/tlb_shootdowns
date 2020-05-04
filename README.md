Every once in a while I involuntarily get involved in heated debates about if reusing memory is better than reclaiming it.  
TLDR: it is. If you want to find out why read on.

Now, I'm dumb; I'm not even close to Martin Thompson, Brendan Gregg or Herb Sutter types so I just uncritically accept whatever they have to say about pretty much anything, especially when they talk about [CPU caches](https://en.wikipedia.org/wiki/CPU_cache), [TLB misses](https://en.wikipedia.org/wiki/Translation_lookaside_buffer) or invoke other terms that I always acknowledge with nervous laugh while noting it down to check what it actually is later.  
But this time "because Martin says so" wasn't good enough for my peers as if they actually knew I had no idea what I was talking about.
And so I was kind of forced onto this path of misery, doubt and self-loathing that some people call "doing research".

Because I'm not the sharpest tool in the shed I take longer to learn new things and require working examples for everything. 
I decided to steal someone else's code from stack overflow and find out for myself if reusing memory is more efficient than just freeing it and letting the allocator do its magic.
Unfortunately there was nothing to steal except some academic discussions on the undesired side effects of freeing (unmapping) memory.

The reason for associating free() with munmap() is that some allocations with malloc()/calloc() will not use sbrk() and fall back to mmap() under the hood (with corresponding munmap() to free memory).  
It's really well captured in the original documentation:

> Normally, malloc() allocates memory from the heap, and adjusts the size of the heap as required, using sbrk(2). When allocating blocks of memory larger than MMAP_THRESHOLD bytes, the glibc malloc() implementation allocates the memory as a private anonymous mapping using mmap(2). MMAP_THRESHOLD is 128 kB by default, but is adjustable using mallopt(3). Allocations performed using mmap(2) are unaffected by the RLIMIT_DATA resource limit (see getrlimit(2)).

Regardless of the method in which your program acquired memory there is a side effect of freeing/reclaiming it.
So what exactly is that side effect?


## The theory

Due to the mechanics of handling memory loads and stores in modern hardware and the supporting physical design of most contemporary CPUs, threads running in the same [VAS](https://en.wikipedia.org/wiki/Virtual_address_space) will negatively impact one another just by deallocating memory.  
In formal terms: for a single program P its thread T<sub>0</sub> running on CPU<sub>0</sub> is expected to disrupt P's thread T<sub>1</sub> running on CPU<sub>1</sub> solely by sharing the same address space.  
In my terms - within the same program, threads can screw with other threads by freeing memory those other threads aren't even using.  
You can probably guess how some react to a seemingly ludicrous statement like this.

![alt text](img/tenor.gif "")  
I don't blame them; the first time around  that was my reaction too.

In order to understand the phenomenon we have to explore the anatomy of a few crucial components and their mutual interactions.
I'll assume we all know what virtual memory is as a concept and start from here.
  
When the CPU executes an instruction that accesses some part of memory, the address points to a virtual and not physical address.
This virtual address has to be translated to the physical address; this means that there has to be some mapping maintained that when given a virtual address returns a corresponding physical address.
Such mapping is called the [page table](https://en.wikipedia.org/wiki/Page_table).  
Nowadays these structures are quite complex with up to [5 levels](https://en.wikipedia.org/wiki/Intel_5-level_paging) from [Intel's Icelake](https://en.wikipedia.org/wiki/Ice_Lake_(microprocessor\)) onwards.
Here's some [nice read](https://lwn.net/Articles/717293/) on how this support came to be in Linux and how stuff works at this level of complexity.



What is TLB
TLB stands for [Translation lookaside buffer](https://en.wikipedia.org/wiki/Translation_lookaside_buffer). It's a piece of hardware


* cries in assembly *
* totally makes sense if you don't think about it

Memes:
https://i.imgur.com/YcBvLG4.gifv
