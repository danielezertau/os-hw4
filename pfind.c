#include <limits.h>
#include <stdatomic.h>
#include <threads.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

struct queue_node {
    struct queue_node* next;
    void* data;
};

struct queue {
    int size;
    struct queue_node* head;
    struct queue_node* tail;
};



int is_dir_searchable(char* dir);
int searching_thread(void *t);
void enqueue(void* data, struct queue* queue, mtx_t* lock, cnd_t* cv_to_signal);
void* dequeue(struct queue* queue, mtx_t* lock, cnd_t* cv_to_wait, struct queue* wait_queue, mtx_t* wait_lock, cnd_t* wait_cv);
struct queue_node* create_node(void* data);

// Main thread exit code
static int exit_code = 0;
// Number of files that match the search term
static atomic_int num_files = 0;
// Search term
static char* search_term;
// Number of threads
static int num_threads;

// Queues and locks
struct queue dir_q;
mtx_t dir_q_lock;

struct queue thread_q;
mtx_t thread_q_lock;
cnd_t thread_q_not_empty;

// Main thread signal
mtx_t threads_start_lock;
cnd_t threads_start_cv;
mtx_t threads_done_lock;
cnd_t threads_done_cv;


int main(int argc, char* argv[]) {
    int i, j, expected_num_args = 4;
    // Make sure we got the expected number of arguments
    if (argc != 4) {
        printf("Wrong number of arguments. Expected: %d, actual: %d", expected_num_args, argc);
    }
    char* search_root_dir = argv[1];
    search_term = argv[2];
    num_threads = (int) strtol(argv[3], NULL, 10);

    // Make sure the search root directory is searchable
    if (is_dir_searchable(search_root_dir) != EXIT_SUCCESS) {
        perror("Search root directory is unsearchable");
        thrd_exit(EXIT_FAILURE);
    }

    // Initialize locks and condition variables
    mtx_init(&threads_start_lock, mtx_plain);
    mtx_init(&threads_done_lock, mtx_plain);
    mtx_init(&dir_q_lock, mtx_plain);
    mtx_init(&thread_q_lock, mtx_plain);
    cnd_init(&threads_start_cv);
    cnd_init(&threads_done_cv);
    cnd_init(&thread_q_not_empty);

    // Create a condition variable for each thread
    cnd_t* cvs = malloc(sizeof(cnd_t) * num_threads);
    for (j = 0; j < num_threads; ++j) {
        cnd_init(&(cvs[j]));
    }

    // Add search root directory to the queue
    enqueue(search_root_dir, &dir_q, &dir_q_lock, NULL);

    // Create threads
    thrd_t *thread_ids = malloc(sizeof(thrd_t) * num_threads);
    for (i = 0; i < num_threads; ++i) {
        if (thrd_create(&thread_ids[i], searching_thread, (void *) &(cvs[i])) != thrd_success) {
            perror("Error creating thread");
            thrd_exit(EXIT_FAILURE);
        }
    }

    // Signal the threads to start working
    cnd_broadcast(&threads_start_cv);

    // Wait for one of the threads to realize we're done
    mtx_lock(&threads_done_lock);
    cnd_wait(&threads_done_lock, &threads_done_cv);
    mtx_unlock(&threads_done_lock);

    // Cleanup
    mtx_destroy(&threads_start_lock);
    mtx_destroy(&threads_done_lock);
    mtx_destroy(&dir_q_lock);
    mtx_destroy(&thread_q_lock);
    cnd_destroy(&threads_start_cv);
    cnd_destroy(&threads_done_cv);
    cnd_destroy(&thread_q_not_empty);
    exit(EXIT_SUCCESS);
}

struct queue_node* create_node(void* data) {
    struct queue_node* node = malloc(sizeof(struct queue_node));
    if (node == NULL) {
        perror("Error creating node");
        thrd_exit(EXIT_FAILURE);
    }
    node->data = data;
    return node;
}

void enqueue(void* data, struct queue* queue, mtx_t* lock, cnd_t* cv_to_signal) {
    struct queue_node* node = create_node(data);
    mtx_lock(&lock);
    // The queue is empty, initialize its head
    if (queue->head == NULL) {
        queue->head = node;
        queue->tail = node;
        queue->size = 1;
    } else {
        // Add the node to the queue's tail
        struct queue_node* tmp = queue->tail;
        tmp->next = node;
        queue->tail = node;
        queue->size += 1;
    }
    if (cv_to_signal != NULL) {
        cnd_signal(&cv_to_signal);
    }
    mtx_unlock(&lock);
}

void* dequeue(struct queue* queue, mtx_t* lock, cnd_t* cv_to_wait, struct queue* wait_queue, mtx_t* wait_lock, cnd_t* wait_cv) {
    mtx_lock(lock);
    while (queue->size == 0) {
        if (wait_queue != NULL) {
            // Add the thread to the thread queue
            enqueue(cv_to_wait, wait_queue, wait_lock, wait_cv);
        }
        cnd_wait(&cv_to_wait, &lock);
    }

    struct queue_node* node = queue->head;
    queue->head = node->next;
    if (queue->size == 1) {
        // The queue is now empty
        queue->tail = queue->head;
    }

    mtx_unlock(lock);

    void *node_data = node->data;
    free(node);
    return node_data;
}

int searching_thread(void *t) {
    cnd_t thread_cv = *t;

    // Wait for a signal from the main thread
    mtx_lock(&threads_start_lock);
    cnd_wait(&threads_start_cv, &threads_start_lock);
    mtx_unlock(&threads_start_lock);

    struct stat buff;

    while (1) {
        // Check if we're done
        mtx_lock(&thread_q);
        if (thread_q.size == num_threads - 1) {
            // All threads are sleeping expect for me
            cnd_signal(&threads_done_cv);
            mtx_unlock(&thread_q);
            thrd_exit(EXIT_SUCCESS);
        }
        mtx_unlock(&thread_q);

        mtx_lock(&dir_q_lock);
        if (thread_q.size >= dir_q.size || dir_q.size == 0) {
            // All directories are assigned. Go so sleep
            cnd_wait(&thread_cv, &dir_q_lock);
            mtx_unlock(&dir_q_lock);
        }
        char *dirname = dequeue(&dir_q, &dir_q_lock, &thread_cv, &thread_q, &thread_q_lock, &thread_q_not_empty);
        DIR *op_dir = opendir(dirname);
        if (op_dir == NULL) {
            perror("Error in open dirent");
            thrd_exit(EXIT_FAILURE);
        }
        struct dirent *dirent;
        while ((dirent = readdir(op_dir)) != NULL) {
            // Skip . and .. entries
            if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0) {
                continue;
            }

            // Get directory type using stat
            if (stat(dirent->d_name, &buff) != EXIT_SUCCESS) {
                perror("Error in stat");
                thrd_exit(EXIT_FAILURE);
            }

            if (S_ISDIR(buff.st_mode)) {
                if (is_dir_searchable(dirent->d_name)) {
                    // Get the longest sleeping thread
                    cnd_t *cv_to_signal = dequeue(&thread_q, &thread_q_lock, &thread_q_not_empty, NULL, NULL, NULL);
                    // Add the directory to the queue and assign it to the thread we just dequeued
                    enqueue(dirent->d_name, &dir_q, &dir_q_lock, cv_to_signal);
                } else {
                    printf("Directory %s: Permission denied.\n", dirent->d_name);
                    exit_code = EXIT_FAILURE;
                }
            } else {
                // This is a file
                if (strstr(dirent->d_name, search_term) != NULL) {
                    num_files += 1;
                    printf("%s", dirent->d_name);
                }
            }
        }
    }
}

int is_dir_searchable(char* dir) {
    return access(dir, R_OK | X_OK);
}