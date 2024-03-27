// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct kmem{
  struct spinlock lock;
  struct run *freelist;
};

#define NMEM NCPU

struct kmem kmems[NMEM];
char name[NMEM][6];

void
kinit()
{
  int id;
  push_off();
  id = cpuid();

  name[id][0] = 'k';
  name[id][1] = 'm';
  name[id][2] = 'e';
  name[id][3] = 'm';
  name[id][4] = '0'+id;
  name[id][5] = '\0';
  initlock(&kmems[id].lock, name[id]);
  if(!id)
    freerange((void*)end, (void*)PHYSTOP);
  pop_off();
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  push_off();
  int id = cpuid();

  acquire(&kmems[id].lock);
  r->next = kmems[id].freelist;
  kmems[id].freelist = r;
  release(&kmems[id].lock);
  pop_off();
}

// steal from othre freelists and not give back.
// Return a pointer as kalloc() if success.
// Return 0 if other freelists is empty.
struct run* steal() {
  struct run * r = 0;
  // 遍历其他 list
  int i;
  for(i = 0; i < NMEM; i++) { //这里不能限制 i != id, 因为很可能在 kalloc 和 steal 之间被中断，freelist 又有内存了
    acquire(&kmems[i].lock);
    r = kmems[i].freelist;
    if(r) {
      kmems[i].freelist = r->next;
      release(&kmems[i].lock);
      break;
    }
    release(&kmems[i].lock);
  }
  return r;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  push_off();
  int id = cpuid();
  pop_off();

  acquire(&kmems[id].lock);
  r = kmems[id].freelist;
  if(r)
    kmems[id].freelist = r->next;
  release(&kmems[id].lock);

  if(!r)
    r = steal();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
