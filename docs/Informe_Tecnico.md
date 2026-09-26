# EcoGrid — Informe técnico

**Trabajo Práctico Integrador · Programación Orientada a Objetos · 2026**

Simulador de una red eléctrica community energy market: 24 ticks (uno por
hora), subasta de doble continua para casar órdenes de compra y venta, y una
batería comunitaria que compra los excedentes sin comprador y los revende en
las horas caras.

---

## 1. Cómo compilar y correr

```bash
make          # compila ./ecogrid
make test     # corre las 58 pruebas (no toca ejemplo.db)
make run      # simula sobre la base existente
./ecogrid --reset          # borra la base y corre los 24 ticks desde cero
./ecogrid --trigger-prueba # instala un trigger extra para ver un ROLLBACK
./ecogrid --help
```

Requisitos: g++ con `-std=c++17`, `libsqlite3-dev`, graphviz + plantuml
(solo para renderizar los diagramas, ver §7).

Verificado con g++ 13.3.0 y SQLite 3.45.1. La compilación es limpia: cero
warnings con `-Wall -Wextra`.

---

## 2. Arquitectura

Tres capas, con una regla de dependencia de una sola dirección:

```
   main.cpp  (SimuladorEcoGrid)
        │
        ├──► GridManager  ── motor de subasta.  NO sabe qué es SQL.
        │
        └──► CapaDatos   ── la única clase que habla SQLite.
```

**El motor no depende de la base de datos.** `GridManager` recibe el saldo de
los nodos por callback (`consultarSaldo`, `actualizarSaldo`, `nodoConocido`)
en vez de ir a buscarlo. Consecuencia práctica: `make test` corre 31 pruebas
del motor **sin abrir ninguna base**, y `build/test_motor` ni siquiera
enlaza contra `-lsqlite3`. Si alguna vez alguien mete una consulta SQL
adentro del matching, el Makefile lo detecta al compilar las pruebas.

Los archivos:

| Archivo | Contenido |
|---|---|
| `src/types.hpp` | Dominio: `NodoRed` y sus tres subclases, `Orden`, `TransaccionEnergia`, `GridManager` |
| `src/GestionDatos.hpp` | Persistencia: `CapaDatos` (alias de `gestionDatos`) |
| `src/util/config.hpp` | Lectura de `config.ini` y armado del timestamp simulado |
| `src/main.cpp` | Cableado de las capas y ciclo del día |
| `src/ecogrid.h` | Cabecera paraguas (una sola línea para usar todo el dominio) |
| `sql/crear_esquema.sql` | Esquema, semilla, tarifa y trigger |
| `tests/` | 58 pruebas: `test_motor.cpp` (31) y `test_datos.cpp` (27) |

### 2.1 Jerarquía de nodos

`NodoRed` es abstracta con un único método `calcularExcedente()`. Las tres
subclases lo interpretan según el rol del nodo:

- **`NodoConsumidor`** — excedente = producción − consumo. Nunca vende.
- **`NodoProsumidor`** — puede comprar y vender en el mismo tick.
- **`NodoAlmacenamiento`** — su excedente es la carga almacenada. Es el
 caso especial que rompe la fórmula común, y por eso tiene su propia
  implementación en vez de heredarla.

La subclase de cada nodo no se decide en el código sino leyendo la columna
`tipo` de la tabla `NODOS` (`main.cpp: construirNodo`). Agregar un tipo de nodo
es agregar una subclase y una rama en esa función.

### 2.2 El libro de órdenes y el matching

`GridManager` guarda las órdenes en dos mapas:

```cpp
using BidMap = std::map<double, std::queue<Orden>, std::greater<double>>;
using AskMap = std::map<double, std::queue<Orden>>;
```

`BidMap` ordena de mayor a menor (comprador `std::greater`); `AskMap` de
menor a mayor (orden por defecto de `std::map`). El `queue` interno de cada
precio aporta el desempate FIFO: `contadorSecuencia` le da a cada orden un
número creciente y se compara cuando dos órdenes empatan en precio. Así el
criterio **precio-tiempo** del enunciado sale de la estructura de datos, sin
ninguna comparación explícita en el bucle.

El matching (`realizarMatching`) toma el mejor bid y el mejor ask; si el precio
de compra supera al de venta, cruza `min(kwh_bid, kwh_ask)` al **precio medio
de los dos precios cotizados**, que es la regla del enunciado.

Dos detalles que costaron encontrar:

- **Autoconsumo.** El matching ignora un bid cuyo nodo es el mismo que el ask.
  Un prosumidor que vende y compra en el mismo tick no puede comprarse a sí
  mismo.
- **Compras sin saldo.** Cuando el motor en memoria ve que el comprador no
  alcanza, no descarta la orden: la aparta en `pendientesPorSaldo` y al final
  del tick la reintenta (`reevaluarPendientesPorSaldo`). Da más chance de que
  una compra que el resumen de saldos no alcanzaba termine=, después de que
  otras transacciones del mismo tick equilibraron las cuentas.

### 2.3 La batería entra antes que el CSV

La especificación dice que la oferta de la batería "se inserta al inicio del
libro de ventas antes de procesar las del CSV". Eso no es cosmético: como el
desempate en el mismo precio es FIFO, insertarla primero le da prioridad
precio-tiempo a la energía almacenada. El caso 4 del enunciado depende de
esto.

Por eso la orquestración del tick vive en `GridManager::procesarTick` y no en
`main.cpp`:

```cpp
std::vector<TransaccionEnergia>
procesarTick(const std::vector<Orden> &ofertasCSV,
             NodoAlmacenamiento *bateria, double precioBaseHora) {
  ordenesDescartadas_ = 0;
  if (bateria) insertarOfertaBateria(bateria->getId(),
                                     bateria->getCargaActual(), precioBaseHora);
  for (const auto &o : ofertasCSV) insertarOrden(o);
  ejecutarMatching();
  if (bateria) transferirExcedentesABateria(*bateria, precioBaseHora);
  reevaluarPendientesPorSaldo();
  return transaccionesDelTick;
}
```

`main.cpp` solo llama a eso y persiste. La ventaja práctica es que las pruebas
del motor ejercitan exactamente el mismo camino de código que producción: si
se desincronizan, las pruebas lo detectan.

### 2.4 La batería se descarga después del COMMIT

Durante el matching la batería "vende" en memoria. Si la base de datos
después rechaza el tick completo, esa venta no ocurrió, y la carga debería
volver a como estaba. `main.cpp` lo resuelve guardando una foto del estado al
inicio del tick:

```cpp
std::map<int, double> saldosPrevio;   // saldos al abrir el tick
const double cargaPrevia = bateria ? bateria->getCargaActual() : 0.0;
```

Si `persistirTick` devuelve `ok == false`, restaura saldos y
`bateria->restablecerCarga(cargaPrevia)`. Verificado: con `--trigger-prueba`
el tick 17 se rechaza y la carga de la batería al terminar ese tick es
exactamente la misma que al terminar el 16 (108,3 kWh).

### 2.5 RAII

El mapa de nodos es `std::map<int, std::unique_ptr<NodoRed>>`. No hay un solo
`delete` en el proyecto. `NodoRed` tiene destructor virtual, así que
destruir un `NodoRed*` que en realidad es un `NodoAlmacenamiento` es correcto.
ASan con `detect_leaks=1` no reporta fugas ni en la corrida completa ni en la
ruta de rollback.

---

## 3. Persistencia

### 3.1 Un tick es atómico

`persistirTick` envuelve el tick entero en `BEGIN IMMEDIATE ... COMMIT`. Usa
`IMMEDIATE` y no `BEGIN` a propósito: `IMMEDIATE` toma el lock de escritura
desde el BEGIN, así que un conflicto de concurrencia aparece al **inicio** del
tick y no a mitad del lote de INSERTs, con el bloque a medio hacer.

Si algo falla —el trigger de saldo, un `CHECK`, una FK— se hace `ROLLBACK` del
bloque completo y no queda **nada** de ese tick. Las pruebas lo verifican
explícitamente: dos transacciones donde la segunda viola una FK, y se
comprueba que la primera (válida) tampoco quedó.

### 3.2 `actualizar_saldo_y_lecturas`: el procedimiento almacenado

El enunciado (sección 4.2) pide un procedimiento almacenado:

```sql
actualizar_saldo_y_lecturas(p_id_nodo, p_kwh, p_precio, p_tipo_operacion)
```

que actualice `saldo_cuenta` en `NODOS` e inserte en `LECTURAS_HISTORICAS`, y
que se ejecute después de cada transacción confirmada.

**SQLite no tiene procedimientos almacenados.** Hay dos caminos posibles:

1. Dejar la lógica en C++ y documentar que SQLite no los tiene. Es lo más
   barato, pero deja el requisito sin cumplir y deja la lógica de saldos
   desparramada por el bucle de transacciones.
2. Registrar una **función SQL de aplicación** con esa firma exacta
   (`sqlite3_create_function_v2`) e invocarla desde C++ igual que se
   invoca un procedimiento en Oracle:

   ```sql
   SELECT actualizar_saldo_y_lecturas(?, ?, ?, ?);
   ```

Se implementó la opción 2 (`gestionDatos::callbackActualizarSaldoYLecturas`).
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
que es exactamente lo mismo que ve el motor de matching en memoria. Si se
invirtiera el orden, el trigger validaría contra el saldo de arranque del tick
y dejaría pasar compras que el motor nunca generaría.

Es una adaptación, no una equivalencia: sigue siendo SQLite, y sigue sin
haber un servidor Oracle detrás. Lo que se conserva es la encapsulación (saldo
y lectura detrás de un único punto de entrada con la firma del enunciado) y el
orden de operaciones. Ver §8 para el punto que no se resolvió.

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
correcto: dentro de un mismo tick un nodo puede hacer tres compras y el saldo
tiene que descontar las tres.

### 3.4 Coherencia memoria ↔ base, antes del COMMIT

Antes de confirmar, `persistirTick` relee todos los saldos y los compara
contra el ledger del tick: `saldo_inicial ± Σ montos de las transacciones del
tick`. Si la base y el ledger no coinciden, o el modelo en memoria no
coincide con el ledger, el tick se revierte con un mensaje que dice cuál de
los dos nodos falló y con qué números.

Esto convierte una clase de bugs Invisible —el modelo de dominio y la base
discrepando en silencio— en un rollback con diagnóstico. El costo es una
lectura de `NODOS` por tick, despreciable para 24 ticks.

---

## 4. Robustez de la entrada

### 4.1 Los CSV ya no matan el proceso

El parser de CSV hace:

```cpp
o.kwh    = std::stod(columnas[3]);   // lanza std::invalid_argument
o.precio = std::stod(columnas[4]);
```

Un `kwh` con letra anywhere abortaba el proceso con
`terminate called after throwing an instance of 'std::invalid_argument'`,
exit 134, y se perdía la simulación entera. Ahora hay un
`leerCSVConInforme` que devuelve las filas válidas y las rechazadas, con
motivo y número de línea:

```
[CSV] ./datos/ofertas_23.csv - linea 2: kwh no es un numero valido -> 6,venta,4,abc,0.95
```

El parseo es explícito y no usa excepciones: `std::stod` acepta sufijos
(`"5kg"` → 5) y lanza si no hay número, así que el helper rechaza cualquier
resto no numérico alrededor del valor, y rechaza `inf`/`nan` con
`std::isfinite`.

Los cinco modos de falla que se probaron dan **exit 0** y un diagnóstico:

| Entrada | Resultado |
|---|---|
| `kwh` no numérico | `kwh no es un numero valido` |
| `kwh` con sufijo (`5kg`) | `kwh no es un numero valido` |
| `lado` inválido | `lado 'tal' invalido` |
| Faltan columnas | `se esperaban 5 columnas, llegaron 4` |
| Archivo inexistente | `archivo inexistente o ilegible` |

### 4.2 Las órdenes se validan antes de entrar al libro

`insertarOrden` rechaza y cuenta, con log, toda orden que no pueda ser
válida:

- **`precio <= 0`** — el enunciado prohíbe precios negativos. Además
  `TRANSACCIONES` tiene `CHECK (precio_unitario > 0)`.
- **`kwh <= UMBRAL`** — acá estaba el defecto más caro. Una fila con `kwh = 0`
  pasaba el matching, generaba una transacción de 0 kWh, y el
  `CHECK (kwh > 0)` de la base abortaba el tick **entero**: se perdían
  también las transacciones legítimas del mismo tick. Ahora la orden se
  descarta antes de tocar el libro, y el tick sigue.
- **Nodo inexistente** — el enunciado (sección 6.1) pide que los nodos
  existan en la base. Sin esta comprobación, un id inexistente entraba al
  libro, `consultarSaldo` devolvía 0, y el matching lo descartaba con el
  diagnóstico equivocado ("compra sin saldo") cuando el problema real era que
  el nodo no existe.

El total del día se informa en el resumen como *Órdenes descartadas por datos
inválidos*.

---

## 5. Correcciones de datos y de esquema

### 5.1 `CONFIG_TARIFAS` no coincidía con el enunciado

Cinco de las 24 horas tenían un precio base distinto del que usan los casos de
aceptación del PDF:

| Hora | Valor en el esquema | Valor del PDF | Efecto |
|---|---|---|---|
| 10 | 1,50 | **1,00** | La batería de la tarde sobrevaloraba su energía |
| 12 | 1,60 | **1,50** | Caso 2, excedente que va a batería |
| 14 | 1,55 | **1,00** | Caso 4, carga de la batería |
| 15 | 1,52 | **1,20** | **Caso 4, tick 15: la batería no cruzaba** |
| 18 | 1,95 | **2,00** | Caso 3 |

El de las 15:00 era el que rompía un caso de aceptación: la batería pedía
1,52 y la única orden de compra del tick cotizaba 1,50, así que no cruzaba y
el tick 15 no generaba **ninguna** transacción. Con 1,20 el tick produce la
transacción que el enunciado describe.

Están fijadas a los valores del PDF y el esquema lo advierte en un comentario,
incluyendo el por qué del caso 4. `test_datos` verifica las cinco contra el
esquema, así que un cambio accidental se detecta.

### 5.2 La batería pasó a ser el nodo 99

El enunciado usa el nodo 99 como batería comunitaria ("configurado en BD con
saldo ilimitado para simplificar"). El proyecto usaba el 5. Se cambió a 99
para que el código, el esquema y los casos del enunciado hablen del mismo
nodo. Ningún CSV referenciaba el nodo 5, así que el cambio no affecta las
órdenes.

`BATERIA` (en `types.hpp`) y la semilla del esquema tienen que coincidir;
`test_datos` lo verifica.

### 5.3 `excedente_neto` de la batería tenía el signo invertido

`excedente_neto` se calculaba como `produccion - consumo` para todos los
nodos. Para la batería eso está al revés: la batería no produce, almacena. Si
en un tick compraba 23 kWh y revendía 18, había almacenado 5, y la fórmula
común daba −5. Ahora es `consumo - produccion` para el nodo de almacenamiento
y sigue siendo `produccion - consumo` para el resto.

Verificado sobre la corrida: la batería del tick 12 vendió 18 y compró 23, y
las filas de `LECTURAS_HISTORICAS` suman **+5,0**, que es la carga neta
realmente almacenada.

### 5.4 `tick_hora` pasó a ser un timestamp

`LECTURAS_HISTORICAS.tick_hora` era un `TEXT` con `"07"`, `"15"`. El
enunciado lo pide como `DATE -- timestamp simulado`. Ahora se arma
`<fecha_simulada> HH:00:00`, con la fecha base en `config.ini`
(`fecha_simulada=2026-06-01`), y queda `2026-06-01 15:00:00`.

`timestampTick` valida que la fecha tenga forma `AAAA-MM-DD` y, si no, usa una
de respaldo con un aviso: un valor mal escrito en `config.ini` no puede
tumbar la simulación entera.

### 5.5 Higiene de datos

- Siete CSV (`01, 10, 12, 14, 15, 18, 23`) no terminaban en salto de línea. Al
  concatenarlos, la última fila se pegaba a la cabecera del siguiente archivo.
  Agregado el salto de línea.
- Borrado `datos/.~lock.ofertas_00.csv#`, un archivo de bloqueo de LibreOffice
  que se había quedado.
- Borrado `main.o` de la raíz, un objeto de 451 KB de una versión vieja del
  Makefile.
- `.gitignore` ahora cubre `build/`, `test_datos.db` y `rollback.log`.

---

## 6. Pruebas

`make test` corre 58 pruebas. No tocan `ejemplo.db`: `test_datos` usa
`test_datos.db` y lo borra al terminar.

```
build/test_motor   31 pruebas   no enlaza -lsqlite3
build/test_datos   27 pruebas   usa el esquema real de sql/crear_esquema.sql
```

### 6.1 `test_motor` — los casos del enunciado

Cada caso del PDF está reproducido con el estado inicial que el enunciado
supone, y se comparan **transacción por transacción y saldo por saldo**:

- **Ejemplo numérico §5.3** — 8 kWh a precio medio 2,75; la segunda compra no
  cruza por debajo del precio de venta.
- **Caso 1** — subasta básica: 1 transacción, saldos 88,75 y 61,25.
- **Caso 2** — prioridad precio-tiempo con 5 órdenes: 4 transacciones en el
  orden exacto, y 5 kWh a la batería.
- **Caso 3** — saldo insuficiente: el nodo 30 tiene 15 y la compra cuesta
  22,50; no compra y el tick no aborta.
- **Caso 4** — dos ticks con batería: carga 0 → 8 kWh (t14), y en t15 la
  batería vende las 8 a 1,35 con comprador.
- Reglas de negocio: kWh 0, kWh negativo, precio 0, precio negativo, nodo
  inexistente, autoconsumo, batería llena, orden FIFO de la batería, tick sin
  órdenes.

### 6.2 `test_datos` — la base real

- `trg_validar_saldo` rechaza la compra sin saldo, y el motivo dice "Trigger".
- Los tres `CHECK` (`kwh > 0`, `precio_unitario > 0`) y la FK.
- **Atomicidad**: con dos transacciones donde la segunda viola una FK, la
  primera tampoco queda.
- Tick válido: 1 transacción, 2 lecturas, saldos correctos en la base.
- El procedimiento `actualizar_saldo_y_lecturas`: la operación `compra` anda,
  y un `tipo_operacion` inválido o un nodo inexistente se rechazan.
- Detección de memoria desincronizada contra el ledger.
- Las 5 tarifas de los casos del PDF.
- La semilla: existe el nodo 99 y coincide con `BATERIA`.
- 7 casos de CSV: 5 inválidos, 1 inexistente, 1 válido.

### 6.3 Verificación adicional

- **Determinismo**: dos corridas limpias producen transacciones, lecturas y
  saldos idénticos. Lo único que cambia entre corridas es
  `TRANSACCIONES.fecha_transaccion`, que es la hora de reloj y por lo tanto
  debe cambiar.
- **ASan + UBSan** (`-fsanitize=address,undefined`, `detect_leaks=1`): sin
  fugas ni comportamiento indefinido, tanto en la corrida completa como en la
  ruta de rollback con `--trigger-prueba`, y en las dos suites de pruebas.
- **`PRAGMA integrity_check`** y **`PRAGMA foreign_key_check`**: limpios.

### 6.4 Código muerto

Se auditó cada función, método y campo del proyecto contando sus
referencias reales. Se eliminó lo que no tenía ningún llamador:

| Eliminado | Por qué |
|---|---|
| `NodoAlmacenamiento::getCapacidadMax()` | `estado()` y `capacidadDisponible()` leen el atributo directo |
| `TransaccionEnergia::getMontoTotal()` | el monto se calcula inline como `kwh * precio` |
| `GridManager::getTransacciones()` | `procesarTick` devuelve `transaccionesDelTick` directo |
| `GridManager::libroVacio()` | sin llamadores |
| `gestionDatos::leerCSV()` | wrapper de `leerCSVConInforme` sin ningún llamador |
| `udfAlloc()` / `udfFree()` | `sqlite3_create_function_v2` **no recibe** callbacks de allocator: su firma es `(db, nombre, nArg, eTextRep, pApp, xFunc, xStep, xFinal, xDestroy)`. No podían invocarse nunca |
| `TransaccionEnergia() = default` | nadie default-construye la entidad |
| `<fstream>`, `<iomanip>`, `<optional>` de `types.hpp`; `<unordered_map>` de `config.hpp`; `<fstream>`, `<sstream>`, `<utility>` de `main.cpp` | sin uso |

Quitar `<iomanip>` de `types.hpp` destapó que `tests/test_datos.cpp` usaba
`std::setprecision` confiando en ese include transitivo; se le agregó el
include propio. Es el tipo de dependencia oculta que aparecen al borrar
includes en vez de dejarlos.

**Lo que NO se eliminó, y por qué:**

- **`calcularExcedente()`** — se declara, se sobrescribe en las tres subclases
  y **no se llama nunca**. No es código muerto: el enunciado lo exige
  literalmente (`virtual double calcularExcedente() = 0;` en `NodoRed`, más el
  valor que debe retornar cada subclase) y además exige "uso sólido de POO…
  herencia, polimorfismo". Es la única función virtual del diseño, así que
  borrarla dejaría de ser polimórfica la jerarquía. Queda como interfaz
  especificada y sin integrar: ver §8.
- **`TransaccionEnergia::timestamp`** — se escribe en el constructor y no se
  lee (la columna `fecha_transaccion` la completa SQLite con
  `CURRENT_TIMESTAMP`). El enunciado pide el atributo, así que se conserva.
- **`Orden::idOrden` y `secuencia`** — el enunciado los declara. `idOrden` se
  parsea y valida pero no se lee; `secuencia` sí, es el desempate FIFO.

---

## 7. Diagramas

`docs/diagrama_clases.puml` y `docs/diagrama_entidad_relacion.puml` son la
fuente de verdad (son texto: se versionan, se revisan en un diff y se
regeneran). Los `.png`/`.pdf` que pide el enunciado son artefactos de build:

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

## 8. Lo que no se resolvió, y por qué

### 8.1 Oracle + SOCI

El enunciado (sección 4.3) pide que la capa de datos use **Oracle** con la
librería **SOCI**. El proyecto usa SQLite desde el principio.

No se implementó, y la decisión fue deliberada: no hay una instancia de
Oracle disponible acá, y **no hay forma de verificar el resultado**. Agregar
un backend SOCI sin poder compilarlo ni correrlo habría dejado en el
repositorio código que no se sabe si funciona, que rompen el `make` de
cualquiera que no tenga SOCI instalado, y que no se habría podido probar
contra los casos de aceptación. Eso es peor que no tenerlo.

Lo que sí quedó preparado para que el cambio sea acotado:

- `CapaDatos` es **la única** clase que incluye `sqlite3.h`. Todo el SQL está
  dentro de ella, así que la superficie a migrar está delimitada.
- El motor no sabe qué es SQL, así que no hay nada que migrar ahí.
- `actualizar_saldo_y_lecturas` ya tiene la firma exacta del procedimiento
  que pide el enunciado. En Oracle sería un `CREATE OR REPLACE PROCEDURE` y
  la llamada `SELECT ... FROM actualizar_saldo_y_lecturas(...)` no cambia en
  C++.

Si se consigue acceso a una instancia Oracle, el trabajo es escribir un
`CapaDatosOracle` con la misma interfaz pública y elegirlo por configuración.

### 8.2 `calcularExcedente()` está definido pero no integrado

La función virtual que el enunciado define para las tres subclases de
`NodoRed` no se invoca desde ningún punto del flujo: ni el matching, ni la
transferencia de excedentes, ni la persistencia la llaman. La jerarquía se
construye y se usa (id, saldo, y los métodos de la batería), pero el
polimorfismo de `calcularExcedente` no participa de ninguna decisión.

Lo más probable es que sea una funcionalidad que quedó a medio integrar, no
que sobre: es exactamente el hook que usaría el paso "Excedentes a batería"
para saber cuánto sobró de cada nodo, que hoy se calcula de otra forma. Se
dejó intacta por ser requisito explícito del enunciado (§6.4).

### 8.3 Otros puntos

- **`docs/` en PNG/PDF** — ver §7. Faltan los archivos renderizados.
- **Los CSV y las tarifas son datos de la cátedra.** Los CSVs de `datos/` se
 dejaron tal como estaban, salvo el salto de línea final. Las tarifas se
 tocaron solo en las 5 horas que los casos del enunciado fijan; el resto de la
 curva es una aproximación con la misma forma, y el esquema lo dice.
- **Sin `delete`, sin `new`/`delete` manual** — resuelto con `unique_ptr` en
  §2.5.
- **La carga de la batería no está en el esquema.** `NODOS` tiene
  `saldo_cuenta` pero no `carga_kwh` / `capacidad_kwh`: la batería vive en
  memoria durante el día y se reinicia en 0 con cada `--reset`. Si el
  enunciado espera que la carga sobreviva entre corridas, falta la columna y
  la carga/descarga no se persisten. No se agregó porque el esquema del
  enunciado (sección 4.2) no la lista.

---

## 9. Resultado de una corrida

```
$ ./ecogrid --reset
Esquema y trigger creados correctamente.
 ... 24 ticks ...
Transacciones persistidas: 87
Energía transada: 844.7 kWh
Lecturas históricas: 174
Órdenes descartadas por datos inválidos: 0
Filas de CSV rechazadas: 0
Simulación finalizada.
```

Con `--trigger-prueba` (un trigger extra que rechaza toda compra individual de
30 créditos o más):

```
Transacciones persistidas: 72
Energía transada: 628.3 kWh
```

5 de los 24 ticks se rechazan y revierten enteros: 17, 19, 20, 21 y 23. Es la
forma de ver el mecanismo de `ROLLBACK` de punta a punta, que es lo que
justifica la existencia del flag.
