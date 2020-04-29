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

Due to the mechanics of handling memory loads and stores in modern hardware and the supporting physical design of most contemporary CPUs thread t0 running on cpu0 will disrupt thread t1 running on cpu1 solely by the virtue of the two executing in the same VAS.
In simpler terms - within the same program threads can screw with other threads by freeing memory those other threads aren't even using.  

![alt text](https://tenor.com/view/dr-evil-right-gif-9743588 "")


[logo]: https://tenor.com/view/dr-evil-right-gif-9743588  
 
In order to understand the anatomy of this side effect

What is TLB
TLB stands for [Translation lookaside buffer](https://en.wikipedia.org/wiki/Translation_lookaside_buffer). It's a piece of hardware

* cries in assembly *


Memes:
https://i.imgur.com/YcBvLG4.gifv
