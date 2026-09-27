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
ecogrid.h ──> constantes.hpp, GestionDatos.hpp, types.hpp, util/config.hpp
```
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
| `datos/ofertas_*.csv`   | Órdenes de cada tick (24 archivos con dato dummy)                                                              |

### 2.1 Jerarquía de nodos

`NodoRed` es **abstracta**, con los atributos `id`, `ubicacion`, `balanceEnergia` (kW actual) y `saldoCuenta` (créditos), y un único **método virtual puro** `virtual double calcularExcedente() const = 0;` 

Las tres subclases lo interpretan según el rol del nodo:

- **`NodoConsumidor`** — solo demanda: `calcularExcedente()` retorna siempre negativo o cero (0 si `balanceEnergia` es positivo, si no el propio
  `balanceEnergia`). No produce; su demanda real entra por las órdenes de compra del CSV.
- **`NodoProsumidor`** — produce y consume; su excedente es `produccion - consumo` (positivo si hay excedente).
- **`NodoAlmacenamiento`** — su excedente es la carga almacenada. Mantiene `balanceEnergia` sincronizado con la carga en `absorberEnergia` / `liberarEnergia`, que es su comportamiento propio.



> **Nota sobre `calcularExcedente()` y la base de datos.** `calcularExcedente()` hoy no tiene llamadores en runtime: `balanceEnergia` del objeto es estado en memoria que nunca se persiste. El `excedente_neto` que se guarda en `LECTURAS_HISTORICAS` lo calcula la capa de datos por transacción, independiente del atributo de la clase.

La subclase de cada nodo no se decide en el código sino leyendo la columna `tipo` de la tabla `NODOS` (`main.cpp: construirNodo`). Agregar un tipo de nodo es agregar una subclase y una rama en esa función.

Para identificar el rol de un nodo en runtime, `NodoRed` expone `virtual bool esBateria() const` (false por defecto), que `NodoAlmacenamiento`override a `true`. Así `main.cpp` localiza la batería por polimorfismo en vez de con `dynamic_cast`: recorre el mapa de nodos y toma el primero cuyo `esBateria()` devuelve verdadero.

### 2.2 El libro de órdenes y el matching

`GridManager` guarda las órdenes en dos mapas:

```cpp
using BidMap = std::map<double, std::queue<Orden>, std::greater<double>>;
using AskMap = std::map<double, std::queue<Orden>>;
```

`BidMap` ordena de mayor a menor (comprador con `std::greater`); `AskMap` de
menor a mayor (orden por defecto de `std::map`). El `queue` interno de cada
precio aporta el desempate FIFO: `contadorSecuencia` le da a cada orden un
número creciente y se compara cuando dos órdenes empatan en precio. Así el
criterio **precio-tiempo** del enunciado sale de la estructura de datos, sin
ninguna comparación explícita en el bucle.

El matching (`realizarMatching`) toma el mejor bid y el mejor ask; si el precio
de compra supera al de venta, cruza `min(kwh_bid, kwh_ask)` al **precio medio
de los dos precios cotizados**, que es la regla del enunciado (sección 5.2).

Dos detalles de la implementación:

- **Autoconsumo.** El matching ignora un bid cuyo nodo es el mismo que el ask.
  Un prosumidor que vende y compra en el mismo tick no puede comprarse a sí
  mismo.
- **Validación de saldo por software.** Cuando el comprador no alcanza a cubrir
  el monto (`kwh * precio_clearing`), la orden se descarta y se registra como
  demanda insatisfecha. El enunciado permite validar el saldo por software en
  lugar de depender solo del trigger ("Podría validarse por software si lo
  prefieren"); el trigger de la base queda como última red de seguridad.

### 2.3 La batería entra antes que el CSV

La especificación dice que la oferta de la batería "se inserta al inicio del
libro de ventas antes de procesar las del CSV" (sección 3.3). Eso no es
cosmético: como el desempate en el mismo precio es FIFO, insertarla primero le
da prioridad precio-tiempo a la energía almacenada. El caso 4 del enunciado
depende de esto.

Por eso la orquestración del tick vive en `GridManager::procesarTick` y no en
`main.cpp`:

```cpp
std::vector<TransaccionEnergia>
procesarTick(const std::vector<Orden> &ofertasCSV,
             NodoAlmacenamiento *bateria, double precioBaseHora) {
  if (bateria) insertarOfertaBateria(bateria->getId(),
                                     bateria->getCargaActual(), precioBaseHora);
  for (const auto &o : ofertasCSV) insertarOrden(o);
  ejecutarMatching();
  if (bateria) transferirExcedentesABateria(*bateria, precioBaseHora);
  return transaccionesDelTick;
}
```

`main.cpp` solo llama a eso y persiste el resultado.

### 2.4 RAII

El mapa de nodos es `std::map<int, std::unique_ptr<NodoRed>>`. No hay un solo
`delete` en el proyecto: los nodos se liberan solos al salir de `main`.
`NodoRed` tiene destructor virtual, así que destruir un `NodoRed*` que en
realidad es un `NodoAlmacenamiento` es correcto.

---

## 3. Persistencia

### 3.1 Un tick es atómico

`persistirTick` envuelve el tick entero en `BEGIN IMMEDIATE ... COMMIT`. Usa
`IMMEDIATE` y no `BEGIN` a propósito: `IMMEDIATE` toma el lock de escritura
desde el BEGIN, así que un conflicto de concurrencia aparece al **inicio** del
tick y no a mitad del lote de INSERTs, con el bloque a medio hacer.

Si algo falla —el trigger de saldo, un `CHECK`, una FK— se hace `ROLLBACK` del
bloque completo y no queda **nada** de ese tick. El programa no aborta:
registra el rechazo y sigue con el tick siguiente, como exige el caso 3 del
enunciado.

### 3.2 `actualizar_saldo_y_lecturas`: el procedimiento almacenado

El enunciado (sección 4.2) pide un procedimiento almacenado:

```sql
actualizar_saldo_y_lecturas(p_id_nodo, p_kwh, p_precio, p_tipo_operacion)
```

que actualice `saldo_cuenta` en `NODOS` e inserte en `LECTURAS_HISTORICAS`, y
que se ejecute después de cada transacción confirmada.

**SQLite no tiene procedimientos almacenados.** Se lo emula con una **función
SQL de aplicación** (`sqlite3_create_function`) con esa firma exacta, invocada
desde C++ con la misma forma que en Oracle:

```sql
SELECT actualizar_saldo_y_lecturas(?, ?, ?, ?);
```

El orden por transacción queda exactamente como lo pide el enunciado:

```sql
BEGIN IMMEDIATE;
  INSERT INTO TRANSACCIONES ...;              -- dispara trg_validar_saldo
  SELECT actualizar_saldo_y_lecturas(vendedor, ..., 'venta');
  SELECT actualizar_saldo_y_lecturas(comprador, ..., 'compra');
  -- ... por cada transacción
COMMIT;
```

**El INSERT va antes del ajuste de saldo, a propósito.** Así el trigger valida
contra el saldo ya descontado por las transacciones anteriores del mismo tick,
que es exactamente lo mismo que ve el motor de matching en memoria.

Es una adaptación, no una equivalencia: sigue siendo SQLite. Lo que se
conserva es la encapsulación (saldo y lectura detrás de un único punto de
entrada con la firma del enunciado) y el orden de operaciones.

Las lecturas históricas se escriben **una fila por operación** (una venta, una
compra), que es la granularidad que describe el enunciado ("se ejecutará
después de cada transacción confirmada"). Por eso en una corrida de 24 ticks
hay exactamente el doble de lecturas que de transacciones.

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

Valida contra el saldo **vivo**, no contra una foto del inicio del tick. Es lo
correcto: dentro de un mismo tick un nodo puede hacer varias compras y el
saldo tiene que descontarlas en orden.

---

## 4. Decisiones sobre el esquema

### 4.1 `CONFIG_TARIFAS` sigue los valores de los casos de aceptación

Cinco de las 24 horas están fijadas a los precios base que el enunciado usa en
los casos de aceptación:

| Hora | Precio base kWh |
| ---- | --------------- |
| 10   | 1,00            |
| 12   | 1,50            |
| 14   | 1,00            |
| 15   | 1,20            |
| 18   | 2,00            |

La de las 15:00 es la que condiciona el caso 4: si la tarifa fuera mayor que
la orden de compra (1,50), la batería no cruzaría con el comprador y el tick
15 no generaría ninguna transacción. El resto de la curva es una aproximación
con la misma forma (valle a la madrugada, pico a la noche). El esquema lo
advierte en un comentario.

### 4.2 La batería es el nodo 99

El enunciado usa el nodo 99 como batería comunitaria ("configurado en BD con
saldo ilimitado para simplificar"). `BATERIA` en `src/constantes.hpp` y la semilla del
esquema coinciden en ese id; ningún CSV la referencia, solo interviene por su
cuenta.

### 4.3 `excedente_neto` de la batería

Para el nodo de almacenamiento el excedente se calcula `consumo - produccion`
(lo que almacenó), y para el resto `produccion - consumo`. La batería no
produce, almacena.

### 4.4 `tick_hora` como timestamp simulado

`LECTURAS_HISTORICAS.tick_hora` se guarda como `<fecha_simulada> HH:00:00`
(por ejemplo `2026-06-01 15:00:00`), aproximando la columna `DATE` que pide el
enunciado. La fecha base se configura en `src/util/config.ini`.

---

## 5. Entrada y salida

### 5.1 CSV de órdenes

En cada tick se lee `datos/ofertas_HH.csv` con formato
`id_orden,lado,id_nodo,kwh,precio` (sección 6.1). Las filas malformadas se
omiten con un aviso en consola sin abortar la simulación.

### 5.2 Salidas

- **Consola**: log de cada transacción (nodos, kWh, precio), demanda
  insatisfecha, excedentes no absorbidos, y resumen del día.
- **Base de datos**: filas en `TRANSACCIONES` y `LECTURAS_HISTORICAS`, y
  saldos actualizados en `NODOS`, todo confirmado en un único bloque
  transaccional por tick.

---

## 6. Diagramas

`docs/diagrama_clases.puml` y `docs/diagrama_entidad_relacion.puml` son la
fuente de verdad (son texto: se versionan y se revisan en un diff). Los
`.png`/`.pdf` que pide el enunciado son artefactos de build:

```bash
sudo apt install default-jre graphviz     # y bajar plantuml.jar
./docs/render.sh                          # genera .png y .pdf
./docs/render.sh pdf                      # solo pdf
```

Si no se quiere instalar nada, el mismo PlantUML se renderiza pegando el
contenido del `.puml` en `plantuml.com/plantuml/uml/`. El script avisa y
sugiere esa alternativa si no encuentra `plantuml`.

> Nota: en la máquina donde se hizo esta entrega no había `plantuml` ni
> `graphviz`, así que los `.png`/`.pdf` **no** están generados. El enunciado
> los pide como entrega: hay que correr `./docs/render.sh` una vez en una
> máquina con esas herramientas.

---

## 7. Lo que no se resolvió, y por qué

### 7.1 Oracle + SOCI

El enunciado (sección 4.3) pide que la capa de datos use **Oracle** con la
librería **SOCI**. El proyecto usa SQLite.

No se implementó, y la decisión fue deliberada: no hay una instancia de Oracle
disponible acá, y **no hay forma de verificar el resultado**. Agregar un
backend SOCI sin poder compilarlo ni correrlo habría dejado código que no se
sabe si funciona y que rompería el `make` de cualquiera que no tenga SOCI
instalado. Eso es peor que no tenerlo.

Lo que sí quedó preparado para que el cambio sea acotado:

- `GestionDatos` es **la única** clase que incluye `sqlite3.h`. Todo el SQL está
  dentro de ella, así que la superficie a migrar está delimitada.
- El motor no sabe qué es SQL, así que no hay nada que migrar ahí.
- `actualizar_saldo_y_lecturas` ya tiene la firma exacta del procedimiento que
  pide el enunciado: en Oracle sería un `CREATE OR REPLACE PROCEDURE` y la
  llamada `SELECT actualizar_saldo_y_lecturas(...)` no cambia en C++.

Si se consigue acceso a una instancia Oracle, el trabajo es escribir un
`GestionDatosOracle` con la misma interfaz pública y elegirlo por configuración.

### 7.2 Otros puntos

- **`docs/` en PNG/PDF** — ver §6. Faltan los archivos renderizados.
- **Los CSV y las tarifas son datos de la cátedra.** Los CSVs de `datos/` se
  dejaron tal como estaban; las tarifas se fijaron solo en las 5 horas de los
  casos del enunciado (§4.1).
- **La carga de la batería no está en el esquema.** `NODOS` tiene
  `saldo_cuenta` pero no `carga_kwh` / `capacidad_kwh`: la batería vive en
  memoria durante el día y arranca en 0 en cada corrida. No se agregó porque
  el esquema del enunciado (sección 4.2) no la lista.

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
(1.001.805 créditos): el ledger del día no crea ni destruye dinero.