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
-- Lecturas históricas de producción/consumo por tick
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
  fecha_transaccion TEXT DEFAULT CURRENT_TIMESTAMP,
  FOREIGN KEY (id_vendedor) REFERENCES NODOS(id_nodo),
  FOREIGN KEY (id_comprador) REFERENCES NODOS(id_nodo)
);

-- ------------------------------------------------------------
-- Precio base horario (24 horas) al que la batería compra los
-- excedentes sin comprador. OR REPLACE: idempotente al reejecutar.
-- ------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CONFIG_TARIFAS (
  hora INTEGER PRIMARY KEY CHECK (hora BETWEEN 0 AND 23),
  precio_base_kwh REAL NOT NULL
);

INSERT OR REPLACE INTO CONFIG_TARIFAS (hora, precio_base_kwh) VALUES
  (0, 0.85), (1, 0.80), (2, 0.78), (3, 0.78),
  (4, 0.82), (5, 0.90), (6, 1.05), (7, 1.20),
  (8, 1.35), (9, 1.45), (10, 1.50), (11, 1.55),
  (12, 1.60), (13, 1.58), (14, 1.55), (15, 1.52),
  (16, 1.55), (17, 1.70), (18, 1.95), (19, 2.20),
  (20, 2.35), (21, 2.05), (22, 1.55), (23, 1.10);

-- ------------------------------------------------------------
-- Nodos iniciales (semilla). INSERT OR IGNORE: no pisa saldos
-- ya persistidos entre ejecuciones.
-- ------------------------------------------------------------
INSERT OR IGNORE INTO NODOS (id_nodo, ubicacion, tipo, saldo_cuenta, perfil_consumo) VALUES
  (1,  'Residencial A', 'Consumidor', 100, 'Residencial'),
  (2,  'Casa Solar',    'Prosumidor', 50,  NULL),
  (3,  'Industrial A',  'Consumidor', 100, 'Industrial'),
  (4,  'Prosumidor D',  'Prosumidor', 150, NULL),
  (5,  'Bateria E',     'Bateria',    1000000, NULL),
  (6,  'Solar 6',       'Prosumidor', 100, NULL),
  (10, 'Industrial B',  'Consumidor', 500, 'Industrial'),
  (11, 'Comercial A',   'Consumidor', 200, 'Comercial'),
  (20, 'Solar 1',       'Prosumidor', 100, NULL),
  (21, 'Solar 2',       'Prosumidor', 80,  NULL),
  (22, 'Solar 3',       'Prosumidor', 50,  NULL),
  (30, 'Residencial B', 'Consumidor', 15,  'Residencial'),
  (40, 'Solar 4',       'Prosumidor', 60,  NULL),
  (50, 'Solar 5',       'Prosumidor', 100, NULL),
  (51, 'Residencial C', 'Consumidor', 200, 'Residencial');

-- ------------------------------------------------------------
-- Disparador: rechaza una compra si el comprador no alcanza
-- a cubrir el monto (kwh * precio). Aborta la transacción.
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