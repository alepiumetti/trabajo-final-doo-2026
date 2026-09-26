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

### Empezar de cero

La base `ejemplo.db` no se limpia entre ejecuciones: `NODOS` se completa con
`INSERT OR IGNORE` y `TRANSACCIONES` sólo crece, así que las corridas se
acumulan. Para simular siempre las 24 horas desde el estado inicial:

```sh
./ecogrid --reset
```

Sin `--reset` la corrida arranca desde los saldos que quedaron de la corrida
anterior y sus filas se apilan al final de la tabla. Para ver sólo las últimas
`n` filas (las de la corrida más reciente):

```sh
sqlite3 ejemplo.db "SELECT * FROM TRANSACCIONES ORDER BY id_transaccion DESC LIMIT 87;"
```

No conviene filtrar por `fecha_transaccion`: tiene resolución de un segundo
(`CURRENT_TIMESTAMP`) y la simulación entera corre en milisegundos, así que
varias corridas pueden compartir timestamp. `--reset` es la forma simple de
tener siempre una base que corresponde exactamente a una corrida.

### Provocar un rollback (trigger de prueba)

`trg_validar_saldo` es la última red de seguridad y con los CSV de fábrica
*nunca* llega a dispararse: el motor de matching ya rechaza en memoria toda
compra que el saldo no cubre, con el mismo predicado sobre el mismo saldo.
Para poder ejercitar el `ROLLBACK` de punta a punta hace falta una regla más
estricta, y eso es lo que instala `--trigger-prueba`: aborta toda compra
individual de más de 30 créditos.

```sh
./ecogrid --reset --trigger-prueba
```

Los ticks rechazados se ven así, y no dejan ni una fila en la base:

```text
No se pudo insertar la transacción: Trigger de prueba: compra individual de 30.000000 creditos o mas
[Rollback] Transacciones del tick rechazadas: Trigger: insertando la transacción: ...
[Tick 17] Tick rechazado: ROLLBACK aplicado, saldos y carga de batería restaurados.
```

El prefijo `Trigger:` / `CHECK:` / `FK:` del rollback dice qué restricción
rechazó el tick. El trigger de prueba se borra solo en la corrida siguiente
sin el flag, así que no queda contaminando `ejemplo.db`.

## Pruebas

```sh
make test
```

Corre 58 pruebas sin tocar `ejemplo.db`:

- `build/test_motor` — 31 pruebas del motor de subasta contra los cuatro casos
  de aceptación del enunciado y el ejemplo numérico de la sección 5.3, más las
  reglas de validación de órdenes. **No enlaza contra SQLite**: el motor no
  depende de la base, y que las pruebas compilen lo demuestran.
- `build/test_datos` — 27 pruebas de la capa de datos contra el esquema real
  de `sql/crear_esquema.sql`, sobre una base temporal (`test_datos.db`) que se
  borra al terminar. Cubren el trigger de saldo, los `CHECK`, las FK, la
  atomicidad del tick y el procedimiento `actualizar_saldo_y_lecturas`.

El detalle de la arquitectura, las decisiones de diseño y los puntos que
quedaron fuera están en [`docs/Informe_Tecnico.md`](docs/Informe_Tecnico.md).

## Diagramas

Las fuentes PlantUML están en `docs/`. Para generar los `.png` y los `.pdf`
hace falta `plantuml` y `graphviz`:

```sh
./docs/render.sh          # png + pdf
./docs/render.sh pdf      # solo pdf
```

## Limpiar artefactos

```sh
make clean
```

Borra los archivos objeto (`.o`), el ejecutable `ecogrid`, los binarios de
las pruebas y la base temporal que usan.

## Estructura de archivos

| Ruta | Descripción |
|---|---|
| `src/main.cpp` | Punto de entrada del programa |
| `src/GestionDatos.hpp` | Capa de datos: esquema, transaccionalidad por tick, lecturas |
| `src/types.hpp` | Tipos del dominio (nodos, órdenes, `GridManager`) |
| `src/util/config.hpp` | Lector de `config.ini` |
| `src/util/config.ini` | Configuración (ruta BD, datos, script SQL) |
| `sql/crear_esquema.sql` | Esquema de la BD: tablas, seed, trigger |
| `src/ecogrid.h` | Cabecera paraguas: una línea para usar todo el dominio |
| `tests/` | Pruebas automáticas (`test_motor.cpp`, `test_datos.cpp`) |
| `docs/` | Informe técnico y diagramas UML/DER (`.puml` + render) |
| `datos/ofertas_*.csv` | Ofertas de cada tick (24 archivos) |
| `ejemplo.db` | Base de datos SQLite3 generada al ejecutar |