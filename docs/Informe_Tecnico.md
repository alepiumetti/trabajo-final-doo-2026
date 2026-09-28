# EcoGrid — Informe técnico

**Trabajo Práctico Integrador · Programación Orientada a Objetos · 2026**

Simulador de red eléctrica, con nodos consumidores, prosumidores y batería que se compran y venden a traves de un matching persistiendo las transacciones en una base de datos SQLite3.

---

## 1. Cómo compilar y correr

```bash
make            
make run        # o directamente: ./ecogrid
./ecogrid       # corre los 24 ticks desde la semilla (borra ejemplo.db antes)
./ecogrid --debug   # imprime cada paso y espera Enter (modo paso a paso)
```

Cada ejecución borra `ejemplo.db` y arranca del estado inicial de la semilla: la simulación siempre es limpia.

Requisitos: g++ con `-std=c++17` y `libsqlite3-dev`. Verificado con g++ 13.3.0
y SQLite 3.45.1. 

Se decidió crear un archivo Makefile para agilizar la compilación para pruebas repetidas que se debieron hacer, facilitando ejecutar la compilación completa con un solo comando, además de su ejecución también con un comando único.

---

## 2. Arquitectura

`main.cpp` contiene toda la simulación completa y consume de los otros `.hpp` permitiendo mejor modularización del código.

Además, con el archivo `ecogrid.h` como dependencia paraguas, desde `main.cpp`  solo es necesario importar a éste. 

```
   main.cpp  (SimuladorEcoGrid)
        │
        ├──► GridManager  ── motor de subasta.  NO sabe qué es SQL.
        │
        └──► GestionDatos   ── la única clase que habla SQLite.
```

```
ecogrid.h ──> constantes.hpp, GestionDatos.hpp, types.hpp, util/config.hpp
```

`GridManager` recibe el saldo de los nodos por callback en vez de ir a buscarlo, y la capa de datos se inyecta desde `main.cpp`. 

> Se decidió mantener la lógica de datos aparte para permitirnos trabajar por separado, por un lado la gestión de los datos y por otro lado la lógica del simulador, facilitandonos la migración de SOCI+Oracle a SQLite3 y la división de tareas en el equipo.

**Los archivos**:

| Archivo                 | Contenido                                                                                                      |
| ----------------------- | -------------------------------------------------------------------------------------------------------------- |
| `src/types.hpp`         | `NodoRed` y sus tres subclases, `Orden`, `TransaccionEnergia`, `GridManager` junto a la lógica de sus métodos. |
| `src/constantes.hpp`    | Constantes y defaults de configuración                                                                         |
| `src/GestionDatos.hpp`  | Persistencia de datos con SQLite3                                                                              |
| `src/util/config.hpp`   | Lectura de `config.ini` y armado del timestamp simulado                                                        |
| `src/main.cpp`          | Cableado de las capas y ciclo del día                                                                          |
| `src/ecogrid.h`         | Cabecera paraguas.                                                                                             |
| `sql/crear_esquema.sql` | Esquema, semilla, tarifa y trigger                                                                             |
| `datos/ofertas_*.csv`   | Órdenes de cada tick (24 archivos con datos dummy)                                                             |

### 2.1 Jerarquía de nodos

`NodoRed` es **abstracta**, con los atributos `id`, `ubicacion`, `balanceEnergia` (kW actual) y `saldoCuenta` (créditos), y un único **método virtual puro** `virtual double calcularExcedente() const = 0;` 

Las tres subclases lo interpretan según el rol del nodo:

- **`NodoConsumidor`** — solo demanda: `calcularExcedente()` retorna siempre negativo o cero (0 si `balanceEnergia` es positivo, si no el propio `balanceEnergia`). No produce; su demanda real entra por las órdenes de compra del CSV.
- **`NodoProsumidor`** — produce y consume; su excedente es `produccion - consumo` (positivo si hay excedente).
- **`NodoAlmacenamiento`** — su excedente es la carga almacenada. Mantiene `balanceEnergia` sincronizado con la carga en `absorberEnergia` / `liberarEnergia`, que es su comportamiento propio.

> **Nota sobre `calcularExcedente()` y la base de datos.** `calcularExcedente()` hoy no tiene llamadores en runtime: `balanceEnergia` del objeto es estado en memoria que nunca se persiste. El `excedente_neto` que se guarda en `LECTURAS_HISTORICAS` lo calcula la capa de datos por transacción, independiente del atributo de la clase.

La subclase de cada nodo no se decide en el código sino leyendo la columna `tipo` de la tabla `NODOS` (`main.cpp: construirNodo`). 

Para identificar el rol de un nodo en runtime, `NodoRed` expone `virtual bool esBateria()` (false por defecto), que `NodoAlmacenamiento` sobreescribe a `true`, así `main.cpp` localiza la batería por polimorfismo: recorre el mapa de nodos y toma el primero cuyo `esBateria()` devuelve verdadero.

### 2.2 El libro de órdenes y el matching

`GridManager` guarda las órdenes en dos mapas:

```cpp
using BidMap = std::map<double, std::queue<Orden>, std::greater<double>>;
using AskMap = std::map<double, std::queue<Orden>>;
```

`BidMap` ordena de mayor a menor (comprador con `std::greater`); `AskMap` de menor a mayor (orden por defecto de `std::map`). El `queue` interno de cada precio aporta el desempate FIFO: las órdenes se emparejan en el orden en que se insertaron, sin comparación explícita entre órdenes. `contadorSecuencia` solo asigna un número creciente a cada orden (no interviene en la comparación). Así el criterio **precio-tiempo** sale de la estructura de datos en sí, sin ninguna regla adicional en el bucle.

El matching (`realizarMatching`) toma el mejor bid y el mejor ask; si el precio de compra supera al de venta, cruza `min(kwh_bid, kwh_ask)` al **precio medio de los dos precios cotizados**.

Dos detalles de la implementación:

- **Autoconsumo.** El matching ignora un bid cuyo nodo es el mismo que el ask. Un prosumidor que vende y compra en el mismo tick no puede comprarse a sí mismo.
- **Validación de saldo por software.** Cuando el comprador no alcanza a cubrir el monto (`kwh * precio_clearing`), la orden se descarta y se registra como demanda insatisfecha. Esta validación es una capa extra al trigger que realiza la base de datos teniendo una doble validación.

### 2.3 La batería entra antes que el CSV

Se ingresa la batería al inicio para darle prioridad precio-tiempo a la orden de la energía almacenada, ya que el desempate en el mismo precio es por orden FIFO.

Por eso la orquestración del tick vive en `GridManager::procesarTick` y no en `main.cpp`:

```cpp
std::vector<TransaccionEnergia>
procesarTick(const std::vector<Orden> &ofertasCSV,
             NodoAlmacenamiento *bateria, double precioBaseHora) {
  if (bateria) insertarOfertaBateria(bateria->getId(),
                                     bateria->getCargaActual(), precioBaseHora);
  for (const auto &oferta : ofertasCSV) insertarOrden(oferta);
  ejecutarMatching();
  if (bateria) transferirExcedentesABateria(*bateria, precioBaseHora);
  return transaccionesDelTick;
}
```

`main.cpp` solo llama a eso y persiste el resultado.

### 2.4 Gestión de nodos

El mapa de nodos es `std::map<int, std::unique_ptr<NodoRed>>`, esto es un puntero inteligente por lo que no se necesita un `delete` en el proyecto: los nodos se liberan solos al salir del scope `main` . De todos modos, al final del `main.cpp` hay una función para limpiar los nodos `nodos.clear()`.

---

## 3. Persistencia

### 3.1 Un tick es atómico

`persistirTick` envuelve el tick entero en `BEGIN IMMEDIATE ... COMMIT`. Usa `IMMEDIATE` y no `BEGIN` a propósito: `IMMEDIATE` toma el lock de escritura desde el `BEGIN`, así que un conflicto de concurrencia aparece al **inicio** del tick y no a mitad del lote de INSERTs, con el bloque a medio hacer. 

Si algo falla —el trigger de saldo, un `CHECK`, una FK— se hace `ROLLBACK` del bloque completo y no queda **nada** de ese tick. El programa no aborta: registra el rechazo y  sigue con el tick siguiente.

### 3.2 `actualizar_saldo_y_lecturas`: el procedimiento almacenado

En base al requerimiento se creó una función con la firma:

```sql
actualizar_saldo_y_lecturas(p_id_nodo, p_kwh, p_precio, p_tipo_operacion)
```

que actualice `saldo_cuenta` en `NODOS` e inserte en `LECTURAS_HISTORICAS` y que se ejecute después de cada transacción confirmada.

**SQLite no tiene procedimientos almacenados.** Se lo emula con una **función SQL de aplicación** (`sqlite3_create_function`) con esa firma exacta, invocada desde C++ con la misma forma que en Oracle:

```sql
SELECT actualizar_saldo_y_lecturas(?, ?, ?, ?);
```

El orden por transacción queda así:

```sql
BEGIN IMMEDIATE;
  INSERT INTO TRANSACCIONES ...;              -- dispara trg_validar_saldo
  SELECT actualizar_saldo_y_lecturas(vendedor, ..., 'venta');
  SELECT actualizar_saldo_y_lecturas(comprador, ..., 'compra');
  -- ... por cada transacción
COMMIT;
```

**El INSERT va antes del ajuste de saldo, a propósito.** Así el trigger valida contra el saldo ya descontado por las transacciones anteriores del mismo tick, que es exactamente lo mismo que ve el motor de matching en memoria.

Es una adaptación, no una equivalencia: sigue siendo SQLite. Lo que se conserva es el encapsulamiento (saldo y lectura detrás de un único punto de entrada con la firma pedida) y el orden de operaciones.

Las lecturas históricas se escriben **una fila por operación** (una venta, una compra), ejecutándose después de cada transacción confirmada. Por eso en una corrida de 24 ticks hay exactamente el doble de lecturas que de transacciones.

### 3.3 El trigger `trg_validar_saldo`

```sql
CREATE TRIGGER trg_validar_saldo BEFORE INSERT ON TRANSACCIONES
FOR EACH ROW
BEGIN
  SELECT CASE
    WHEN (SELECT saldo_cuenta FROM NODOS WHERE id_nodo = NEW.id_comprador)
         < (NEW.kwh * NEW.precio_unitario)
    THEN RAISE(ABORT, 'Saldo insuficiente para realizar la compra')
  END;
END;
```

Valida contra el saldo **vivo**, no contra una foto del inicio del tick. Es lo correcto: dentro de un mismo tick un nodo puede hacer varias compras y el saldo tiene que descontarlas en orden.

---

## 4. Decisiones sobre el esquema

### 4.1 `CONFIG_TARIFAS` fija cinco horas a valores puntuales

Cinco de las 24 horas están fijadas a valores puntuales de precio base:

| Hora | Precio base kWh |
| ---- | --------------- |
| 10   | 1,00            |
| 12   | 1,50            |
| 14   | 1,00            |
| 15   | 1,20            |
| 18   | 2,00            |

La de las 15:00 es la que condiciona el cruce de la batería: si la tarifa fuera mayor que la orden de compra (1,50), la batería no cruzaría con el comprador y el tick 15 no generaría ninguna transacción.

### 4.2 Nodo 99 - Batería

La batería comunitaria usa el nodo **99** configurado en BD con saldo "ilimitado" para simplificar. `BATERIA` en `src/constantes.hpp` y la semilla del esquema coinciden en ese id. La batería no se referencia en ningún CSV, solo interviene por su cuenta.

### 4.3 `excedente_neto` de la batería

Para el nodo de almacenamiento el excedente se calcula `consumo - produccion` (lo que almacenó) y para el resto `produccion - consumo`. La batería no produce, solo almacena.

### 4.4 `tick_hora` como timestamp simulado

`LECTURAS_HISTORICAS.tick_hora` se guarda como `<fecha_simulada> HH:00:00` (por ejemplo `2026-06-01 15:00:00`), como aproximación de una columna `DATE`. La fecha base se configura en `src/util/config.ini`.

---

## 5. Entrada y salida

### 5.1 CSV de órdenes

En cada tick se lee `datos/ofertas_HH.csv` con formato `id_orden,lado,id_nodo,kwh,precio`. Las filas malformadas se omiten con un aviso en  consola sin abortar la simulación.

### 5.2 Salidas

- **Consola**: log de cada transacción (nodos, kWh, precio), demanda insatisfecha, excedentes no absorbidos, y resumen del día.
- **Base de datos**: filas en `TRANSACCIONES` y `LECTURAS_HISTORICAS`, y saldos actualizados en `NODOS`, todo confirmado en un único bloque transaccional por tick.

### 5.3 Diagramas

Se incluyen el diagrama de clases (`docs/Diagrama de Clases UML`) y el diagrama entidad-relación (`docs/Diagrama Entidad-Relacion.png`).

---

## 6. Lo que no se resolvió, y por qué

### 6.1 Oracle + SOCI

La capa de datos debía usar **Oracle** con la librería **SOCI**. El proyecto usa SQLite3.

El uso de Oracle y SOCI requería una instancia con la cuál el equipo no contaba por lo que se desestimó para facilitar el desarrollo del programa.

Lo que sí quedó preparado para que el cambio sea acotado:

- `GestionDatos` es **la única** clase que incluye `sqlite3.h`. Todo el SQL está dentro de ella, así que la superficie a migrar está delimitada.
- `actualizar_saldo_y_lecturas` ya tiene la firma exacta del procedimiento pedido: en Oracle sería un `CREATE OR REPLACE PROCEDURE` y la llamada `SELECT actualizar_saldo_y_lecturas(...)` no cambia en C++.

En el caso de querer hacer una migración para usar Oracle y SOCI, se debe escribir un módulo `GestionDatosOracle` con la misma interfaz pública y elegirlo por configuración.

### 6.2 Otros puntos

- **Los CSV y las tarifas son datos dummy.** Los CSVs de `datos/` se
  dejaron tal como estaban; las tarifas se fijaron solo en las 5 horas con
  valores puntuales.
- **La carga de la batería no está en el esquema.** `NODOS` tiene
  `saldo_cuenta` pero no `carga_kwh` / `capacidad_kwh`: la batería vive en
  memoria durante el día y arranca en 0 en cada corrida. No se agregó porque
  el esquema no la lista.

---

## 8. Resultado de una corrida

```
$ ./ecogrid
Esquema y trigger creados correctamente.
 ... 24 ticks ...
Transacciones persistidas: 87
Energía transada: 844.7 kWh
Lecturas históricas: 174
Simulación finalizada.
```

Los saldos de los 15 nodos cierran exactamente con el total inicial
(1.001.805 créditos): el balance del día no crea ni destruye dinero.

---

## 9. Distribución de tareas y conclusiones

Se partió de la base de código propuesta por la consigna. A partir de la descripción de los nodos, se generaron en conjunto la clase types y los Nodos. Luego, Alejandro se encargó principalmente de la configuración general del proyecto y de la capa de datos. Mientras que Augusto se ocupó del GridManager y del funcionamiento general del main. En una segunda instancia de revisión y corrección del código, el límite de la separación de tareas se desdibujó y cada uno pudo interactuar y corregir el trabajo del otro.

El mayor desafío del proyecto fue interpretar la lógica de negocios e integrar el pseudocódigo provisto en las consignas al funcionamiento general. Se evaluaron múltiples enfoques, llegando a este resultado final priorizando la simplicidad y la síntesis.