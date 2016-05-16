#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>

/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * beore locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
//static struct semaphore *intersectionSem;
static struct lock *intersectionLk;


static volatile int arr[12];
static struct cv *cvarr[12];

static int translate(Direction origin, Direction destination){
  int index = -1;
  if((origin == north)&&(destination == east)){
    index = 0;
  } else if((origin == north)&&(destination == south)){
    index = 1;
  } else if((origin == north)&&(destination == west)){
    index = 2;
  } else if((origin == south)&&(destination == west)){
    index = 3;
  } else if((origin == south)&&(destination == north)){
    index = 4;
  } else if((origin == south)&&(destination == east)){
    index = 5;
  } else if((origin == west)&&(destination == north)){
    index = 6;
  } else if((origin == west)&&(destination == east)){
    index = 7;
  } else if((origin == west)&&(destination == south)){
    index = 8;
  } else if((origin == east)&&(destination == south)){
    index = 9;
  } else if((origin == east)&&(destination == west)){
    index = 10;
  } else {
    index = 11;
  }
  return index;
}

static void
incre(int i){arr[i]++;}

static void
decre(int i){arr[i]--;}

static void
wakeup(int i){
  if(arr[i] == 0) cv_broadcast(cvarr[i], intersectionLk);
}

static void
modify(int index, void (*change)(int)){
  if(index == 0){
    change(3);
    change(4);
    change(5);
    change(6);
    change(7);
    change(9);
    change(10);
  } else if(index == 1){
    change(3);
    change(6);
    change(7);
    change(8);
    change(9);
    change(10);
  } else if(index == 2){
    change(3);
    change(10);
  } else if(index == 3){
    change(0);
    change(1);
    change(2);
    change(6);
    change(7);
    change(9);
    change(10);
  } else if(index == 4){
    change(0);
    change(6);
    change(7);
    change(9);
    change(10);
    change(11);
  } else if(index == 5){
    change(0);
    change(7);
  } else if(index == 6){
    change(0);
    change(1);
    change(3);
    change(4);
    change(9);
    change(10);
    change(11);
  } else if(index == 7){
    change(0);
    change(1);
    change(3);
    change(4);
    change(5);
    change(9);
  } else if(index == 8){
    change(1);
    change(9);
  } else if(index == 9){
    change(0);
    change(1);
    change(3);
    change(4);
    change(6);
    change(7);
    change(8);
  } else if(index == 10){
    change(0);
    change(1);
    change(2);
    change(3);
    change(4);
    change(6);
  } else {
    change(4);
    change(6);
  }
}
/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void)
{
  /* replace this default implementation with your own implementation */
  intersectionLk = lock_create("directionState");
  if(intersectionLk == NULL) {
    panic("could not create intersection lock");
  }
  cvarr[0] = cv_create("ne");
  cvarr[1] = cv_create("ns");
  cvarr[2] = cv_create("nw");
  cvarr[3] = cv_create("sw");
  cvarr[4] = cv_create("sn");
  cvarr[5] = cv_create("se");
  cvarr[6] = cv_create("wn");
  cvarr[7] = cv_create("we");
  cvarr[8] = cv_create("ws");
  cvarr[9] = cv_create("es");
  cvarr[10] = cv_create("ew");
  cvarr[11] = cv_create("en");
  for(int i = 0; i < 12; i++){
    arr[i] = 0;
  }
  return;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  /* replace this default implementation with your own implementation */
  KASSERT(intersectionLk != NULL);
  lock_destroy(intersectionLk);
  for(int i = 0; i < 12; i++){
    cv_destroy(cvarr[i]);
  }
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination) 
{
  /* replace this default implementation with your own implementation */
  KASSERT(intersectionLk != NULL);

  lock_acquire(intersectionLk);
  int index = translate(origin,destination);
  while(arr[index]){
    cv_wait(cvarr[index],intersectionLk);
  }
  modify(index,incre);
  lock_release(intersectionLk);
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{
  /* replace this default implementation with your own implementation */
  KASSERT(intersectionLk != NULL);

  lock_acquire(intersectionLk);
  int index = translate(origin, destination);
  modify(index, decre);
  modify(index, wakeup);
  lock_release(intersectionLk);
}


