## Guide: Determining the Reason for a Process Crash

When a process crashes unexpectedly, it's important to determine the root cause. One common cause is the operating system killing the process due to resource constraints, such as running out of memory (OOM). Here’s how to check and interpret such events:

### 1. Check System Logs for Killed Processes

Use the following command to search for killed processes in the kernel log:

```sh
dmesg | grep -i "killed process"
```

#### Example Output

```
[127692.538087] Out of memory: Killed process 99988 (deepstream) total-vm:49471188kB, anon-rss:5760364kB, file-rss:668kB, shmem-rss:15852kB, UID:0 pgtables:17732kB oom_score_adj:0
```

### 2. Explanation of Output

- **Out of memory:** The system ran out of available memory and invoked the OOM killer.
- **Killed process 99988 (deepstream):** The process with PID 99988 and name `deepstream` was terminated.
- **total-vm:** Total virtual memory used by the process.
- **anon-rss:** Anonymous memory (RAM) used by the process.
- **file-rss:** File-backed memory used.
- **shmem-rss:** Shared memory used.
- **UID:** User ID running the process.
- **pgtables:** Memory used for page tables.
- **oom_score_adj:** OOM killer score adjustment for the process.

### 3. What to Do Next

- **Check application logs** for additional context around the crash.
- **Monitor memory usage** of your application and system.
- **Optimize memory usage** in your application if possible.
- **Consider adding swap space** or increasing physical memory if OOM events are frequent.

---
This guide helps you quickly identify if a process was killed due to OOM and understand the details provided by the kernel log.
