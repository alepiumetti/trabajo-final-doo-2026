#ifndef constantes_h
#define constantes_h

// Id del nodo Batería Comunitaria. Debe coincidir con la semilla de
// sql/crear_esquema.sql y con los casos de aceptación del PDF.
#define BATERIA 99

// Capacidad máxima de la batería comunitaria (kWh).
#define CAPACIDAD_BATERIA 1000.0

// Umbral de energía residual para considerar una orden "completada".
#define UMBRAL 0.001

// Cantidad de ticks/horas que dura la simulación diaria.
#define HORAS_POR_DIA 24

// Tipos de nodo (columna tipo de la tabla NODOS).
#define TIPO_CONSUMIDOR "Consumidor"
#define TIPO_PROSUMIDOR "Prosumidor"
#define TIPO_BATERIA "Bateria"

// Perfiles de consumo (columna perfil_consumo de la tabla NODOS).
#define PERFIL_RESIDENCIAL "Residencial"
#define PERFIL_COMERCIAL "Comercial"
#define PERFIL_INDUSTRIAL "Industrial"

// Tipo de operación del procedimiento actualizar_saldo_y_lecturas.
#define OP_COMPRA "compra"
#define OP_VENTA "venta"

// Lecturas históricas que se escriben por transacción
// (una para el vendedor y otra para el comprador).
#define LECTURAS_POR_TRANSACCION 2

// Timestamp por defecto si el tick no trae fecha (fallback).
#define TS_TICK_DEFAULT "00"

// Flag de CLI que activa el modo depuración.
#define FLAG_DEBUG "--debug"

// ------------------------------------------------------------
// Defaults de configuración (los pisa src/util/config.ini).
// ------------------------------------------------------------
#define DB_PATH_DEFAULT "./ejemplo.db"
#define DATOS_DIR_DEFAULT "./datos"
#define SQL_PATH_DEFAULT "./sql/crear_esquema.sql"
#define FECHA_SIMULADA_DEFAULT "2026-06-01"
#define CONFIG_INI_PATH "./src/util/config.ini"

// Tamaño del buffer para armar el timestamp del tick.
#define BUFFER_TICK_TS 32

#endif // constantes_h
