#include <stdarg.h>
#include <stdbool.h>
#include "whackamole.h"
#include "random.h"
#include "sapi.h"
#include "keys.h"

#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"


#define WAM_GAMEPLAY_TIMEOUT        20000   //gameplay time
#define WAM_MOLE_SHOW_MAX_TIME      4000//4000
#define WAM_MOLE_OUTSIDE_MAX_TIME   2000//2000
#define WAM_MOLE_SHOW_MIN_TIME      1000//1000
#define WAM_MOLE_OUTSIDE_MIN_TIME   500//500


/*Defines para colas*/
#define COLATECLALEN 1
#define NMOLES 4



/*===[Handlers Tareas]=====================================*/
TaskHandle_t handlewhackmole_sl;
TaskHandle_t handlemole_sl[4];

/*===[Colas]==============================================*/
QueueHandle_t hndlColaPuntaje;
QueueHandle_t hndlColaTecla;
QueueHandle_t hndlColaTopo0;
QueueHandle_t hndlColaTopo1;
QueueHandle_t hndlColaTopo2;
QueueHandle_t hndlColaTopo3;

/*===[Semaforo]==========================================*/
SemaphoreHandle_t SemATecla;

/*===[Mutexes]===========================================*/
SemaphoreHandle_t hndlUARTmutex;


/*===[Variables Globales Privadas]======================*/
typedef struct
{

	gpioMap_t led;                //led asociado al mole
	TickType_t tVisible;
	TickType_t tEscondida;
	TickType_t tGolpe;
	TickType_t tdiff;
	int32_t puntaje;
	uint32_t index;

} mole_t;

mole_t arrayDeMoles[NMOLES];






/* prototypes */
void mole_service_logic( void* pvParameters );
void whackamole_service_logic( void* pvParameters );

void logicaTopo0(void*, TickType_t, TickType_t );
void logicaTopo1(void*, TickType_t, TickType_t );
void logicaTopo2(void*, TickType_t, TickType_t );
void logicaTopo3(void*, TickType_t, TickType_t );



/**
   @brief init game
 */
void whackamole_init()
{
	BaseType_t res0, res1;

	uint8_t i = 0;

	/*Inicializo las moles*/
	for(i = 0; i < NMOLES; i++){

		arrayDeMoles[i].led = (LEDB + i); //Asigno un led a  cada mole. LEDB hasta LED3
		arrayDeMoles[i].puntaje = 0;	//Las demas variables las inicializo en 0
		arrayDeMoles[i].tEscondida = 0;
		arrayDeMoles[i].tVisible = 0;
		arrayDeMoles[i].tdiff = 0;
		arrayDeMoles[i].tGolpe = 0;
		arrayDeMoles[i].index = i;
	}

	/*Creacion de tareas */

	res0 = xTaskCreate(
			whackamole_service_logic,
			(const char *)"whackamole_service_logic",
			configMINIMAL_STACK_SIZE*6,
			0,
			tskIDLE_PRIORITY+1,
			&handlewhackmole_sl);

	configASSERT( res0 == pdPASS);

	for(i = 0; i < NMOLES; i++){ //Una tarea por mole-> Esto crea 4 tareas
		res1 = xTaskCreate(
				mole_service_logic,
				(const char *)"mole_service_logic",
				configMINIMAL_STACK_SIZE*8,
				(void*)&arrayDeMoles[i],
				tskIDLE_PRIORITY+1,
				&handlemole_sl[i]); /*Una handle para cada tarea de mole*/

		configASSERT(res1 == pdPASS);

		/*Suspende mole_service_logic y deja ready whackamole_service_logic y la de teclas */
		vTaskSuspend(handlemole_sl[i]);
	}


	/* creacion de objetos */

	/*Semaforo*/

	SemATecla = xSemaphoreCreateBinary();
	configASSERT( SemATecla != NULL );

	/* Colas*/
	hndlColaTecla = xQueueCreate(COLATECLALEN, sizeof(t_key_data)); //La cola pasa la estructura de la tecla(martillazos)
	configASSERT( hndlColaTecla != NULL );

	hndlColaPuntaje = xQueueCreate(1, sizeof(int32_t)); //La cola pasa la estructura de la tecla(martillazos)
	configASSERT( hndlColaPuntaje != NULL );

	hndlColaTopo0 = xQueueCreate(1, sizeof(mole_t)); //La cola pasa la estructura de la mole
	configASSERT( hndlColaTopo0 != NULL )

	hndlColaTopo1 = xQueueCreate(1, sizeof(mole_t)); //La cola pasa la estructura de la mole
	configASSERT( hndlColaTopo1 != NULL )

	hndlColaTopo2 = xQueueCreate(1, sizeof(mole_t)); //La cola pasa la estructura de la mole
	configASSERT( hndlColaTopo2 != NULL )

	hndlColaTopo3 = xQueueCreate(1, sizeof(mole_t)); //La cola pasa la estructura de la mole
	configASSERT( hndlColaTopo3 != NULL )

	/* Mutexes*/
	hndlUARTmutex = xSemaphoreCreateMutex(); //Semaforo para proteger la UART
	configASSERT( hndlUARTmutex != NULL );
}


/**
   @brief devuelve el puntaje de haber martillado al mole
   @param tiempo_afuera             tiempo q hubiera estado el mole esperando
   @param tiempo_reaccion_usuario   tiempo de reaccion del usuario en martillar
   @return uint32_t
 */
uint32_t whackamole_points_success( TickType_t tiempo_afuera,TickType_t tiempo_reaccion_usuario )
{
	return ( WAM_MOLE_OUTSIDE_MAX_TIME*WAM_MOLE_OUTSIDE_MAX_TIME ) /( tiempo_afuera*tiempo_reaccion_usuario );
}

/**
   @brief devuelve el puntaje por haber perdido al mole
   @return uint32_t
 */
uint32_t whackamole_points_miss()
{
	return -10;
}

/**
   @brief devuelve el puntaje por haber martillado cuando no habia mole
   @return uint32_t
 */
uint32_t whackamole_points_no_mole()
{
	return -20;
}



/**
   @brief servicio principal del juego
   @param pvParameters
 */
void whackamole_service_logic( void* pvParameters )
{
	static bool passInicio = FALSE;
	bool game_alive = FALSE;


	BaseType_t res;

	TickType_t tInicioJuego = 0; //Para el inicio del juego

	t_key_data martillazo;
	mole_t topo;

	//wam_event_t evnt;

	int32_t puntos = 0; //Inicializo puntos en 0
	int32_t puntajexCola = 0; //Variable para recibir los datos por la cola

	int32_t hits;
	int32_t miss;

	TickType_t tInicio = 0;
	TickType_t tActual = 1;


	while( 1 )
	{
		printf("Presionar un boton por al menos 500ms \n\r");

		/* Inicio de juego*/
		/* Recibe por la cola tiempo de pulsado de una tecla
		 * tiempo > 500ms --> game-alive = True
		 * tiempo <= 500ms --> game-alive = False*/
		if(passInicio == FALSE){ //Agragado para que esta porción de codigo se ejecuta una sola vez.

			xQueueReceive( hndlColaTecla, &martillazo, portMAX_DELAY );

			if(martillazo.time_diff > 500){

				passInicio = TRUE;
				game_alive = true;
				tInicio = xTaskGetTickCount(); //Guardo la cuenta de tick en la cual se inicio el programa

				/*Se reactiva la mole_service_logic.*/
				for(uint8_t i = 0; i < NMOLES; i++){

					vTaskResume(handlemole_sl[i]);

				}

			}else{game_alive = false;}
		}

		/* randomizo (se usa el tick count del sistema)*/
		random_seed_freertos();

		while( true == game_alive )
		{
			/*Acá inicia el programa*/

			/* Recibirá los martillazos desde el driver de teclas.*/
			if(xQueueReceive( hndlColaTecla, &martillazo, 0) == pdTRUE){

				topo.index = martillazo.index;
				topo.tGolpe = martillazo.time_down;

				/*Enviará el martillazo al topo correspondiente*/
				if(topo.index == 0){xQueueSend(hndlColaTopo0, &topo, 0);}
				if(topo.index == 1){xQueueSend(hndlColaTopo1, &topo, 0);}
				if(topo.index == 2){xQueueSend(hndlColaTopo2, &topo, 0);}
				if(topo.index == 3){xQueueSend(hndlColaTopo3, &topo, 0);}

			}



			/* Deberá informar por UART cada actualización del puntaje. */
			/* Recibe puntaje por cola cuando se martilla un topo, cuando se martilla un topo oculto o cuando no se martilla */
			if(xQueueReceive(hndlColaPuntaje, &puntajexCola, 0) == pdTRUE){
				puntos = puntos + puntajexCola; //Calcula el puntaje total

				/*Protección de acceso a la UART ante acceso de tarea mole_service_logic*/
				xSemaphoreTake(hndlUARTmutex, portMAX_DELAY);

				printf("Sumaste/restaste: %d puntos.\n\r", puntos );
				printf("Tu puntaje total es: %d puntos.\n\r", puntos );

				xSemaphoreGive(hndlUARTmutex);
			}

			tActual = xTaskGetTickCount(); //Guardo la cuenta de tick en actual


//			/*tiempo de juego>tiempo limite -> Finaliza el juego*/
//			if( pdMS_TO_TICKS(tActual-tInicio) > WAM_GAMEPLAY_TIMEOUT){
//
//				printf("Fin del Juego \n\r");
//				printf("Puntaje Final: %d", puntos);
//
//				vTaskDelay(pdMS_TO_TICKS(3000));
//
//				/*Ver que hago con todas las demas tareas*/
//			}

		}

	}
}

/**
   @brief servicio instanciado de cada mole
   @param pvParameters
 */
void mole_service_logic( void* pvParameters )
{
	mole_t* mole = ( mole_t* ) pvParameters;

	int32_t puntoxMartillazo = 0;

	mole_t moleMartillada;

	TickType_t tiempo_aparicion;
	TickType_t tiempo_afuera;
	TickType_t tReaccion;


	while( 1 )
	{
		/* preparo el turno */
		tiempo_aparicion = random( WAM_MOLE_SHOW_MIN_TIME, WAM_MOLE_SHOW_MAX_TIME ); 		//Tiempo de topo escondido
		tiempo_afuera    = random( WAM_MOLE_OUTSIDE_MIN_TIME, WAM_MOLE_OUTSIDE_MAX_TIME );  //Tiempo tiempo visible

		/*Funciones con la logica de cada mole*/
		/*Analiza la mole correspondite a la tarea en running*/
		switch(mole->index){
		case 0:
			logicaTopo0(mole, tiempo_aparicion, tiempo_afuera);
			break;
		case 1:
			logicaTopo1(mole, tiempo_aparicion, tiempo_afuera);
			break;

		case 2:
			logicaTopo2(mole, tiempo_aparicion, tiempo_afuera);
			break;
		case 3:
			logicaTopo3(mole, tiempo_aparicion, tiempo_afuera);
			break;
		}
	}

}

/**
   @brief función lógica de cada mole
   @param pvParameters
   @param tiempo_aparicion
   @param tiempo_afuera
 */
void logicaTopo0(void* pvParameters, TickType_t tiempo_aparicion, TickType_t tiempo_afuera){

	mole_t* mole = ( mole_t* ) pvParameters;

	int32_t puntoxMartillazo = 0;

	mole_t moleMartillada; //Estructura para recibir datos de martillazo desde tarea principal

	TickType_t tReaccion; //Variable para guardar tiempo de reaccion

	/* Martillazo con topo oculto */
	/* Si el martillazo llega cuando el topo está oculto, resta -20 puntos */
	/* La cola espera un martillado durante el tiempo oculto */

	if(xQueueReceive(hndlColaTopo0, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdTRUE ){

		/* Si recibe martillazo envia el valor por cola a tarea principal */

		puntoxMartillazo = whackamole_points_no_mole();

		/*Enviar por cola puntoxMartillazo a tarea principal -20*/
		xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);


	}else{
		/*Sale el topo*/
		/* Si el topo estaba "afuera del hoyo" cuando ocurrió el martillazo, medirá el tiempo
		entre que "salió del hoyo" hasta que recibe el martillazo, y se calculará el puntaje. */

		/* Luego de estar oculto sale el topo */
		/* Registra el tick de salida en el topo correspondiente */

		gpioWrite( mole->led, ON );
		mole->tVisible = xTaskGetTickCount();

		/*Acertó el martillazo*/
		/* Espera por cola un martillazo durante el tiempo que el topo esta afuera */

		if(xQueueReceive(hndlColaTopo0, &moleMartillada,pdMS_TO_TICKS(tiempo_aparicion)) == pdTRUE ){

			/* Martillazo recibido. Calcula tiempo de reaccion */

			tReaccion = moleMartillada.tGolpe - mole->tVisible;

			gpioWrite(mole->led , OFF);

			/* Calcula el puntaje y lo envia por cola a tarea principal */

			puntoxMartillazo = whackamole_points_success(tiempo_afuera, tReaccion);

			/*Enviar puntaje por cola*/
			xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);

			/* Deberá informar por UART cada vez que reciba un martillazo, enviando el tiempo de
			   reacción del usuario. */
			xSemaphoreTake(hndlUARTmutex, portMAX_DELAY);
			printf("HIT %d || tR %d \n\r", mole->index, tReaccion);
			xSemaphoreGive(hndlUARTmutex);

		}else{
			/*El topo se esconde transcurrido el tiempo de aparación en el caso de que no hubo martillazo*/
			gpioWrite(mole->led, OFF );

			/*No se martillo*/
			/*Si no se martilla durante la aparición del topo, resta -10 puntos.
			(función whackamole_points_miss)*/
			if(xQueueReceive(hndlColaTopo0, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdFALSE ){

				puntoxMartillazo = whackamole_points_miss();
				/*Enviar por cola el puntaje -10*/
				xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);
			}
		}
	}
}

/**
   @brief función lógica de cada mole
   @param pvParameters
   @param tiempo_aparicion
   @param tiempo_afuera
 */
void logicaTopo1(void* pvParameters, TickType_t tiempo_aparicion, TickType_t tiempo_afuera){

	mole_t* mole = ( mole_t* ) pvParameters;

	int32_t puntoxMartillazo = 0;

	mole_t moleMartillada;

	TickType_t tReaccion;

	/*Martillazo con topo oculto*/
	if(xQueueReceive(hndlColaTopo1, &moleMartillada,pdMS_TO_TICKS(tiempo_aparicion)) == pdTRUE ){

		puntoxMartillazo = whackamole_points_no_mole();
		/*Enviar por cola puntoxMartillazo a tarea principal -20*/
		xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);

	}else{
		/*Sale el topo*/
		gpioWrite( mole->led, ON );
		mole->tVisible = xTaskGetTickCount();

		/*Acertó el martillazo*/
		if(xQueueReceive(hndlColaTopo1, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdTRUE ){

			tReaccion = moleMartillada.tGolpe - mole->tVisible;


			gpioWrite(mole->led , OFF);

			puntoxMartillazo = whackamole_points_success(tiempo_afuera, tReaccion);

			/*Enviar por cola puntoxMartillazo a tarea principal*/
			xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);

			xSemaphoreTake(hndlUARTmutex, portMAX_DELAY);
			printf("HIT %d || tR %d \n\r", mole->index, tReaccion);
			xSemaphoreGive(hndlUARTmutex);

		}else{
			/*El topo se esconde*/
			gpioWrite( mole->led, OFF );

			/*No se martillo*/
			if(xQueueReceive(hndlColaTopo1, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdFALSE ){

				puntoxMartillazo = whackamole_points_miss();
				/*Enviar por cola puntoxMartillazo a tarea principal -10*/
				xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);
			}
		}
	}
}

/**
   @brief función lógica de cada mole
   @param pvParameters
   @param tiempo_aparicion
   @param tiempo_afuera
 */
void logicaTopo2(void* pvParameters, TickType_t tiempo_aparicion, TickType_t tiempo_afuera){

	mole_t* mole = ( mole_t* ) pvParameters;

	int32_t puntoxMartillazo = 0;

	mole_t moleMartillada;

	TickType_t tReaccion;

	/*Martillazo con topo oculto*/
	if(xQueueReceive(hndlColaTopo2, &moleMartillada,pdMS_TO_TICKS(tiempo_aparicion)) == pdTRUE ){

		puntoxMartillazo = whackamole_points_no_mole();

		/*Enviar por cola puntoxMartillazo a tarea principal -20*/
		xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);

	}else{
		/*Sale el topo*/
		gpioWrite( mole->led, ON );
		mole->tVisible = xTaskGetTickCount();

		/*Acertó el martillazo*/
		if(xQueueReceive(hndlColaTopo2, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdTRUE ){

			tReaccion = moleMartillada.tGolpe - mole->tVisible;


			gpioWrite(mole->led , OFF);

			puntoxMartillazo = whackamole_points_success(tiempo_afuera, tReaccion);

			/*Enviar por cola puntoxMartillazo a tarea principal*/
			xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);

			xSemaphoreTake(hndlUARTmutex, portMAX_DELAY);
			printf("HIT %d || tR %d \n\r", mole->index, tReaccion);
			xSemaphoreGive(hndlUARTmutex);
		}
		else{
			/*El topo se esconde*/
			gpioWrite( mole->led, OFF );

			/*No se martillo*/
			if(xQueueReceive(hndlColaTopo2, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdFALSE ){

				puntoxMartillazo = whackamole_points_miss();

				/*Enviar por cola puntoxMartillazo a tarea principal -10*/
				xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);
			}
		}
	}
}

/**
   @brief función lógica de cada mole
   @param pvParameters
   @param tiempo_aparicion
   @param tiempo_afuera
 */
void logicaTopo3(void* pvParameters, TickType_t tiempo_aparicion, TickType_t tiempo_afuera){

	mole_t* mole = ( mole_t* ) pvParameters;

	int32_t puntoxMartillazo = 0;

	mole_t moleMartillada;

	TickType_t tReaccion;

	/*Martillazo con topo oculto*/
	if(xQueueReceive(hndlColaTopo3, &moleMartillada,pdMS_TO_TICKS(tiempo_aparicion)) == pdTRUE ){

		puntoxMartillazo = whackamole_points_no_mole();

		/*Enviar por cola puntoxMartillazo a tarea principal -20*/
		xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);

	}else{
		/*Sale el topo*/
		gpioWrite( mole->led, ON );
		mole->tVisible = xTaskGetTickCount();

		/*Acertó el martillazo*/
		if(xQueueReceive(hndlColaTopo3, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdTRUE ){

			tReaccion = moleMartillada.tGolpe - mole->tVisible;


			gpioWrite(mole->led , OFF);

			puntoxMartillazo = whackamole_points_success(tiempo_afuera, tReaccion);

			xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);

			xSemaphoreTake(hndlUARTmutex, portMAX_DELAY);
			printf("HIT %d || tR %d \n\r", mole->index, tReaccion);
			xSemaphoreGive(hndlUARTmutex);
		}
		else{
			/*El topo se esconde*/
			gpioWrite( mole->led, OFF );

			/*No se martillo*/
			if(xQueueReceive(hndlColaTopo3, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdFALSE ){

				puntoxMartillazo = whackamole_points_miss();
				/*Enviar por cola puntoxMartillazo a tarea principal -10*/
				xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);

			}
		}
	}
}




