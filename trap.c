#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
extern pte_t *walkpgdir(pde_t *pgdir, const void *va, int alloc); // used in pgfualt
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);
  
  initlock(&tickslock, "time");extern uint vectors[];  // in vectors.S: array of 256 entry pointers

}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
//  char *mem;  // for use in trap = PGFLT
//  uint a;     // for use in trap = PGFLT
//  pte_t* pte_user;

  if(tf->trapno == T_SYSCALL){
    if(proc->killed)
      exit();
    proc->tf = tf;
    syscall();
    if(proc->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpu->id == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpu->id, tf->cs, tf->eip);
    lapiceoi();
    break;
  //added for assignment 3: handle lazy allocation
    case T_PGFLT:
      if (proc) {
        //check the virtual address is in the process range that is allowed.
        // and check that the size of the virtual address isn't overlapping the process size
        if ((PGROUNDDOWN(proc->tf->ebp) > rcr2()) && (PGROUNDDOWN(proc->tf->esp) > rcr2()) && (rcr2() > proc->sz)) {
          cprintf("problem with virtual address\n");
          proc->killed = 1;
          return;
        }
        runTlb(rcr2());  // rcr2() it's the virtual address
        break;
      } else {
        cprintf("got a page fualt");
        panic("trap");
        break;
//        pte_user = walkpgdir(proc->pgdir,(void *)rcr2(),0);
//        if (!pte_user || !(*pte_user & PTE_P)) {
//          mem = kalloc();
//          if (mem == 0) {
//              cprintf("out of memory - bad i kill u now");
//              cprintf("pid %d %s: trap %d err %d on cpu %d "
//                              "eip 0x%x addr 0x%x--kill proc\n",
//                      proc->pid, proc->name, tf->trapno, tf->err, cpu->id, tf->eip,
//                      rcr2());
//              proc->killed = 1;
//              return;
//          }
//          memset(mem, 0, PGSIZE);
//          a = PGROUNDDOWN(rcr2());
//          cprintf("doing lazy allocation\n");
//          mappages(proc->pgdir, (char *) a, PGSIZE, v2p(mem), PTE_W | PTE_U);
//          return;
          }
  //PAGEBREAK: 13
  default:
    if(proc == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpu->id, tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            proc->pid, proc->name, tf->trapno, tf->err, cpu->id, tf->eip, 
            rcr2());
    proc->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running 
  // until it gets to the regular system call return.)
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(proc && proc->state == RUNNING && tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(proc && proc->killed && (tf->cs&3) == DPL_USER)
    exit();
}
