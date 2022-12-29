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

#define QUEUE_SIZE 100

int is_dir_searchable(char* dir);
int searching_thread(void *t);
void enqueue(char* dirname);
char* dequeue();

// Main thread exit code
static int exit_code = 0;

struct queue_node {
    struct queue_node* next;
    struct queue_node* prev;
    char* data;
};

struct queue {
    int size;
    struct queue_node* head;
    struct queue_node* tail;
};

struct queue dir_q;

mtx_t q_lock;
cnd_t q_not_empty;

int main(int argc, char* argv[]) {
    int i, expected_num_args = 4;
    // Make sure we got the expected number of arguments
    if (argc != 4) {
        printf("Wrong number of arguments. Expected: %d, actual: %d", expected_num_args, argc);
    }
    char* search_root_dir = argv[1];
    char* search_term = argv[2];
    int num_thread = (int) strtol(argv[3], NULL, 10);

    // Make sure the search root directory is searchable
    if (is_dir_searchable(search_root_dir) != EXIT_SUCCESS) {
        perror("Search root directory is unsearchable");
        thrd_exit(EXIT_FAILURE);
    }

    // Initialize queue
    mtx_init(&q_lock, mtx_plain);
    cnd_init(&q_not_empty);

    // Create threads
    thrd_t *thread_ids = malloc(sizeof(thrd_t) * num_thread);
    for (i = 0; i < num_thread; ++i) {
        if (thrd_create(&thread_ids[i], searching_thread, NULL) != thrd_success) {
            perror("Error creating thread");
            thrd_exit(EXIT_FAILURE);
        }
    }
    enqueue(search_root_dir);
}

void enqueue(char* dirname) {
    // Create a new queue node
    struct queue_node* node = malloc(sizeof(struct queue_node));
    if (node == NULL) {
        perror("Error creating queue node");
        thrd_exit(EXIT_FAILURE);
    }
    node->data = dirname;

    mtx_lock(&q_lock);
    // The queue is empty, initialize its head
    if (dir_q.head == NULL) {
        dir_q.head = node;
        dir_q.tail = node;
        dir_q.size = 1;
    } else {
        // Add the node to the queue's tail
        struct queue_node* tmp = dir_q.tail;
        tmp->next = node;
        node->prev = tmp;
        dir_q.tail = node;
        dir_q.size += 1;
    }
    cnd_signal(&q_not_empty);
    mtx_unlock(&q_lock);
}

char* dequeue() {
    mtx_lock(&q_lock);
    while (dir_q.size == 0) {
        cnd_wait(&q_not_empty, &q_lock);
    }

    struct queue_node* node = dir_q.tail;
    // Move tail one step back
    dir_q.tail = node->prev;
    // Remove prev pointer from the old tail
    node->prev = NULL;
    // Remove next pointer from the new tail
    if (dir_q.tail != NULL) {
        dir_q.tail->next = NULL;
    }
    // If the list had one element, update the head pointer
    if (dir_q.tail == NULL) {
        dir_q.head = NULL;
    }

    dir_q.size -= 1;
    mtx_unlock(&q_lock);

    char *node_data = node->data;
    free(node);
    return node_data;
}

int searching_thread(void *t) {
    char* search_term = (char *) t;
    struct stat buff;
    char* dirname = dequeue();
    DIR* op_dir = opendir(dirname);
    if (op_dir == NULL) {
        perror("Error in open dir");
        thrd_exit(EXIT_FAILURE);
    }
    errno = 0;
    struct dirent* dir;
    while ((dir = readdir(op_dir)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        if (stat(dir->d_name, &buff) != EXIT_SUCCESS) {
            perror("Error in stat");
            thrd_exit(EXIT_FAILURE);
        }

        if (S_ISDIR(buff.st_mode)) {
            if (is_dir_searchable(dir->d_name)) {
                enqueue(dir->d_name);
            } else {
                printf("Directory %s: Permission denied.\n", dir->d_name);
                exit_code = 1;
            }
        } else {
            // This is a file
            if (strstr(dir->d_name, search_term) != NULL) {
                printf("%s", dir->d_name);
            }
        }
    }
    // TODO: Start from step 2 again
}

int is_dir_searchable(char* dir) {
    return access(dir, R_OK | X_OK);
}