# Módulos importados en EcoGrid

Seguimiento de las librerías y módulos que se importan en el proyecto:
qué proveen, por qué se usan y dónde. Revisado el 2026-09-26.

## 1. src/main.cpp (entrada principal)

| Import         | Provee                                   | Uso                                                     |
| -------------- | ---------------------------------------- | ------------------------------------------------------- |
| `<cstdio>`     | `std::remove()`                          | Borra `ejemplo.db` en cada corrida para partir de la semilla (`main.cpp:51`) |
| `<iostream>`   | `std::cout` / `std::cerr`                | Logs de tick, batería, resumen y errores (`:60,109,197,204`) |
| `<map>`        | `std::map`                               | `std::map<int, std::unique_ptr<NodoRed>> nodos` (`:65`) |
| `<memory>`     | `std::unique_ptr`, `std::make_unique`    | Dueño de los nodos; liberación automática sin `delete` (`:21-37,65,195`) |
| `<string>`     | `std::string`, `std::to_string`          | Flags, rutas de CSV, logs (`:44,114-116`)               |
| `<vector>`     | `std::vector`                            | Órdenes del CSV y transacciones del tick (`:125,148`)   |

### Módulos propios

| Import           | Provee                                                   |
| ---------------- | -------------------------------------------------------- |
| `GestionDatos.hpp` | `GestionDatos`: persistencia SQLite, CSV, tarifas |
| `types.hpp`        | `NodoRed` y subclases, `GridManager`, `Orden`, `TransaccionEnergia` |
| `util/config.hpp`  | `Config`, `cargarConfig()`, `timestampTick()`            |

> **Pendiente**: el bloque `#include <stdexcept>` quedó dentro de un conflicto
> de merge sin resolver (`main.cpp:5-8`). El código usa `std::exception` en el
> `catch` (`:203`); hoy funciona de forma transitiva por `GestionDatos.hpp`,
> pero conviene incluirlo explícitamente.

## 2. src/types.hpp (dominio)

| Import       | Provee                                          | Uso                                                       |
| ------------ | ----------------------------------------------- | --------------------------------------------------------- |
| `<chrono>`   | `std::chrono::system_clock::time_point`         | Timestamp real de `TransaccionEnergia` (`:165,172`)       |
| `<functional>`| `std::function<...>`                           | Callbacks de saldo/log en `GridManager`; desacopla el motor de la BD (`:201,208-209`) |
| `<queue>`    | `std::queue<Orden>`                             | FIFO por precio en el libro de órdenes (`:181-183`)       |
| `<utility>`  | `std::move`, `std::pair`                        | Evitar copias de `std::string` en constructores (`:51,77,102,123`) |
| `<cstdint>`  | `uint64_t`                                      | Contador FIFO `secuencia` (`:27,190`)                     |
| `<iostream>` | `std::cout`                                     | Logs del motor de subasta (`:213-215,274,...`)            |
| `<map>`      | `std::map`                                      | `bidMap` (compra, mayor precio primero) y `askMap` (venta, menor) (`:181-182,459-461`) |
| `<string>`   | `std::string`, `std::to_string`                 | Ids, logs, `NodoAlmacenamiento::estado()` (`:134-139`)    |
| `<vector>`   | `std::vector`                                   | `transaccionesDelTick` (`:193,281,410`)                   |
| `<unistd.h>` | `isatty`, `STDIN_FILENO`                        | Pausa "Enter" solo si stdin es terminal, en modo debug (`:213`) |

## 3. src/GestionDatos.hpp (persistencia)

| Import         | Provee                                   | Uso                                                       |
| -------------- | ---------------------------------------- | --------------------------------------------------------- |
| `<fstream>`    | `std::ifstream`                          | Script SQL (`:290`) y CSV (`:463`)                        |
| `<iostream>`   | `std::cout` / `std::cerr`                | Errores y debug                                           |
| `<sqlite3.h>`  | API completa de SQLite3                  | `open`, `create_function` (procedimiento almacenado), `prepare_v2`/`step`/`bind`, `exec` para BEGIN/COMMIT/ROLLBACK (`:224,267,309-311,389,419,446`) |
| `<sstream>`    | `std::stringstream`                      | Buffer del esquema SQL (`:296`) y split de CSV (`:479`)   |
| `<stdexcept>`  | `std::runtime_error`, `std::exception`   | Errores fatales al abrir BD/script (`:270,292`)           |
| `<string>`     | `std::string`, `std::to_string`          | Mensajes, timestamps, CSV                                 |
| `<unistd.h>`   | `isatty`, `STDIN_FILENO`                 | Pausa "Enter" en modo debug (`:60`)                       |
| `<vector>`     | `std::vector`                            | `DatoNodo` (`:383`) y `Orden` (`:460`)                    |

### Módulos propios

| Import            | Provee                                                          |
| ----------------- | --------------------------------------------------------------- |
| `types.hpp`       | `Orden`, `TransaccionEnergia`, `BATERIA`                        |
| `util/config.hpp` | `Config`                                                        |

> Se quitaron `<cmath>` y `<map>`: no se usaban en este archivo
> (revisión 2026-09-26).

## 4. src/util/config.hpp

| Import     | Provee                  | Uso                                                  |
| ---------- | ----------------------- | ---------------------------------------------------- |
| `<cstdio>` | `std::snprintf`         | Formateo `"%s %02d:00:00"` del timestamp del tick (`:20`) |
| `<fstream>`| `std::ifstream`         | Lectura de `config.ini` (`:27`)                      |
| `<string>` | `std::string` y métodos | Parseo `clave=valor` (`:33-56`)                      |

## 5. src/ecogrid.h (cabecera paraguas)

No importa librerías: agrega los tres headers del proyecto
(`GestionDatos.hpp`, `types.hpp`, `util/config.hpp`) para que un consumidor
externo incluya una sola línea y tenga todo el dominio (requisito del PDF,
sección 7).

## 6. tests/test_datos.cpp

| Import      | Provee                                | Uso                                                  |
| ----------- | ------------------------------------- | ---------------------------------------------------- |
| `<cmath>`   | `std::fabs`                           | Comparación de doubles con tolerancia (`:37`)        |
| `<cstdio>`  | `std::fopen`/`fputs`/`fclose`, `std::remove` | CSV temporales (`:59,259-276`)                  |
| `<iomanip>` | `std::fixed`, `std::setprecision`     | Formato de salida (`:55`)                            |
| `<iostream>`| `std::cout`                           | Resultados de pruebas                                |
| `<map>`     | `std::map<int,double>`                | Mapa de saldos de semilla (`:42`)                    |
| `<string>`  | `std::string`                         | Nombres de tests, mensajes                           |
| `<vector>`  | `std::vector`                         | Datos de prueba                                      |

### Módulos propios

| Import            | Provee                                    |
| ----------------- | ----------------------------------------- |
| `GestionDatos.hpp`| `GestionDatos` (persistencia bajo prueba)    |
| `types.hpp`       | `TransaccionEnergia`, `BATERIA`           |

> **Pendiente**: los tests están desincronizados con la API actual.
> Usan `g.persistirTick(txns, saldosSemilla(...), tick)` (firma con 3
> argumentos) y `GestionDatos::leerCSVConInforme` (no existe; hoy la lectura de
> CSV es `leerCSV` que devuelve `std::vector<Orden>`). Parecen escritos contra
> la otra rama del merge pendiente. El `Makefile` tampoco tiene target `test`.

## Grafo de dependencias entre módulos propios

```
main.cpp ──> GestionDatos.hpp ──> types.hpp
   │                │                └──> (sin dependencias propias)
   │                └──> util/config.hpp
   └──> types.hpp
ecogrid.h ──> GestionDatos.hpp, types.hpp, util/config.hpp
test_datos.cpp ──> GestionDatos.hpp, types.hpp
```

- `types.hpp` (dominio) no depende de nada propio: es la base.
- `GestionDatos.hpp` (persistencia) depende del dominio y de la config.
- `main.cpp` orquesta las tres capas.
- `ecogrid.h` es un agregador: no agrega lógica.

## Pendientes detectados en la revisión

1. **Conflictos de merge sin resolver** que rompen la compilación
   (marcadores `<<<<<<< HEAD` en el código):
   - `src/main.cpp:5-8` — `#include <stdexcept>`
   - `src/types.hpp:433-451` — `libroVacio()`/`getTransacciones()` vs `reevaluarPendientesPorSaldo()`
   - `src/GestionDatos.hpp:98-119` — comentario del procedimiento vs `udfError`
   - `src/GestionDatos.hpp:507-511` — `return filas;` vs `return informe;` (la rama `informe` ni siquiera está declarada)
2. **Tests desincronizados** con la API actual (ver sección 6).
3. **`Makefile` sin target `test`** aunque `test_datos.cpp` documenta `make test`.