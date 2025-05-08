#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <signal.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>

#define PORT 12345
#define MAX_CONSUMERS_PER_GROUP 10
#define MESSAGE_SIZE 256
#define MAX_QUEUE_SIZE 1024
#define MAX_CONNECTIONS 1020

int THREAD_POOL_SIZE;
int active_producers = 0;
int active_consumers = 0;
int producer_count = 0;
int consumer_total = 0;
int message_count = 0;
static int consumer_id_counter = 0;

typedef struct MessageNode {
    int id;
    char content[MESSAGE_SIZE];
    struct MessageNode* next;
} MessageNode;

typedef struct {
    MessageNode* head;
    MessageNode* tail;
    pthread_mutex_t lock;
} MessageQueue;

typedef struct TaskNode {
    int client_socket;
    struct TaskNode* next;
} TaskNode;

typedef struct {
    TaskNode* head;
    TaskNode* tail;
    pthread_mutex_t lock;
    sem_t task_count;
} TaskQueue;

TaskQueue task_queue = { NULL, NULL, PTHREAD_MUTEX_INITIALIZER, {{0}}};

typedef struct Consumer {
    int id;
    int socket;
    int last_message_id;
    struct Consumer* next;
} Consumer;

typedef struct ConsumerGroup {
    int id;
    Consumer* consumers;
    int consumer_count;
    int rr_index;
    pthread_mutex_t lock;  // NUEVO
    struct ConsumerGroup* next;
} ConsumerGroup;


MessageQueue message_queue = { NULL, NULL, PTHREAD_MUTEX_INITIALIZER };
sem_t queue_slots;
sem_t messages_ready;

ConsumerGroup* group_head = NULL;
pthread_mutex_t group_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t msg_count_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_message(const char* msg, int id) {
    FILE* log = fopen("messages.log", "a");
    if (log) {
        fprintf(log, "Mensaje %d. %s\n", id, msg);
        fclose(log);
    }
}

void log_interaction(const char* interaction) {
    FILE* log = fopen("interactions.log", "a");
    if (log) {
        fprintf(log, "%s\n", interaction);
        fclose(log);
    }
}

void enqueue_message(int id, const char* msg) {
    sem_wait(&queue_slots);

    MessageNode* new_node = malloc(sizeof(MessageNode));
    if (!new_node) {
        perror("Error de memoria");
        sem_post(&queue_slots);
        return;
    }

    new_node->id = id;
    strncpy(new_node->content, msg, MESSAGE_SIZE);
    new_node->next = NULL;

    pthread_mutex_lock(&message_queue.lock);
    if (message_queue.tail) {
        message_queue.tail->next = new_node;
        message_queue.tail = new_node;
    } else {
        message_queue.head = message_queue.tail = new_node;
    }
    pthread_mutex_unlock(&message_queue.lock);

    sem_post(&messages_ready);
}

ConsumerGroup* create_new_group(int id) {
    ConsumerGroup* group = malloc(sizeof(ConsumerGroup));
    group->id = id;
    group->consumers = NULL;
    group->consumer_count = 0;
    group->rr_index = 0;
    group->next = NULL;
    pthread_mutex_init(&group->lock, NULL);
    return group;
}


ConsumerGroup* assign_consumer_to_group(int client_socket) {
    pthread_mutex_lock(&group_mutex);

    ConsumerGroup* current = group_head;
    ConsumerGroup* last_group = NULL; 

    while (current) {
        if (current->consumer_count < MAX_CONSUMERS_PER_GROUP) {
            break; 
        }
        last_group = current; 
        current = current->next;
    }

    if (!current) {
        int new_group_id = (last_group ? last_group->id + 1 : 1);
        ConsumerGroup* new_group = create_new_group(new_group_id);

        if (!last_group) {
            group_head = new_group;
        } else {
            last_group->next = new_group;
        }

        current = new_group;
        printf("Nuevo grupo creado con ID %d\n", current->id);
    }

    Consumer* new_consumer = malloc(sizeof(Consumer));
    if (!new_consumer) {
        perror("Error al asignar memoria para el consumidor");
        pthread_mutex_unlock(&group_mutex);
        return NULL;
    }

    pthread_mutex_lock(&current->lock);

    new_consumer->id = __sync_add_and_fetch(&consumer_id_counter, 1);
    new_consumer->socket = client_socket;
    new_consumer->last_message_id = 0;
    new_consumer->next = current->consumers;
    current->consumers = new_consumer;
    current->consumer_count++;

    pthread_mutex_unlock(&current->lock);


    pthread_mutex_unlock(&group_mutex); 
    return current;
}

void clean_disconnected(ConsumerGroup* group) {
    if (!group) {
        printf("Error: clean_disconnected llamado con un grupo NULL\n");
        return;
    }

    Consumer** ptr = &group->consumers;
    int index = 0;

    while (*ptr) {
        char tmp[1];
        int recv_result = recv((*ptr)->socket, tmp, 0, MSG_PEEK | MSG_DONTWAIT);

        if (recv_result == 0) {
            close((*ptr)->socket);

            Consumer* to_delete = *ptr;
            *ptr = (*ptr)->next;
            free(to_delete);

            group->consumer_count--;
            printf("Total consumidores del grupo %d: %d\n", group->id, group->consumer_count);

            if (group->rr_index > index) {
                group->rr_index--;
            }
        } else if (recv_result < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
            } else {
                perror("Error en recv");
                printf("Socket %d inválido o error al recibir datos\n", (*ptr)->socket);
            }
            ptr = &(*ptr)->next;
            index++;
        } else {
            ptr = &(*ptr)->next;
            index++;
        }
    }

    if (group->rr_index >= group->consumer_count) {
        group->rr_index = 0;
    }
}

int send_to_next(ConsumerGroup* group, const char* msg, int msg_id) {
    clean_disconnected(group);
    if (group->consumer_count == 0) return 0;

    Consumer* target = group->consumers;
    for (int i = 0; i < group->rr_index && target; i++) target = target->next;
    if (!target) target = group->consumers;

    if (msg_id == target->last_message_id) {
        return 0;
    }

    if (send(target->socket, msg, strlen(msg), 0) <= 0) {
        printf("Consumidor %d desconectado durante el envío\n", target->id);
        close(target->socket);
        pthread_mutex_lock(&group_mutex);
        clean_disconnected(group);
        pthread_mutex_unlock(&group_mutex);
        return 0;
    } else {
        char interaction[256];
        snprintf(interaction, sizeof(interaction), 
                 "Grupo %d - mensaje %d entregado al consumidor %d", 
                 group->id, msg_id, target->id);
        log_interaction(interaction);

        target->last_message_id = msg_id;

        group->rr_index = (group->rr_index + 1) % group->consumer_count;
        return 1;
    }
}


int try_lock_with_retry(pthread_mutex_t* mtx, int attempts, int ms_wait) {
    for (int i = 0; i < attempts; i++) {
        if (pthread_mutex_trylock(mtx) == 0) return 1;
        struct timespec ts = {0, ms_wait * 1000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}



void* distributor_thread(void* arg) {
    while (1) {
        sem_wait(&messages_ready);

        // ORDEN FIJO: primero group_mutex, luego message_queue.lock
        if (pthread_mutex_lock(&group_mutex) != 0) {
            perror("Error al adquirir group_mutex");
            continue;
        }

        if (pthread_mutex_lock(&message_queue.lock) != 0) {
            perror("Error al adquirir message_queue.lock");
            pthread_mutex_unlock(&group_mutex);
            continue;
        }

        MessageNode* current = message_queue.head;

        if (current) {
            char full_message[MESSAGE_SIZE + 2];
            snprintf(full_message, sizeof(full_message), "%s\n", current->content);

            ConsumerGroup* group = group_head;
            int groups_total = 0;
            int groups_delivered = 0;

            while (group) {
                clean_disconnected(group);
                if (group->consumer_count > 0) {
                    groups_total++;
                    if (send_to_next(group, full_message, current->id)) {
                        groups_delivered++;
                    }
                }
                group = group->next;
            }

            if (groups_total == 0) {
                sem_post(&messages_ready);
            } else if (groups_delivered == groups_total) {
                message_queue.head = current->next;
                if (!message_queue.head) message_queue.tail = NULL;
                free(current);
                sem_post(&queue_slots);
            } else {
                sem_post(&messages_ready);
            }
        } else {
            sem_post(&messages_ready);
        }

        pthread_mutex_unlock(&message_queue.lock);
        pthread_mutex_unlock(&group_mutex);
    }
    return NULL;
}

int recv_line(int sock, char* buffer, int size) {
    int total = 0;
    char c;
    while (total < size - 1) {
        int bytes = recv(sock, &c, 1, 0);
        if (bytes <= 0) return bytes;
        if (c == '\n') break;
        buffer[total++] = c;
    }
    buffer[total] = '\0';
    return total;
}

void* handle_client(void* arg) {
    int client_socket = *(int*)arg;

    char buffer[MESSAGE_SIZE];
    memset(buffer, 0, sizeof(buffer));

    if (recv_line(client_socket, buffer, sizeof(buffer)) <= 0) {
        close(client_socket);
        pthread_exit(NULL);
    }

    if (strncmp(buffer, "PRODUCER", 8) == 0) {
        __sync_add_and_fetch(&active_producers, 1);
        int producer_id = __sync_add_and_fetch(&producer_count, 1);
        while (1) {
            memset(buffer, 0, sizeof(buffer));
            int bytes = recv_line(client_socket, buffer, sizeof(buffer));
            if (bytes <= 0) break;

            pthread_mutex_lock(&msg_count_mutex);
            int message_id = ++message_count;
            pthread_mutex_unlock(&msg_count_mutex);

            printf("Productor %d envió mensaje %d: %s\n", producer_id, message_id, buffer);
            log_message(buffer, message_id);
            enqueue_message(message_id, buffer);

            char interaction[256];
            snprintf(interaction, sizeof(interaction), "Productor %d envió mensaje %d", producer_id, message_id);
            log_interaction(interaction);
        }
        __sync_sub_and_fetch(&active_producers, 1);
    } else if (strncmp(buffer, "CONSUMER", 8) == 0) {
        __sync_add_and_fetch(&active_consumers, 1);
        int consumer_id = __sync_add_and_fetch(&consumer_total, 1);
        ConsumerGroup* group = assign_consumer_to_group(client_socket);
        printf("Consumidor %d asignado al grupo %d -> Total consumidores del grupo %d: %d\n", consumer_id, group->id, group->id, group->consumer_count);

        while (1) {
            char tmp[1];
            if (recv(client_socket, tmp, sizeof(tmp), MSG_PEEK | MSG_DONTWAIT) == 0) {
                printf("Consumidor %d desconectado del grupo %d\n", consumer_id, group->id);

                pthread_mutex_lock(&group_mutex);
                clean_disconnected(group);
                pthread_mutex_unlock(&group_mutex);

                __sync_sub_and_fetch(&active_consumers, 1);

                pthread_exit(NULL);
            }
            sleep(1);
        }
    }

    close(client_socket);
    pthread_exit(NULL);
}

void enqueue_task(int client_socket) {
    TaskNode* new_task = malloc(sizeof(TaskNode));
    if (!new_task) {
        perror("Error de memoria para tarea");
        return;
    }
    new_task->client_socket = client_socket;
    new_task->next = NULL;

    pthread_mutex_lock(&task_queue.lock);
    if (task_queue.tail) {
        task_queue.tail->next = new_task;
        task_queue.tail = new_task;
    } else {
        task_queue.head = task_queue.tail = new_task;
    }
    pthread_mutex_unlock(&task_queue.lock);

    sem_post(&task_queue.task_count);
}

int dequeue_task() {
    sem_wait(&task_queue.task_count);

    pthread_mutex_lock(&task_queue.lock);
    TaskNode* task = task_queue.head;
    if (!task) {
        pthread_mutex_unlock(&task_queue.lock);
        return -1; 
    }

    int client_socket = task->client_socket;
    task_queue.head = task->next;
    if (!task_queue.head) {
        task_queue.tail = NULL;
    }
    if(task) {
        free(task);
    }
    pthread_mutex_unlock(&task_queue.lock);

    return client_socket;
}

void* thread_pool_worker(void* arg) {
    while (1) {
        int client_socket = dequeue_task();
        if (client_socket != -1) {
            handle_client(&client_socket); 
        }
    }
    return NULL;
}

int main() {

    THREAD_POOL_SIZE = sysconf(_SC_NPROCESSORS_ONLN) * 2000;
    if (THREAD_POOL_SIZE > 4000) {
        THREAD_POOL_SIZE = 4000;
    }
    signal(SIGPIPE, SIG_IGN);
    sem_init(&queue_slots, 0, MAX_QUEUE_SIZE);
    sem_init(&messages_ready, 0, 0);
    sem_init(&task_queue.task_count, 0, 0);

    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    pthread_t distributor;
    if (pthread_create(&distributor, NULL, distributor_thread, NULL) != 0) {
        perror("Error creando hilo distribuidor");
        exit(EXIT_FAILURE);
    }
    pthread_detach(distributor);

    pthread_t thread_pool[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&thread_pool[i], NULL, thread_pool_worker, NULL) != 0) {
            perror("Error creando hilo del pool");
            exit(EXIT_FAILURE);
        }
        pthread_detach(thread_pool[i]);
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Error al crear el socket");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error en bind");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, MAX_CONNECTIONS) < 0) {
        perror("Error en listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("Broker escuchando en el puerto %d...\n", PORT);

    while (1) {
        if (active_producers + active_consumers >= MAX_CONNECTIONS) {
            printf("Límite de conexiones alcanzado\n");
            int temp_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (temp_socket >= 0) {
                const char* message = "Límite de conexiones alcanzado. Intente más tarde.\n";
                send(temp_socket, message, strlen(message), 0);
                close(temp_socket);
            }
        } else {
            int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket < 0) {
                perror("Error en accept");
                continue;
            }

            enqueue_task(client_socket);
        }
    }

    close(server_socket);
    return 0;
}
