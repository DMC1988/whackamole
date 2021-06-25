#ifndef WHACKAMOLES_H
#define WHACKAMOLES_H

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "sapi.h"

/*===[Definicion de Variables]=============================*/
typedef struct
{
	gpioMap_t led;                	//led asociado al topo
	TickType_t tVisible;		  	//Momento cuando el topo se hace visible
	TickType_t tGolpe;			 	//Momento del golpe
	uint32_t index;					//Numero de topo

} mole_t;

/*===[Handlers Tareas]=====================================*/
TaskHandle_t handlewhackmole_sl;
TaskHandle_t handlemole_sl[4];

/*===[Colas]===============================================*/
QueueHandle_t hndlColaPuntaje;
QueueHandle_t hndlColaTecla;
QueueHandle_t hndlColaTopo0;
QueueHandle_t hndlColaTopo1;
QueueHandle_t hndlColaTopo2;
QueueHandle_t hndlColaTopo3;

/*===[Mutexes]=============================================*/
SemaphoreHandle_t hndlUARTmutex;

/*===[Prototipos de Tareas]===========================*/
void mole_service_logic( void* pvParameters );
void whackamole_service_logic( void* pvParameters );

/*===[Prototipos de funciones]=======================*/
void whackamole_init(void);
void logicaTopo0(void*, TickType_t, TickType_t );
void logicaTopo1(void*, TickType_t, TickType_t );
void logicaTopo2(void*, TickType_t, TickType_t );
void logicaTopo3(void*, TickType_t, TickType_t );


#endif
