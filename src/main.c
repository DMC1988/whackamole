/*=============================================================================
 * Copyright (c) 2021, Franco Bucafusco <franco_bucafusco@yahoo.com.ar>
 * 					   Martin N. Menendez <mmenendez@fi.uba.ar>
 * All rights reserved.
 * License: Free
 * Version: v1.0
 *===========================================================================*/

#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "sapi.h"
#include "queue.h"
#include "semphr.h"

#include "keys.h"
#include "whackamole.h"

/*===============================================*/
#define semilla 4



int main( void )
{
    /* Inicializar la placa */
    boardConfig();

    printf( "Booting Game\n" );

    /*Semilla para generar numeros random*/
    random_set_seed( semilla );

    /* inicializo driver de teclas */
    keys_Init();

    /* inicializo juego */
    whackamole_init();

    /* arranco el scheduler */
    vTaskStartScheduler();

    return 0;
}
