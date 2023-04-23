#ifndef MIM_ERR_H
#define MIM_ERR_H

#include <stddef.h>
#include <semaphore.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdbool.h>

enum {
  MAX_BUFFER_SIZE = 511,
  MAX_OUT_SIZE = 1022,
  MAX_N_TASKS = 4096,
  WAIT_BEFORE_KILL = 200000 // 200 ms
};


struct global_data {
  sem_t* mutex;
  sem_t* pending_wait;
  sem_t* new_process;
  sem_t* wait_for_barier;
  int* creation_steps;
  int* total_num_of_tasks;
  atomic_int* pending_tasks;
  pid_t* assistants;
  pid_t main_pid;
  bool* main_busy;
};

/* Parses input given in a specification of a task. Invokes appropriate function.*/
void parse_input(char **buffer, struct global_data gd, bool* quit, 
    char** executor_out, char** executor_err);

/* Creates a new process and activates Task T with arguments ... in it*/
void run(char **args, struct global_data gd, char** executor_out, char** executor_err);
/* Main function of assistant*/
void assistant(pid_t my_worker, struct global_data gd, int task_num);

/* Returns a number of first available descriptor graten then arg*/
int first_open_greater_that(int arg);

/* Main function of out_reader/err_reader. 
 * Gets  input from input_descriptor and writes it to a buffer.
 * Continues to do so until the end of input, then exits.*/
_Noreturn void run_watcher(int input_descriptor, struct global_data gd, int task_num, char** executor_buffer);
#endif
