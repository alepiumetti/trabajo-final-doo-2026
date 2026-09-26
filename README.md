# EcoGrid

Sistema de energía P2P con nodos (consumidores, prosumidores y baterías), órdenes de compra/venta y matching de transacciones. Simula los 24 ticks (horas) de un día con subasta de doble continua, una batería comunitaria y persistencia transaccional en SQLite3.

## Requisitos

- `gcc` / `g++` (con `make`)
- `libsqlite3` (header `sqlite3.h` + librería `libsqlite3.so`)

Instalación según distribución:

```sh
# Arch / Manjaro
sudo pacman -S gcc make sqlite3

# Debian / Ubuntu
sudo apt install g++ make libsqlite3-dev
```

## Compilar

```sh
make
```

Genera el ejecutable `ecogrid` en la raíz del proyecto.

## Ejecutar

```sh
./ecogrid
```

o directamente:

```sh
make run
```

Cada ejecución borra `ejemplo.db` y arranca desde la semilla, así que la
simulación es siempre limpia y determinista (24 ticks, de `ofertas_00.csv`
a `ofertas_23.csv`).

### Modo depuración (paso a paso)

Con  el flag`--debug` el programa imprime cada paso de la simulación (matching,
persistencia, saldos, lecturas) y espera `Enter` para continuar:

```sh
./ecogrid --debug
```

## Limpiar artefactos

```sh
make clean
```

Borra los archivos objeto (`.o`) y el ejecutable `ecogrid`.

## Estructura de archivos

| Ruta                    | Descripción                                                                |
| ----------------------- | -------------------------------------------------------------------------- |
| `src/main.cpp`          | Punto de entrada del programa                                              |
| `src/GestionDatos.hpp`  | Capa de datos (`CapaDatos`): esquema, transaccionalidad por tick, lecturas |
| `src/types.hpp`         | Tipos del dominio (nodos, órdenes, `GridManager`)                          |
| `src/util/config.hpp`   | Lector de `config.ini`                                                     |
| `src/util/config.ini`   | Configuración (ruta BD, datos, script SQL, fecha simulada)                 |
| `src/ecogrid.h`         | Cabecera paraguas: una línea para usar todo el dominio                     |
| `sql/crear_esquema.sql` | Esquema de la BD: tablas, seed, trigger                                    |
| `docs/`                 | Informe técnico y diagramas UML/DER (`.puml` + render)                     |
| `datos/ofertas_*.csv`   | Ofertas de cada tick (24 archivos)                                         |
| `ejemplo.db`            | Base de datos SQLite3 generada al ejecutar                                 |