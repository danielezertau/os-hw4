#include <stdatomic.h>
#include <threads.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

struct dir_queue_node {
    struct dir_queue_node* next;
    char data[PATH_MAX];
};

struct thread_queue_node {
    struct thread_queue_node* next;
    cnd_t *data;
};

struct dir_queue {
    int size;
    struct dir_queue_node* head;
    struct dir_queue_node* tail;
};

struct thread_queue {
    int size;
    struct thread_queue_node* head;
    struct thread_queue_node* tail;
};

int is_dir_searchable(char* dir);
int searching_thread(void *t);
void dir_enqueue(char data[PATH_MAX], cnd_t* cv_to_signal);
void dir_dequeue(cnd_t* cv_to_wait, char result_buff[PATH_MAX]);
void thread_enqueue(cnd_t* data, cnd_t* cv_to_signal);
cnd_t* thread_dequeue(cnd_t* cv_to_wait);
struct dir_queue_node* create_dir_node(char* data);
struct thread_queue_node* create_thread_node(cnd_t* data);
int is_queue_empty(mtx_t* queue_lock, struct thread_queue* queue);

// Main thread exit code
static int exit_code = 0;
// Number of files that match the search term
static atomic_int num_files = 0;
// Search term
static char* search_term;
// Number of threads
static int num_threads;
struct dir_queue dir_q;
mtx_t dir_q_lock;

struct thread_queue thread_q;
mtx_t thread_q_lock;
cnd_t thread_q_not_empty;

// Main thread signal
mtx_t threads_start_lock;
cnd_t threads_start_cv;
mtx_t threads_done_lock;
cnd_t threads_done_cv;


int main(int argc, char* argv[]) {
    int expected_num_args = 4;
    long i;
    // Make sure we got the expected number of arguments
    if (argc != 4) {
        printf("Wrong number of arguments. Expected: %d, actual: %d\n", expected_num_args, argc);
        exit(EXIT_FAILURE);
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

    // Add search root directory to the queue
    dir_enqueue(search_root_dir, NULL);

    // Create threads
    thrd_t *thread_ids = malloc(sizeof(thrd_t) * num_threads);
    for (i = 0; i < num_threads; ++i) {
        if (thrd_create(&thread_ids[i], searching_thread, (void *) i) != thrd_success) {
            perror("Error creating thread");
            thrd_exit(EXIT_FAILURE);
        }
    }
    // Signal the threads to start working
    printf("Waking everybody up\n");
    cnd_broadcast(&threads_start_cv);

    // Wait for one of the threads to realize we're done
    mtx_lock(&threads_done_lock);
    printf("Main thread going to sleep\n");
    cnd_wait(&threads_done_cv, &threads_done_lock);
    mtx_unlock(&threads_done_lock);

    // Cleanup
    mtx_destroy(&threads_start_lock);
    mtx_destroy(&threads_done_lock);
    mtx_destroy(&dir_q_lock);
    mtx_destroy(&thread_q_lock);
    cnd_destroy(&threads_start_cv);
    cnd_destroy(&threads_done_cv);
    cnd_destroy(&thread_q_not_empty);
    exit(exit_code);
}

struct thread_queue_node* create_thread_node(cnd_t* data) {
    struct thread_queue_node* node = malloc(sizeof(struct thread_queue_node));
    if (node == NULL) {
        perror("Error creating node");
        thrd_exit(EXIT_FAILURE);
    }
    memcpy(node->data, data, sizeof(cnd_t));
    return node;
}

struct dir_queue_node* create_dir_node(char* data) {
    struct dir_queue_node* node = malloc(sizeof(struct dir_queue_node));
    if (node == NULL) {
        perror("Error creating node");
        thrd_exit(EXIT_FAILURE);
    }
    strcpy(node->data, data);
    return node;
}

void dir_enqueue(char* data, cnd_t* cv_to_signal) {
    struct dir_queue_node* node = create_dir_node(data);
    mtx_lock(&dir_q_lock);
    // The queue is empty, initialize its head
    if (dir_q.head == NULL) {
        dir_q.head = node;
        dir_q.tail = node;
        dir_q.size = 1;
    } else {
        // Add the node to the queue's tail
        struct dir_queue_node* tmp = dir_q.tail;
        tmp->next = node;
        dir_q.tail = node;
        dir_q.size += 1;
    }
    if (cv_to_signal != NULL) {
        cnd_signal(cv_to_signal);
    }
    mtx_unlock(&dir_q_lock);
}

void thread_enqueue(cnd_t* data, cnd_t* cv_to_signal) {
    struct thread_queue_node* node = create_thread_node(data);
    mtx_lock(&thread_q_lock);
    // The queue is empty, initialize its head
    if (thread_q.head == NULL) {
        thread_q.head = node;
        thread_q.tail = node;
        thread_q.size = 1;
    } else {
        // Add the node to the queue's tail
        struct thread_queue_node* tmp = thread_q.tail;
        tmp->next = node;
        thread_q.tail = node;
        thread_q.size += 1;
    }
    if (cv_to_signal != NULL) {
        cnd_signal(cv_to_signal);
    }
    mtx_unlock(&thread_q_lock);
}

void dir_dequeue(cnd_t* cv_to_wait, char result_buff[PATH_MAX]) {
    mtx_lock(&dir_q_lock);
    while (dir_q.size == 0) {
        if (!is_work_done()) {
            // Add the thread to the thread queue
            thread_enqueue(cv_to_wait,  &thread_q_not_empty);
            cnd_wait(cv_to_wait, &dir_q_lock);
        } else {
            // We're done!
            printf("Everybody's done!\n");
            cnd_signal(&threads_done_cv);
            mtx_unlock(&dir_q_lock);
            cnd_destroy(cv_to_wait);
            thrd_exit(EXIT_SUCCESS);
        }
    }
    struct dir_queue_node* node = dir_q.head;
    dir_q.head = node->next;
    if (dir_q.size == 1) {
        // The queue is now empty
        dir_q.tail = dir_q.head;
    }
    dir_q.size -= 1;


    strcpy(result_buff, node->data);
    free(node);
    mtx_unlock(&dir_q_lock);
}

cnd_t* thread_dequeue(cnd_t* cv_to_wait) {
    mtx_lock(&thread_q_lock);
    while (thread_q.size == 0) {
        cnd_wait(cv_to_wait, &thread_q_lock);
    }
    struct thread_queue_node* node = thread_q.head;
    thread_q.head = node->next;
    if (thread_q.size == 1) {
        // The queue is now empty
        thread_q.tail = thread_q.head;
    }
    thread_q.size -= 1;


    void *node_data = node->data;
    free(node);
    mtx_unlock(&thread_q_lock);
    return node_data;
}

int searching_thread(void *t) {
//    long thread_idx = (long) t;
    cnd_t *cv_to_signal;
    cnd_t thread_cv;
    char* base_dir_path = malloc(PATH_MAX * sizeof(char));
    char* curr_dir_path = malloc(PATH_MAX * sizeof(char));

    // Wait for a signal from the main thread
    mtx_lock(&threads_start_lock);
    cnd_init(&thread_cv);
    cnd_wait(&threads_start_cv, &threads_start_lock);
    mtx_unlock(&threads_start_lock);

    printf("Searching thread woke up\n");
    struct stat buff;

    while (1) {
        mtx_lock(&dir_q_lock);
        // Check if we're done
        if (is_work_done()) {
            printf("Everybody's done!\n");
            cnd_signal(&threads_done_cv);
            mtx_unlock(&dir_q_lock);
            cnd_destroy(&thread_cv);
            thrd_exit(EXIT_SUCCESS);
        }
        // Go to sleep and wait for more work
        if (thread_q.size >= dir_q.size || dir_q.size == 0) {
            // All directories are assigned. Go so sleep
            thread_enqueue(&thread_cv, &thread_q_not_empty);
            cnd_wait(&thread_cv, &dir_q_lock);
        }
        mtx_unlock(&dir_q_lock);

        dir_dequeue(&thread_cv, base_dir_path);
        DIR *base_dir_op = opendir(base_dir_path);
        if (base_dir_op == NULL) {
            perror("Error in opendir");
            thrd_exit(EXIT_FAILURE);
        }
        struct dirent *dirent;
        while ((dirent = readdir(base_dir_op)) != NULL) {
            if (chdir(base_dir_path) != EXIT_SUCCESS) {
                perror("Error in chdir");
                thrd_exit(EXIT_FAILURE);
            }
            // Skip . and .. entries
            if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0) {
                continue;
            }

            // Get directory path
            strcpy(curr_dir_path, base_dir_path);
            strcat(curr_dir_path, "/");
            strcat(curr_dir_path, dirent->d_name);

            // Get directory type using stat
            if (stat(curr_dir_path, &buff) != EXIT_SUCCESS) {
                perror("Error in stat");
                thrd_exit(EXIT_FAILURE);
            }

            if (S_ISDIR(buff.st_mode)) {
                if (is_dir_searchable(curr_dir_path) == EXIT_SUCCESS) {
                    // Get the longest sleeping thread
                    if (!is_queue_empty(&thread_q_lock, &thread_q)) {
                        cv_to_signal = thread_dequeue(&thread_q_not_empty);
                    } else {
                        cv_to_signal = NULL;
                    }
                    // Add the directory to the queue and assign it to the thread we just dequeued
                    dir_enqueue(curr_dir_path, cv_to_signal);
                } else {
                    printf("Directory %s: Permission denied.\n", curr_dir_path);
                    exit_code = EXIT_FAILURE;
                }
            } else {
                // This is a file
                if (strstr(curr_dir_path, search_term) != NULL) {
                    num_files += 1;
                    printf("%s\n", curr_dir_path);
                }
            }
        }
    }
}

int is_queue_empty(mtx_t* queue_lock, struct thread_queue* queue) {
    int res;
    mtx_lock(queue_lock);
    res = queue->size;
    mtx_unlock(queue_lock);
    return res == 0;
}

int is_dir_searchable(char* dir) {
    return access(dir, R_OK | X_OK);
}