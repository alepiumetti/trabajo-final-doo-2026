#ifndef ecogrid_h
#define ecogrid_h

// Cabecera principal de EcoGrid. El PDF (sección 7) pide que el proyecto
// tenga un punto de entrada único que junte las tres capas, para que un
// consumidor externo (o una prueba) incluya una sola línea y tenga todo el
// dominio disponible:
//
//   #include "ecogrid.h"
//
// Las capas son:
//
//   types.hpp       dominio: NodoRed y sus subclases, Orden, GridManager
//                   (el motor de subasta). No sabe nada de SQL.
//   GestionDatos.hpp  persistencia: CapaDatos. Es la única capa que habla
//                   SQLite.
//   util/config.hpp   lectura de src/util/config.ini.
//
// El motor no depende de la base de datos: se le inyectan callbacks
// (consultarSaldo / actualizarSaldo / nodoConocido), así que se puede
// probar sin levantar ninguna base. Eso es lo que hace tests/test_motor.cpp.

#include "GestionDatos.hpp"
#include "types.hpp"
#include "util/config.hpp"

#endif // ecogrid_h
