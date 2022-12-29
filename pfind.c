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
void enqueue(struct queue_node* node, struct queue* queue);
void* dequeue(struct queue* queue);
struct queue_node* create_node(void* data);

// Main thread exit code
static int exit_code = 0;

struct queue dir_q;
struct queue thread_q;

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

    // Add search root directory to the queue
    struct queue_node* root_node = create_node(search_root_dir);
    enqueue(root_node, &dir_q);

    // Create threads
    thrd_t *thread_ids = malloc(sizeof(thrd_t) * num_thread);
    for (i = 0; i < num_thread; ++i) {
        if (thrd_create(&thread_ids[i], searching_thread, NULL) != thrd_success) {
            perror("Error creating thread");
            thrd_exit(EXIT_FAILURE);
        }
    }
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

void enqueue(struct queue_node* node, struct queue* queue) {
    mtx_lock(&q_lock);
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
    cnd_signal(&q_not_empty);
    mtx_unlock(&q_lock);
}

void* dequeue(struct queue* queue) {
    mtx_lock(&q_lock);
    while (queue->size == 0) {
        cnd_wait(&q_not_empty, &q_lock);
    }

    struct queue_node* node = queue->head;
    queue->head = node->next;
    if (queue->size == 1) {
        // The queue is now empty
        queue->tail = queue->head;
    }

    mtx_unlock(&q_lock);

    void *node_data = node->data;
    free(node);
    return node_data;
}

int searching_thread(void *t) {
    char* search_term = (char *) t;
    struct stat buff;
    char* dirname = dequeue(&dir_q);
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
                struct queue_node* new_node = create_node(dir->d_name);
                enqueue(new_node, &dir_q);
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