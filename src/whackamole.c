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

/*Definiciones generales*/
#define WAM_GAMEPLAY_TIMEOUT        20000   //gameplay time
#define WAM_MOLE_SHOW_MAX_TIME      4500
#define WAM_MOLE_OUTSIDE_MAX_TIME   2500
#define WAM_MOLE_SHOW_MIN_TIME      1500
#define WAM_MOLE_OUTSIDE_MIN_TIME   750

#define NMOLES 4

/*Definiciones para colas*/
#define COLATECLALEN 1

/*===[Variables Globales Privadas]========================*/
mole_t arrayDeMoles[NMOLES];	//Array de topos. Parametro para tarea

/*=======================================================*/
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
		arrayDeMoles[i].tVisible = 0;
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

	for(i = 0; i < NMOLES; i++){ //Una tarea por topo-> Se crean 4 tareas.
		res1 = xTaskCreate(
				mole_service_logic,
				(const char *)"mole_service_logic",
				configMINIMAL_STACK_SIZE*8,
				(void*)&arrayDeMoles[i],
				tskIDLE_PRIORITY+1,
				&handlemole_sl[i]); /*Una handle para cada tarea de topo*/

		configASSERT(res1 == pdPASS);

		/*Suspende mole_service_logic y deja ready whackamole_service_logic y la de teclas */
		vTaskSuspend(handlemole_sl[i]);
	}

	/* creacion de objetos */

	/* Colas*/
	hndlColaTecla = xQueueCreate(COLATECLALEN, sizeof(t_key_data)); //La cola pasa la estructura de la tecla(martillazos)
	configASSERT( hndlColaTecla != NULL );

	hndlColaPuntaje = xQueueCreate(1, sizeof(int32_t)); //La cola pasa el puntaje
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
	hndlUARTmutex = xSemaphoreCreateMutex(); //Mutex para proteger la UART
	configASSERT( hndlUARTmutex != NULL );
}

/**
   @brief devuelve el puntaje de haber martillado al mole
   @param tiempo_afuera             tiempo q hubiera estado el mole esperando
   @param tiempo_reaccion_usuario   tiempo de reaccion del usuario en martillar
   @return uint32_t
 */
int32_t whackamole_points_success( TickType_t tiempo_afuera,TickType_t tiempo_reaccion_usuario )
{
	return ( WAM_MOLE_OUTSIDE_MAX_TIME*WAM_MOLE_OUTSIDE_MAX_TIME ) /( tiempo_afuera*tiempo_reaccion_usuario );
}

/**
   @brief devuelve el puntaje por haber perdido al mole
   @return uint32_t
 */
int32_t whackamole_points_miss()
{
	return -10;
}

/**
   @brief devuelve el puntaje por haber martillado cuando no habia mole
   @return uint32_t
 */
int32_t whackamole_points_no_mole()
{
	return -20;
}

/**
   @brief servicio principal del juego
   @param pvParameters
 */
void whackamole_service_logic( void* pvParameters )
{
	static bool passInicio = FALSE;	// Pass para los 500ms iniciales
	bool game_alive = FALSE;		// Pass para activar el juego

	t_key_data martillazo;			//Estructura para recibir los martillazos de keys.c
	mole_t topo;					//Estructura para pasar datos a mole_service_logic

	int32_t puntos = 0; 			//Inicializo puntos en 0
	int32_t puntajexCola = 0; 		//Variable para recibir los datos por la cola

	TickType_t tInicio = 0; 		//Variable para registrar el inicio del juego
	TickType_t tActual = 1;			//Variable para registrar el tick actual del juego

	while( 1 )
	{
		printf("Presionar un boton por al menos 500ms para iniciar. \n\r");

		/* Inicio de juego*/
		/* Recibe por la cola tiempo de pulsado de una tecla
		 * tiempo > 500ms --> game-alive = True
		 * tiempo <= 500ms --> game-alive = False*/
		if(passInicio == FALSE){ //Agragado para que esta porción de codigo se ejecuta una sola vez.

			xQueueReceive( hndlColaTecla, &martillazo, portMAX_DELAY );

			if(martillazo.time_diff > 500){

				passInicio = TRUE;

				/*Inicia el juego*/
				game_alive = true;
				tInicio = xTaskGetTickCount(); //Guardo la cuenta de tick en la cual se inicio el programa

				/*Se reactiva cada mole_service_logic para dar inicio al juego.*/
				for(uint8_t i = 0; i < NMOLES; i++){

					vTaskResume(handlemole_sl[i]);

				}

			}else{game_alive = false;} //Si no se presiono durante 500ms no inicia el juego.
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
			/* Recibe puntaje por cola cuando se martille un topo, cuando se martille un topo oculto o cuando no se martilla */
			if(xQueueReceive(hndlColaPuntaje, &puntajexCola, 0) == pdTRUE){

				/*Calcula el puntaje total*/
				puntos = puntos + puntajexCola;

				/*Protección de acceso a la UART ante acceso de tarea mole_service_logic*/
				xSemaphoreTake(hndlUARTmutex, portMAX_DELAY);
				printf("Tu puntaje actual es: %d puntos.\n\r", puntos );
				xSemaphoreGive(hndlUARTmutex);
			}

			tActual = xTaskGetTickCount(); //Guardo la cuenta de tick en actual

			/*tiempo de juego>tiempo limite -> Finaliza el juego*/
			if( pdMS_TO_TICKS(tActual-tInicio) > WAM_GAMEPLAY_TIMEOUT){

				printf("Fin del Juego. \n\r");
				printf("Puntaje Final: %d \n\r", puntos);

				for(uint8_t i = 0; i < NMOLES; i++){

					/* Cuando finalice el juego, las otras tareas, deberán cesar su actividad. */
					vTaskSuspend(handlemole_sl[i]);

					/*Apago les LEDS*/
					gpioWrite(LEDB+i, OFF);

				}

					/*Desactivo el pass de los 500ms al iniciar*/
					passInicio = FALSE;

					/*Desactivo el pass del juego*/
					game_alive = FALSE;
					/*Reinicio puntaje*/
					puntos = 0;

					/*Reinicio los tiempos*/
					tInicio = 0;
					tActual = 1;

				}
			}

		}

	}

/**
   @brief servicio instanciado de cada mole
   @param pvParameters
 */
void mole_service_logic( void* pvParameters )
{
	mole_t* mole = ( mole_t* ) pvParameters;	//Parametros

	TickType_t tiempo_aparicion;	//Tiempo de topo escondido
	TickType_t tiempo_afuera;		//Tiempo tiempo visible

	while( 1 )
	{
		/* preparo el turno */
		tiempo_aparicion = random( WAM_MOLE_SHOW_MIN_TIME, WAM_MOLE_SHOW_MAX_TIME );
		tiempo_afuera    = random( WAM_MOLE_OUTSIDE_MIN_TIME, WAM_MOLE_OUTSIDE_MAX_TIME );

		/*Funciones con la logica de cada topo*/
		/*Ejecuta la función correspondiente al topo en estado running*/
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

	mole_t* mole = ( mole_t* ) pvParameters;	//Parametro proveniente de la tarea

	int32_t puntoxMartillazo = 0;				//Variable para enviar puntos a tarea principal

	mole_t moleMartillada; 						//Estructura para recibir datos de martillazo desde tarea principal

	TickType_t tReaccion; 						//Variable para guardar tiempo de reaccion

	/* Martillazo con topo oculto */
	/* Si el martillazo llega cuando el topo está oculto, resta -20 puntos */
	/* La cola espera un martillazo durante el tiempo oculto */
	if(xQueueReceive(hndlColaTopo0, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdTRUE ){

		puntoxMartillazo = whackamole_points_no_mole();

		/* Si recibe martillazo envia el valor por cola a tarea principal */
		/*Enviar por cola puntoxMartillazo a tarea principal -20*/
		xQueueSend(hndlColaPuntaje, &puntoxMartillazo,0);

	}else{

		/* Sale el topo */
		/* Si el topo estaba "afuera del hoyo" cuando ocurrió el martillazo, medirá el tiempo
		entre que "salió del hoyo" hasta que recibe el martillazo, y se calculará el puntaje. */

		/* Luego de estar oculto sale el topo */
		/* Registra el tick de salida en el topo correspondiente */
		gpioWrite( mole->led, ON );
		mole->tVisible = xTaskGetTickCount();

		/* Acertó el martillazo*/
		/* Espera por cola un martillazo durante el tiempo que el topo esta afuera */
		if(xQueueReceive(hndlColaTopo0, &moleMartillada,pdMS_TO_TICKS(tiempo_aparicion)) == pdTRUE ){

			/* Martillazo recibido. Calcula tiempo de reaccion */
			tReaccion = moleMartillada.tGolpe - mole->tVisible;

			/*Apaga el LED, se esconde el topo*/
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




