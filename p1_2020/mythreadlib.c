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

/*Queues containing ready threads to be executed. One for LOW_PRIORITY, one for HIGH_PRIORITY*/
struct queue *ready_low;
struct queue *ready_high;
/*Queue containing waiting threads to be ready*/
struct queue *waiting;

/* Array of state thread control blocks: the process allows a maximum of N threads */
static TCB t_state[N];

/* Current running thread */
static TCB* running;
static int current = 0;

/* Variable indicating if the library is initialized (init == 1) or not (init == 0) */
static int init=0;

/* Thread control block for the idle thread */
static TCB idle;

static void idle_function()
{
  while(1);
}

void function_thread(int sec)
{
    //time_t end = time(NULL) + sec;
    while(running->remaining_ticks)
    {
      //sleep(1);
      //read_disk();
      //do something
    }

    mythread_exit();
}


/* Initialize the thread library */
void init_mythreadlib()
{
  int i;

  /* Create context for the idle thread */
  if(getcontext(&idle.run_env) == -1)
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
  makecontext(&idle.run_env, idle_function, 1);

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

  /*Initialize threads queues*/
  ready_low = queue_new();
  ready_high = queue_new();
  waiting = queue_new();


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

  //if ready thread is low priority enqueue it
  if(t_state[i].priority == LOW_PRIORITY){
    disable_interrupt();
    enqueue(ready_low, &t_state[i]);
    enable_interrupt();
    return i;
  }

  //if ready thread is high priority check if running is high or low
  if(running->priority == LOW_PRIORITY){
    //if running is low, preempt it and reset ticks. Run shortest high one.
      running->ticks = QUANTUM_TICKS;
      running->ticks = INIT;
      disable_interrupt();
      enqueue(ready_low, running);
      sorted_enqueue(ready_high, &t_state[i], t_state[i].remaining_ticks);
      enable_interrupt();
      TCB *next = scheduler();
      activator(next);
      return i;
  }

  //if running is high, sort them by execution time and run SJF
  if(running->remaining_ticks > t_state[i].remaining_ticks){
        disable_interrupt();
        sorted_enqueue(ready_high, &t_state[i], t_state[i].remaining_ticks);
        sorted_enqueue(ready_high, running, running->remaining_ticks);
        enable_interrupt();
        TCB *next = scheduler();
        activator(next);
        return i;
  }
  //If running is not preempted, enqueue in ready_high
  disable_interrupt();
  sorted_enqueue(ready_high, &t_state[i], t_state[i].remaining_ticks);
  enable_interrupt();
  return i;
}
/****** End my_thread_create() ******/


/* Read disk syscall */
int read_disk()
{
  //R3.1.3-1
  //C#
  if(data_in_page_cache() != 0){
      running->state =  WAITING;
      disable_interrupt();
      enqueue(waiting, running);
      enable_interrupt();
      TCB *next = scheduler();
      activator(next);
  }
   return 1;
}

/* Disk interrupt  */
void disk_interrupt(int sig)
{
  //if waiting queue is empty, return
  if(queue_empty(waiting)==1){
    return;
  }
  //if waiting queue is not empty, dequeue and enqueue it in corresponding ready queue
  disable_interrupt();
  disable_disk_interrupt();
  TCB *ready_thread= dequeue(waiting);
  enable_disk_interrupt();
  enable_interrupt(); //IMPORTANT: last to enable

  //Cheking if timeout has preempted it
  if(ready_thread->state==FREE){
    return;
  }

  printf("*** THREAD %d READY\n", ready_thread->tid);

  //If it is LOW_PRIORITY, enqueue and return
  if(ready_thread->priority == LOW_PRIORITY){
    disable_interrupt();
    disable_disk_interrupt();
    enqueue(ready_low, ready_thread);
    enable_disk_interrupt();
    enable_interrupt();
    return;
  }

  //If it is HIGH_PRIORITY, enqueue it
  if(ready_thread->priority == HIGH_PRIORITY){
    disable_interrupt();
    disable_disk_interrupt();
    sorted_enqueue(ready_high, ready_thread, ready_thread->remaining_ticks);
    enable_disk_interrupt();
    enable_interrupt();
    if (running->priority == LOW_PRIORITY){
      //Reset slice
      running->ticks =  QUANTUM_TICKS;
      //Change state to ready (INIT)
      running->state = INIT;
      disable_interrupt();
      disable_disk_interrupt();
      enqueue(ready_low, running);
      enable_disk_interrupt();
      enable_interrupt();
      //Get next thread to be executed
      TCB *next = scheduler();
      activator(next);
    }
  }
}


/* Free terminated thread and exits */
void mythread_exit() {
  //int tid = mythread_gettid();
  int tid = running->tid;
  printf("*** THREAD %d FINISHED\n", tid);
  running->state = FREE;
  free(t_state[tid].run_env.uc_stack.ss_sp);
  TCB* next = scheduler();
  activator(next);
}

/* Free running thread due to exceeded time*/
void mythread_timeout(int tid) {
    printf("*** THREAD %d EJECTED\n", tid);
    t_state[tid].state = FREE;
    free(t_state[tid].run_env.uc_stack.ss_sp);
    TCB* next = scheduler();
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


/* SJF for high priority, RR for low*/
TCB* scheduler()
{
  TCB *next;
  //If the high priority queue is not empty
  if(queue_empty(ready_high)==0){
    disable_interrupt();
    next = dequeue(ready_high);
    enable_interrupt();
    return next;
  }

  //If low priority queue is not empty
  if(queue_empty(ready_low)==0){
    disable_interrupt();
    next = dequeue(ready_low);
    enable_interrupt();
    return next;
  }

  //If waiting queue is not empty
  if(queue_empty(waiting)==0){
    return &idle;
  }
  //printf("mythread_free: No thread in the system\nExiting...\n");
  printf("*** FINISHED\n");
  exit(1);
}

/*TCB* running*/
/* Timer interrupt */
void timer_interrupt(int sig)
{
  //If running is IDLE, check if there are ready threads to be executed
  if(running->tid == -1){
    if(queue_empty(ready_high)==0 || queue_empty(ready_low)==0){
      TCB *next = scheduler();
      activator(next);
    }//If any, return
  return;
  }

  //Updating remaining_ticks. If exceed execution time, preempt it
  running->remaining_ticks = running->remaining_ticks -1;
  if(running->remaining_ticks<0){
    mythread_timeout(running->tid);
    return;
  }

  //If running is high, let it execute
  if (running->priority == HIGH_PRIORITY){
  return;
  }

  //After updating running ticks, if time slice is exceeded, get next thread to be executed
  running->ticks = running->ticks -1;
  if (running->ticks == 0){
    running->ticks = QUANTUM_TICKS;
    running->state = INIT;
    disable_interrupt();
    enqueue(ready_low, running);
    enable_interrupt();
    TCB *next = scheduler();
    activator(next);
  }
}

/* Activator */
void activator(TCB* next)
{
  TCB *prev = running;
  running = next;
  //If current and next threads are the same, return. (No swapcontext required)
  if(running==prev){
    return;
  }
  if(prev->state == FREE){
    printf("*** THREAD %d TERMINATED: SETCONTEXT OF %d\n", prev->tid, next->tid);
    setcontext(&(next->run_env));
    printf("mythread_free: After setcontext, should never get here!!...\n");
  }else if(prev->state == IDLE){
    printf("*** THREAD READY: SET CONTEXT TO %d\n", running->tid);
		printf("*** SWAPCONTEXT FROM %d TO %d\n", prev->tid, next->tid);
		swapcontext(&(prev->run_env), &(running->run_env));
  }else if(prev->state == WAITING){
    printf("*** THREAD %d READ FROM DISK\n", prev->tid);
		printf("*** SWAPCONTEXT FROM %d TO %d\n", prev->tid, next->tid);
		swapcontext(&(prev->run_env), &(running->run_env));
  }else{
    if(prev->priority == LOW_PRIORITY && running->priority == HIGH_PRIORITY){
      printf("*** THREAD %d PREEMPTED: SETCONTEXT OF %d\n", prev->tid, running->tid);
      printf("*** SWAPCONTEXT FROM %d TO %d\n", prev->tid, next->tid);
      swapcontext(&(prev->run_env), &(running->run_env));
    }else{
      printf("*** SWAPCONTEXT FROM %d TO %d\n", prev->tid, next->tid);
      swapcontext(&(prev->run_env), &(running->run_env));
    }
  }
}
