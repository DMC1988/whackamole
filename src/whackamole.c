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
#define COLAINICIOLEN 1
#define NMOLES 4



/*===[Handlers Tareas]=====================================*/
TaskHandle_t handlewhackmole_sl;
TaskHandle_t handlemole_sl[4];

/*===[Colas]==============================================*/
QueueHandle_t hndlColaInicio;
QueueHandle_t hndlColaPuntaje;
QueueHandle_t hndlColaTecla;
QueueHandle_t hndlColaTopo;

/*===[Semaforo]==========================================*/
SemaphoreHandle_t SemATecla;

/*===[Mutexes]===========================================*/
SemaphoreHandle_t hndlUARTmutex;

gpioMap_t leds_t[] = {LEDB, LED1, LED2,LED3};

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
			0, /*parametro*/
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
	hndlColaInicio = xQueueCreate(COLAINICIOLEN, sizeof(TickType_t)); //La cola pasa el tiempo pulsado de tecla y luego habilita el juego según corresponda
	configASSERT( hndlColaInicio != NULL );

	hndlColaTecla = xQueueCreate(1, sizeof(t_key_data)); //La cola pasa la estructura de la tecla(martillazos)
	configASSERT( hndlColaTecla != NULL );

	hndlColaTopo = xQueueCreate(1, sizeof(mole_t)); //La cola pasa la estructura de la mole
	configASSERT( hndlColaTopo != NULL )

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
		printf("Press Btn for at least 500ms\n\r");

		/* Inicio de juego*/
		/* Recibe por la cola tiempo de pulsado de una tecla
		 * tInicioJuego > 500ms --> game-alive = True
		 * tInicioJuego <= 500ms --> game-alive = False*/
		if(passInicio == FALSE){ //Agragado para que esta porción de codigo se ejecuta una sola vez.

			xQueueReceive( hndlColaInicio, &tInicioJuego, portMAX_DELAY );

			if(tInicioJuego > 500){

				passInicio = TRUE;
				game_alive = true;
				tInicio = xTaskGetTickCount(); //Guardo la cuenta de tick en la cual se inicio el programa

				/*Se reactiva la mole_service_logic. Tener en cuenta que se ejecuta indmediatamente*/
				for(uint8_t i = 0; i < NMOLES; i++){

					vTaskResume(handlemole_sl[i]);

				}

			}else{game_alive = false;}
		}

		/* randomizo (se usa el tick count del sistema)*/
		random_seed_freertos();

		while( true == game_alive )
		{
			//printf("Iniciado\n\r");

			/*Acá inicia el programa*/

			/* Recibe el martillazo.
			 * El tiempo del golpe lo paso al topo.
			 * El topo con todos los tiempos pasa a mole_service_logic*/
			if(xQueueReceive( hndlColaTecla, &martillazo, 100) == pdTRUE){

				topo.index = martillazo.index;
				topo.tGolpe = martillazo.time_down;
				printf("martillado %d \n\r", martillazo.index);

				xQueueSend(hndlColaTopo, &topo, 0);

			}














			/* Recibe el puntaje por parte del topo, y lo suma/resta a puntos
//			 * Imprime el resultado por UART.
//			 * Se actualiza cada vez que sale un topo.
//			 * */
//			if(xQueueReceive(hndlColaPuntaje, &puntajexCola, 0) == pdTRUE){
//				puntos = puntos + puntajexCola; //Calcula el puntaje total
//
//				/*Protección de acceso a la UART*/
//				xSemaphoreTake(hndlUARTmutex, portMAX_DELAY);
//
//				printf("Sumaste/restaste: %d puntos.\n\r", puntajexCola );
//				printf("Tu puntaje total es: %d puntos.\n\r", puntajexCola );
//
//				xSemaphoreGive(hndlUARTmutex);
//			}
//
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


		/*Martillazo con topo oculto*/
		if(xQueueReceive(hndlColaTopo, &moleMartillada,pdMS_TO_TICKS(tiempo_aparicion)) == pdTRUE ){

			puntoxMartillazo = whackamole_points_no_mole();
			printf("MISS -20\n\r");
		}


		/*Sale el topo*/
		gpioWrite( mole->led, ON );
		mole->tVisible = xTaskGetTickCount();


		/*Acertó el martillazo*/
		if(xQueueReceive(hndlColaTopo, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdTRUE ){

			tReaccion = moleMartillada.tGolpe - mole->tVisible;

			if((mole->index == moleMartillada.index) && (tReaccion > 0)){ //Mole golpeada.
				gpioWrite(mole->led , OFF);

				puntoxMartillazo = whackamole_points_success(tiempo_afuera, tReaccion);

				printf("HIT %d || tR %d \n\r", mole->index, tReaccion);
			}
		}


		/*El topo se esconde*/
		gpioWrite( mole->led, OFF );


		/*No se martillo*/
		if(xQueueReceive(hndlColaTopo, &moleMartillada,pdMS_TO_TICKS(tiempo_afuera)) == pdFALSE ){

			puntoxMartillazo = whackamole_points_miss();
			printf("MISS -10\n\r");
		}
	}


}

