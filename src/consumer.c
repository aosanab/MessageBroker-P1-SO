#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>

#define PORT 12345
#define BUFFER_SIZE 256

int sock;

void cleanup(int sock) {
    if (sock >= 0) {
        close(sock);
        printf("Socket cerrado.\n");
    }
}

void handle_sigint(int sig) {
    printf("\nConsumidor desconectado.\n");
    cleanup(sock);
    exit(0);
}

int main() {
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    int bytes_received;
    char *ptr, *end;

    // Configura el manejo de la señal SIGINT
    signal(SIGINT, handle_sigint);

    // Crea el socket TCP
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Error al crear el socket");
        exit(EXIT_FAILURE);
    }

    // Configura la dirección del servidor
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Intentar conectar al broker con reintentos
    while (1) {
        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Error al conectar con el broker. Reintentando en 5 segundos...");
            cleanup(sock);
            sleep(5);
            continue; // Reintentar la conexión
        }
        break; // Salir del bucle si la conexión es exitosa
    }

    
    if (send(sock, "CONSUMER\n", 9, 0) < 0) {
        perror("Error al enviar identificación al broker");
        cleanup(sock);
        exit(EXIT_FAILURE);
    }

    // Bucle para recibir mensajes del broker
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0) {
            printf("Conexión cerrada por el broker.\n");
            break;
        }

        // Procesar e imprimir línea por línea si vienen varias
        ptr = buffer;
        while ((end = strchr(ptr, '\n')) != NULL) {
            *end = '\0';
            printf("Consumidor recibió: %s\n", ptr);
            ptr = end + 1;
        }
    }

    cleanup(sock);
    return 0;
}

