#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <netdb.h>
#include "SerialManager.h"


/**
 * La EDU-CIAA se comunicará con el servicio SerialService mediante el puerto serie, que deberá
 * recibir las tramas de la placa indicando que se presionó un pulsador. Cuando esto ocurra, deberá
 * informarlo al servicio InterfaceService mediante socket TCP.
 * También deberá enviar por el puerto serie la trama que le indica el estado de las salidas,
 * información que le provee el servicio InterfaceService. (Cuando se modifican los archivos desde el
 * sitio web). Este servicio iniciará un servidor TCP para que el servicio InterfaceService se pueda
 * conectar y comunicar.
 */

/**
 * Protocolo serie entre EDU-CIAA y SerialService
 * Seteo estado de salidas (hacia la edu-ciaa)
 * “>OUTS:X,Y,W,Z\r\n”
 * Siendo X,Y,W,Z el estado de cada salida. 
 * Los estados posibles son ““0” (apagado),”1”(encendido) o “2” (blink)
 * Evento pulsador (desde la edu-ciaa)
 * “>TOGGLE STATE:X\r\n”
 * Siendo X el número de pulsador (0,1, 2 o 3)
 */

/**
 * Protocolo TCP entre SerialService e InterfaceService
 * Seteo estado de salidas (desde InterfaceService a serialService)
 * “:STATESXYWZ\n”
 * Siendo X,Y,W,Z el estado de cada salida. 
 * Los estados posibles son ““0” (apagado),”1”(encendido) o “2” (blink)
 * Evento pulsador (desde serialService a InterfaceService)
 * “:LINEXTG\n”
 * Siendo X el número de pulsador (0,1, 2 o 3)
 */


#define BUFFER_SIZE 128                 // Tamaño del buffer
#define SERIAL_BUFFER_SIZE 17                 // Tamaño del buffer
#define SOCKET_BACKLOG 10               // Cantidad de conexiones en cola que puede tener el socket
#define BAUDRATE 115200
#define TTYYUSB1 1
#define SOCKET_PORT 10000
volatile sig_atomic_t running_program = 1;        // Nombre de variable global para ser usada en los sigs functions.
volatile sig_atomic_t connected_status = 1;        // Nombre de variable global para ser usada en los sigs functions.
pthread_t socket_thread;											// Instanciamos el thread
int sock_fd;
uint32_t read_bytes;
int accept_fd;								// File descriptor para la conexión entrante.
socklen_t addr_len;							// Variable de longitud de la dirección
struct sockaddr_in clientaddr;				// Dirección del servidor
struct sockaddr_in serveraddr;				// Dirección del cliente
char sock_buffer[BUFFER_SIZE];					// Creación del buffer
char sock_auxbuffer[18];
char s_buffer[SERIAL_BUFFER_SIZE];
char aux_buffer[10];
int thr,inet,b,l,r;


void sigint_handler(int sig);		// Prototipo de la función que manejará la señal

void closeSocketRequest(int socket){
	if(close(socket)==0){
		printf("*** OK *** Cerramos correctamente el socket solicitado correctamente\r\n");
	}else{
		printf("*** ERROR *** No fue posible cerrar el socket solicitado\r\n");
		exit(EXIT_FAILURE);
	}
}

void globalCloseProcedure(){
	printf("--- Solicitamos cierre de socket principal ---\r\n");
	closeSocketRequest(sock_fd);

	printf("--- Cancelamos el hilo en ejecución ---\r\n");
	pthread_cancel(socket_thread);

	int err = pthread_join(socket_thread,NULL);
	if(err == EINVAL){
		printf("No es posible hacer join con el hilo\r\n");
	}
	printf("*** OK *** Cancelado correctamente ---\r\n");
	printf("--- FIN DEL PROGRAMA ---\r\n");
	exit(EXIT_FAILURE);
}

void acceptSocketCloseProcedure(){
	printf("--- Solicitamos cierre de socket de aceptación ---\r\n");
	closeSocketRequest(accept_fd);
	connected_status = 0;
}

void sigint_handler(int sig)
{
	write(0, "*** Se detectó que presionaron control + C ***\r\n", 45);
	running_program = 0;
}

void sigterm_handler(int sig)
{
	write(0, "*** Se detectó SIGTERM ***\r\n", 45);
	running_program = 0;
}


void* socket_handler (void* arg){
	/* Como pudimos crear el socket entramos en el hilo donde nos manentemos con el socket */
	while (1){
		printf("--- Hilo ejecutandose, en espera de conexión ---\r\n");
		connected_status = 0;
		
    	accept_fd = accept(sock_fd, (struct sockaddr *)&clientaddr, &addr_len);
    	if (accept_fd==-1){
    		printf("*** ERROR *** leyendo de la cola del socket con el accept... Devuelve errno: %d, cuyo mensaje es: %s\r\n",errno,strerror(errno));
    		acceptSocketCloseProcedure();
	    }
	    printf  ("--- CONEXIÓN RECIBIDA ---- %s\r\n", inet_ntoa(clientaddr.sin_addr));
	    connected_status = 1;

    	while(connected_status == 1){
			// Leemos mensaje de cliente
			r =read(accept_fd,sock_buffer,BUFFER_SIZE);
			if(r== -1){
				perror("*** ERROR *** leyendo mensaje en socket\r\n");
				acceptSocketCloseProcedure();
			}else if (r==0){
				printf("*** ATENCIÓN *** El cliente se ha desconectado\r\n");
				acceptSocketCloseProcedure();
			}else if (r>0){
		    	sock_buffer[r] = 0x00;
				printf("--- SE RECIBE por el socket:%s", sock_buffer);

				//Creacion del string para enviar por la UART
				sprintf(sock_auxbuffer, ">OUTS:%c,%c,%c,%c\r\n", sock_buffer[7], sock_buffer[8], sock_buffer[9], sock_buffer[10]);
				printf("--- SE ENVÍA por la UART: %s", sock_auxbuffer);

				//Uso de mutex por el recurso compartido
				serial_send(sock_auxbuffer, sizeof(sock_auxbuffer));
			}
		}
	}
}

void socketInit(){
	/* Creación del socket de tipo STREAM y para IPv4 y protocolo por defecto para el tipo seleccionado. */
	sock_fd = socket(PF_INET,SOCK_STREAM, 0);

	// Realizamos el chequeo de error si obtenemos error mostramos mensaje y retornamos.
	if (sock_fd==-1){
		printf("*** ERROR *** creando el socket. Devuelve errno: %d, cuyo mensaje es: %s\r\n",errno,strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* Establecemos los parametros de la dirección del servidor */
    bzero((char *) &serveraddr, sizeof(serveraddr));			// Limpiamos la variable.
    serveraddr.sin_family = AF_INET; 							// Establecemos para IPv4
    serveraddr.sin_port = htons(SOCKET_PORT);					// Puerto 10000
    inet = inet_pton(AF_INET, "127.0.0.1", &(serveraddr.sin_addr));
    if(!inet){
    	printf("*** ERROR *** estableciendo la IP para la dirección del servidor.. Devuelve errno: %d, cuyo mensaje es: %s\r\n",errno,strerror(errno));
    	closeSocketRequest(sock_fd);
    	exit(EXIT_FAILURE);
	}

	b = bind(sock_fd, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
	if (b==-1){
		printf("*** ERROR *** haciendo bind con el socket.. Devuelve errno: %d, cuyo mensaje es: %s\r\n",errno,strerror(errno));
    	closeSocketRequest(sock_fd);
    	exit(EXIT_FAILURE);
	}

	l = listen(sock_fd, SOCKET_BACKLOG);
	if (l==-1){
		printf("*** ERROR *** estableciendo en listen el socket.. Devuelve errno: %d, cuyo mensaje es: %s\r\n",errno,strerror(errno));
    	closeSocketRequest(sock_fd);
    	exit(EXIT_FAILURE);
	}
}

void checkSerial(){
	read_bytes = serial_receive(s_buffer, sizeof(s_buffer));
	if (read_bytes > 0)
	{
		if(strcmp(s_buffer,">OK\r\n") == 0){
			printf("--- SE RECIBIÓ OK ---\r\n");
		}

		if(strcmp(s_buffer,">TOGGLE STATE:0\r\n")==0 ||	strcmp(s_buffer,">TOGGLE STATE:1\r\n")==0 
			||	strcmp(s_buffer,">TOGGLE STATE:2\r\n")==0  || strcmp(s_buffer,">TOGGLE STATE:3\r\n")==0){

			printf("--- SE RECIBIÓ por la UART: %s",s_buffer);
			
			sprintf(aux_buffer, ":LINE%cTG\n", s_buffer[14]);

			if(connected_status == 1){
				printf("--- ENVÍO al socket: %s", aux_buffer);
				int32_t retval = write(accept_fd, aux_buffer, sizeof(aux_buffer));
				if (retval == -1){
					perror("*** ERROR **** Escribiendo mensaje en el socket\r\n");
					exit(1);
				}
			}
		}
	} 	
	usleep(100);
}



int main()
{
	struct sigaction sa_int;				// Creamos la variable de tipo sigaction
	struct sigaction sa_term;				// Creamos la variable de tipo sigaction
	sa_int.sa_handler = sigint_handler;		// Le indicamos cual va a ser el manejador.
	sa_int.sa_flags = 0;					// No habrá ningún flag.
	sigemptyset(&sa_int.sa_mask);			// Establecemos en 0 las señales que se bloquean mientras esta esta siendo manejada.

	sa_term.sa_handler = sigterm_handler;	// Le indicamos cual va a ser el manejador.
	sa_term.sa_flags = 0;					// No habrá ningún flag.
	sigemptyset(&sa_term.sa_mask);			// Establecemos en 0 las señales que se bloquean mientras esta esta siendo manejada.

	printf("--- Inicia el programa --- \r\n");

	// Verificamos que podamos dar el handle a la señal correspondiente.
	if(sigaction(SIGINT, &sa_int, NULL) == -1){	
		perror("Sigaction not made sucessfully\r\n");
		exit(EXIT_FAILURE);
	}

	// Verificamos que podamos dar el handle a la señal correspondiente.
	if(sigaction(SIGTERM, &sa_term, NULL) == -1){	
		perror("Sigaction not made sucessfully\r\n");
		exit(EXIT_FAILURE);
	}

	printf("--- Establecemos correctamente las señales --- \r\n");


	// Realizamos el chequeo de error si obtenemos error mostramos mensaje y retornamos.
	int32_t serial_r;
	serial_r = serial_open(TTYYUSB1, BAUDRATE);
	if (serial_r!=0){
		printf("*** ERROR *** estableciendo el serial \r\n");
		return 0;
	}
	printf("--- Puerto serial estblecido correctamente --- \r\n");


	socketInit();
	printf("--- Socket configurado correctamente --- \r\n");	

	thr = pthread_create(&socket_thread, NULL, socket_handler, NULL);	// Creamos el thread

	// Realizamos el chequeo de error si obtenemos error mostramos mensaje y retornamos.
	if (thr!=0){
		printf("*** ERROR *** creando el thread del socket. Devuelve errno: %d, cuyo mensaje es: %s\r\n",errno,strerror(errno));
		return 0;
	}

	while (1)
	{
		checkSerial();
		if(!running_program){
			printf("--- Procedimiento de cierre de programa ---\r\n");
			acceptSocketCloseProcedure();
			globalCloseProcedure();
		}
	}
	printf("--- FIN DEL PROGRAMA ---\r\n");
	return 0;
}