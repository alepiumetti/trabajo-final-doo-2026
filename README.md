# EcoGrid

Sistema de energía P2P con nodos (consumidores, prosumidores y baterías), órdenes de compra/venta y matching de transacciones.

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

### Modo depuración (paso a paso)

Con `--debug` el programa imprime cada paso de la simulación (matching,
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

| Ruta | Descripción |
|---|---|
| `src/main.cpp` | Punto de entrada del programa |
| `src/GestionDatos.hpp` | Capa de datos: esquema, transaccionalidad por tick, lecturas |
| `src/types.hpp` | Tipos del dominio (nodos, órdenes, `GridManager`) |
| `src/util/config.hpp` | Lector de `config.ini` |
| `src/util/config.ini` | Configuración (ruta BD, datos, script SQL) |
| `sql/crear_esquema.sql` | Esquema de la BD: tablas, seed, trigger |
| `docs/` | Diagramas UML/DER e informe técnico (pendientes) |
| `datos/ofertas_*.csv` | Ofertas de cada tick (24 archivos) |
| `ejemplo.db` | Base de datos SQLite3 generada al ejecutar |