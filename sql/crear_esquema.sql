-- ============================================================
-- EcoGrid - Script de creación del esquema (SQLite3)
-- Asignatura: Programación Orientada a Objetos - 2026
-- Tablas, datos semilla y trigger de validación de saldo.
-- ============================================================

PRAGMA foreign_keys = ON;

-- ------------------------------------------------------------
-- Nodos de la red (consumidores, prosumidores y batería)
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS NODOS (
  id_nodo INTEGER PRIMARY KEY,
  ubicacion TEXT NOT NULL,
  tipo TEXT NOT NULL CHECK (tipo IN ('Consumidor', 'Prosumidor', 'Bateria')),
  saldo_cuenta REAL DEFAULT 0 CHECK (saldo_cuenta >= 0),
  perfil_consumo TEXT
);

-- ------------------------------------------------------------
-- Lecturas históricas de producción/consumo por tick.
-- tick_hora es el timestamp simulado del tick (el PDF lo pide como
-- DATE), no la hora de reloj: se construye como <fecha_simulada> + hora.
-- El prefijo de la fecha base vive en src/util/config.ini.
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS LECTURAS_HISTORICAS (
  id_lectura INTEGER PRIMARY KEY AUTOINCREMENT,
  id_nodo INTEGER NOT NULL,
  tick_hora TEXT NOT NULL,
  produccion_kwh REAL DEFAULT 0,
  consumo_kwh REAL DEFAULT 0,
  excedente_neto REAL,
  FOREIGN KEY (id_nodo) REFERENCES NODOS(id_nodo)
);

-- ------------------------------------------------------------
-- Transacciones de energía realizadas
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS TRANSACCIONES (
  id_transaccion INTEGER PRIMARY KEY AUTOINCREMENT,
  id_vendedor INTEGER NOT NULL,
  id_comprador INTEGER NOT NULL,
  kwh REAL NOT NULL CHECK (kwh > 0),
  precio_unitario REAL NOT NULL CHECK (precio_unitario > 0),
  tick_hora INTEGER CHECK (tick_hora BETWEEN 0 AND 23),
  fecha_transaccion TEXT DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (id_vendedor) REFERENCES NODOS(id_nodo),
  FOREIGN KEY (id_comprador) REFERENCES NODOS(id_nodo)
);

-- ------------------------------------------------------------
-- Precio base horario (24 horas) al que la batería compra los
-- excedentes sin comprador. OR REPLACE: idempotente al reejecutar.
--
-- Las horas 10, 12, 14, 15 y 18 están fijadas a los valores que el
-- PDF usa en los casos de aceptación (sección "Casos de Uso de
-- Ejemplo y Validación"). El resto de la curva es una aproximación
-- con la misma forma de valle-medio/pico-noche.
--
-- ADVERTENCIA: tocar estos valores cambia el resultado de los casos
-- del PDF. El caso 4 (tick 15) depende de que 15:00 sea 1.20: si
-- la tarifa fuera mayor que el precio de la orden de compra (1.50),
-- la batería no cruza con el comprador y el tick no genera ninguna
-- transacción.
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CONFIG_TARIFAS (
  hora INTEGER PRIMARY KEY CHECK (hora BETWEEN 0 AND 23),
  precio_base_kwh REAL NOT NULL
);

INSERT OR REPLACE INTO CONFIG_TARIFAS (hora, precio_base_kwh) VALUES
  (0, 0.85), (1, 0.80), (2, 0.78), (3, 0.78),
  (4, 0.82), (5, 0.90), (6, 1.05), (7, 1.20),
  (8, 1.35), (9, 1.45), (10, 1.00), (11, 1.25),
  (12, 1.50), (13, 1.40), (14, 1.00), (15, 1.20),
  (16, 1.35), (17, 1.60), (18, 2.00), (19, 2.20),
  (20, 2.35), (21, 2.05), (22, 1.55), (23, 1.10);

-- ------------------------------------------------------------
-- Nodos iniciales (semilla). INSERT OR IGNORE: no pisa saldos
-- ya persistidos entre ejecuciones.
--
-- La batería comunitaria es el nodo 99, que es el id que usan los
-- casos de aceptación del PDF (debe coincidir con BATERIA en
-- src/types.hpp). Tiene saldo alto para que nunca la bloquee el
-- CHECK (saldo_cuenta >= 0), como pide el PDF ("ilimitado para
-- simplificar"). No aparece en ningún CSV: sólo interviene por
-- su cuenta.
-- ------------------------------------------------------------
INSERT OR IGNORE INTO NODOS (id_nodo, ubicacion, tipo, saldo_cuenta, perfil_consumo) VALUES
  (1,  'Residencial A', 'Consumidor', 100, 'Residencial'),
  (2,  'Casa Solar',    'Prosumidor', 50,  NULL),
  (3,  'Industrial A',  'Consumidor', 100, 'Industrial'),
  (4,  'Prosumidor D',  'Prosumidor', 150, NULL),
  (6,  'Solar 6',       'Prosumidor', 100, NULL),
  (10, 'Industrial B',  'Consumidor', 500, 'Industrial'),
  (11, 'Comercial A',   'Consumidor', 200, 'Comercial'),
  (20, 'Solar 1',       'Prosumidor', 100, NULL),
  (21, 'Solar 2',       'Prosumidor', 80,  NULL),
  (22, 'Solar 3',       'Prosumidor', 50,  NULL),
  (30, 'Residencial B', 'Consumidor', 15,  'Residencial'),
  (40, 'Solar 4',       'Prosumidor', 60,  NULL),
  (50, 'Solar 5',       'Prosumidor', 100, NULL),
  (51, 'Residencial C', 'Consumidor', 200, 'Residencial'),
  (99, 'Bateria Comunitaria', 'Bateria', 1000000, NULL);

-- ------------------------------------------------------------
-- Disparador: rechaza una compra si el comprador no alcanza
-- a cubrir el monto (kwh * precio). Aborta la transacción.
--
-- Nota: se valida contra el saldo VIVO de NODOS, y la capa de datos
-- descuenta cada transacción a medida que la inserta. Por eso el
-- trigger ve el saldo ya ajustado por las transacciones anteriores
-- del mismo tick, igual que el motor de matching en memoria.
-- ------------------------------------------------------------
CREATE TRIGGER IF NOT EXISTS trg_validar_saldo
BEFORE INSERT ON TRANSACCIONES
FOR EACH ROW
BEGIN
  SELECT CASE
    WHEN (
      SELECT saldo_cuenta
      FROM NODOS
      WHERE id_nodo = NEW.id_comprador
    ) < (NEW.kwh * NEW.precio_unitario)
    THEN RAISE(ABORT, 'Saldo insuficiente para realizar la compra')
  END;
END;

-- ------------------------------------------------------------
-- Procedimiento almacenado exigido por el PDF (sección 4.2):
--
--   actualizar_saldo_y_lecturas(p_id_nodo, p_kwh, p_precio,
--                              p_tipo_operacion)
--
-- SQLite no tiene procedimientos almacenados, así que la capa de
-- datos lo registra como FUNCIÓN SQL de aplicación
-- (sqlite3_create_function) con esa firma exacta, y se invoca desde
-- C++ con la misma forma que en Oracle:
--
--   SELECT actualizar_saldo_y_lecturas(?, ?, ?, ?);
--
-- Eso conserva el punto del enunciado: la actualización de
-- saldo_cuenta en NODOS y el registro en LECTURAS_HISTORICAS están
-- encapsulados detrás de un único punto de entrada con esa firma, y
-- la lógica no queda expuesta en el bucle de matching. Todo esto sin
-- depender de un servidor Oracle para poder correr la simulación.
--
-- p_tipo_operacion: 'compra' (descuenta saldo) | 'venta' (acumula).
-- El cuerpo vive en gestionDatos::callbackActualizarSaldoYLecturas.
-- Ver src/GestionDatos.hpp.
-- ------------------------------------------------------------
