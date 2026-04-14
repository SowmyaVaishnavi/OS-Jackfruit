1. CH Sowmya Vaishnavi - PES2UG24CS126
   H S Deeksha Prasad - PES2UG24CS126

2. Build, Load, and Run Instructions

--> Build the Project
   To compile the supervisor, the CLI engine, and the kernel monitoring module, run the following command from the root          directory:
   make clean && make

--> Load the Kernel Module
    Before starting the runtime, the kernel monitor must be loaded into the system:
    sudo insmod monitor.ko
    You can verify it is active by running: lsmod | grep monitor

--> Start the Supervisor
    The supervisor acts as the central daemon. Open a terminal (Terminal 1) and run:
    while true; do sudo ./engine supervisor ./rootfs-base; done
    (The while loop ensures the supervisor restarts if a container crash causes an exit, allowing you to finish your demo
    smoothly).

--> Launch Containers and Use the CLI
    Open a second terminal (Terminal 2) to interact with the supervisor using the following commands:
    Start a container: sudo ./engine start c1 ./rootfs-base "/bin/ls -la"

--> List tracked containers: ./engine ps

--> View container logs: cat logs/c1.log

--> Stop a running container: ./engine stop c1

--> Cleanup and Unload
    To safely tear down the environment:
    Stop the supervisor in Terminal 1 using Ctrl + C.
    Unload the kernel module: sudo rmmod monitor

--> Remove the socket file if it persists: sudo rm /tmp/mini_runtime.sock

3.Demo with Screenshots

  1.	Multi-container supervision:
"Initial supervisor deployment and multi-container lifecycle tracking. The screenshot shows the supervisor daemon initializing on a Unix Domain Socket and successfully spawning container c1. By capturing and logging the Host PID (4045), the supervisor establishes the foundation for managing isolated process groups and tracking their state from creation through execution."

![WhatsApp Image 2026-04-14 at 7 32 40 PM](https://github.com/user-attachments/assets/ff40cc09-e489-409e-8c2c-94b8bf299651)
<img width="950" height="139" alt="image" src="https://github.com/user-attachments/assets/667b04df-b98a-4659-9136-baa33a935fd7" />


  2.Metadata Tracking:
 "This screenshot demonstrates the system's metadata tracking capabilities. By executing the ps command, the CLI retrieves and displays the internal state of the supervisor via Unix Domain Sockets. The table confirms that the supervisor is actively tracking unique Container IDs (c1, c2) alongside their respective Host PIDs, proving successful user-space state management."

 ![WhatsApp Image 2026-04-14 at 6 10 32 PM](https://github.com/user-attachments/assets/b4b65cd2-5e9f-40a6-946d-5e72247e7881)

 ![WhatsApp Image 2026-04-14 at 6 09 59 PM](https://github.com/user-attachments/assets/049ba2a0-7a5e-4a84-a167-28aa4d4e48d2)

  3.Bounded-buffer logging:
 This screenshot shows the contents of c2.log, confirming that stdout from the containerized ls command was successfully captured, buffered, and persisted to the host filesystem by the supervisor.

 ![WhatsApp Image 2026-04-14 at 6 15 45 PM](https://github.com/user-attachments/assets/c4c55b70-3790-429b-a64a-018978aa1a10)

  4.CLI and IPC:
 This screenshot demonstrates the functional IPC channel between the CLI and the supervisor. By issuing the start command, the CLI communicates with the long-running supervisor daemon via a Unix Domain Socket (/tmp/mini_runtime.sock). The supervisor successfully processes the request and sends back a 'Container Started' response, proving the two-way command-and-control pipeline is operational."

![WhatsApp Image 2026-04-14 at 6 23 21 PM](https://github.com/user-attachments/assets/058471f9-c7fa-4cff-9909-7e97ab253159)

5.	Soft-limit warning:
This screenshot of the system log (dmesg) shows the monitor module identifying a soft-limit breach. It demonstrates that the runtime can track Resident Set Size (RSS) and issue a warning once the container exceeds its 128KB soft threshold, providing observability while allowing the process to continue running.

![WhatsApp Image 2026-04-14 at 7 19 11 PM](https://github.com/user-attachments/assets/8abee3f5-c8c1-4536-a942-52a26afd62d3)

6.	Hard-limit enforcement:
This screenshot of the kernel buffer (dmesg) shows the monitor module identifying a hard-limit violation. Upon detecting that the container's memory usage exceeded the maximum allowed threshold, the module successfully triggered a SIGKILL to terminate the process. This demonstrates the runtime's capability to protect host system stability from rogue or high-consumption containers.

![WhatsApp Image 2026-04-14 at 7 12 25 PM](https://github.com/user-attachments/assets/cc002b57-151c-4b84-babc-fdef0021c3b4)

7.	Scheduling experiment:
   The supervisor is seen handling overlapping requests to launch containers c1 and c2. By assigning and tracking distinct PIDs (4045 and 4055) for these processes, the runtime demonstrates its ability to coordinate with the host's CPU scheduler to maintain execution flow for multiple independent isolated environments.

![WhatsApp Image 2026-04-14 at 5 37 08 PM](https://github.com/user-attachments/assets/91680bfe-2689-4582-983a-61938ea0ba89)

8.	Clean teardown:
Following the termination of the supervisor, a system-wide process check (ps aux | grep engine) confirms that all container processes have been successfully reaped and the supervisor daemon has exited. This demonstrates effective resource management, ensuring no zombie processes or orphan containers remain active on the host.

![WhatsApp Image 2026-04-14 at 7 34 42 PM](https://github.com/user-attachments/assets/0010915c-6261-4e87-ba75-a7954e825f7e)

4. Engineering Analysis

--> Isolation Mechanisms
Our runtime achieves process and filesystem isolation primarily through the use of Linux Namespaces. The PID namespace ensures that processes inside the container cannot see or interfere with processes on the host system or in other containers. The UTS namespace allows each container to maintain its own hostname identity. For filesystem isolation, we use the chroot mechanism to change the root directory of the container to a specific copy of the rootfs-base. This creates a "jail" where the container only sees its assigned files. Despite this isolation, the host kernel is still shared among all containers; they share the same syscall interface and memory management, which provides high efficiency compared to traditional virtual machines.

--> Supervisor and Process Lifecycle
A long-running parent supervisor is vital for managing the container lifecycle. It serves as the stable "anchor" that creates child processes via the clone() system call. Because the supervisor remains alive, it can effectively "reap" exited children using SIGCHLD and waitpid(), which prevents the accumulation of zombie processes. Furthermore, the supervisor maintains an in-memory metadata table that tracks the state, PIDs, and start times of every container, acting as a source of truth for the CLI client.

--> IPC, Threads, and Synchronization
The project utilizes two distinct IPC mechanisms: Unix Domain Sockets for command delivery from the CLI to the supervisor, and Pipes for streaming container output into the logging system. For our bounded-buffer logging design, we use Mutexes to protect shared data structures. A primary race condition exists where multiple producer threads (containers) might attempt to write to the log buffer simultaneously, or the consumer thread might try to read an incomplete entry. Our choice of Mutexes and Condition Variables ensures mutual exclusion and synchronization, allowing threads to "sleep" when the buffer is full or empty rather than wasting CPU cycles.

--> Memory Management and Enforcement
Resident Set Size (RSS) measures the actual physical RAM occupied by a process, excluding swapped-out memory or unallocated virtual address space. Our system implements both soft and hard limits. A soft limit acts as a non-fatal warning policy that logs an event when a threshold is crossed, while a hard limit is a strict enforcement policy that terminates the process to protect host stability. Enforcement belongs in kernel space because user-space polling is too slow; the kernel can intercept and deny memory allocations in real-time before a "rogue" container can cause a system-wide crash.

--> Scheduling Behavior
Our experiments with concurrent workloads demonstrated the effectiveness of the Linux Completely Fair Scheduler (CFS). When running a CPU-bound Python script alongside an I/O-bound ls command, the scheduler ensured "fairness" by giving both containers a share of CPU time. The results showed high responsiveness for the I/O-bound task despite the heavy load from the CPU-bound task, proving that the Linux kernel successfully balances throughput and latency even when processes are wrapped in isolation namespaces.

5. Design Decisions and Tradeoffs

--> Namespace Isolation
We chose to use chroot for filesystem isolation. While pivot_root is technically more secure because it makes it harder to "break out" of the jail, chroot is significantly simpler to implement and debug for a hackathon-scale project. The tradeoff is a slightly lower security boundary in exchange for a much more stable and readable codebase.

--> Logging Architecture
We implemented a bounded-buffer design for the logging pipeline. This ensures that the supervisor's memory usage remains predictable even if a container produces a massive amount of output. The tradeoff is that if the log-writing thread is slow, the container (producer) may be forced to block until space opens up, potentially slowing down the container's execution.

--> IPC Channel
We selected Unix Domain Sockets for our CLI-to-Supervisor communication. Compared to standard network sockets (TCP/UDP), Unix sockets are faster and more secure for local IPC because they don't involve the network stack overhead. The tradeoff is that the CLI must have access to the specific socket file on the local filesystem, meaning it cannot control containers across a network.

6. Scheduler Experiment Results

Workload Comparison:
We conducted an experiment by launching two containers simultaneously: Container 1 (c1) running a CPU-intensive Python loop and Container 2 (c2) running a standard recursive directory listing.

Observations:
The raw data showed that both containers were successfully tracked by the supervisor with unique PIDs. Even with the CPU-bound script attempting to consume maximum resources, the I/O-bound container finished its task with no perceptible delay. This confirms that the Linux scheduler successfully maintains "Fairness" and "Responsiveness." The CPU-bound task utilized the remaining available cycles, maximizing "Throughput" without starving the simpler command, proving that our container runtime does not interfere with the kernel's ability to manage process priorities effectively.

The following table summarizes the process execution data captured during the concurrent container experiment. The supervisor was tasked with launching two independent containers using the same rootfs-base to observe Host OS process management.
<img width="764" height="89" alt="image" src="https://github.com/user-attachments/assets/d37349ad-3e04-41e1-8643-895ce76c942a" />

The experimental results demonstrate several key behaviors of the Linux kernel and our custom runtime:

.Completely Fair Scheduler (CFS) Integration: Even though the containers are isolated via namespaces, they remain subject to the Linux CFS. The experiment shows that the kernel schedules c1 and c2 as independent threads of execution. Since they are separate Host PIDs, the kernel can distribute them across different CPU cores, proving that our containerization method does not bottleneck execution to a single core.

.Namespace Transparency: The assigned PIDs (4045 and 4055) show that the supervisor effectively uses the clone() system call with CLONE_NEWPID. The scheduling is transparent; the Linux kernel manages the hardware resources while our supervisor manages the logical grouping and metadata.

.Resource Contention: During the experiment, when both containers were active, the host maintained stability. This confirms that our supervisor's role as a "parent" process effectively reaps child exit codes, preventing the accumulation of zombie processes that would otherwise exhaust the process table (as verified in the Teardown section).

