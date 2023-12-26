Created by:
Johan Lee
Kevin He

How to execute the shell
------------------------
To execute from the command line, make sure you are in the src 
directory and use './cush'

Important Notes
---------------
Implementation written in cush.c



Description of Base Functionality
---------------------------------
<describe your IMPLEMENTATION of the following commands:
jobs, fg, bg, kill, stop, \ˆC, \ˆZ >

jobs: We iterated through the jobs list and kept track of the index with a 
currentJob variable, printing out the whole list.

fg: We got the job ID from the integer argument and set the status of the
job to FOREGROUND as well as the bg_job to false. We then printed out the 
commandline that belonged to that job. We also had to give the termstate to
the job and send a SIGCONT to let it run in the foreground as we wait_for_job
and let it complete, blocking SIGCHLD before and unblocking after.

bg: We got the job ID from the integer argument and set the status of the
job to BACKGROUND as well as the bg_job to true. We then printed out the 
commandline that belonged to that job. We made sure to give the termstate
to the shell and sent a SIGCONT signal to the job to allow it to run in
the background.

kill: We got the job ID from the integer argument and sent the SIGTERM signal
to the process group to terminate it.

stop: We got the job ID from the integer argument and sent the SIGTSTP signal
to the process group to stop it. We also printed out the stopped job.

\^C: We implemented this in handle_child_status for the case where
(WTERMSIG(status) == SIGINT), we would set the currentJob status to 
FOREGROUND and decrement the num_processes_alive.

\^Z: We implemented this in handle_child_status for the case where
(WIFSTOPSIG(status) == SIGTSTP), we would set the currentJob status to 
STOPPED and print out the stopped job. We would also save the termstate before.



Description of Extended Functionality
-------------------------------------

I/O: We checked the pipeline fields for non-NULL iored_input and iored_output. 
If iored_input was not NULL and it was the first command, we would use
posix_spawn_file_actions_addopen to open the given iored_input(file name) when it
spawned and redirect input there. Similarly, if iored_output was not NULL and it 
was the last command, we would use posix_spawn_file_actions_addopen to open the given
iored_output(file name) when it spawned. We also checked the append_to_output field 
and would append the output to the file if it was true.

Pipes: We initialized a 2D pipe array before looping through the pipeline. Inside
the for loop, we initialized a second 2D pipe array and initialized the pipes if 
the size of the commands list was greater than 1, using pipe2(). We used 
posix_spawn_file_actions_adddup2 to wire the connections between pipes and stdin/stdout
We would then close the pipes after posix spawn spawned a processed and if needed we would
save the read end of the pipe for connecting processes to the next iteration. We implemented
in each iteration a switch from the inside pipe to the pipe outside the loop. This helped us 
pass on the correct end to the next iteration to be read from.

Exclusive access: We manage background and foreground processes through the use
of the termstate functions provided. When running foreground processes, we give
the termstate_give_terminal_to to the job. When running background processes,
we make sure the shell saves the termstate and then gives terminal back to shell.
When the shell returns to the prompt it makes itself the foreground process group of the 
terminal. When a job exits successfully with a status of 0, we make sure to sample the termstate.
Thus, the terminal will be in this sampled state when a new job is started. 


List of Additional Builtins Implemented
---------------------------------------
(Written by Your Team)

cd: This simple built in allows the user to switch into the directory given by the first
argument (argv[1]). If no argument is given, the user will be redirected to the home
directory. If a given argument for the directory does not exist, an error message
will be sent to the user such as "No such file or directory". In our test case we made 
a directory and cd'ed into it, then we used pwd to check that we were in the correct
directory.

history: This complex built in allows users to see previously typed commands as
well as using the up and down arrows to get previously typed commands into the current
prompt. Typing in 'history' will allow users to see a list of chronologically
typed commands. In our test case, we sent a command called 'sleep 1' and then history, 
then made sure that this was correct.
