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

## Limpiar artefactos

```sh
make clean
```

Borra los archivos objeto (`.o`) y el ejecutable `ecogrid`.

## Estructura de archivos

| Archivo | Descripción |
|---|---|
| `main.cpp` | Punto de entrada del programa |
| `GestionDatos.hpp` | Creación de tablas SQLite3 y lectura de CSV |
| `types.hpp` | Tipos del dominio (nodos, órdenes, `GridManager`) |
| `datos/ofertas_01.csv` | Datos de ofertas |
| `ejemplo.db` | Base de datos SQLite3 generada al ejecutar |