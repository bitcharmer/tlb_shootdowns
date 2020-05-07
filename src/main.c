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
#include <numa.h>


#define WRITER_BATCH_SIZE_KB 64 // write into this much worth of pages and capture the time
#define REPORT_STATS_EVERY_MB 128 // every N megabytes we'll dump percentiles into our "time series"

#define PG_SIZE 4096L // only testing standard 4k pages
#define ALLOC_SIZE 1L * 1024L * 1024L * 1024L // using 1 GB chunks

#define CULPRIT_CPU 22 // cpu of the thread responsible for freeing chunk A (at some point)
#define VICTIM_CPU 23 // cpu of the thread that is expected to be impacted by the thread that does free()

#define INFLUX_BUF_SIZE 1500 // fit within default MTU

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
    void *chunkA;
    void *chunkB;
    void *metrics;
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

void publish_marker(long long ts, char *op_name) {
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

// The victim thread runs this over and over again. The first pass is over chunkA and all the
// subsequent ones are over the chunkB
void traverse_chunk(char *addr, struct stats *metrics, int *count) {
    long long offset = 0;
    long long batches_done = 0;
    long long batch_size_bytes = WRITER_BATCH_SIZE_KB * 1024;
    long long pages_per_batch = batch_size_bytes / PG_SIZE;
    long long next_report_bytes = 1024L * 1024L * REPORT_STATS_EVERY_MB;

//    if (metrics != NULL) {
//        printf("Batch size: %lli\n", batch_size_bytes);
//        printf("Pages per batch: %lli\n", pages_per_batch);
//        printf("Next report: %lli\n", next_report_bytes);
//        printf("Alloc size: %lli\n", ALLOC_SIZE);
//    }

    while (offset + batch_size_bytes <= ALLOC_SIZE) {
        long long int start = now();

        // perform some writes in a batch (one write per page)
        int i = 0;
        for (; i < pages_per_batch; i++) {
            *(addr + offset) = 0xAA;
            offset += PG_SIZE;
        }

        // record duration of a batch in the histogram
        long long int finish = now();
        hdr_record_value(histo, (finish - start) / pages_per_batch);
        batches_done++;

//        printf("Batches done = %lli, off = %lli, next_rep = %lli\n", batches_done, offset + PG_SIZE, next_report_bytes);

        // report results periodically and reset the histogram
        if (offset >= next_report_bytes && metrics != NULL) {
            struct stats *s = &metrics[*count];
            s->timestamp = now();
            s->mean = hdr_mean(histo);
            s->min = hdr_min(histo);
            s->p50 = hdr_value_at_percentile(histo, 50);
            s->p90 = hdr_value_at_percentile(histo, 90);
            s->p99 = hdr_value_at_percentile(histo, 99);
            s->max = hdr_max(histo);
            (*count)++;

//            printf("Batches done = %lli, p50 = %lli, p90 = %lli, p99 = %lli, max = %lli\n", batches_done, s->p50, s->p90, s->p99, s->max);
            hdr_reset(histo);
            next_report_bytes += (1024L * 1024L * REPORT_STATS_EVERY_MB);
            batches_done = 0;
        }
    }
}


// victim thread function
void write_in_background(struct thread_args *args) {
    long long cpu = 1UL << args->target_cpu;
    sched_setaffinity(0, sizeof(cpu), (const cpu_set_t *) &cpu);

    // traverse chunkA to fill the TLB (don't record latency in the histogram)
    traverse_chunk(args->chunkA, NULL, args->count);
    puts("Victim finished populating chunk A. Switching to chunk B...");

    // loop infinitely over chunk B (record latency in the histogram)
    while(1) {
        traverse_chunk(args->chunkB, args->metrics, args->count);
    }
}


int main() {
    // pin the culprit thread to a dedicated isolated cpu
    long long cpu = 1L << CULPRIT_CPU; // yup, deal with it
    sched_setaffinity(0, sizeof(cpu), (const cpu_set_t *) &cpu);

    // init stuff
    hdr_init(1, INT64_C(3600000000), 3, &histo);
    init_udp("localhost", "8089");

    // this is where we will store the latency of each batch. 10000 data items is totally arbitrary and this
    // will blow up if you keep it running too long or have super frequent stats reporting period
    struct stats *metrics = calloc(10000, sizeof(struct stats));
    for (int i = 0; i < 10000; i++) metrics[i].timestamp = 0;

    puts("Allocating chunks...");
    char* chunkA = numa_alloc_local(ALLOC_SIZE);
    char* chunkB = numa_alloc_local(ALLOC_SIZE);

    // start the parallel victim thread
    pthread_t victim_thread;
    int cnt = 0;
    struct thread_args args = {.chunkA = chunkA, .chunkB = chunkB, .target_cpu = VICTIM_CPU, .metrics = metrics, .count = &cnt};
    pthread_create(&victim_thread, NULL, (void *(*)(void *)) write_in_background, &args);
    puts("Spawned victim thread...");

    // hope and pray the writer thread traverses the entire chunkA and switches to chunkB by the time we exit the sleep and press enter
    // normally this would be referred to as 'sheer luck', however in my field we call this 'heuristics'
    sleep(2);

    printf("Press enter to free chunkA...");
    getchar();
    long long int before = now();
    numa_free(chunkA, ALLOC_SIZE);
    long long int after = now();
    printf("FOOOO...\n");
    sleep(1);

    printf("Publishing %i data points to influx...\n", cnt);
    for (int i = 0; i < cnt; i++) {
        publish_stats(&metrics[i]);
    }
    // also publish the time of free() call and its return
    publish_marker(before, "before");
    publish_marker(after, "after");

    puts("Finished"); // tada!
}
