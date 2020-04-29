#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdbool.h>
#include <hdr_histogram.h>
#include <sched.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <zconf.h>
#include <sys/io.h>
#include <errno.h>

#define WRITER_BATCH_SIZE_KB 16 // write into this much worth of pages and capture the time
#define REPORT_STATS_EVERY_MB 1024 // every N megabytes we'll dump percentiles into our "time series"

#define PG_SIZE 4096L // only testing standard 4k pages
#define MAP_SIZE 1L * 1024L * 1024L * 1024L // using 1 GB mappings

#define MUNMAP_CPU 22 // cpu of the thread responsible for unmapping the first file asynchronously
#define WRITER_CPU 23 // cpu of the thread that is expected to be impacted by MUNMAP thread

#define INFLUX_BUF_SIZE 1500 // has to fit the default MTU
int sockfd;
char buf[INFLUX_BUF_SIZE];
struct sockaddr_in serverAddr;
struct timespec sleep_requested = {.tv_sec = 0, .tv_nsec = 1000000l};
struct timespec sleep_remaining;

struct hdr_histogram* histo;

typedef struct stats {
    long long timestamp;
    long double mean;
    long long min;
    long long p50;
    long long p90;
    long long p99;
    long long max;
} stats;

typedef struct thread_args {
    void *addr1;
    void *addr2;
    void *data;
    int *count;
    unsigned int target_cpu;
} thread_args;


long long now() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// ----------------------------------------------------------------
// Support functions for sending data to InfluxDB. Skip this

void transmit() {
    sendto(sockfd, buf, strlen(buf), SOCK_NONBLOCK, &serverAddr, sizeof serverAddr);
    bzero(buf, INFLUX_BUF_SIZE);
    nanosleep(&sleep_requested, &sleep_remaining);
}

void publish_stats(struct stats* stats) {
    sprintf((char*) (buf), "tlb_test mean=%.2Lf,min=%llu,p50=%llu,p90=%llu,p99=%llu,max=%llu %llu\n",
            stats->mean, stats->min, stats->p50, stats->p90, stats->p99, stats->max, stats->timestamp);
    transmit();
}

void publish_mmap(long long ts, char* op_name) {
    sprintf((char*) (buf), "tlb_test,op_name=%s foo=1 %llu\n", op_name, ts);
    transmit();
}

void init_udp(char *host, char *port) {
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&serverAddr, 0, sizeof serverAddr);
    struct hostent *he;
    he = gethostbyname(host);
    memcpy(&serverAddr.sin_addr, he->h_addr_list[0], he->h_length);

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(strtol(port, (char **)NULL, 10));
}

// ===================================================================


// takes a file path, creates the file and mmaps it up to MAP_SIZE bytes
void* create_mapping(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, (mode_t) 0644);
    lseek(fd, MAP_SIZE - 1, SEEK_SET);
    write(fd, "", 1);

    char *addr = mmap(NULL, MAP_SIZE, PROT_WRITE | PROT_READ, MAP_SHARED | MAP_ANONYMOUS, fd, 0L);

    // pre-fault every page
    for (long idx = 0; idx < MAP_SIZE - 1; idx += PG_SIZE) *(addr + idx) = 0;

    return addr;
}

// The writer thread runs this over and over again. The first pass is over the first mmaped file and
// all the subsequent ones are over the second file
void write_loop(char *addr, struct stats *data, int *count) {
    long offset = 0;
    long batches_done = 0;
    int batch_size_bytes = WRITER_BATCH_SIZE_KB * 1024;
    long max_batches = batch_size_bytes / PG_SIZE;
    long next_report_bytes = offset + batch_size_bytes * REPORT_STATS_EVERY_MB;

    while (offset + batch_size_bytes < MAP_SIZE) {
        long long int start = now();

        // perform some writes in a batch (one per page)
        int i = 0;
        for (; i < max_batches; i++) {
            *(addr + offset) = 0xAA;
            offset += PG_SIZE;
        }

        // record duration of a batch in the histo
        long long int finish = now();
        hdr_record_value(histo, (finish - start) / max_batches);
        batches_done++;

        // report results periodically and reset the histo
        if (offset >= next_report_bytes && data != NULL) {
            struct stats *s = &data[*count];
            s->timestamp = now();
            s->mean = hdr_mean(histo);
            s->min = hdr_min(histo);
            s->p50 = hdr_value_at_percentile(histo, 50);
            s->p90 = hdr_value_at_percentile(histo, 90);
            s->p99 = hdr_value_at_percentile(histo, 99);
            s->max = hdr_max(histo);
            (*count)++;

            hdr_reset(histo);
            next_report_bytes += batch_size_bytes * REPORT_STATS_EVERY_MB;
        }
    }
}


// writer thread function
void write_in_background(struct thread_args *args) {
    long long cpu = 1UL << args->target_cpu;
    sched_setaffinity(0, sizeof(cpu), (const cpu_set_t *) &cpu);

    // loop over the first mmapped file to fill in the TLB (don't record latency in the histo)
    write_loop(args->addr1, NULL, args->count);
    puts("Finished writing into the first file. Switching to the second...");

    // loop infinitely over the second mmapped file (record latency in the histo)
    while(1) write_loop(args->addr2, args->data, args->count);
}


int main() {
    // pin the main thread to a dedicated isolated cpu
    long long cpu = 1L << MUNMAP_CPU;
    sched_setaffinity(0, sizeof(cpu), (const cpu_set_t *) &cpu);

    hdr_init(1, INT64_C(3600000000), 3, &histo);
    init_udp("localhost", "8089");
    // this is where we will store the latency of each batch
    struct stats *data = calloc(10000, sizeof(struct stats));

    // create, mmap and pre-fault the files
    puts("Creating mappings...");
    char* addr1 = create_mapping("/dev/shm/file01");
    char* addr2 = create_mapping("/dev/shm/file02");

    // start the parallel writer
    pthread_t writer_thread;
    int cnt = 0;
    struct thread_args args = {.addr1 = addr1, .addr2 = addr2, .target_cpu = WRITER_CPU, .data = data, .count = &cnt};
    pthread_create(&writer_thread, NULL, (void *(*)(void *)) write_in_background, &args);
    puts("Spawned writer thread...");

    sleep(2);
    // pray and hope the writer thread traverses the entire file01 and switches to file02
    // by the time we exit the sleep :)
    long long int before = now();
    munmap(addr1, MAP_SIZE); // unmap the 1st file
    long long int after = now();
    sleep(1);

    printf("Publishing %i data points to influx...\n", cnt);
    for (int i = 0; i < cnt; i++) {
        publish_stats(&data[i]);
    }
    // also publish the time of munmap() call and its return
    publish_mmap(before, "before");
    publish_mmap(after, "after");

    puts("Finished"); // tada!
}
