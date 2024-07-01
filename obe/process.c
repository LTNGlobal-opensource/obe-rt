#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "process.h"

#define BUFFER_SIZE 1024

int obe_process_calculate_cpu_usage(int pid, double *total_time)
{
    char buf[256];
    char fn[64];
    snprintf(fn, sizeof(fn), "/proc/%d/stat", pid);

    // Open the /proc/[pid]/stat file
    FILE *fh = fopen(fn, "r");
    if (fh == NULL) {
        return -1;
    }

    if (fgets(buf, sizeof(buf), fh) == NULL) {
        fclose(fh);
        return -1;
    }

    fclose(fh);

    // Parse the required fields from the stat file
    int utime, stime;
    sscanf(buf, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %d %d", &utime, &stime);

    // Calculate total time spent in user and kernel mode
    *total_time = (double)(utime + stime);

    return 0; /* Success */
}
