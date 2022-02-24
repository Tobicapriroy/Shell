Student Information 
------------------- 
Zi Huang(zih19)
Mark Dang(marktd19) 

How to execute the shell 
------------------------ 

The execuation regarding our shell can be illustrated below. First, the status of the child process can be handled properly. 
If the pid is valid, we can determine their corresponding status: WIFEXITED, WIFSIGNALED, and WSTOPPED. Later, we concentrated 
on the basic functionality of the shell, including jobs, fg, bg, kill, stop, \^C, and \^Z. Last but not least, The implementation 
regarding I/O Redirection and pipelines can be executed using posix_spawn, a method that is the combination of fork() and exec().  

Important Notes 
--------------- 
<important notes about your project>
The project 1 in CS3214 about the customable shell is done by I and my partner Mark Dang equally. 

Description of Base Functionality 
--------------------------------- 

<describe your IMPLEMENTATION of the following commands: 
jobs, fg, bg, kill, stop, \ˆC, \ˆZ >

We create a method named runBuiltin to deal with the six commands above with exit.

Before these seven commands are implemented, the number of arguments in each command must be counted. Later, we picked the first argument
as the most important indicator to determine what the command is exactly. 

(1) If the first argument is <jobs>, we will need to SIGBLOCK and SIGUNBLOCK at first. Later, we are required to check the total number of 
arguments in the jobs. If it is equal to one, I can print each job properly. If not, I will be required to print an error message.

(2) If the first argument is <fg>, we will identify that this is a foreground command. As usual, a pair of SIGBLOCK and SIGUNBLOCK is utilized.
Therefore, what we are supposed to do is to set two local variables named fgJob and its corresponding jid, verifying the total number of
arguments. If the number of arguments for fg is 1, we can indicate that the job id will be missing. If the number of arguments for fg is 2,
we will obtain its current jid and the its job. If the job is already in the foreground stage, we will not need to pay attention to its 
foreground status. Last but not least, the status of foreground should not be ignored. To be able to implement it properly, the status can
be retrieved using killpg(). If the status is recognized to be zero, the status can be updated as foreground, print the job, wait for other
processes, and remove it.

(3) If the first argument is <bg>, its implementation is somehow identical to fg. The basic requirement, as depicted above, is to utilize a pair of
SIGBLOCK and SIGUNBLOCK. We then need to check the total number of arguments for bg command and initialize two variables named jidforBg and
bgJob. If the command bg has only 1 argument, we will state that its job id is currently missing. If the command bg contains 2 more arguments, its
jobid can be obtained from the second argument in fg, looking for its job via the method get_job_from_jid(). When the job bg is NULL, the statement
"No Such Job" will be printed. When the job bg is never stopped, we remark that the bg command was already in background. Later, the status of
the bg job can be detected by the combination of the local variable named status and the killpg() method. If the status is equal to zero, we will
realize that the procedure over the command bg is successfully operated and prints the job. Otherwise, we will indicate that the bg command fails
to be exeucted properly.

(4) If the first argument is <kill>, only two possibilities will take into account: the number of arguments equal to 2 and the number of arguments greater
than 2. However, before doing so, it is important not to forget a pair of SIGBLOCK and SIGUNBLOCK statements. Next, we will need to check the total number
of arguments for the command kill. If the number of commands for kill exceeds 2, the statement "Incorrect Number of Arguments for the command" will be 
displayed on your cush. Later, under normal circumstance, both the job id and its respective job can be identified so that we are available to determine
its status using the term SIGKILL.

(5) If the first argument is <stop>, it will no doubt to determine the we are required to terminate its job. Similar to what we already implemented in kill, 
the most critical prerequisite is to check the number of arguments for the stop command. If the number of arguments exceeds 2, we will indicate that this is
an incorrect number of arguments. Otherwise, Its job id and the specific details about its job are obtained properly, making use of killpg() method to 
finish the execution using the term SIGSTOP.

(6) If the first argument is either <\^c> or <\^z>, it can be generalized as the command named exit. Instead of concentrating on its desired implementation,
we are available to type an integer like 2 to represent its return value.    


Description of Extended Functionality 
-------------------------------------

<describe your IMPLEMENTATION of the following functionality: 
I/O, Pipes, Exclusive Access >

The category named Description of Extended Functionality is mainly used to analyze other advanced properties involving the shell, such as Redirection I/O for
file descriptors, Pipes for read and write, and its exclusive access.

To comprehend how I implement the extended functionality of the shell, I created a method named execute() with the parameter of type pipeline and instantiated the
variables job from add_job() method and length for keeping track of the number of pipelines between each command. If only a single command exists in the pipeline, 
the number of pipelines will be 1 by default. Later, a two-dimensional array is also initialized with two brackets, where the first bracket indicates the total number
of commands we concentrate on and the second bracket is used to display whether the pipeline is during the process of reading or writing, using a for loop to finish 
the construction of pipelines. 

After all commands and pipelines are set up, we are ready to deal with each child process by calling the posix_spawn() method recommended by TAs and instructors.
What we need to do is to use a for loop to traverse each command, prepare for the file actions and attributes each command encompasses, and analyze each process
steps by steps. If the current pipeline already becomes a member of the background job, its process group id will be an important indicator to know the current
situation of the job. More specifically, if the progress group id is currently 0, a flag can be set for each process, and its attribute is the numerical value 0.
Otherwise, its flag can be set with its attribute equivalent to the process group id of the job. However, the only distinction above is to guarantee that the process
group id equal to 0 needs to check its terminal status.

The most essential aspect of the extended functionality is the combination of addopen() and pipe() method so that both Redirection I/O and Pipes are able to be operated
normally. There are two possibilities required to take into account: the one at the beginning of the list and the one at the end of the list. If the command is the one at 
the beginning of the list, and its reading process can be implemented, the method addopen() in the posix_spawn class can be utilized. After the completion of the reading
process, the pipe() method appears that we need to consider the three scenarios altogether. If the number of commands in the job list is less than or equal to 1, the pipe
will be executed as usual. However, if the number of commands in the job list is greater than 1, all four possbilities, at the beginning of the list, in the middle of the
list, at the end of the list, and the standard error will pay serious attention to. If the command is at the beginning of the list, we will dpipe it from 0 and write it 
into the next command. If the command is at the end of the list, we will dpipe it from the end of the list and read it into the next standard input. If the command is located 
in the middle of the list, we will first read it from the previous command and write it into the next command. Last but not least, if a standard error is encountered, we will 
write the standard error into the standard output.

When a child process experiences many steps mentioned above and succeeds, its command pid points to the child process, becoming a process group id if the child process is
a first command. Otherwise, a statement "No such file or directory" will be printed, displaying it at the background job. After all child processes are finished processing,
the parent process is out of for loop to close all the pipes the child processes have. Meanwhile, each job must also sift all those processes that are failed. If failed, it 
must be removed from the job list.                               


List of Additional Builtins Implemented 
--------------------------------------- 
<cd>
The main purpose of the builtin cd is to ensure that the current directory we are located is HOME. If the number of arguments is any other numbers except 1, we will print the
statement cush: cd No such file or directory.

<history>
The second builtin we are available to concentrate on is history. It is used to help programmers recall the lines they have input. The method
can be brieflv overviewed as a history list to initialize and a for loop to traverse. Later, we created two submethods, one used to check
whether the input the user types contains a digit and one be identified as several scenarios to run the history command. 

(1) If the history command refers to my previous command, I can get its line and determine whether it is valid.   
(2) If the history command refers to the current histroy substitution, we will be able to keep track of the second part. If the second 
    part is a number, we can get its line, determining whether it is null. Otherwise, we can traverse the whole records in a reverse
    order, verifying whether it corresponds to the second part we got. If they match, we will return the whole value. 