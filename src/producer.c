#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>

#define PORT 12345
#define BUFFER_SIZE 256

int sock;

void handle_sigint(int sig) {
    printf("\nProductor desconectado.\n");
    close(sock);
    exit(0);
}

void cleanup() {
    if (sock >= 0) {
        close(sock);
        printf("Socket cerrado.\n");
    }
}

int main() {
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];

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
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // Localhost

    // Conecta al broker
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Error al conectar con el broker");
        cleanup();
        exit(EXIT_FAILURE);
    }

    if (send(sock, "PRODUCER\n", 9, 0) < 0) {
        perror("Error al enviar identificación al broker");
        cleanup();
        exit(EXIT_FAILURE);
    }

    snprintf(buffer, BUFFER_SIZE, "Producto\n");
    if (send(sock, buffer, strlen(buffer), 0) < 0) {
        perror("Error al enviar mensaje al broker");
    }
    printf("Productor envió: %s", buffer);

    cleanup();
    return 0;
}
