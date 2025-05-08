# Sistema Distribuido de Broker de Mensajes (Mini-Kafka)

## Descripción del Sistema
Este proyecto implementa un sistema de comunicación basado en el modelo Productor-Consumidor utilizando un Broker como intermediario. El sistema está compuesto por tres componentes principales:

1. **Broker**: Actúa como intermediario entre los productores y los consumidores. Recibe mensajes de los productores, los almacena en una cola y los distribuye a los consumidores utilizando un esquema de balanceo de carga.

2. **Productores**: Envían mensajes al broker para que sean procesados y entregados a los consumidores.

3. **Consumidores**: Reciben mensajes del broker. Los consumidores están organizados en grupos, y el broker utiliza un esquema de round-robin para distribuir los mensajes entre ellos.

El sistema soporta múltiples productores y consumidores concurrentes, y utiliza hilos para manejar las conexiones y la distribución de mensajes.

---

## Instrucciones para Compilar y Ejecutar

### Requisitos
- **Sistema operativo**: Linux
- **Compilador**: gcc
- **Bibliotecas**: pthread, semaphore

### Pasos para Compilar
1. Asegurarse de que el archivo Makefile esté en el directorio raíz del proyecto.

2. Asegurarse de tener gcc y make instalados.

3. Ejecutar, desde la raíz del proyecto el siguiente comando para compilar los ejecutables: 
    make

### Pasos para Ejecutar
1. Iniciar el broker: 
    ./src/broker

2. Ejecutar los consumidores y productores en el orden deseado:
    ./src/consumer
    ./src/producer

---

## Estrategias para Evitar Interbloqueos

1. Los bloqueos de mutex y semáforos se realizan en un orden consistente en todo el código.

2. Si no se puede garantizar el orden de adquisición de ciertos locks, se utiliza trylock con reintento.

3. Los semáforos se utilizan para controlar el acceso a los recursos compartidos, como la cola de mensajes y la cola de tareas. Esto asegura que los hilos no se queden esperando indefinidamente.

4. Los productores y consumidores no interactúan directamente entre sí. En su lugar, el broker actúa como intermediario, lo que reduce la posibilidad de interbloqueos.

5. El broker detecta y limpia conexiones de consumidores desconectados para evitar que los recursos queden bloqueados.

---

## Problemas Conocidos o Limitaciones

1. Por defecto, en Linux, se permiten 1024 descriptores de archivos abiertos simultáneamente por proceso. Cada socket abierto cuenta como un descriptor de archivo.
Y el sistema al utilizar sockets TCP para comunicación con consumidores y productores, se ve afectado por esta limitación. Por lo que admite un máximo de 1024 conexiones simultáneas. Esto significa que la cantidad total de productores y consumidores conectados al mismo tiempo no puede exceder este valor.

2. Aunque el broker puede recibir muchos mensajes, la cola de mensajes en memoria tiene un tamaño fijo de 100 mensajes. Si la cola está llena y no hay consumidores disponibles para liberar espacio, los productores se bloquean y esperan hasta que haya espacio disponible. Esto asegura la integridad del sistema, pero puede causar cuellos de botella bajo alta carga.

3. Cada grupo puede tener como máximo 10 consumidores conectados simultáneamente. Si este número se supera, se crea automáticamente un nuevo grupo. Esto permite escalar horizontalmente, pero también impone una restricción artificial al tamaño de los grupos, lo que podría no ser óptimo en todos los escenarios.

4. Todos los mensajes y estados, como offsets y grupos, se mantienen en memoria. Si el broker se reinicia, toda la información se pierde, incluyendo mensajes no entregados. Aunque existe un log básico en archivo para depuración, no se implementa persistencia real de los mensajes.

5. Los consumidores se asignan automáticamente al grupo con el ID más bajo disponible. No es posible que un consumidor especifique a qué grupo desea pertenecer, lo que limita la flexibilidad en escenarios avanzados como suscripciones temáticas o control manual de grupos.

6. Aunque el sistema usa un pool de hilos para manejar múltiples conexiones concurrentes, crear muchos hilos más allá del número de núcleos del procesador puede provocar degradación del rendimiento por exceso de cambio de contexto. Idealmente, el número de hilos del pool debería ajustarse al hardware del entorno de ejecución.
