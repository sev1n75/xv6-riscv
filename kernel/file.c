//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"
#include "fcntl.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

struct {
  struct spinlock lock;
  struct vma vmas[NVMAS];
} kvma;

uint64 vma_addr_start;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
  initlock(&kvma.lock, "kvma");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

struct vma* 
allocvma(uint64* paddr, size_t length, int prot, int flags, struct file* f, off_t off) {
  uint64 addr;
  struct vma* pvma;
  int i;
  struct proc* p;
  p = myproc();
  pvma = p->vma;
  // find a approriate address
  addr = vma_addr_start;
  while(pvma) {
    addr = pvma->end > addr ? pvma->end : addr;
    pvma = pvma->next;
  }
  addr = PGROUNDUP(addr);
  *paddr = addr;

  // alloc a vacant vmas
  acquire(&kvma.lock);
  for(i = 0; i < NVMAS && kvma.vmas[i].f ; i++);
  if(i >= NVMAS) {
    release(&kvma.lock);
    return 0;
  }
  pvma = &kvma.vmas[i];
  pvma->f = f;
  release(&kvma.lock);

  // set vma
  filedup(f);
  pvma->next = p->vma;
  p->vma = pvma;
  pvma->start = addr;
  pvma->end = addr + length;
  pvma->prot = prot;
  pvma->off = off;
  pvma->flags = flags;

  return pvma;
}

int
vma_unmap(uint64 addr, uint64 length) {
  struct proc* p = myproc();
  struct vma *pvma, *prev;
  int do_clean;
  uint64 npages, a;
  pte_t* pte;

  pvma = p->vma;
  prev = pvma;  // prev may be equal to pvma
  while(pvma) {
    if(pvma->start <= addr && addr < pvma->end)
      break;
    prev = pvma;
    pvma = pvma->next;
  }
  if(!pvma)
    return -1;
  // addr is valid
  do_clean = 0;
  if(addr + length >= pvma->end) {
    length = pvma->end - addr;
    if(addr == pvma->start)
      do_clean = 1;
  }

  if(pvma->flags & MAP_SHARED) {
    // if MAP_SHARED write dirty back
    for(a = addr; a < addr + length; a += PGSIZE) {
      if((pte = walk(p->pagetable, a, 0)) == 0)
        continue; // because we map lazily
      if (*pte & PTE_D) {
        // found dirty pte
        begin_op();
        ilock(pvma->f->ip);
        writei(pvma->f->ip, 0, PTE2PA(*pte), pvma->off+a-addr, PGSIZE);
        iunlock(pvma->f->ip);
        end_op();
      }
    }
  }

  // unmap
  npages = PGROUNDUP(length) / PGSIZE;
  uvmunmap(p->pagetable, addr, npages, 1); 

  // update pvma
  if(!do_clean) {
    if(addr == pvma->start)
      pvma->start = addr + PGROUNDUP(length);
    // other conditions is not tested in mmaptest
  } else {
    // unlink pvma in p->vma single linked list    
    if(prev == pvma)
      p->vma = pvma->next;
    else
      prev->next = pvma->next;
    pvma->next = 0;
    pvma->start = 0;
    pvma->end = 0;
    pvma->flags = 0;
    pvma->off = 0;
    pvma->prot = 0;
    acquire(&ftable.lock);
    pvma->f->ref--;
    release(&ftable.lock);
    acquire(&kvma.lock);
    pvma->f = 0;
    release(&kvma.lock);
  }
  return 0;
}

int
vmacopy(struct proc* parent, struct proc* child) {
  struct vma *pvma, *c_pvma, *prev;
  int i;

  prev = 0;
  pvma = parent->vma;
  while(pvma) {
    acquire(&kvma.lock);
    for(i = 0; i < NVMAS && kvma.vmas[i].f ; i++);
    if(i >= NVMAS) {
      release(&kvma.lock);
      return 0;
    }
    c_pvma = &kvma.vmas[i];
    c_pvma->f = pvma->f;
    release(&kvma.lock);
    if (uvmcopy(parent->pagetable, child->pagetable, pvma->end-pvma->start, pvma->start) < 0) {
      // free vma alloc fo child before
      return -1;
    }
    filedup(c_pvma->f);
    c_pvma->start = pvma->start;
    c_pvma->end =  pvma->end;
    c_pvma->prot = pvma->prot;
    c_pvma->off =  pvma->off;
    c_pvma->flags = pvma->flags;
    child->vma = c_pvma;
    c_pvma->next = prev;
    prev = c_pvma;

    pvma = pvma->next;
  }
  return 0;
}

