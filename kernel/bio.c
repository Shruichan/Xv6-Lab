// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#define NBUCKET 13
#define NB_SZ 1024
char bcachename[NB_SZ];

struct {
  struct spinlock lock[NBUCKET];
  struct buf buf[NBUF];
  struct buf head[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;
  int n=0;

  for (int i=0; i<NBUCKET; i++) {
    initlock(&bcache.lock[i], bcachename+n);
    n += snprintf(bcachename+n, NB_SZ-n, "bcache%d", i);
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
    *(bcachename+n) = '\0';
    n++;
  }

  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    initsleeplock(&b->lock, "buffer");
    b->next = bcache.head[0].next;
    b->prev = &bcache.head[0];
    bcache.head[0].next->prev = b;
    bcache.head[0].next = b;
  }
}


// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int index;
  index = blockno % NBUCKET;
  acquire(&bcache.lock[index]);
  for (b = bcache.head[index].next; b != &bcache.head[index]; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bcache.lock[index]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  for (b = bcache.head[index].next; b != &bcache.head[index]; b = b->next) {
    if (b->refcnt == 0) {
      b->blockno = blockno;
      b->dev = dev;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock[index]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  for (int i = (index + 1) % NBUCKET; i != index; i = (i + 1) % NBUCKET) {
    acquire(&bcache.lock[i]);
    for (b = bcache.head[i].next; b != &bcache.head[i]; b = b->next) {
      if (b->refcnt == 0) {
        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        release(&bcache.lock[i]);
        b->next = bcache.head[index].next;
        b->prev = &bcache.head[index];
        bcache.head[index].next->prev = b;
        bcache.head[index].next = b;
        release(&bcache.lock[index]);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.lock[i]);
  }
  return 0; 
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  int index;
  index = b->blockno % NBUCKET;
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  acquire(&bcache.lock[index]);
  b->refcnt--; 
  release(&bcache.lock[index]);
}

void
bpin(struct buf *b) {
  int index = b->blockno % NBUCKET;
  acquire(&bcache.lock[index]);
  b->refcnt++;
  release(&bcache.lock[index]);
}

void
bunpin(struct buf *b) {
  int index = b->blockno % NBUCKET;
  acquire(&bcache.lock[index]);
  b->refcnt--;
  release(&bcache.lock[index]);
}