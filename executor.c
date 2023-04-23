#include <bits/types/siginfo_t.h>
#include <bits/types/sigset_t.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>
#include <assert.h>
#include <signal.h>
#include <fcntl.h>

#include "executor.h"
#include "err.h"
#include "utils.h"

/* Idea explanation:
 * For each executor job, there are 4 processes created:
 * - bare executor, its job is to just run the task
 * - output/error readers - their job is to read output from bare executor and store it
 *   in a buffer. This way main can always look for the last line, without need for updating
 * - assistant - it's job is to wait for signals from bare executor/main.
 *   If bare executor finishes, assistant gets a signal and is able to print that information 
 *   even if main waits for next line of input. 
 */

int main() {
  char *buffer_in;
  size_t max_tasks = MAX_N_TASKS;

  // Allocating buffer for input
  size_t buffer_size = MAX_BUFFER_SIZE;
  buffer_in = (char*) malloc(buffer_size * sizeof(char));
  if(buffer_in == NULL) {
      perror("Unable to allocate buffer");
      exit(1);
  }

  const size_t shmem_size = 4 * sizeof(sem_t) 
    + 2 * sizeof(int) 
    + sizeof(atomic_int) 
    + sizeof(bool)
    + max_tasks * sizeof(pid_t);
  const size_t buffers_total = 2 * max_tasks * (MAX_BUFFER_SIZE * sizeof(char) + sizeof(char*)); // For stdout and stderr
  void* mapped_mem_all = mmap(NULL, shmem_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  // Table that contains last line of a given task (from stdout and stderr)
  void* mapped_mem_for_out = mmap(NULL, buffers_total, PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0); 
  void* mapped_mem_for_err = mmap(NULL, buffers_total, PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  if (mapped_mem_for_out == MAP_FAILED || mapped_mem_all == MAP_FAILED || mapped_mem_for_err == MAP_FAILED) {
    perror("mmap failed");
    exit(EXIT_FAILURE);
  }

  char** executor_out = (char**) mapped_mem_for_out;
  for (size_t ii = 0; ii < max_tasks; ii++)
    *(executor_out + ii) = (char*) (mapped_mem_for_out + max_tasks * sizeof(char*) + ii * MAX_BUFFER_SIZE * sizeof(char));

  char** executor_err = (char**) mapped_mem_for_err;
  for (size_t ii = 0; ii < max_tasks; ii++)
    *(executor_err + ii) = (char*) (mapped_mem_for_err + max_tasks * sizeof(char*) + ii * MAX_BUFFER_SIZE * sizeof(char));

  sem_t* mutex = (sem_t*) mapped_mem_all;
  sem_t* pending_wait = (sem_t*) (mapped_mem_all + sizeof(sem_t)); // For processes that ended while main was busy
  sem_t* new_process = (sem_t*) (mapped_mem_all + 2 * sizeof(sem_t)); // For synchronizing while creating new process
  sem_t* wait_for_barier = (sem_t*) (mapped_mem_all + 3 * sizeof(sem_t)); // Bariers for signals
  sem_init(mutex, 1, 1);
  sem_init(pending_wait, 1, 0); // For tasks that ended while main was busy
  sem_init(new_process, 1, 0); // Stops main until creation of a new executor instance is compleated
  sem_init(wait_for_barier, 1, 0); // Prevent assistants from writing while main is busy
  int* creation_steps = (int*) (mapped_mem_all + 4 * sizeof(sem_t));
  *creation_steps = 0; // To keep main waiting until assistant, executor and both readers are ready
  int* total_num_of_tasks = (int*) (mapped_mem_all + 4 * sizeof(sem_t) + sizeof(int));
  *total_num_of_tasks = 0;
  atomic_int* pending_tasks = (atomic_int*) (mapped_mem_all + 4 * sizeof(sem_t) + 2 * sizeof(int));
  *pending_tasks = 0; // Task that are waiting for main to finish its current job (read_line is not treated as a job)
  bool* main_busy = (bool*) (mapped_mem_all + 4 * sizeof(sem_t) + 2 * sizeof(int) + sizeof(atomic_int));
  *main_busy = false;
  pid_t* assistants = (pid_t*)(mapped_mem_all + 4 * sizeof(sem_t) 
      + 2 * sizeof(int) + sizeof(atomic_int) + sizeof(bool));

  struct global_data gd;
  gd.mutex = mutex;
  gd.pending_wait = pending_wait;
  gd.pending_tasks = pending_tasks;
  gd.wait_for_barier = wait_for_barier;
  gd.creation_steps = creation_steps; 
  gd.assistants = assistants;
  gd.main_pid = getpid();
  gd.main_busy = main_busy;
  gd.new_process = new_process;
  gd.total_num_of_tasks = total_num_of_tasks;

  bool continue_loop = true; 
  do {
    continue_loop = read_line(buffer_in, buffer_size, stdin);   

    sem_wait(gd.mutex);
    *gd.main_busy = true;
    sem_post(gd.mutex);

    if (continue_loop) 
      parse_input(&buffer_in, gd, &continue_loop, executor_out, executor_err);

    sem_wait(gd.mutex);
    *gd.main_busy = false;

    if (*gd.pending_tasks > 0) {
      sem_post(gd.mutex);
      sem_post(gd.pending_wait);
      sem_wait(gd.wait_for_barier);
    }
    else
      sem_post(gd.mutex);
  } while(continue_loop);

  // Killing remaining tasks
  for (size_t ii = 0; ii < *gd.total_num_of_tasks; ii++)
    kill(assistants[ii], SIGUSR1);

  int status;
  // Waiting for remaining tasks to end
  for (size_t ii = 0; ii < *gd.total_num_of_tasks; ii++)
    waitpid(assistants[ii], &status, 0);

  free(buffer_in);
}

void parse_input(char **buffer, struct global_data gd, bool* continue_loop, 
    char** executor_out, char** executor_err) {
  char **res = split_string(*buffer);

  if (strcmp(*res, "run") == 0) {
    run(res + 1, gd, executor_out, executor_err);
  } else if (strcmp(*res, "out") == 0) {
    int index = atoi(*(res + 1));
    sem_wait(gd.mutex);
    printf("Task %d stdout: '%s'.\n", index, *(executor_out + index));
    fflush(stdout);
    sem_post(gd.mutex);
  } else if (strcmp(*res, "err") == 0) {
    int index = atoi(*(res + 1));
    sem_wait(gd.mutex);
    printf("Task %d stderr: '%s'.\n", index, *(executor_err + index));
    fflush(stdout);
    sem_post(gd.mutex);
  } else if (strcmp(*res, "kill") == 0) {
    assert(*(res + 1) != NULL);
    int to_kill = atoi(*(res + 1));
    kill(gd.assistants[to_kill], SIGINT);
    sem_wait(gd.wait_for_barier); 
  } else if (strcmp(*res, "sleep") == 0) {
    usleep(1000 * atoi(*(res + 1))); // 1000 is a conversion between micro and miliseconds
  } else if (strcmp(*res, "quit") == 0) {
    *continue_loop = false;
  } 
  
  free_split_string(res); 
}

void run(char **args, struct global_data gd, char** executor_out, char** executor_err) {
  int task_num = *gd.total_num_of_tasks;
  (*gd.total_num_of_tasks)++;
  pid_t pid = fork();
  
  if (!pid) { // Created an assistant
    // Assistant

    pid_t pidworker = fork(); // Creating an executor instance
    if (!pidworker) { 
      // Create out/err readers
      int pipe_dsc_out[2];
      pipe(pipe_dsc_out);
      pid_t pidreader = fork(); 

      int input_descriptor = first_open_greater_that(STDERR_FILENO);
      if (!pidreader) {
        // Out reader
        close(pipe_dsc_out[1]);
        dup2(pipe_dsc_out[0], input_descriptor);
        close(pipe_dsc_out[0]);
        close(STDIN_FILENO); 

        run_watcher(input_descriptor, gd, task_num, executor_out);
      }
      else { 
        int pipe_dsc_err[2];
        pipe(pipe_dsc_err);
        pid_t piderror = fork(); // Creating an executor instance

        if (!piderror) {
          // err reader
          close(pipe_dsc_out[0]);
          close(pipe_dsc_out[1]);

          int error_descriptor = first_open_greater_that(input_descriptor);
          close(pipe_dsc_err[1]);
          dup2(pipe_dsc_err[0], error_descriptor);
          close(pipe_dsc_err[0]);
          close(STDIN_FILENO);

          run_watcher(error_descriptor, gd, task_num, executor_err);
        } else {
          close(pipe_dsc_out[0]);
          dup2(pipe_dsc_out[1], STDOUT_FILENO);
          close(pipe_dsc_out[1]);

          close(pipe_dsc_err[0]);
          dup2(pipe_dsc_err[1], STDERR_FILENO);
          close(pipe_dsc_err[1]);

          sem_wait(gd.mutex);
          if (*gd.creation_steps == 3) {
            (*gd.creation_steps) = 0;
            sem_post(gd.new_process);
            sem_post(gd.mutex);
          }
          else {
            (*gd.creation_steps)++;
            sem_post(gd.mutex);
          }

          int status;
          waitpid(pidreader, &status, WNOHANG);
          waitpid(piderror, &status, WNOHANG);
          execvp(*args, args);
          exit(1);
        }
      }
    }
    else {
      sem_wait(gd.mutex);
      printf("Task %d started: pid %d.\n", task_num, pidworker);
      fflush(stdout);
      close(STDIN_FILENO);
      sem_post(gd.mutex);
      // Assistant signals gd.new_process!!!
      assistant(pidworker, gd, task_num);
      exit(1);
    }
  }
  else {
    // Wait until creation process is over
    sem_wait(gd.new_process);
    sem_wait(gd.mutex);
    *(gd.assistants + task_num) = pid;
    sem_post(gd.mutex);
  }
}


void assistant(pid_t my_worker, struct global_data gd, int task_num) {
    sigset_t signals_to_wait_for;
    sigemptyset(&signals_to_wait_for);
    sigaddset(&signals_to_wait_for, SIGCHLD);
    sigaddset(&signals_to_wait_for, SIGINT);
    sigaddset(&signals_to_wait_for, SIGUSR1);
    sigprocmask(SIG_BLOCK, &signals_to_wait_for, NULL);

    sem_wait(gd.mutex);
    if (*gd.creation_steps == 3) {
      (*gd.creation_steps) = 0;
      sem_post(gd.new_process);
      sem_post(gd.mutex);
    }
    else {
      (*gd.creation_steps)++;
      sem_post(gd.mutex);
    }

    siginfo_t info;
    int status;
    int exit_status = 0;
    bool signal_sent = false;
    while (true) {
      sigwaitinfo(&signals_to_wait_for, &info);

      // Task is finished, write information and exit
      if (info.si_signo == SIGCHLD) {
        if (*gd.main_busy) {
          (*gd.pending_tasks)++;
          sem_wait(gd.pending_wait); 
          (*gd.pending_tasks)--;
        }

        waitpid(my_worker, &status, 0);
        if (WIFEXITED(status)) {
          exit_status = WEXITSTATUS(status);

          sem_wait(gd.mutex);
          printf("Task %d ended: status %d.\n", task_num, exit_status);
          fflush(stdout);
          sem_post(gd.mutex);
        } else if (WIFSIGNALED(status)) {
          sem_wait(gd.mutex);
          printf("Task %d ended: signalled.\n", task_num);
          fflush(stdout);
          sem_post(gd.mutex);
        }

        if (*gd.pending_tasks > 0)
          sem_post(gd.pending_wait);
        else 
          sem_post(gd.wait_for_barier);
        exit(1);
      }

      // Send SIGINT and continue waiting for other signals (since a task doesn't have to end on SIGINT)
      if (info.si_signo == SIGINT) {
        kill(my_worker, SIGINT);
        if (!signal_sent) {
          signal_sent = true;
          waitpid(my_worker, &status, WNOHANG);
        }
        sem_post(gd.wait_for_barier);
      }

      // The whole program ends, every task has to finish
      if (info.si_signo == SIGUSR1) { // Special signal to kill the program
        kill(my_worker, SIGINT);
        usleep(WAIT_BEFORE_KILL); // Suggested scenario to end process on quit()
        kill(my_worker, SIGKILL);
        waitpid(my_worker, &status, 0);

        sem_wait(gd.mutex);
        printf("Task %d ended: signalled.\n", task_num);
        fflush(stdout);
        sem_post(gd.mutex);

        sem_post(gd.wait_for_barier); 
        exit(1); 
      }
    }
}


int first_open_greater_that(int arg) {
  int fd = arg + 1;
  // Keep incrementing the file descriptor until we find one that is not in use
  while (fcntl(fd, F_GETFL) != -1)
    ++fd;
  return fd;
}

_Noreturn void run_watcher(int input_descriptor, struct global_data gd, int task_num, char** executor_buffer){
        char* buffer = (char *)malloc(MAX_BUFFER_SIZE * sizeof(char));
        size_t buffer_size = MAX_BUFFER_SIZE;
        FILE* input_stream = fdopen(input_descriptor, "r");

        sem_wait(gd.mutex);
        if (*gd.creation_steps == 3) {
          (*gd.creation_steps) = 0;
          sem_post(gd.new_process); 
          sem_post(gd.mutex);
        }
        else {
          (*gd.creation_steps)++;
          sem_post(gd.mutex);
        }
        
        bool res;
        do {
          res = read_line(buffer, buffer_size, input_stream);
          sem_wait(gd.mutex);
          if (res) {
            char *newline = strrchr(buffer, '\n');
            if (newline)
                strcpy(newline, newline + 1);
            strcpy(*(executor_buffer + task_num), buffer); 
          }
          sem_post(gd.mutex);
        } while(res);
        free(buffer);
        fclose(input_stream);
        exit(1);
}
