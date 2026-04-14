1.



2. Build, Load, and Run Instructions
Build the Project
To compile the supervisor, the CLI engine, and the kernel monitoring module, run the following command from the root directory:

make clean && make

Load the Kernel Module
Before starting the runtime, the kernel monitor must be loaded into the system:

sudo insmod monitor.ko

You can verify it is active by running: lsmod | grep monitor

Start the Supervisor
The supervisor acts as the central daemon. Open a terminal (Terminal 1) and run:

while true; do sudo ./engine supervisor ./rootfs-base; done

(The while loop ensures the supervisor restarts if a container crash causes an exit, allowing you to finish your demo smoothly).

Launch Containers and Use the CLI
Open a second terminal (Terminal 2) to interact with the supervisor using the following commands:

Start a container: sudo ./engine start c1 ./rootfs-base "/bin/ls -la"

List tracked containers: ./engine ps

View container logs: cat logs/c1.log

Stop a running container: ./engine stop c1

Cleanup and Unload
To safely tear down the environment:

Stop the supervisor in Terminal 1 using Ctrl + C.

Unload the kernel module: sudo rmmod monitor

Remove the socket file if it persists: sudo rm /tmp/mini_runtime.sock

4. Engineering Analysis
Isolation Mechanisms
Our runtime achieves process and filesystem isolation primarily through the use of Linux Namespaces. The PID namespace ensures that processes inside the container cannot see or interfere with processes on the host system or in other containers. The UTS namespace allows each container to maintain its own hostname identity. For filesystem isolation, we use the chroot mechanism to change the root directory of the container to a specific copy of the rootfs-base. This creates a "jail" where the container only sees its assigned files. Despite this isolation, the host kernel is still shared among all containers; they share the same syscall interface and memory management, which provides high efficiency compared to traditional virtual machines.

Supervisor and Process Lifecycle
A long-running parent supervisor is vital for managing the container lifecycle. It serves as the stable "anchor" that creates child processes via the clone() system call. Because the supervisor remains alive, it can effectively "reap" exited children using SIGCHLD and waitpid(), which prevents the accumulation of zombie processes. Furthermore, the supervisor maintains an in-memory metadata table that tracks the state, PIDs, and start times of every container, acting as a source of truth for the CLI client.

IPC, Threads, and Synchronization
The project utilizes two distinct IPC mechanisms: Unix Domain Sockets for command delivery from the CLI to the supervisor, and Pipes for streaming container output into the logging system. For our bounded-buffer logging design, we use Mutexes to protect shared data structures. A primary race condition exists where multiple producer threads (containers) might attempt to write to the log buffer simultaneously, or the consumer thread might try to read an incomplete entry. Our choice of Mutexes and Condition Variables ensures mutual exclusion and synchronization, allowing threads to "sleep" when the buffer is full or empty rather than wasting CPU cycles.

Memory Management and Enforcement
Resident Set Size (RSS) measures the actual physical RAM occupied by a process, excluding swapped-out memory or unallocated virtual address space. Our system implements both soft and hard limits. A soft limit acts as a non-fatal warning policy that logs an event when a threshold is crossed, while a hard limit is a strict enforcement policy that terminates the process to protect host stability. Enforcement belongs in kernel space because user-space polling is too slow; the kernel can intercept and deny memory allocations in real-time before a "rogue" container can cause a system-wide crash.

Scheduling Behavior
Our experiments with concurrent workloads demonstrated the effectiveness of the Linux Completely Fair Scheduler (CFS). When running a CPU-bound Python script alongside an I/O-bound ls command, the scheduler ensured "fairness" by giving both containers a share of CPU time. The results showed high responsiveness for the I/O-bound task despite the heavy load from the CPU-bound task, proving that the Linux kernel successfully balances throughput and latency even when processes are wrapped in isolation namespaces.

5. Design Decisions and Tradeoffs
Namespace Isolation
We chose to use chroot for filesystem isolation. While pivot_root is technically more secure because it makes it harder to "break out" of the jail, chroot is significantly simpler to implement and debug for a hackathon-scale project. The tradeoff is a slightly lower security boundary in exchange for a much more stable and readable codebase.

Logging Architecture
We implemented a bounded-buffer design for the logging pipeline. This ensures that the supervisor's memory usage remains predictable even if a container produces a massive amount of output. The tradeoff is that if the log-writing thread is slow, the container (producer) may be forced to block until space opens up, potentially slowing down the container's execution.

IPC Channel
We selected Unix Domain Sockets for our CLI-to-Supervisor communication. Compared to standard network sockets (TCP/UDP), Unix sockets are faster and more secure for local IPC because they don't involve the network stack overhead. The tradeoff is that the CLI must have access to the specific socket file on the local filesystem, meaning it cannot control containers across a network.

6. Scheduler Experiment Results
Workload Comparison
We conducted an experiment by launching two containers simultaneously: Container 1 (c1) running a CPU-intensive Python loop and Container 2 (c2) running a standard recursive directory listing.

Observations
The raw data showed that both containers were successfully tracked by the supervisor with unique PIDs. Even with the CPU-bound script attempting to consume maximum resources, the I/O-bound container finished its task with no perceptible delay. This confirms that the Linux scheduler successfully maintains "Fairness" and "Responsiveness." The CPU-bound task utilized the remaining available cycles, maximizing "Throughput" without starving the simpler command, proving that our container runtime does not interfere with the kernel's ability to manage process priorities effectively.
