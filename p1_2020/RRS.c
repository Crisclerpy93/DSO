#include <stdio.h>
#include <sys/time.h>
#include <signal.h>
#include <stdlib.h>
#include <ucontext.h>
#include <unistd.h>
#include "my_io.h"

//#include "mythread.h"
#include "interrupt.h"
#include "queue.h"

TCB* scheduler();
void activator();
void timer_interrupt(int sig);
void disk_interrupt(int sig);

/*Queue containing ready threads to be executed. One for LOW_PRIORITY and one for HIGH_PRIORITY*/
struct queue *ready_low;
struct queue *ready_high;

/* Array of state thread control blocks: the process allows a maximum of N threads */
static TCB t_state[N];

/* Current running thread */
static TCB* running;
static int current = 0;

/* Variable indicating if the library is initialized (init == 1) or not (init == 0) */
static int init=0;

/* Thread control block for the idle thread */
/*static TCB idle;

static void idle_function()
{
  while(1);
}*/

void function_thread(int sec)
{
    //time_t end = time(NULL) + sec;
    while(running->remaining_ticks)
    {
      //do something
    }

    mythread_exit();
}


/* Initialize the thread library */
void init_mythreadlib()
{
  int i;

  /* Create context for the idle thread */
  /*if(getcontext(&idle.run_env) == -1)
  {
    perror("*** ERROR: getcontext in init_thread_lib");
    exit(-1);
  }

  idle.state = IDLE;
  idle.priority = SYSTEM;
  idle.function = idle_function;
  idle.run_env.uc_stack.ss_sp = (void *)(malloc(STACKSIZE));
  idle.tid = -1;

  if(idle.run_env.uc_stack.ss_sp == NULL)
  {
    printf("*** ERROR: thread failed to get stack space\n");
    exit(-1);
  }

  idle.run_env.uc_stack.ss_size = STACKSIZE;
  idle.run_env.uc_stack.ss_flags = 0;
  idle.ticks = QUANTUM_TICKS;
  makecontext(&idle.run_env, idle_function, 1);*/

  t_state[0].state = INIT;
  t_state[0].priority = LOW_PRIORITY;
  t_state[0].ticks = QUANTUM_TICKS;

  if(getcontext(&t_state[0].run_env) == -1)
  {
    perror("*** ERROR: getcontext in init_thread_lib");
    exit(5);
  }

  for(i=1; i<N; i++)
  {
    t_state[i].state = FREE;
  }

  t_state[0].tid = 0;
  running = &t_state[0];

  /*Initialize ready threads queues*/
  ready_low = queue_new();
  ready_high = queue_new();

  /* Initialize disk and clock interrupts */
  init_disk_interrupt();
  init_interrupt();
}


/* Create and intialize a new thread with body fun_addr and one integer argument */
int mythread_create (void (*fun_addr)(),int priority,int seconds)
{
  int i;

  if (!init) { init_mythreadlib(); init=1;}

  for (i=0; i<N; i++)
    if (t_state[i].state == FREE) break;

  if (i == N) return(-1);

  if(getcontext(&t_state[i].run_env) == -1)
  {
    perror("*** ERROR: getcontext in my_thread_create");
    exit(-1);
  }

  /*if (priority != LOW_PRIORITY && priority != HIGH_PRIORITY){
    perror("*** ERROR: invalid priority in my_thread_create");
    exit(-1);
  }*/

  t_state[i].ticks = QUANTUM_TICKS;
  t_state[i].state = INIT;
  t_state[i].priority = priority;
  t_state[i].function = fun_addr;
  t_state[i].execution_total_ticks = seconds_to_ticks(seconds);
  t_state[i].remaining_ticks = t_state[i].execution_total_ticks;
  t_state[i].run_env.uc_stack.ss_sp = (void *)(malloc(STACKSIZE));

  if(t_state[i].run_env.uc_stack.ss_sp == NULL)
  {
    printf("*** ERROR: thread failed to get stack space\n");
    exit(-1);
  }

  t_state[i].tid = i;
  t_state[i].run_env.uc_stack.ss_size = STACKSIZE;
  t_state[i].run_env.uc_stack.ss_flags = 0;
  makecontext(&t_state[i].run_env, fun_addr,2,seconds);

  disable_interrupt();
  //enqueue(ready, &t_state[i]); --> RR
  //if ready thread is high priority check if running is high or low
  if(t_state[i].priority == HIGH_PRIORITY){
    //if running is low, preempt it and reset ticks. Run high one
    if(running->priority == LOW_PRIORITY){
      running->ticks = QUANTUM_TICKS;
      enqueue(ready_low, running);
      sorted_enqueue(ready_high, &t_state[i], t_state[i].remaining_ticks);
      //enqueue(ready_high, &t_state[i]);
      queue_print(ready_high);
      TCB *next = scheduler();
      activator(next);
    //if runing is high, sort them by execution time and run SJF
    }else{
      //enqueue(ready_high, &t_state[i]);
      sorted_enqueue(ready_high, &t_state[i], t_state[i].remaining_ticks);
      queue_print(ready_high);
      //TCB *next = scheduler();
      //activator(next);
    }
  //if ready thread is low priority enqueue it
  }else{
    enqueue(ready_low, &t_state[i]);
  }
  enable_interrupt();

  return i;
}
/****** End my_thread_create() ******/


/* Read disk syscall */
int read_disk()
{
   return 1;
}

/* Disk interrupt  */
void disk_interrupt(int sig)
{

}


/* Free terminated thread and exits */
void mythread_exit() {
  //int tid = mythread_gettid();
  int tid = running->tid;
  printf("*** THREAD %d FINISHED\n", tid);
  running->state = FREE;
  free(t_state[tid].run_env.uc_stack.ss_sp);
  disable_interrupt();
  TCB* next = scheduler();
  activator(next);
  enable_interrupt();
}


void mythread_timeout(int tid) {

    printf("*** THREAD %d EJECTED\n", tid);
    t_state[tid].state = FREE;
    free(t_state[tid].run_env.uc_stack.ss_sp);
    disable_interrupt();
    TCB* next = scheduler();
    enable_interrupt();
    activator(next);
}


/* Sets the priority of the calling thread */
void mythread_setpriority(int priority)
{
  int tid = mythread_gettid();
  t_state[tid].priority = priority;
  if(priority ==  HIGH_PRIORITY){
    t_state[tid].remaining_ticks = 195;
  }
}

/* Returns the priority of the calling thread */
int mythread_getpriority(int priority)
{
  int tid = mythread_gettid();
  return t_state[tid].priority;
}


/* Get the current thread id.  */
int mythread_gettid(){
  if (!init) { init_mythreadlib(); init=1;}
  return current;
}


/* SJF para alta prioridad, RR para baja*/
TCB* scheduler()
{
  /*if(queue_empty(ready) == 1){
    printf("*** FINISHED\n");
    exit(1);
  }else{
    return dequeue(ready);

  }*/
  if(queue_empty(ready_high)==0){
    return dequeue(ready_high);
  }else if(queue_empty(ready_low)==0){
    return dequeue(ready_low);
  }else{
    //printf("mythread_free: No thread in the system\nExiting...\n");
    printf("*** FINISHED\n");
    exit(1);
  }
}

/*TCB* running*/
/* Timer interrupt */
void timer_interrupt(int sig)
{
  /*running->ticks = running->ticks -1;
  running->remaining_ticks = running->remaining_ticks -1;
  if (running->ticks == 0){
    running->ticks = QUANTUM_TICKS;
    disable_interrupt();
    if (queue_empty(ready)==0){
      enqueue(ready, running);
      TCB *next = scheduler();
      activator(next);
    }
    enable_interrupt();
  }*/
  if (running->priority == LOW_PRIORITY){
    running->ticks = running->ticks -1;
    running->remaining_ticks = running->remaining_ticks -1;
    if (running->ticks == 0){
      running->ticks = QUANTUM_TICKS;
      disable_interrupt();
      if (queue_empty(ready_low)==0){
        enqueue(ready_low, running);
        TCB *next = scheduler();
        activator(next);
      }
      enable_interrupt();
    }
  }
}

/* Activator */
void activator(TCB* next)
{
  TCB *prev = running;
  running = next;
  if(prev->state == FREE){
    printf("*** THREAD %d TERMINATED: SETCONTEXT OF %d\n", prev->tid, next->tid);
    setcontext(&(next->run_env));
    printf("mythread_free: After setcontext, should never get here!!...\n");
  }else{
    if(prev->tid!=next->tid){
      if(prev->priority == LOW_PRIORITY && running->priority == HIGH_PRIORITY){
        printf("*** THREAD %d PREEMPTED: SETCONTEXT OF %d\n", prev->tid, running->tid);
        printf("*** SWAPCONTEXT FROM %d TO %d\n", prev->tid, next->tid);
        swapcontext(&(prev->run_env), &(running->run_env));
        printf("ESTOY AQUI");
      }else{
        printf("*** SWAPCONTEXT FROM %d TO %d\n", prev->tid, next->tid);
        swapcontext(&(prev->run_env), &(running->run_env));
      }
    }
  }
}
